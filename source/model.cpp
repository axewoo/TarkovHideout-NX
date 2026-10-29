#include "model.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

const Station *HideoutData::station(const std::string &id) const {
    for (const auto &s : stations)
        if (s.id == id)
            return &s;
    return nullptr;
}

int State::builtLevel(const Station &st) const {
    int built = 0;
    for (const auto &lv : st.levels) {
        auto it = modules.find(lv.id);
        if (it == modules.end() || !it->second)
            break;
        built = lv.level;
    }
    return built;
}

int State::have(const ReqItem &r) const {
    auto it = parts.find(r.id);
    if (it == parts.end())
        return 0;
    return it->second < r.count ? it->second : r.count;
}

static std::string str(const json &j, const char *key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return "";
    return it->get<std::string>();
}

static int num(const json &j, const char *key, int def = 0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return def;
    return it->get<int>();
}

static std::vector<CraftItem> craftItems(const json &arr) {
    std::vector<CraftItem> out;
    if (!arr.is_array())
        return out;
    for (const auto &e : arr) {
        CraftItem ci;
        ci.name = str(e, "n");
        ci.shortName = str(e, "s");
        ci.count = num(e, "c", 1);
        out.push_back(ci);
    }
    return out;
}

bool parseHideout(const std::string &text, HideoutData &out, std::string &err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "hideout data is not valid JSON";
        return false;
    }
    HideoutData d;
    d.mode = str(j, "mode");
    d.generated = str(j, "generated");
    for (const auto &s : j.value("stations", json::array())) {
        Station st;
        st.id = str(s, "id");
        st.name = str(s, "name");
        for (const auto &l : s.value("levels", json::array())) {
            Level lv;
            lv.id = str(l, "id");
            lv.level = num(l, "lvl");
            lv.constructionTime = num(l, "time");
            for (const auto &r : l.value("items", json::array())) {
                ReqItem ri;
                ri.id = str(r, "id");
                ri.itemId = str(r, "item");
                ri.name = str(r, "n");
                ri.shortName = str(r, "s");
                ri.count = num(r, "c", 1);
                ri.fir = r.value("fir", false);
                lv.items.push_back(ri);
            }
            for (const auto &o : l.value("other", json::array()))
                if (o.is_string())
                    lv.other.push_back(o.get<std::string>());
            st.levels.push_back(lv);
        }
        d.stations.push_back(st);
    }
    for (const auto &c : j.value("crafts", json::array())) {
        Craft cr;
        cr.id = str(c, "id");
        cr.stationId = str(c, "st");
        cr.level = num(c, "lvl");
        cr.duration = num(c, "dur");
        cr.req = craftItems(c.value("req", json::array()));
        cr.rew = craftItems(c.value("rew", json::array()));
        d.crafts.push_back(cr);
    }
    if (d.stations.empty()) {
        err = "hideout data has no stations";
        return false;
    }
    out = std::move(d);
    return true;
}

bool loadState(const std::string &path, State &st) {
    std::ifstream f(path);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    st.token = str(j, "token");
    if (!str(j, "mode").empty())
        st.mode = str(j, "mode");
    st.syncOnLaunch = j.value("syncOnLaunch", true);
    st.lastSync = str(j, "lastSync");
    st.account = str(j, "account");
    st.modules.clear();
    st.parts.clear();
    st.timers.clear();
    const json mods = j.value("modules", json::object());
    const json parts = j.value("parts", json::object());
    for (auto &[k, v] : mods.items())
        if (v.is_boolean())
            st.modules[k] = v.get<bool>();
    for (auto &[k, v] : parts.items())
        if (v.is_number())
            st.parts[k] = v.get<int>();
    for (const auto &t : j.value("timers", json::array())) {
        Timer tm;
        tm.craftId = str(t, "craft");
        tm.endTime = t.value("end", (int64_t)0);
        if (!tm.craftId.empty())
            st.timers.push_back(tm);
    }
    return true;
}

bool saveState(const std::string &path, const State &st) {
    json j;
    j["token"] = st.token;
    j["mode"] = st.mode;
    j["syncOnLaunch"] = st.syncOnLaunch;
    j["lastSync"] = st.lastSync;
    j["account"] = st.account;
    json mods = json::object();
    for (auto &[k, v] : st.modules)
        if (v)
            mods[k] = true;
    j["modules"] = mods;
    json parts = json::object();
    for (auto &[k, v] : st.parts)
        if (v > 0)
            parts[k] = v;
    j["parts"] = parts;
    json timers = json::array();
    for (const auto &t : st.timers)
        timers.push_back({{"craft", t.craftId}, {"end", t.endTime}});
    j["timers"] = timers;

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f)
            return false;
        f << j.dump(1);
        if (!f)
            return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

bool applyTrackerProgress(const std::string &text, const HideoutData &data, State &st, std::string &err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "TarkovTracker sent invalid JSON";
        return false;
    }
    if (!j.value("success", false) || !j.contains("data")) {
        err = j.value("error", std::string("TarkovTracker refused the request"));
        return false;
    }
    const json &d = j["data"];
    std::map<std::string, int> reqCount;
    for (const auto &s : data.stations)
        for (const auto &l : s.levels)
            for (const auto &r : l.items)
                reqCount[r.id] = r.count;

    st.modules.clear();
    st.parts.clear();
    for (const auto &m : d.value("hideoutModulesProgress", json::array()))
        if (m.value("complete", false))
            st.modules[str(m, "id")] = true;
    for (const auto &p : d.value("hideoutPartsProgress", json::array())) {
        std::string id = str(p, "id");
        int count = num(p, "count");
        if (p.value("complete", false)) {
            auto it = reqCount.find(id);
            if (it != reqCount.end() && it->second > count)
                count = it->second;
        }
        if (count > 0)
            st.parts[id] = count;
    }
    st.account = str(d, "displayName");
    return true;
}

std::string formatDuration(int64_t s) {
    if (s < 0)
        s = 0;
    char buf[32];
    int64_t h = s / 3600, m = (s / 60) % 60, sec = s % 60;
    if (h > 0)
        snprintf(buf, sizeof buf, "%lldh %02lldm", (long long)h, (long long)m);
    else if (m > 0)
        snprintf(buf, sizeof buf, "%lldm %02llds", (long long)m, (long long)sec);
    else
        snprintf(buf, sizeof buf, "%llds", (long long)sec);
    return buf;
}
