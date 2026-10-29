#include <switch.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model.h"
#include "net.h"
#include "ui.h"

#define APP_DIR "sdmc:/switch/TarkovHideout"
#define STATE_PATH APP_DIR "/state.json"
#define DATA_URL "https://raw.githubusercontent.com/axewoo/TarkovHideout-NX/data/"
#define TRACKER_API "https://api.tarkovtracker.org"

enum Tab { TAB_STATIONS, TAB_ITEMS, TAB_CRAFTS, TAB_TIMERS, TAB_SETTINGS, TAB_COUNT };
static const char *kTabNames[TAB_COUNT] = {"Stations", "Needed items", "Crafts", "Timers", "Settings"};

static const int kRowH = 40;
static const int kTop = 92;
static const int kBottom = 664;

static std::string readFile(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool writeFile(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << data;
        if (!f)
            return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

static std::string groupDigits(int n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3)
        s.insert(i, ",");
    return s;
}

static std::string nowString() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

// Directional buttons with hold-to-repeat.
struct Input {
    PadState pad;
    u64 down = 0, held = 0;
    u64 repeatMask = 0;
    u64 heldSince = 0, lastRepeat = 0;

    void init() {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
    }

    void update() {
        padUpdate(&pad);
        down = padGetButtonsDown(&pad);
        held = padGetButtons(&pad);
        const u64 dirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown | HidNpadButton_AnyLeft |
                         HidNpadButton_AnyRight;
        u64 now = armGetSystemTick();
        u64 freq = armGetSystemTickFreq();
        if (down & dirs) {
            heldSince = lastRepeat = now;
        } else if (held & dirs) {
            if (now - heldSince > freq * 35 / 100 && now - lastRepeat > freq * 6 / 100) {
                down |= held & dirs;
                lastRepeat = now;
            }
        }
    }

    bool pressed(u64 mask) const { return down & mask; }
};

struct NeededItem {
    std::string name;
    std::string itemId;
    bool fir;
    int need = 0;
    std::vector<std::string> usedBy;
};

struct ListView {
    int sel = 0;
    int scroll = 0;

    void clamp(int count, int visible) {
        if (count <= 0) {
            sel = scroll = 0;
            return;
        }
        sel = std::clamp(sel, 0, count - 1);
        if (sel < scroll)
            scroll = sel;
        if (sel >= scroll + visible)
            scroll = sel - visible + 1;
        scroll = std::clamp(scroll, 0, std::max(0, count - visible));
    }

    void clampSel(int count) {
        sel = count <= 0 ? 0 : std::clamp(sel, 0, count - 1);
    }

    void move(int delta, int count) {
        if (count <= 0)
            return;
        sel += delta;
        if (sel < 0)
            sel = delta < -1 ? 0 : count - 1;
        else if (sel >= count)
            sel = delta > 1 ? count - 1 : 0;
    }
};

class App {
public:
    bool init();
    void run();
    void shutdown();

private:
    void loadData();
    bool downloadData(bool quiet);
    void syncTracker();
    void editToken();
    void save();
    void setStatus(const std::string &s, SDL_Color c = col::dim);
    void busy(const std::string &s);

    void input();
    void inputStations();
    void inputItems();
    void inputCrafts();
    void inputTimers();
    void inputSettings();

    void draw();
    void drawHeader();
    void drawFooter(const std::string &hints);
    void drawStations();
    void drawItems();
    void drawCrafts();
    void drawTimers();
    void drawSettings();
    void drawScrollbar(int x, int y, int h, int count, int visible, int scroll);

    void rebuildNeeded();
    std::vector<const Craft *> visibleCrafts() const;
    const Craft *craftById(const std::string &id) const;
    Timer *timerFor(const std::string &craftId);
    std::string craftTitle(const Craft &c) const;

    Ui ui;
    Input in;
    HideoutData data;
    State st;
    bool running = true;
    Tab tab = TAB_STATIONS;

    ListView stationList;
    ListView reqList;
    bool reqFocus = false;
    int viewLevel = -1;   // level shown on the right pane, -1 = next to build

    std::vector<NeededItem> needed;
    ListView itemList;
    bool needDirty = true;

    ListView craftList;
    int craftFilter = 0;  // 0 = all built stations, else station index + 1

    ListView timerList;
    ListView settingsList;
    bool confirmReset = false;

    std::string status;
    SDL_Color statusColor = col::dim;
    u64 statusTick = 0;
};

bool App::init() {
    romfsInit();
    plInitialize(PlServiceType_User);
    socketInitializeDefault();
    netInit();
    mkdir(APP_DIR, 0777);
    in.init();
    if (!ui.init())
        return false;

    loadState(STATE_PATH, st);
    loadData();
    if (data.empty())
        downloadData(true);
    if (st.syncOnLaunch && !st.token.empty())
        syncTracker();
    if (status.empty()) {
        if (data.empty())
            setStatus("No hideout data yet. Connect to the internet and use Settings > Download.", col::warn);
        else
            setStatus("Hideout data from " + data.generated);
    }
    return true;
}

void App::shutdown() {
    save();
    ui.shutdown();
    netExit();
    socketExit();
    plExit();
    romfsExit();
}

void App::save() {
    if (!saveState(STATE_PATH, st))
        setStatus("Could not save progress to the SD card", col::bad);
}

void App::setStatus(const std::string &s, SDL_Color c) {
    status = s;
    statusColor = c;
    statusTick = armGetSystemTick();
}

// Shows a message immediately, before a blocking network call.
void App::busy(const std::string &s) {
    setStatus(s, col::accent);
    draw();
}

void App::loadData() {
    std::string mode = st.mode == "pve" ? "pve" : "pvp";
    std::string name = "hideout-" + mode + ".json";
    HideoutData best;
    std::string err;
    for (const std::string &path : {std::string(APP_DIR "/") + name, std::string("romfs:/") + name}) {
        std::string text = readFile(path);
        if (text.empty())
            continue;
        HideoutData d;
        if (!parseHideout(text, d, err))
            continue;
        if (best.empty() || d.generated > best.generated)
            best = std::move(d);
    }
    data = std::move(best);
    needDirty = true;
    viewLevel = -1;
}

bool App::downloadData(bool quiet) {
    std::string mode = st.mode == "pve" ? "pve" : "pvp";
    std::string name = "hideout-" + mode + ".json";
    busy("Downloading latest hideout data...");
    HttpResult r = httpGet(DATA_URL + name);
    if (!r.ok || r.status != 200) {
        if (!quiet)
            setStatus("Download failed: " + (r.ok ? "HTTP " + std::to_string(r.status) : r.error), col::bad);
        return false;
    }
    HideoutData d;
    std::string err;
    if (!parseHideout(r.body, d, err)) {
        setStatus("Downloaded data is broken: " + err, col::bad);
        return false;
    }
    writeFile(std::string(APP_DIR "/") + name, r.body);
    data = std::move(d);
    needDirty = true;
    viewLevel = -1;
    setStatus("Hideout data updated (" + data.generated + ")", col::good);
    return true;
}

void App::syncTracker() {
    if (st.token.empty()) {
        setStatus("Add your TarkovTracker token in Settings first", col::warn);
        return;
    }
    busy("Syncing with TarkovTracker...");
    std::vector<std::string> headers = {"Authorization: Bearer " + st.token, "Accept: application/json"};

    HttpResult info = httpGet(TRACKER_API "/token", headers);
    if (!info.ok) {
        setStatus("TarkovTracker unreachable: " + info.error, col::bad);
        return;
    }
    if (info.status == 401 || info.status == 403) {
        setStatus("TarkovTracker rejected the token (needs Get Progression permission)", col::bad);
        return;
    }
    if (info.status == 200) {
        auto j = nlohmann::json::parse(info.body, nullptr, false);
        if (!j.is_discarded()) {
            std::string mode = j.value("gameMode", std::string());
            std::string want = mode == "pve" ? "pve" : "pvp";
            if (!mode.empty() && want != st.mode) {
                st.mode = want;
                loadData();
                if (data.empty())
                    downloadData(true);
            }
        }
    }

    HttpResult r = httpGet(TRACKER_API "/progress", headers);
    if (!r.ok) {
        setStatus("TarkovTracker unreachable: " + r.error, col::bad);
        return;
    }
    if (r.status == 429) {
        setStatus("TarkovTracker daily limit reached, try again later", col::warn);
        return;
    }
    if (r.status != 200) {
        setStatus("TarkovTracker error: HTTP " + std::to_string(r.status), col::bad);
        return;
    }
    std::string err;
    if (!applyTrackerProgress(r.body, data, st, err)) {
        setStatus("Sync failed: " + err, col::bad);
        return;
    }
    st.lastSync = nowString();
    needDirty = true;
    save();
    setStatus("Synced " + (st.account.empty() ? std::string("progress") : st.account) + " from TarkovTracker (" +
                  (st.mode == "pve" ? "PVE" : "PVP") + ")",
              col::good);
}

void App::editToken() {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, "TarkovTracker API token");
    swkbdConfigSetGuideText(&kbd, "Paste or type your token");
    swkbdConfigSetInitialText(&kbd, st.token.c_str());
    swkbdConfigSetStringLenMax(&kbd, 128);
    char out[160] = {0};
    Result rc = swkbdShow(&kbd, out, sizeof out);
    swkbdClose(&kbd);
    if (R_FAILED(rc))
        return;
    std::string tok = out;
    tok.erase(std::remove_if(tok.begin(), tok.end(), [](char c) { return c == ' ' || c == '\n' || c == '\r'; }),
              tok.end());
    st.token = tok;
    save();
    if (!tok.empty())
        syncTracker();
}

void App::rebuildNeeded() {
    needDirty = false;
    std::map<std::string, NeededItem> agg;
    for (const auto &s : data.stations) {
        int built = st.builtLevel(s);
        for (const auto &l : s.levels) {
            if (l.level <= built)
                continue;
            for (const auto &r : l.items) {
                int missing = r.count - st.have(r);
                if (missing <= 0)
                    continue;
                std::string key = r.itemId + (r.fir ? "#fir" : "");
                NeededItem &n = agg[key];
                n.name = r.name;
                n.itemId = r.itemId;
                n.fir = r.fir;
                n.need += missing;
                n.usedBy.push_back(s.name + " " + std::to_string(l.level));
            }
        }
    }
    needed.clear();
    for (auto &[k, v] : agg)
        needed.push_back(v);
    std::sort(needed.begin(), needed.end(), [](const NeededItem &a, const NeededItem &b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.fir < b.fir;
    });
}

std::vector<const Craft *> App::visibleCrafts() const {
    std::vector<const Craft *> out;
    for (const auto &c : data.crafts) {
        const Station *s = data.station(c.stationId);
        if (!s)
            continue;
        if (craftFilter > 0) {
            if (craftFilter - 1 >= (int)data.stations.size() || data.stations[craftFilter - 1].id != c.stationId)
                continue;
        } else if (st.builtLevel(*s) < c.level) {
            continue;
        }
        out.push_back(&c);
    }
    return out;
}

const Craft *App::craftById(const std::string &id) const {
    for (const auto &c : data.crafts)
        if (c.id == id)
            return &c;
    return nullptr;
}

Timer *App::timerFor(const std::string &craftId) {
    for (auto &t : st.timers)
        if (t.craftId == craftId)
            return &t;
    return nullptr;
}

std::string App::craftTitle(const Craft &c) const {
    std::string t;
    for (size_t i = 0; i < c.rew.size(); i++) {
        if (i)
            t += ", ";
        t += c.rew[i].name;
        if (c.rew[i].count > 1)
            t += " x" + std::to_string(c.rew[i].count);
    }
    return t.empty() ? c.id : t;
}

// ---------------------------------------------------------------- input

void App::input() {
    in.update();
    if (in.pressed(HidNpadButton_Plus)) {
        running = false;
        return;
    }
    if (in.pressed(HidNpadButton_R) || in.pressed(HidNpadButton_L)) {
        int d = in.pressed(HidNpadButton_R) ? 1 : -1;
        tab = Tab((tab + d + TAB_COUNT) % TAB_COUNT);
        reqFocus = false;
        confirmReset = false;
        return;
    }
    if (needDirty)
        rebuildNeeded();
    switch (tab) {
    case TAB_STATIONS: inputStations(); break;
    case TAB_ITEMS: inputItems(); break;
    case TAB_CRAFTS: inputCrafts(); break;
    case TAB_TIMERS: inputTimers(); break;
    case TAB_SETTINGS: inputSettings(); break;
    default: break;
    }
}

static int shownLevel(const State &st, const Station &s, int viewLevel) {
    if (s.levels.empty())
        return 0;
    int maxLv = s.levels.back().level;
    if (viewLevel > 0)
        return std::min(viewLevel, maxLv);
    return std::min(st.builtLevel(s) + 1, maxLv);
}

static const Level *levelOf(const Station &s, int lvl) {
    for (const auto &l : s.levels)
        if (l.level == lvl)
            return &l;
    return nullptr;
}

void App::inputStations() {
    int n = data.stations.size();
    if (n == 0)
        return;
    stationList.clampSel(n);
    const Station &s = data.stations[stationList.sel];
    int lvl = shownLevel(st, s, viewLevel);
    const Level *L = levelOf(s, lvl);

    if (in.pressed(HidNpadButton_ZL) || in.pressed(HidNpadButton_ZR)) {
        int d = in.pressed(HidNpadButton_ZR) ? 1 : -1;
        int maxLv = s.levels.empty() ? 0 : s.levels.back().level;
        viewLevel = std::clamp(lvl + d, s.levels.empty() ? 0 : s.levels.front().level, maxLv);
        reqList = ListView();
        return;
    }

    if (reqFocus && L) {
        int m = L->items.size();
        if (in.pressed(HidNpadButton_AnyUp))
            reqList.move(-1, m);
        if (in.pressed(HidNpadButton_AnyDown))
            reqList.move(1, m);
        if (in.pressed(HidNpadButton_B) || in.pressed(HidNpadButton_AnyLeft) || m == 0) {
            reqFocus = false;
            return;
        }
        reqList.clampSel(m);
        const ReqItem &r = L->items[reqList.sel];
        int have = st.have(r);
        if (in.pressed(HidNpadButton_A))
            have = std::min(r.count, have + 1);
        if (in.pressed(HidNpadButton_X))
            have = std::max(0, have - 1);
        if (in.pressed(HidNpadButton_Y))
            have = have == r.count ? 0 : r.count;
        if (have != st.have(r)) {
            st.parts[r.id] = have;
            needDirty = true;
            save();
        }
        return;
    }

    if (in.pressed(HidNpadButton_AnyUp)) {
        stationList.move(-1, n);
        viewLevel = -1;
        reqList = ListView();
    }
    if (in.pressed(HidNpadButton_AnyDown)) {
        stationList.move(1, n);
        viewLevel = -1;
        reqList = ListView();
    }
    if (in.pressed(HidNpadButton_AnyRight) && L && !L->items.empty())
        reqFocus = true;

    int built = st.builtLevel(s);
    if (in.pressed(HidNpadButton_A) && L) {
        if (lvl <= built) {
            setStatus(s.name + " level " + std::to_string(lvl) + " is already built");
        } else {
            // Building a level implies every level below it, with all items handed in.
            for (const auto &l : s.levels) {
                if (l.level > lvl)
                    break;
                st.modules[l.id] = true;
                for (const auto &r : l.items)
                    st.parts[r.id] = r.count;
            }
            setStatus(s.name + " level " + std::to_string(lvl) + " marked as built", col::good);
            viewLevel = -1;
            needDirty = true;
            save();
        }
    }
    if (in.pressed(HidNpadButton_X) && built > 0) {
        for (const auto &l : s.levels)
            if (l.level == built)
                st.modules.erase(l.id);
        setStatus(s.name + " set back to level " + std::to_string(built - 1));
        viewLevel = -1;
        needDirty = true;
        save();
    }
}

void App::inputItems() {
    int n = needed.size();
    if (in.pressed(HidNpadButton_AnyUp))
        itemList.move(-1, n);
    if (in.pressed(HidNpadButton_AnyDown))
        itemList.move(1, n);
    if (in.pressed(HidNpadButton_AnyLeft))
        itemList.move(-10, n);
    if (in.pressed(HidNpadButton_AnyRight))
        itemList.move(10, n);
}

void App::inputCrafts() {
    auto list = visibleCrafts();
    int n = list.size();
    if (in.pressed(HidNpadButton_AnyLeft) || in.pressed(HidNpadButton_AnyRight)) {
        int d = in.pressed(HidNpadButton_AnyRight) ? 1 : -1;
        int count = data.stations.size() + 1;
        craftFilter = (craftFilter + d + count) % count;
        craftList = ListView();
        return;
    }
    if (in.pressed(HidNpadButton_AnyUp))
        craftList.move(-1, n);
    if (in.pressed(HidNpadButton_AnyDown))
        craftList.move(1, n);
    if (n == 0)
        return;
    craftList.clampSel(n);
    const Craft &c = *list[craftList.sel];
    Timer *t = timerFor(c.id);
    if (in.pressed(HidNpadButton_A)) {
        int64_t now = time(nullptr);
        if (t && t->endTime <= now) {
            st.timers.erase(st.timers.begin() + (t - st.timers.data()));
            setStatus("Collected " + craftTitle(c), col::good);
        } else if (t) {
            setStatus("Already running, " + formatDuration(t->endTime - now) + " left");
        } else {
            st.timers.push_back({c.id, now + c.duration});
            setStatus("Started " + craftTitle(c) + " (" + formatDuration(c.duration) + ")", col::good);
        }
        save();
    }
    if (in.pressed(HidNpadButton_X) && t) {
        st.timers.erase(st.timers.begin() + (t - st.timers.data()));
        setStatus("Timer cancelled");
        save();
    }
}

void App::inputTimers() {
    int n = st.timers.size();
    if (in.pressed(HidNpadButton_AnyUp))
        timerList.move(-1, n);
    if (in.pressed(HidNpadButton_AnyDown))
        timerList.move(1, n);
    if (n == 0)
        return;
    timerList.clampSel(n);
    std::sort(st.timers.begin(), st.timers.end(),
              [](const Timer &a, const Timer &b) { return a.endTime < b.endTime; });
    Timer &t = st.timers[timerList.sel];
    int64_t now = time(nullptr);
    if (in.pressed(HidNpadButton_A) && t.endTime <= now) {
        st.timers.erase(st.timers.begin() + timerList.sel);
        setStatus("Collected", col::good);
        save();
    } else if (in.pressed(HidNpadButton_X)) {
        st.timers.erase(st.timers.begin() + timerList.sel);
        setStatus("Timer cancelled");
        save();
    }
}

enum SettingRow { SET_TOKEN, SET_SYNC, SET_SYNC_LAUNCH, SET_MODE, SET_DOWNLOAD, SET_RESET, SET_COUNT };

void App::inputSettings() {
    if (in.pressed(HidNpadButton_AnyUp)) {
        settingsList.move(-1, SET_COUNT);
        confirmReset = false;
    }
    if (in.pressed(HidNpadButton_AnyDown)) {
        settingsList.move(1, SET_COUNT);
        confirmReset = false;
    }
    if (!in.pressed(HidNpadButton_A))
        return;
    switch (settingsList.sel) {
    case SET_TOKEN: editToken(); break;
    case SET_SYNC: syncTracker(); break;
    case SET_SYNC_LAUNCH:
        st.syncOnLaunch = !st.syncOnLaunch;
        save();
        break;
    case SET_MODE:
        st.mode = st.mode == "pve" ? "pvp" : "pve";
        loadData();
        if (data.empty())
            downloadData(false);
        save();
        setStatus(std::string("Switched to ") + (st.mode == "pve" ? "PVE" : "PVP") +
                  " (your token decides the mode when you sync)");
        break;
    case SET_DOWNLOAD: downloadData(false); break;
    case SET_RESET:
        if (!confirmReset) {
            confirmReset = true;
            setStatus("Press A again to clear all local progress and timers", col::warn);
        } else {
            st.modules.clear();
            st.parts.clear();
            st.timers.clear();
            confirmReset = false;
            needDirty = true;
            save();
            setStatus("Local progress cleared");
        }
        break;
    }
}

// ---------------------------------------------------------------- drawing

void App::drawScrollbar(int x, int y, int h, int count, int visible, int scroll) {
    if (count <= visible)
        return;
    int th = std::max(30, h * visible / count);
    int ty = y + (h - th) * scroll / std::max(1, count - visible);
    ui.fill(x, y, 4, h, col::panel);
    ui.fill(x, ty, 4, th, col::line);
}

void App::drawHeader() {
    ui.fill(0, 0, 1280, 76, col::panel);
    ui.text(24, 10, "Tarkov Hideout", FS_TITLE, col::accent);
    std::string mode = st.mode == "pve" ? "PVE" : "PVP";
    ui.textRight(1256, 16, mode, FS_BODY, col::accent);
    int x = 24;
    for (int i = 0; i < TAB_COUNT; i++) {
        bool on = i == tab;
        int w = ui.measure(kTabNames[i], FS_SMALL) + 24;
        if (on)
            ui.fill(x, 48, w, 28, col::bg);
        ui.text(x + 12, 50, kTabNames[i], FS_SMALL, on ? col::text : col::dim);
        x += w + 4;
    }
    ui.text(x + 8, 50, "L / R", FS_SMALL, col::line);
}

void App::drawFooter(const std::string &hints) {
    ui.fill(0, 676, 1280, 44, col::panel);
    bool fresh = armTicksToNs(armGetSystemTick() - statusTick) < 8000000000ULL;
    if (!status.empty() && fresh)
        ui.text(24, 686, status, FS_SMALL, statusColor, 760);
    ui.textRight(1256, 686, hints, FS_SMALL, col::dim);
}

void App::drawStations() {
    if (data.empty()) {
        ui.text(40, 120, "No hideout data loaded.", FS_BODY, col::text);
        ui.text(40, 160, "Go to Settings and choose \"Download latest hideout data\".", FS_SMALL, col::dim);
        drawFooter("L/R tabs   + quit");
        return;
    }
    int visible = (kBottom - kTop) / kRowH;
    stationList.clamp(data.stations.size(), visible);
    for (int i = 0; i < visible && stationList.scroll + i < (int)data.stations.size(); i++) {
        int idx = stationList.scroll + i;
        const Station &s = data.stations[idx];
        int y = kTop + i * kRowH;
        bool on = idx == stationList.sel;
        if (on)
            ui.fill(16, y, 420, kRowH - 4, reqFocus ? col::panel : col::panelHi);
        int built = st.builtLevel(s);
        int maxLv = s.levels.empty() ? 0 : s.levels.back().level;
        ui.text(28, y + 6, s.name, FS_BODY, col::text, 290);
        std::string lv = built >= maxLv ? "MAX" : std::to_string(built) + " / " + std::to_string(maxLv);
        ui.textRight(424, y + 8, lv, FS_SMALL, built >= maxLv ? col::good : col::dim);
    }
    drawScrollbar(440, kTop, visible * kRowH, data.stations.size(), visible, stationList.scroll);

    const Station &s = data.stations[stationList.sel];
    int built = st.builtLevel(s);
    int lvl = shownLevel(st, s, viewLevel);
    const Level *L = levelOf(s, lvl);
    int px = 468;
    ui.fill(px - 8, kTop, 1280 - px - 8, kBottom - kTop, col::panel);
    if (!L) {
        ui.text(px + 12, kTop + 12, s.name + " has no levels", FS_BODY, col::dim);
        drawFooter("L/R tabs   + quit");
        return;
    }
    std::string head = s.name + "  level " + std::to_string(lvl);
    ui.text(px + 12, kTop + 10, head, FS_TITLE, col::text, 560);
    std::string tag = lvl <= built ? "built" : (lvl == built + 1 ? "next" : "locked");
    ui.textRight(1248, kTop + 18, tag, FS_SMALL, lvl <= built ? col::good : (lvl == built + 1 ? col::accent : col::dim));

    int y = kTop + 56;
    if (L->constructionTime > 0) {
        ui.text(px + 12, y, "Build time " + formatDuration(L->constructionTime), FS_SMALL, col::dim);
        y += 28;
    }
    for (const auto &o : L->other) {
        ui.text(px + 12, y, "Requires " + o, FS_SMALL, col::dim, 760);
        y += 26;
    }
    y += 8;
    ui.fill(px + 4, y, 1280 - px - 32, 1, col::line);
    y += 10;

    int m = L->items.size();
    int rows = std::max(1, (kBottom - y - 8) / 36);
    if (reqFocus)
        reqList.clamp(m, rows);
    if (m == 0)
        ui.text(px + 12, y, "No items needed", FS_BODY, col::dim);
    for (int i = 0; i < rows && reqList.scroll + i < m; i++) {
        int idx = reqList.scroll + i;
        const ReqItem &r = L->items[idx];
        int ry = y + i * 36;
        bool on = reqFocus && idx == reqList.sel;
        if (on)
            ui.fill(px + 2, ry - 2, 1280 - px - 28, 34, col::panelHi);
        int have = lvl <= built ? r.count : st.have(r);
        SDL_Color c = have >= r.count ? col::good : col::text;
        int w = ui.text(px + 14, ry + 2, r.name, FS_BODY, c, 560);
        if (r.fir)
            ui.text(px + 22 + w, ry + 6, "FIR", FS_SMALL, col::warn);
        ui.textRight(1240, ry + 2, groupDigits(have) + " / " + groupDigits(r.count), FS_BODY, c);
    }
    drawScrollbar(1260, y, rows * 36, m, rows, reqList.scroll);

    if (reqFocus)
        drawFooter("A +1   X -1   Y all/none   B back");
    else
        drawFooter("A build   X undo   ZL/ZR level   Right items");
}

void App::drawItems() {
    int visible = (kBottom - kTop) / kRowH;
    itemList.clamp(needed.size(), visible);
    if (needed.empty()) {
        ui.text(40, 120, data.empty() ? "No hideout data loaded." : "Nothing left to collect. Hideout maxed!",
                FS_BODY, col::text);
        drawFooter("L/R tabs   + quit");
        return;
    }
    for (int i = 0; i < visible && itemList.scroll + i < (int)needed.size(); i++) {
        int idx = itemList.scroll + i;
        const NeededItem &n = needed[idx];
        int y = kTop + i * kRowH;
        if (idx == itemList.sel)
            ui.fill(16, y, 1240, kRowH - 4, col::panelHi);
        ui.textRight(150, y + 6, "x" + groupDigits(n.need), FS_BODY, col::accent);
        int w = ui.text(168, y + 6, n.name, FS_BODY, col::text, 540);
        if (n.fir)
            ui.text(176 + w, y + 10, "FIR", FS_SMALL, col::warn);
        std::string used;
        for (size_t k = 0; k < n.usedBy.size(); k++)
            used += (k ? ", " : "") + n.usedBy[k];
        ui.text(760, y + 10, used, FS_SMALL, col::dim, 480);
    }
    drawScrollbar(1262, kTop, visible * kRowH, needed.size(), visible, itemList.scroll);
    drawFooter(std::to_string(needed.size()) + " different items   Left/Right page");
}

void App::drawCrafts() {
    auto list = visibleCrafts();
    std::string filter = craftFilter == 0 ? "Built stations" : data.stations[craftFilter - 1].name;
    ui.text(28, kTop - 6, "< " + filter + " >", FS_BODY, col::accent);
    int top = kTop + 34;
    int rowH = 58;
    int visible = (kBottom - top) / rowH;
    craftList.clamp(list.size(), visible);
    if (list.empty()) {
        ui.text(28, top + 10,
                craftFilter == 0 ? "No crafts available at your current station levels." : "This station has no crafts.",
                FS_BODY, col::dim);
    }
    int64_t now = time(nullptr);
    for (int i = 0; i < visible && craftList.scroll + i < (int)list.size(); i++) {
        int idx = craftList.scroll + i;
        const Craft &c = *list[idx];
        int y = top + i * rowH;
        if (idx == craftList.sel)
            ui.fill(16, y, 1240, rowH - 4, col::panelHi);
        const Station *s = data.station(c.stationId);
        bool locked = s && st.builtLevel(*s) < c.level;
        ui.text(28, y + 4, craftTitle(c), FS_BODY, locked ? col::dim : col::text, 800);
        std::string sub = (s ? s->name : c.stationId) + " " + std::to_string(c.level) + (c.req.empty() ? "" : "  <-  ");
        for (size_t k = 0; k < c.req.size(); k++)
            sub += (k ? ", " : "") + c.req[k].shortName + (c.req[k].count > 1 ? " x" + std::to_string(c.req[k].count) : "");
        ui.text(28, y + 32, sub, FS_SMALL, col::dim, 960);
        Timer *t = timerFor(c.id);
        if (t) {
            int64_t left = t->endTime - now;
            ui.textRight(1240, y + 6, left <= 0 ? "READY" : formatDuration(left), FS_BODY,
                         left <= 0 ? col::good : col::accent);
        } else {
            ui.textRight(1240, y + 8, formatDuration(c.duration), FS_SMALL, col::dim);
        }
    }
    drawScrollbar(1262, top, visible * rowH, list.size(), visible, craftList.scroll);
    drawFooter("A start/collect   X cancel   Left/Right station");
}

void App::drawTimers() {
    std::sort(st.timers.begin(), st.timers.end(),
              [](const Timer &a, const Timer &b) { return a.endTime < b.endTime; });
    int visible = (kBottom - kTop) / 52;
    timerList.clamp(st.timers.size(), visible);
    if (st.timers.empty())
        ui.text(28, kTop + 10, "No crafts running. Start one from the Crafts tab.", FS_BODY, col::dim);
    int64_t now = time(nullptr);
    for (int i = 0; i < visible && timerList.scroll + i < (int)st.timers.size(); i++) {
        int idx = timerList.scroll + i;
        const Timer &t = st.timers[idx];
        const Craft *c = craftById(t.craftId);
        int y = kTop + i * 52;
        if (idx == timerList.sel)
            ui.fill(16, y, 1240, 48, col::panelHi);
        std::string name = c ? craftTitle(*c) : t.craftId;
        const Station *s = c ? data.station(c->stationId) : nullptr;
        ui.text(28, y + 10, name, FS_BODY, col::text, 760);
        if (s)
            ui.text(820, y + 14, s->name, FS_SMALL, col::dim, 220);
        int64_t left = t.endTime - now;
        ui.textRight(1240, y + 10, left <= 0 ? "READY" : formatDuration(left), FS_BODY,
                     left <= 0 ? col::good : col::accent);
    }
    drawFooter("A collect   X cancel");
}

void App::drawSettings() {
    std::string tok = st.token.empty() ? "not set" : "..." + st.token.substr(st.token.size() > 4 ? st.token.size() - 4 : 0);
    std::string rows[SET_COUNT] = {
        "TarkovTracker token: " + tok,
        "Sync from TarkovTracker now",
        std::string("Sync on launch: ") + (st.syncOnLaunch ? "on" : "off"),
        std::string("Game mode: ") + (st.mode == "pve" ? "PVE" : "PVP"),
        "Download latest hideout data",
        confirmReset ? "Clear local progress? Press A again" : "Clear local progress and timers",
    };
    settingsList.clamp(SET_COUNT, SET_COUNT);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = kTop + i * 50;
        if (i == settingsList.sel)
            ui.fill(16, y, 760, 46, col::panelHi);
        ui.text(32, y + 10, rows[i], FS_BODY, i == SET_RESET && confirmReset ? col::warn : col::text, 730);
    }
    int y = kTop + SET_COUNT * 50 + 30;
    auto info = [&](const std::string &k, const std::string &v) {
        ui.text(32, y, k, FS_SMALL, col::dim);
        ui.text(260, y, v, FS_SMALL, col::text, 900);
        y += 30;
    };
    info("Account", st.account.empty() ? "-" : st.account);
    info("Last sync", st.lastSync.empty() ? "never" : st.lastSync);
    info("Hideout data", data.empty() ? "none" : data.generated + "  (" + std::to_string(data.stations.size()) + " stations, " +
                                                       std::to_string(data.crafts.size()) + " crafts)");
    info("Version", APP_VERSION);
    y += 10;
    ui.text(32, y, "Get a token at tarkovtracker.org > Settings > API tokens, with Get Progression permission.",
            FS_SMALL, col::dim, 1220);
    ui.text(32, y + 28, "Sync only reads from TarkovTracker. Changes made here stay on this Switch.", FS_SMALL,
            col::dim, 1220);
    ui.text(32, y + 56, "Game data from tarkov.dev.", FS_SMALL, col::dim, 1220);
    drawFooter("A select   + quit");
}

void App::draw() {
    if (needDirty)
        rebuildNeeded();
    ui.clear();
    drawHeader();
    switch (tab) {
    case TAB_STATIONS: drawStations(); break;
    case TAB_ITEMS: drawItems(); break;
    case TAB_CRAFTS: drawCrafts(); break;
    case TAB_TIMERS: drawTimers(); break;
    case TAB_SETTINGS: drawSettings(); break;
    default: break;
    }
    ui.present();
    ui.endFrame();
}

void App::run() {
    while (running && appletMainLoop()) {
        input();
        if (!running)
            break;
        draw();
    }
}

int main(int argc, char **argv) {
    App app;
    if (app.init())
        app.run();
    app.shutdown();
    return 0;
}
