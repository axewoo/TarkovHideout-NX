#include "model.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
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

int HideoutData::icon(const std::string &itemId) const {
    auto it = icons.find(itemId);
    return it == icons.end() ? -1 : it->second;
}

int HideoutData::playerLevel(int64_t exp) const {
    // levelExp holds the exp each level costs; level n needs the sum of the first n entries.
    int64_t total = 0;
    int level = 0;
    for (int need : levelExp) {
        total += need;
        if (exp < total)
            break;
        level++;
    }
    return std::max(level, 1);
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
        ci.itemId = str(e, "i");
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
    const json items = j.value("items", json::object());
    for (auto &[k, v] : items.items())
        if (v.is_object() && v.contains("ic") && v["ic"].is_number())
            d.icons[k] = v["ic"].get<int>();
    const json skills = j.value("skills", json::object());
    for (auto &[k, v] : skills.items())
        if (v.is_string())
            d.skills[k] = v.get<std::string>();
    for (const auto &e : j.value("levelExp", json::array()))
        if (e.is_number())
            d.levelExp.push_back(e.get<int>());
    if (d.stations.empty()) {
        err = "hideout data has no stations";
        return false;
    }
    out = std::move(d);
    return true;
}

const DbItem *Database::item(const std::string &id) const {
    auto it = items.find(id);
    return it == items.end() ? nullptr : &it->second;
}

static std::vector<std::string> strings(const json &arr) {
    std::vector<std::string> out;
    if (arr.is_array())
        for (const auto &e : arr)
            if (e.is_string())
                out.push_back(e.get<std::string>());
    return out;
}

static std::vector<Pin> pins(const json &arr) {
    std::vector<Pin> out;
    if (arr.is_array())
        for (const auto &p : arr)
            if (p.is_array() && p.size() == 3 && p[0].is_string())
                out.push_back({p[0].get<std::string>(), p[1].get<float>(), p[2].get<float>()});
    return out;
}

static std::vector<ItemCount> itemCounts(const json &arr) {
    std::vector<ItemCount> out;
    if (arr.is_array())
        for (const auto &p : arr)
            if (p.is_array() && p.size() == 2 && p[0].is_string())
                out.push_back({p[0].get<std::string>(), p[1].is_number() ? p[1].get<int>() : 1});
    return out;
}

static std::string numberText(const json &v) {
    if (v.is_number_integer())
        return std::to_string(v.get<int64_t>());
    if (v.is_number()) {
        char buf[32];
        snprintf(buf, sizeof buf, "%g", v.get<double>());
        return buf;
    }
    return v.is_string() ? v.get<std::string>() : "";
}

bool parseDatabase(const std::string &text, Database &out, std::string &err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "database is not valid JSON";
        return false;
    }
    Database d;
    const json items = j.value("items", json::object());
    for (auto &[id, v] : items.items()) {
        DbItem it;
        it.name = str(v, "n");
        it.shortName = str(v, "s");
        it.category = str(v, "c");
        it.desc = str(v, "d");
        it.price = num(v, "p");
        if (v.contains("t") && v["t"].is_array() && v["t"].size() == 2) {
            it.trader = v["t"][0].is_string() ? v["t"][0].get<std::string>() : "";
            it.traderPrice = v["t"][1].is_number() ? v["t"][1].get<int>() : 0;
        }
        it.weight = v.value("w", 0.f);
        it.noFlea = v.value("nf", false);
        it.questItem = v.value("q", false);
        it.icon = num(v, "ic", -1);
        d.items.emplace(id, std::move(it));
    }
    std::set<std::string> cals;
    for (const auto &a : j.value("ammo", json::array())) {
        Ammo am;
        am.item = str(a, "i");
        am.caliber = str(a, "cal");
        am.damage = num(a, "dmg");
        am.pen = num(a, "pen");
        am.armorDamage = num(a, "ad");
        am.frag = num(a, "fr");
        am.speed = num(a, "v");
        am.projectiles = num(a, "pc", 1);
        am.tracer = a.value("tr", false);
        cals.insert(am.caliber);
        d.ammo.push_back(std::move(am));
    }
    d.calibers.assign(cals.begin(), cals.end());
    for (const auto &k : j.value("keys", json::array())) {
        KeyInfo ki;
        ki.item = str(k, "i");
        ki.uses = num(k, "u");
        ki.quests = strings(k.value("q", json::array()));
        ki.pins = pins(k.value("pins", json::array()));
        d.keys.push_back(std::move(ki));
    }
    for (const auto &t : j.value("tasks", json::array())) {
        Quest q;
        q.id = str(t, "id");
        q.name = str(t, "n");
        q.trader = str(t, "tr");
        q.map = str(t, "map");
        q.faction = str(t, "fac");
        q.wiki = str(t, "wiki");
        q.level = num(t, "lvl");
        q.exp = num(t, "exp");
        q.kappa = t.value("k", false);
        q.lightkeeper = t.value("lk", false);
        q.prereqs = strings(t.value("pre", json::array()));
        q.traderReqs = strings(t.value("trl", json::array()));
        q.keys = strings(t.value("keys", json::array()));
        for (const auto &o : t.value("obj", json::array())) {
            Objective ob;
            ob.desc = str(o, "d");
            ob.type = str(o, "t");
            ob.count = num(o, "c");
            ob.more = num(o, "more");
            ob.optional = o.value("o", false);
            ob.fir = o.value("fir", false);
            ob.items = strings(o.value("it", json::array()));
            ob.pins = pins(o.value("pins", json::array()));
            if (o.contains("gs") && o["gs"].is_object()) {
                const json &g = o["gs"];
                ob.hasBuild = true;
                ob.build.weapon = str(g, "w");
                for (const auto &at : g.value("attrs", json::array()))
                    if (at.is_array() && at.size() == 3)
                        ob.build.attrs.push_back(numberText(at[0]) + " " + numberText(at[1]) + " " + numberText(at[2]));
                ob.build.parts = strings(g.value("parts", json::array()));
                ob.build.categories = strings(g.value("cats", json::array()));
            }
            q.objectives.push_back(std::move(ob));
        }
        const json rw = t.value("rw", json::object());
        for (const auto &r : rw.value("rep", json::array()))
            if (r.is_array() && r.size() == 2 && r[0].is_string())
                q.rewards.rep.push_back({r[0].get<std::string>(), r[1].get<float>()});
        q.rewards.items = itemCounts(rw.value("it", json::array()));
        for (const auto &u : rw.value("un", json::array()))
            if (u.is_array() && u.size() == 3 && u[0].is_string())
                q.rewards.unlocks.push_back({u[0].get<std::string>(), numberText(u[1]), u[2].is_number() ? u[2].get<int>() : 1});
        for (const auto &s : rw.value("sk", json::array()))
            if (s.is_array() && s.size() == 2)
                q.rewards.skills.push_back({numberText(s[0]), s[1].is_number() ? s[1].get<int>() : 0});
        q.rewards.traders = strings(rw.value("tu", json::array()));
        q.startItems = itemCounts(t.value("st", json::array()));
        // Quests come sorted in the game's trader order; keep it for the filter.
        if (std::find(d.traders.begin(), d.traders.end(), q.trader) == d.traders.end())
            d.traders.push_back(q.trader);
        d.quests.push_back(std::move(q));
    }
    if (d.quests.empty() && d.items.empty()) {
        err = "database is empty";
        return false;
    }
    out = std::move(d);
    return true;
}

bool parseWiki(const std::string &text, std::map<std::string, std::string> &out) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return false;
    out.clear();
    const json pages = j.value("pages", json::object());
    for (auto &[k, v] : pages.items())
        if (v.is_string()) {
            // Older data kept tab-only lines from wiki tables; drop them and collapse blank runs.
            std::string src = v.get<std::string>(), clean, line;
            std::istringstream ls(src);
            int blank = 0;
            while (std::getline(ls, line)) {
                size_t b = line.find_first_not_of(" \t");
                line = b == std::string::npos ? "" : line.substr(b);
                blank = line.empty() ? blank + 1 : 0;
                if (blank < 2)
                    clean += line + "\n";
            }
            while (!clean.empty() && clean.back() == '\n')
                clean.pop_back();
            out[k] = clean;
        }
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
    st.profileId = str(j, "profileId");
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
    st.tasksDone.clear();
    for (const auto &t : j.value("tasksDone", json::array()))
        if (t.is_string())
            st.tasksDone.insert(t.get<std::string>());
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
    j["profileId"] = st.profileId;
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
    j["tasksDone"] = st.tasksDone;

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
    st.tasksDone.clear();
    for (const auto &t : d.value("tasksProgress", json::array()))
        if (t.value("complete", false))
            st.tasksDone.insert(str(t, "id"));
    st.account = str(d, "displayName");
    return true;
}

// Levels are [w, h, hex] with bit i of the hex number set when piece i exists.
static std::vector<MapLevel> parseLevels(const json &j) {
    std::vector<MapLevel> out;
    if (!j.is_array())
        return out;
    for (const auto &l : j) {
        if (!l.is_array() || l.size() != 3 || !l[2].is_string())
            continue;
        MapLevel lv;
        lv.w = l[0].get<int>();
        lv.h = l[1].get<int>();
        int cols = (lv.w + kMapChunk - 1) / kMapChunk, rows = (lv.h + kMapChunk - 1) / kMapChunk;
        lv.have.assign(cols * rows, false);
        std::string hex = l[2].get<std::string>();
        for (int i = 0; i < (int)lv.have.size() && i / 4 < (int)hex.size(); i++) {
            char c = hex[hex.size() - 1 - i / 4];
            int v = c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
            lv.have[i] = (v >> (i % 4)) & 1;
        }
        if (lv.w > 0 && lv.h > 0)
            out.push_back(std::move(lv));
    }
    return out;
}

bool parseMaps(const std::string &text, std::vector<MapInfo> &out, std::string &err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "map data is not valid JSON";
        return false;
    }
    static const std::map<std::string, MarkerType> types = {
        {"pmc", MK_PMC}, {"scav", MK_SCAV}, {"shared", MK_SHARED}, {"transit", MK_TRANSIT}, {"boss", MK_BOSS}};
    std::vector<MapInfo> maps;
    for (const auto &m : j.value("maps", json::array())) {
        MapInfo mi;
        mi.id = str(m, "id");
        mi.name = str(m, "name");
        mi.image = str(m, "img");
        mi.alt = str(m, "alt");
        mi.thumb = str(m, "thumb");
        mi.levels = parseLevels(m.value("lv", json::array()));
        mi.altLevels = parseLevels(m.value("alv", json::array()));
        for (const auto &f : m.value("floors", json::array())) {
            MapFloor fl;
            fl.name = str(f, "n");
            fl.image = str(f, "img");
            fl.alt = str(f, "alt");
            fl.levels = parseLevels(f.value("lv", json::array()));
            fl.altLevels = parseLevels(f.value("alv", json::array()));
            for (int k = 0; k < 4; k++) {
                if (f.contains("r") && f["r"].size() == 4)
                    fl.r[k] = f["r"][k].get<float>();
                if (f.contains("ar") && f["ar"].size() == 4)
                    fl.ar[k] = f["ar"][k].get<float>();
            }
            if (fl.image.empty())
                mi.baseFloor = mi.floors.size();
            mi.floors.push_back(fl);
        }
        mi.w = num(m, "w");
        mi.h = num(m, "h");
        mi.duration = num(m, "dur");
        mi.players = str(m, "players");
        for (const auto &k : m.value("markers", json::array())) {
            auto t = types.find(str(k, "t"));
            if (t == types.end())
                continue;
            mi.markers.push_back({t->second, str(k, "n"), k.value("u", 0.f), k.value("v", 0.f)});
        }
        if (!mi.image.empty() && mi.w > 0 && mi.h > 0)
            maps.push_back(std::move(mi));
    }
    if (maps.empty()) {
        err = "map data has no maps";
        return false;
    }
    out = std::move(maps);
    return true;
}

static std::string fmtInt(int64_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > (s[0] == '-' ? 1 : 0); i -= 3)
        s.insert(i, ",");
    return s;
}

static std::string pct(int64_t a, int64_t b) {
    if (b <= 0)
        return "-";
    char buf[16];
    snprintf(buf, sizeof buf, "%.1f%%", 100.0 * a / b);
    return buf;
}

static std::string ratio(int64_t a, int64_t b) {
    char buf[16];
    snprintf(buf, sizeof buf, "%.2f", b > 0 ? double(a) / b : double(a));
    return buf;
}

// overAllCounters.Items is a list of {Key: [..], Value: n}; keys are joined with '/'.
static std::map<std::string, int64_t> counters(const json &stats) {
    std::map<std::string, int64_t> out;
    if (!stats.is_object())
        return out;
    const json eft = stats.value("eft", json::object());
    const json oc = eft.value("overAllCounters", json::object());
    for (const auto &it : oc.value("Items", json::array())) {
        std::string key;
        for (const auto &k : it.value("Key", json::array()))
            if (k.is_string())
                key += (key.empty() ? "" : "/") + k.get<std::string>();
        if (it.contains("Value") && it["Value"].is_number())
            out[key] = it["Value"].get<int64_t>();
    }
    out["#time"] = eft.value("totalInGameTime", (int64_t)0);
    return out;
}

static std::vector<ProfileStat> sideStats(const std::map<std::string, int64_t> &c, const char *side) {
    auto get = [&](const std::string &k) {
        auto it = c.find(k);
        return it == c.end() ? (int64_t)0 : it->second;
    };
    std::string s = side;
    int64_t raids = get("Sessions/" + s);
    int64_t survived = get("ExitStatus/Survived/" + s);
    int64_t kills = get("Kills"), deaths = get("Deaths");
    std::vector<ProfileStat> out;
    out.push_back({"Raids", fmtInt(raids)});
    out.push_back({"Survived", fmtInt(survived) + "  (" + pct(survived, raids) + ")"});
    out.push_back({"Killed in action", fmtInt(get("ExitStatus/Killed/" + s))});
    out.push_back({"Run-throughs", fmtInt(get("ExitStatus/Runner/" + s))});
    out.push_back({"Missing in action", fmtInt(get("ExitStatus/Left/" + s) + get("ExitStatus/MissingInAction/" + s))});
    out.push_back({"Transits", fmtInt(get("ExitStatus/Transit/" + s))});
    out.push_back({"Kills", fmtInt(kills)});
    if (s == "Pmc") {
        out.push_back({"Deaths", fmtInt(deaths)});
        out.push_back({"K/D", ratio(kills, deaths)});
        out.push_back({"PMC kills", fmtInt(get("KilledPmc"))});
    }
    out.push_back({"Longest survival streak", fmtInt(get("LongestWinStreak/" + s))});
    int64_t t = get("#time");
    if (t > 0 && s == "Pmc")
        out.push_back({"Time in game", fmtInt(t / 3600) + " h"});
    return out;
}

bool parseProfile(const std::string &text, const HideoutData &data, Profile &out, std::string &err) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "profile is not valid JSON";
        return false;
    }
    if (j.contains("err") || !j.contains("info")) {
        err = j.value("errmsg", std::string("profile not found"));
        return false;
    }
    Profile p;
    const json info = j["info"];
    p.nickname = str(info, "nickname");
    p.side = str(info, "side");
    p.exp = info.value("experience", (int64_t)0);
    p.prestige = num(info, "prestigeLevel");
    int64_t upd = j.value("updated", (int64_t)0);
    p.updated = upd > 100000000000LL ? upd / 1000 : upd;  // milliseconds in current snapshots
    auto pmcCounters = counters(j.value("pmcStats", json::object()));
    auto scavCounters = counters(j.value("scavStats", json::object()));
    p.pmc = sideStats(pmcCounters, "Pmc");
    p.scav = sideStats(scavCounters, "Scav");
    const json skills = j.value("skills", json::object());
    for (const auto &sk : skills.value("Common", json::array())) {
        std::string id = str(sk, "Id");
        double prog = sk.value("Progress", 0.0);
        if (id.empty() || prog < 1 || id.rfind("Bot", 0) == 0)
            continue;
        auto it = data.skills.find(id);
        p.skills.push_back({it != data.skills.end() ? it->second : id, float(std::min(prog / 100.0, 51.0))});
    }
    std::sort(p.skills.begin(), p.skills.end(),
              [](const ProfileSkill &a, const ProfileSkill &b) { return a.level > b.level; });
    if (p.nickname.empty()) {
        err = "profile has no nickname";
        return false;
    }
    out = std::move(p);
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
