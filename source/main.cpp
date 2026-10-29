#include <switch.h>

#include <sys/stat.h>
#include <dirent.h>

#include <algorithm>
#include <cmath>
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
#define PROFILE_API "https://players.tarkov.dev/"

enum Tab { TAB_STATIONS, TAB_ITEMS, TAB_CRAFTS, TAB_TIMERS, TAB_MAPS, TAB_DATABASE, TAB_PROFILE, TAB_SETTINGS, TAB_COUNT };
static const char *kTabNames[TAB_COUNT] = {"Stations", "Needed", "Crafts", "Timers", "Maps", "Database", "Profile", "Settings"};

enum DbSection { DB_QUESTS, DB_AMMO, DB_KEYS, DB_ITEMS, DB_COUNT };
static const char *kDbNames[DB_COUNT] = {"Quests", "Ammo", "Keys", "Items"};
static const int kMaxIconPages = 8;

static const int kTop = 68;
static const int kBottom = 664;
static const int kMapTop = 50;
static const int kMapBottom = 676;
static const int kMapTile = 2048;

static const SDL_Color kMarkerColor[MK_COUNT] = {col::good, col::info, col::accent, col::violet, col::bad};
static const char *kMarkerName[MK_COUNT] = {"PMC extract", "Scav extract", "Shared extract", "Transit", "Boss"};

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

static bool fileExists(const std::string &path) {
    struct stat sb;
    return stat(path.c_str(), &sb) == 0;
}

static std::string groupDigits(int64_t n) {
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

static std::string dateString(int64_t seconds) {
    time_t t = seconds;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d", &tmv);
    return buf;
}

// Directional buttons with hold-to-repeat, plus sticks and touch.
struct Input {
    PadState pad;
    u64 down = 0, held = 0;
    u64 heldSince = 0, lastRepeat = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up
    bool touching = false;
    int touchX = 0, touchY = 0, touchDX = 0, touchDY = 0;

    void init() {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&pad);
        hidInitializeTouchScreen();
    }

    static float axis(int v) {
        float f = v / 32767.f;
        if (std::fabs(f) < 0.15f)
            return 0;
        return f;
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
        HidAnalogStickState l = padGetStickPos(&pad, 0);
        HidAnalogStickState r = padGetStickPos(&pad, 1);
        lx = axis(l.x);
        ly = axis(l.y);
        rx = axis(r.x);
        ry = axis(r.y);

        HidTouchScreenState ts = {};
        bool was = touching;
        touching = hidGetTouchScreenStates(&ts, 1) && ts.count > 0;
        touchDX = touchDY = 0;
        if (touching) {
            int x = ts.touches[0].x, y = ts.touches[0].y;
            if (was) {
                touchDX = x - touchX;
                touchDY = y - touchY;
            }
            touchX = x;
            touchY = y;
        }
    }

    bool pressed(u64 mask) const { return down & mask; }
    // The left stick also moves through lists.
    bool up() const { return pressed(HidNpadButton_Up | HidNpadButton_StickLUp); }
    bool downDir() const { return pressed(HidNpadButton_Down | HidNpadButton_StickLDown); }
    bool left() const { return pressed(HidNpadButton_Left | HidNpadButton_StickLLeft); }
    bool right() const { return pressed(HidNpadButton_Right | HidNpadButton_StickLRight); }
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
    std::string modeName() const { return st.mode == "pve" ? "pve" : "pvp"; }
    void loadData();
    void loadIcons(const std::string &root);
    void loadMaps();
    bool downloadData(bool quiet);
    void syncTracker();
    void editToken();
    void editProfileId();
    void loadProfile();
    void fetchProfile();
    void save();
    void setStatus(const std::string &s, SDL_Color c = col::dim);
    void busy(const std::string &s);

    void input();
    void inputStations();
    void inputItems();
    void inputCrafts();
    void inputTimers();
    void inputMaps();
    void inputDatabase();
    void inputQuest();
    void inputProfile();
    void inputSettings();

    void draw();
    void drawHeader();
    void freeMapTiles();
    void mapViewport(int &top, int &bottom) const;
    void drawFooter(const std::vector<std::pair<std::string, std::string>> &hints);
    void drawEmpty(const std::string &title, const std::string &sub);
    void drawStations();
    void drawItems();
    void drawCrafts();
    void drawTimers();
    void drawMaps();
    void drawDatabase();
    void drawDbHeader();
    void drawQuestList();
    void drawAmmo();
    void drawKeys();
    void drawDbItems();
    void drawItemDetail(const std::string &id, int x, int y, int w);
    void drawQuest();
    void drawProfile();
    void drawSettings();
    void drawScrollbar(int x, int y, int h, int count, int visible, int scroll);
    int drawBadge(int x, int y, const std::string &s, SDL_Color c);

    void rebuildNeeded();
    std::vector<const Craft *> visibleCrafts() const;
    const Craft *craftById(const std::string &id) const;
    Timer *timerFor(const std::string &craftId);
    std::string craftTitle(const Craft &c) const;

    // Database
    void ensureDb();
    void ensureWiki();
    void filterDb();
    void openQuestPage(int idx);
    void buildQuestRows(const Quest &q);
    void layoutQuest();
    void showPins(const std::string &label, const std::vector<Pin> &pins);
    std::string itemName(const std::string &id) const;
    int itemIcon(const std::string &id) const;
    std::vector<std::string> usedFor(const std::string &id) const;

    // Maps
    const MapInfo *currentMap() const { return maps.empty() ? nullptr : &maps[mapSel % maps.size()]; }
    bool ensureMapTexture();
    bool ensureFloorTexture();
    std::string mapFile(const MapInfo &m, const std::string &image, bool quiet);
    void openMap(int idx);
    void inputMapPicker();
    void drawMapPicker();
    void drawTiles(const std::vector<SDL_Texture *> &tiles, int imgW, int imgH, float x, float y, float w, float h,
                   int top, int bottom);
    std::vector<int> visibleMarkers() const;
    void centerOnMarker();

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
    bool firOnly = false;

    ListView craftList;
    int craftFilter = 0;  // 0 = all built stations, else station index + 1

    ListView timerList;
    ListView settingsList;
    bool confirmReset = false;

    std::vector<MapInfo> maps;
    std::string mapsRoot;
    int mapSel = 0;
    std::vector<SDL_Texture *> mapTiles; // kMapTile-sized pieces of the map image, row-major
    int mapImgW = 0, mapImgH = 0;
    std::string mapTexId;
    bool mapFull = false;
    bool mapPlan = false; // show the vector plan instead of the game render
    bool mapPicked = false; // false shows the map picker
    int pickSel = 0;
    int mapFloor = 0;     // index into MapInfo::floors
    std::vector<SDL_Texture *> floorTiles;
    int floorImgW = 0, floorImgH = 0;
    std::string floorTexId;
    std::map<std::string, SDL_Texture *> mapThumbs;
    bool mapFailed = false;
    float mapZoom = 1, mapU = 0.5f, mapV = 0.5f;
    int markerFilter = -1;  // -1 = all, else MarkerType
    int markerSel = -1;     // index into visibleMarkers()
    bool mapLabels = true;

    Profile profile;
    int skillScroll = 0;

    std::string dataRoot;
    Database db;
    bool dbLoaded = false;
    std::map<std::string, std::string> wiki;
    bool wikiLoaded = false;
    int dbSection = DB_QUESTS;
    std::string dbSearch;
    int dbFilter = 0;  // trader or caliber index + 1, 0 = all
    bool dbDirty = true;
    std::vector<int> dbRows;                 // indices into quests, ammo or keys
    std::vector<std::string> dbItemRows;     // item ids for the Items section
    std::vector<std::string> itemsByName;
    ListView dbList;
    std::string usedForId;
    std::vector<std::string> usedForCache;

    struct QRow {
        enum Kind { HEAD, TEXT, OBJ, ITEM } kind;
        int h = 0;
        std::string text;
        SDL_Color color = col::text;
        int obj = -1;          // objective index for OBJ
        std::string item;      // item id for ITEM
        int count = 0;
        std::vector<std::string> lines;
    };
    int openQuest = -1;
    std::vector<std::string> questText;
    int textScroll = 0;
    int questView = 0; // 0 guide + details, 1 guide only, 2 details only
    std::vector<QRow> qrows;
    int qrowSel = -1;
    int qrowScroll = 0;

    std::string pinLabel;
    std::vector<Pin> pins;
    bool pinReturn = false;

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
    mkdir(APP_DIR "/maps", 0777);
    in.init();
    if (!ui.init())
        return false;

    loadState(STATE_PATH, st);
    loadData();
    if (data.empty())
        downloadData(true);
    if (st.syncOnLaunch && !st.token.empty())
        syncTracker();
    loadProfile();
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
    freeMapTiles();
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

void App::loadIcons(const std::string &root) {
    // Icon indices belong to the data files next to the atlas pages.
    std::vector<SDL_Texture *> pages;
    for (int i = 0; i < kMaxIconPages; i++) {
        SDL_Texture *t = ui.loadImage(root + "icons-" + std::to_string(i) + ".jpg");
        if (!t)
            break;
        pages.push_back(t);
    }
    ui.setIcons(pages);
}

void App::loadData() {
    std::string name = "hideout-" + modeName() + ".json";
    HideoutData best;
    std::string bestRoot;
    std::string err;
    bool bestIcons = false;
    for (const std::string &root : {std::string(APP_DIR "/"), std::string("romfs:/")}) {
        std::string text = readFile(root + name);
        if (text.empty())
            continue;
        HideoutData d;
        if (!parseHideout(text, d, err))
            continue;
        // Data from before icons existed loses to any copy that has them.
        bool icons = fileExists(root + "icons-0.jpg") && !d.icons.empty();
        if (best.empty() || icons > bestIcons || (icons == bestIcons && d.generated > best.generated)) {
            bestIcons = icons;
            best = std::move(d);
            bestRoot = root;
        }
    }
    data = std::move(best);
    dataRoot = bestRoot;
    if (!bestRoot.empty())
        loadIcons(bestRoot);
    db = Database();
    dbLoaded = false;
    openQuest = -1;
    needDirty = true;
    viewLevel = -1;
    loadMaps();
}

void App::loadMaps() {
    std::string name = "maps-" + modeName() + ".json";
    std::string err;
    maps.clear();
    mapsRoot.clear();
    for (const std::string &root : {std::string(APP_DIR "/"), std::string("romfs:/")}) {
        std::vector<MapInfo> m;
        if (parseMaps(readFile(root + name), m, err)) {
            maps = std::move(m);
            mapsRoot = root;
            break;
        }
    }
    mapTexId.clear();
    mapFailed = false;
}

bool App::downloadData(bool quiet) {
    std::string name = "hideout-" + modeName() + ".json";
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
    busy("Downloading item icons and maps...");
    for (int i = 0; i < kMaxIconPages; i++) {
        std::string page = "icons-" + std::to_string(i) + ".jpg";
        HttpResult icons = httpGet(DATA_URL + page);
        if (!icons.ok || icons.status != 200)
            break;
        writeFile(std::string(APP_DIR "/") + page, icons.body);
    }
    busy("Downloading the quest and item database...");
    for (const std::string &file : {std::string("maps-") + modeName() + ".json", std::string("db-") + modeName() + ".json",
                                    std::string("wiki.json")}) {
        HttpResult f = httpGet(DATA_URL + file);
        if (f.ok && f.status == 200)
            writeFile(std::string(APP_DIR "/") + file, f.body);
    }
    writeFile(std::string(APP_DIR "/") + name, r.body);
    // Map images can change with the data; drop downloaded copies so they refetch on view.
    for (const char *sub : {"/maps/", "/maps/floors/", "/maps/thumbs/"}) {
        std::string d = std::string(APP_DIR) + sub;
        if (DIR *dir = opendir(d.c_str())) {
            while (struct dirent *e = readdir(dir))
                if (e->d_name[0] != '.')
                    remove((d + e->d_name).c_str());
            closedir(dir);
        }
    }
    freeMapTiles();
    for (auto &kv : mapThumbs)
        if (kv.second)
            SDL_DestroyTexture(kv.second);
    mapThumbs.clear();
    wikiLoaded = false;
    loadData();
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
                loadProfile();
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

static bool keyboard(const char *header, const char *guide, const std::string &initial, bool numeric,
                     std::string &out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    swkbdConfigMakePresetDefault(&kbd);
    if (numeric)
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&kbd, header);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 128);
    char buf[160] = {0};
    Result rc = swkbdShow(&kbd, buf, sizeof buf);
    swkbdClose(&kbd);
    if (R_FAILED(rc))
        return false;
    out = buf;
    out.erase(std::remove_if(out.begin(), out.end(), [](char c) { return c == ' ' || c == '\n' || c == '\r'; }),
              out.end());
    return true;
}

void App::editToken() {
    std::string tok;
    if (!keyboard("TarkovTracker API token", "Paste or type your token", st.token, false, tok))
        return;
    st.token = tok;
    save();
    if (!tok.empty())
        syncTracker();
}

void App::editProfileId() {
    std::string id;
    if (!keyboard("tarkov.dev profile ID", "The number in your tarkov.dev/players link", st.profileId, true, id))
        return;
    id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return c < '0' || c > '9'; }), id.end());
    st.profileId = id;
    save();
    profile = Profile();
    if (!id.empty())
        fetchProfile();
}

void App::loadProfile() {
    profile = Profile();
    if (st.profileId.empty())
        return;
    std::string err;
    std::string text = readFile(std::string(APP_DIR "/profile-") + modeName() + ".json");
    if (!text.empty())
        parseProfile(text, data, profile, err);
}

void App::fetchProfile() {
    if (st.profileId.empty()) {
        setStatus("Add your tarkov.dev profile ID first", col::warn);
        return;
    }
    busy("Loading profile from tarkov.dev...");
    std::string url = PROFILE_API + std::string(st.mode == "pve" ? "pve/" : "profile/") + st.profileId + ".json";
    HttpResult r = httpGet(url, {"Accept: application/json"});
    if (!r.ok) {
        setStatus("tarkov.dev unreachable: " + r.error, col::bad);
        return;
    }
    if (r.status == 404) {
        setStatus("No " + std::string(st.mode == "pve" ? "PVE" : "PVP") + " profile with that ID on tarkov.dev",
                  col::bad);
        return;
    }
    if (r.status != 200) {
        setStatus("tarkov.dev error: HTTP " + std::to_string(r.status), col::bad);
        return;
    }
    Profile p;
    std::string err;
    if (!parseProfile(r.body, data, p, err)) {
        setStatus("Profile error: " + err, col::bad);
        return;
    }
    profile = std::move(p);
    writeFile(std::string(APP_DIR "/profile-") + modeName() + ".json", r.body);
    setStatus("Loaded profile " + profile.nickname, col::good);
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
        if (!firOnly || v.fir)
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
    case TAB_MAPS: inputMaps(); break;
    case TAB_DATABASE: inputDatabase(); break;
    case TAB_PROFILE: inputProfile(); break;
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
        if (in.up())
            reqList.move(-1, m);
        if (in.downDir())
            reqList.move(1, m);
        if (in.pressed(HidNpadButton_B) || in.left() || m == 0) {
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

    if (in.up()) {
        stationList.move(-1, n);
        viewLevel = -1;
        reqList = ListView();
    }
    if (in.downDir()) {
        stationList.move(1, n);
        viewLevel = -1;
        reqList = ListView();
    }
    if (in.right() && L && !L->items.empty())
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
    if (in.up())
        itemList.move(-1, n);
    if (in.downDir())
        itemList.move(1, n);
    if (in.left())
        itemList.move(-8, n);
    if (in.right())
        itemList.move(8, n);
    if (in.pressed(HidNpadButton_Y)) {
        firOnly = !firOnly;
        itemList = ListView();
        needDirty = true;
    }
}

void App::inputCrafts() {
    auto list = visibleCrafts();
    int n = list.size();
    if (in.left() || in.right()) {
        int d = in.right() ? 1 : -1;
        int count = data.stations.size() + 1;
        craftFilter = (craftFilter + d + count) % count;
        craftList = ListView();
        return;
    }
    if (in.up())
        craftList.move(-1, n);
    if (in.downDir())
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
    if (in.up())
        timerList.move(-1, n);
    if (in.downDir())
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

std::vector<int> App::visibleMarkers() const {
    std::vector<int> out;
    const MapInfo *m = currentMap();
    if (!m)
        return out;
    for (int i = 0; i < (int)m->markers.size(); i++)
        if (markerFilter < 0 || m->markers[i].type == markerFilter)
            out.push_back(i);
    // Walk markers left to right so the D-pad feels spatial.
    std::sort(out.begin(), out.end(), [m](int a, int b) { return m->markers[a].u < m->markers[b].u; });
    return out;
}

void App::centerOnMarker() {
    auto vis = visibleMarkers();
    const MapInfo *m = currentMap();
    if (!m || markerSel < 0 || markerSel >= (int)vis.size())
        return;
    const MapMarker &mk = m->markers[vis[markerSel]];
    mapU = mk.u;
    mapV = mk.v;
    mapZoom = std::max(mapZoom, 2.5f);
}

bool App::ensureMapTexture() {
    const MapInfo *m = currentMap();
    if (!m)
        return false;
    const std::string &image = mapPlan && !m->alt.empty() ? m->alt : m->image;
    if (!mapTiles.empty() && mapTexId == image)
        return true;
    if (mapFailed && mapTexId == image)
        return false;
    freeMapTiles();
    mapTexId = image;
    mapFailed = false;
    std::string path = mapFile(*m, image, false);
    if (!path.empty() && !ui.loadImageTiles(path, kMapTile, mapTiles, mapImgW, mapImgH))
        setStatus("Not enough memory for the " + m->name + " map", col::bad);
    mapFailed = mapTiles.empty();
    return !mapFailed;
}

// Finds a map image on the SD card or in romfs, downloading it once if neither has it.
std::string App::mapFile(const MapInfo &m, const std::string &image, bool quiet) {
    std::string sd = std::string(APP_DIR "/") + image;
    if (fileExists(sd))
        return sd;
    if (fileExists("romfs:/" + image))
        return "romfs:/" + image;
    if (quiet)
        return "";
    busy("Downloading " + m.name + " map...");
    HttpResult r = httpGet(DATA_URL + image);
    size_t slash = image.rfind('/');
    if (slash != std::string::npos)
        mkdir((std::string(APP_DIR "/") + image.substr(0, slash)).c_str(), 0777);
    if (r.ok && r.status == 200 && writeFile(sd, r.body)) {
        setStatus(m.name + " map saved for offline use", col::good);
        return sd;
    }
    setStatus("Could not download the " + m.name + " map", col::bad);
    return "";
}

bool App::ensureFloorTexture() {
    const MapInfo *m = currentMap();
    if (!m || mapFloor == m->baseFloor || mapFloor >= (int)m->floors.size())
        return false;
    const MapFloor &f = m->floors[mapFloor];
    const std::string &image = mapPlan && !f.alt.empty() ? f.alt : f.image;
    if (floorTexId == image)
        return !floorTiles.empty();
    for (SDL_Texture *t : floorTiles)
        SDL_DestroyTexture(t);
    floorTiles.clear();
    floorTexId = image;
    std::string path = mapFile(*m, image, false);
    if (!path.empty())
        ui.loadImageTiles(path, kMapTile, floorTiles, floorImgW, floorImgH);
    return !floorTiles.empty();
}

void App::openMap(int idx) {
    mapSel = idx;
    mapPicked = true;
    mapZoom = 1;
    mapU = mapV = 0.5f;
    markerSel = -1;
    mapFloor = maps[idx].baseFloor;
}

void App::freeMapTiles() {
    for (SDL_Texture *t : mapTiles)
        SDL_DestroyTexture(t);
    mapTiles.clear();
    for (SDL_Texture *t : floorTiles)
        SDL_DestroyTexture(t);
    floorTiles.clear();
    floorTexId.clear();
}

void App::mapViewport(int &top, int &bottom) const {
    top = mapFull ? 0 : kMapTop;
    bottom = mapFull ? 720 : kMapBottom;
}

static const int kPickCols = 4;

void App::inputMapPicker() {
    int n = maps.size();
    pickSel = std::clamp(pickSel, 0, n - 1);
    if (in.left() && pickSel % kPickCols > 0)
        pickSel--;
    if (in.right() && pickSel % kPickCols < kPickCols - 1 && pickSel + 1 < n)
        pickSel++;
    if (in.up() && pickSel >= kPickCols)
        pickSel -= kPickCols;
    if (in.downDir() && pickSel + kPickCols < n)
        pickSel += kPickCols;
    if (in.pressed(HidNpadButton_A))
        openMap(pickSel);
}

void App::inputMaps() {
    if (maps.empty())
        return;
    if (!mapPicked) {
        inputMapPicker();
        return;
    }
    const MapInfo *cm = currentMap();
    if ((in.pressed(HidNpadButton_ZL) || in.pressed(HidNpadButton_ZR)) && !cm->floors.empty()) {
        int d = in.pressed(HidNpadButton_ZR) ? 1 : -1;
        mapFloor = std::clamp(mapFloor + d, 0, (int)cm->floors.size() - 1);
        return;
    }
    auto vis = visibleMarkers();
    if (in.pressed(HidNpadButton_Right) || in.pressed(HidNpadButton_Left)) {
        int n = vis.size();
        if (n > 0) {
            int d = in.pressed(HidNpadButton_Right) ? 1 : -1;
            markerSel = markerSel < 0 ? (d > 0 ? 0 : n - 1) : (markerSel + d + n) % n;
            centerOnMarker();
        }
    }
    if (in.pressed(HidNpadButton_Up) || in.pressed(HidNpadButton_Down)) {
        int d = in.pressed(HidNpadButton_Down) ? 1 : -1;
        markerFilter = (markerFilter + 1 + d + MK_COUNT + 1) % (MK_COUNT + 1) - 1;
        markerSel = -1;
    }
    if (in.pressed(HidNpadButton_Y))
        mapLabels = !mapLabels;
    if (in.pressed(HidNpadButton_Minus))
        mapFull = !mapFull;
    if (in.pressed(HidNpadButton_StickR) && !currentMap()->alt.empty())
        mapPlan = !mapPlan;
    if (in.pressed(HidNpadButton_B) && pinReturn) {
        // Came from a quest or key page: go back to it.
        pinReturn = false;
        pins.clear();
        tab = TAB_DATABASE;
        return;
    }
    if (in.pressed(HidNpadButton_B)) {
        mapPicked = false;
        pickSel = mapSel;
        return;
    }

    // Right stick or A/X zooms, left stick or touch pans.
    float zoomStep = in.ry * 0.04f;
    if (in.held & HidNpadButton_A)
        zoomStep += 0.03f;
    if (in.held & HidNpadButton_X)
        zoomStep -= 0.03f;
    mapZoom = std::clamp(mapZoom * (1 + zoomStep), 1.f, 12.f);

    const MapInfo *m = currentMap();
    int top, bottom;
    mapViewport(top, bottom);
    float viewW = 1280, viewH = bottom - top;
    float fit = std::min(viewW / m->w, viewH / m->h);
    float scale = fit * mapZoom;
    float pan = 14.f / scale;
    mapU += in.lx * pan / m->w;
    mapV -= in.ly * pan / m->h;
    if (in.touching && (in.touchDX || in.touchDY)) {
        mapU -= in.touchDX / (scale * m->w);
        mapV -= in.touchDY / (scale * m->h);
    }
    mapU = std::clamp(mapU, 0.f, 1.f);
    mapV = std::clamp(mapV, 0.f, 1.f);
}

void App::inputProfile() {
    if (in.pressed(HidNpadButton_A) || in.pressed(HidNpadButton_X)) {
        if (st.profileId.empty() || in.pressed(HidNpadButton_X))
            editProfileId();
        else
            fetchProfile();
    }
    int n = profile.skills.size();
    if (in.up())
        skillScroll = std::max(0, skillScroll - 1);
    if (in.downDir())
        skillScroll = std::min(std::max(0, n - 12), skillScroll + 1);
}

enum SettingRow { SET_TOKEN, SET_SYNC, SET_SYNC_LAUNCH, SET_PROFILE, SET_MODE, SET_DOWNLOAD, SET_RESET, SET_COUNT };

void App::inputSettings() {
    if (in.up()) {
        settingsList.move(-1, SET_COUNT);
        confirmReset = false;
    }
    if (in.downDir()) {
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
    case SET_PROFILE: editProfileId(); break;
    case SET_MODE:
        st.mode = st.mode == "pve" ? "pvp" : "pve";
        loadData();
        if (data.empty())
            downloadData(false);
        loadProfile();
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


// ---------------------------------------------------------------- database

void App::ensureDb() {
    if (dbLoaded)
        return;
    dbLoaded = true;
    std::string name = "db-" + modeName() + ".json";
    std::string err;
    std::vector<std::string> roots = {dataRoot, std::string(APP_DIR "/"), std::string("romfs:/")};
    for (const auto &root : roots) {
        if (root.empty())
            continue;
        std::string text = readFile(root + name);
        if (!text.empty() && parseDatabase(text, db, err))
            break;
    }
    itemsByName.clear();
    for (const auto &[id, it] : db.items)
        itemsByName.push_back(id);
    std::sort(itemsByName.begin(), itemsByName.end(),
              [this](const std::string &a, const std::string &b) { return db.items.at(a).name < db.items.at(b).name; });
    dbDirty = true;
}

void App::ensureWiki() {
    if (wikiLoaded)
        return;
    wikiLoaded = true;
    for (const std::string &root : {std::string(APP_DIR "/"), std::string("romfs:/")})
        if (parseWiki(readFile(root + "wiki.json"), wiki) && !wiki.empty())
            break;
}

std::string App::itemName(const std::string &id) const {
    const DbItem *it = db.item(id);
    return it ? it->name : id;
}

int App::itemIcon(const std::string &id) const {
    const DbItem *it = db.item(id);
    if (it && it->icon >= 0)
        return it->icon;
    return data.icon(id);
}

static std::string lower(std::string s) {
    for (auto &c : s)
        c = std::tolower((unsigned char)c);
    return s;
}

static bool matches(const std::string &hay, const std::string &needle) {
    return needle.empty() || lower(hay).find(needle) != std::string::npos;
}

void App::filterDb() {
    dbDirty = false;
    dbRows.clear();
    dbItemRows.clear();
    std::string q = lower(dbSearch);
    switch (dbSection) {
    case DB_QUESTS:
        for (int i = 0; i < (int)db.quests.size(); i++) {
            const Quest &qu = db.quests[i];
            if (dbFilter > 0 && qu.trader != db.traders[dbFilter - 1])
                continue;
            if (matches(qu.name, q) || matches(qu.map, q))
                dbRows.push_back(i);
        }
        break;
    case DB_AMMO:
        for (int i = 0; i < (int)db.ammo.size(); i++) {
            const Ammo &a = db.ammo[i];
            if (dbFilter > 0 && a.caliber != db.calibers[dbFilter - 1])
                continue;
            if (matches(itemName(a.item), q) || matches(a.caliber, q))
                dbRows.push_back(i);
        }
        break;
    case DB_KEYS:
        for (int i = 0; i < (int)db.keys.size(); i++)
            if (matches(itemName(db.keys[i].item), q))
                dbRows.push_back(i);
        break;
    case DB_ITEMS:
        for (const auto &id : itemsByName) {
            const DbItem &it = db.items.at(id);
            if (matches(it.name, q) || matches(it.shortName, q))
                dbItemRows.push_back(id);
        }
        break;
    }
}

int filterCount(int section, const Database &db) {
    return section == DB_QUESTS ? db.traders.size() : (section == DB_AMMO ? db.calibers.size() : 0);
}

void App::showPins(const std::string &label, const std::vector<Pin> &list) {
    if (list.empty())
        return;
    int idx = -1;
    for (const Pin &p : list) {
        for (int i = 0; i < (int)maps.size() && idx < 0; i++)
            if (maps[i].id == p.map)
                idx = i;
        if (idx >= 0)
            break;
    }
    if (idx < 0) {
        setStatus("That map is not in the map data yet", col::warn);
        return;
    }
    pins = list;
    pinLabel = label;
    pinReturn = true;
    openMap(idx);
    // Center on the pins of this map and zoom so they all stay in view.
    float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    for (const Pin &p : list)
        if (p.map == maps[idx].id) {
            u0 = std::min(u0, p.u);
            v0 = std::min(v0, p.v);
            u1 = std::max(u1, p.u);
            v1 = std::max(v1, p.v);
        }
    mapU = (u0 + u1) / 2;
    mapV = (v0 + v1) / 2;
    float span = std::max(u1 - u0, v1 - v0);
    mapZoom = std::clamp(0.6f / std::max(span, 0.12f), 1.f, 4.f);
    tab = TAB_MAPS;
}

// Hideout levels, crafts and quests that use an item.
std::vector<std::string> App::usedFor(const std::string &id) const {
    std::vector<std::string> out;
    for (const auto &s : data.stations)
        for (const auto &l : s.levels)
            for (const auto &r : l.items)
                if (r.itemId == id)
                    out.push_back("Hideout: " + s.name + " " + std::to_string(l.level) + "  x" + groupDigits(r.count) +
                                  (r.fir ? " (FIR)" : ""));
    for (const auto &q : db.quests)
        for (const auto &o : q.objectives)
            if (std::find(o.items.begin(), o.items.end(), id) != o.items.end() ||
                std::find(o.build.parts.begin(), o.build.parts.end(), id) != o.build.parts.end()) {
                out.push_back("Quest: " + q.name + " (" + q.trader + ")");
                break;
            }
    for (const auto &q : db.quests)
        if (std::find(q.keys.begin(), q.keys.end(), id) != q.keys.end())
            out.push_back("Key for: " + q.name);
    for (const auto &c : data.crafts) {
        const Station *s = data.station(c.stationId);
        for (const auto &r : c.req)
            if (r.itemId == id) {
                out.push_back("Craft input: " + craftTitle(c) + " (" + (s ? s->name : "") + ")");
                break;
            }
        for (const auto &r : c.rew)
            if (r.itemId == id) {
                out.push_back("Crafted at " + (s ? s->name : std::string("hideout")) + " " + std::to_string(c.level));
                break;
            }
    }
    return out;
}

void App::inputDatabase() {
    ensureDb();
    if (openQuest >= 0) {
        inputQuest();
        return;
    }
    if (in.pressed(HidNpadButton_ZL) || in.pressed(HidNpadButton_ZR)) {
        int d = in.pressed(HidNpadButton_ZR) ? 1 : -1;
        dbSection = (dbSection + d + DB_COUNT) % DB_COUNT;
        dbFilter = 0;
        dbList = ListView();
        dbDirty = true;
        return;
    }
    if (in.pressed(HidNpadButton_Y)) {
        std::string q;
        if (keyboard("Search", "Part of a name", dbSearch, false, q)) {
            dbSearch = q;
            dbList = ListView();
            dbDirty = true;
        }
    }
    if (in.pressed(HidNpadButton_X) && !dbSearch.empty()) {
        dbSearch.clear();
        dbList = ListView();
        dbDirty = true;
    }
    int nf = filterCount(dbSection, db);
    if (nf > 0 && (in.left() || in.right())) {
        int d = in.right() ? 1 : -1;
        dbFilter = (dbFilter + d + nf + 1) % (nf + 1);
        dbList = ListView();
        dbDirty = true;
        return;
    }
    if (dbDirty)
        filterDb();
    int n = dbSection == DB_ITEMS ? dbItemRows.size() : dbRows.size();
    if (in.up())
        dbList.move(-1, n);
    if (in.downDir())
        dbList.move(1, n);
    if (dbSection == DB_ITEMS || dbSection == DB_KEYS) {
        // No filter on these, so Left/Right page through the list.
        if (in.left())
            dbList.move(-10, n);
        if (in.right())
            dbList.move(10, n);
    }
    dbList.clampSel(n);
    if (n == 0 || !in.pressed(HidNpadButton_A))
        return;
    if (dbSection == DB_QUESTS)
        openQuestPage(dbRows[dbList.sel]);
    else if (dbSection == DB_KEYS) {
        const KeyInfo &k = db.keys[dbRows[dbList.sel]];
        if (k.pins.empty())
            setStatus("No lock position known for this key", col::warn);
        else
            showPins(itemName(k.item), k.pins);
    }
}

void App::openQuestPage(int idx) {
    ensureWiki();
    openQuest = idx;
    textScroll = 0;
    layoutQuest();
    qrowSel = -1;
    for (int i = 0; i < (int)qrows.size(); i++)
        if (qrows[i].kind == QRow::OBJ) {
            qrowSel = i;
            break;
        }
    qrowScroll = 0;
}

// Wraps the guide and details for the current view mode's panel widths.
void App::layoutQuest() {
    const Quest &q = db.quests[openQuest];
    auto it = wiki.find(q.wiki);
    std::string text = it != wiki.end() ? it->second : "";
    questText = text.empty() ? std::vector<std::string>{"No wiki guide cached for this quest.",
                                                          "Download the latest data in Settings to get the guides."}
                             : ui.wrap(text, FS_SMALL, questView == 1 ? 1200 : 560);
    buildQuestRows(q);
}

void App::buildQuestRows(const Quest &q) {
    qrows.clear();
    const int w = questView == 2 ? 1160 : 540;
    auto head = [&](const std::string &t) {
        QRow r{QRow::HEAD};
        r.text = t;
        r.h = 40;
        qrows.push_back(r);
    };
    auto text = [&](const std::string &t, SDL_Color c) {
        QRow r{QRow::TEXT};
        r.lines = ui.wrap(t, FS_SMALL, w);
        r.color = c;
        r.h = r.lines.size() * 26 + 4;
        qrows.push_back(r);
    };
    auto item = [&](const std::string &id, int count, const std::string &suffix = "") {
        QRow r{QRow::ITEM};
        r.item = id;
        r.count = count;
        r.text = suffix;
        r.h = 40;
        qrows.push_back(r);
    };

    if (!q.prereqs.empty() || !q.traderReqs.empty() || q.level > 1) {
        head("Requirements");
        if (q.level > 1)
            text("Level " + std::to_string(q.level), col::text);
        for (const auto &t : q.traderReqs)
            text(t, col::text);
        for (const auto &p : q.prereqs)
            text("Complete " + p, col::dim);
    }
    if (!q.keys.empty() || !q.startItems.empty()) {
        head("Bring / you get");
        for (const auto &k : q.keys)
            item(k, 1, "key");
        for (const auto &s : q.startItems)
            item(s.item, s.count, "given at start");
    }
    head("Objectives");
    for (int i = 0; i < (int)q.objectives.size(); i++) {
        const Objective &o = q.objectives[i];
        QRow r{QRow::OBJ};
        r.obj = i;
        r.lines = ui.wrap(o.desc, FS_SMALL, w - 40);
        int h = r.lines.size() * 26 + 12;
        if (!o.items.empty())
            h += 44;
        if (o.hasBuild)
            h += 26 * (o.build.attrs.size() + (o.build.categories.empty() ? 0 : 1)) + (o.build.parts.empty() ? 0 : 44) + 8;
        r.h = h;
        qrows.push_back(r);
    }
    const Rewards &rw = q.rewards;
    head("Rewards");
    if (q.exp)
        text("+" + groupDigits(q.exp) + " EXP", col::good);
    for (const auto &rp : rw.rep) {
        char buf[64];
        snprintf(buf, sizeof buf, "%s rep %+.2f", rp.first.c_str(), rp.second);
        text(buf, rp.second >= 0 ? col::good : col::bad);
    }
    for (const auto &it : rw.items)
        item(it.item, it.count);
    for (const auto &sk : rw.skills)
        text(sk.first + " +" + std::to_string(sk.second) + " levels", col::text);
    for (const auto &t : rw.traders)
        text("Unlocks trader " + t, col::accent);
    for (const auto &u : rw.unlocks)
        item(u.item, 0, "unlocked at " + u.trader + " LL" + std::to_string(u.level));
}

void App::inputQuest() {
    const Quest &q = db.quests[openQuest];
    if (in.pressed(HidNpadButton_B)) {
        openQuest = -1;
        return;
    }
    if (in.pressed(HidNpadButton_Y)) {
        // Keep roughly the same reading position when the guide rewraps.
        float frac = questText.empty() ? 0 : (float)textScroll / questText.size();
        questView = (questView + 1) % 3;
        layoutQuest();
        textScroll = (int)(frac * questText.size());
    }
    // Right stick or ZL/ZR scrolls the wiki text (the d-pad too when only the guide is shown).
    int visible = (kBottom - kTop - 110) / 26;
    int maxScroll = std::max(0, (int)questText.size() - visible);
    static float acc = 0;
    acc += -in.ry * 0.6f;
    if (questView == 1) {
        if (in.up())
            acc -= 3;
        if (in.downDir())
            acc += 3;
    }
    if (in.pressed(HidNpadButton_ZR))
        acc += visible - 2;
    if (in.pressed(HidNpadButton_ZL))
        acc -= visible - 2;
    int step = (int)acc;
    acc -= step;
    textScroll = std::clamp(textScroll + step, 0, maxScroll);

    std::vector<int> sel;
    for (int i = 0; i < (int)qrows.size(); i++)
        if (qrows[i].kind == QRow::OBJ || qrows[i].kind == QRow::ITEM)
            sel.push_back(i);
    if (questView != 1 && !sel.empty() && (in.up() || in.downDir())) {
        int pos = std::find(sel.begin(), sel.end(), qrowSel) - sel.begin();
        if (pos >= (int)sel.size())
            pos = 0;
        else
            pos = std::clamp(pos + (in.downDir() ? 1 : -1), 0, (int)sel.size() - 1);
        qrowSel = sel[pos];
    }
    if (questView != 1 && in.pressed(HidNpadButton_A) && qrowSel >= 0 && qrowSel < (int)qrows.size()) {
        const QRow &r = qrows[qrowSel];
        if (r.kind == QRow::OBJ) {
            const Objective &o = q.objectives[r.obj];
            if (!o.pins.empty())
                showPins(q.name, o.pins);
            else
                setStatus("No map position for this objective", col::dim);
        } else if (r.kind == QRow::ITEM) {
            // Jump to the item in the Items section.
            dbSection = DB_ITEMS;
            dbSearch = itemName(r.item);
            dbFilter = 0;
            dbList = ListView();
            dbDirty = true;
            openQuest = -1;
        }
    }
}

// ---------------------------------------------------------------- drawing

void App::drawScrollbar(int x, int y, int h, int count, int visible, int scroll) {
    if (count <= visible)
        return;
    int th = std::max(30, h * visible / count);
    int ty = y + (h - th) * scroll / std::max(1, count - visible);
    ui.fill(x, y, 4, h, col::panel);
    ui.fill(x, ty, 4, th, col::faint);
}

int App::drawBadge(int x, int y, const std::string &s, SDL_Color c) {
    int w = ui.measure(s, FS_SMALL) + 14;
    SDL_Color bg{c.r, c.g, c.b, 48};
    ui.fill(x, y, w, 24, bg);
    ui.fill(x, y, 3, 24, c);
    ui.text(x + 8, y + 12 - ui.lineHeight(FS_SMALL) / 2, s, FS_SMALL, c);
    return w;
}

void App::drawHeader() {
    // One compact bar: tabs only. The Profile tab carries the player name and game mode.
    ui.fill(0, 0, 1280, 46, col::bgTop);
    ui.fill(0, 0, 1280, 2, col::accent);
    bool pve = st.mode == "pve";
    const char *mode = pve ? "PVE" : "PVP";
    std::string who = !profile.empty() ? profile.nickname : st.account;
    if (who.empty())
        who = kTabNames[TAB_PROFILE];

    int x = 64;
    ui.hint(20, 10, "L", "");
    for (int i = 0; i < TAB_COUNT; i++) {
        bool on = i == tab;
        std::string label = i == TAB_PROFILE ? who : kTabNames[i];
        int lw = ui.measure(label, FS_SMALL);
        int w = lw + 28 + (i == TAB_PROFILE ? ui.measure(mode, FS_SMALL) + 22 : 0);
        if (on) {
            ui.fill(x, 4, w, 42, col::panel);
            ui.fill(x, 43, w, 3, col::accent);
        }
        ui.text(x + 14, 12, label, FS_SMALL, on ? col::text : col::dim);
        if (i == TAB_PROFILE)
            drawBadge(x + 22 + lw, 11, mode, pve ? col::good : col::bad);
        x += w + 2;
    }
    ui.hint(x + 10, 10, "R", "");
    ui.fill(0, 46, 1280, 1, col::line);
}

void App::drawFooter(const std::vector<std::pair<std::string, std::string>> &hints) {
    ui.fill(0, 676, 1280, 44, col::panel);
    ui.fill(0, 676, 1280, 1, col::line);
    bool fresh = armTicksToNs(armGetSystemTick() - statusTick) < 8000000000ULL;
    int w = 0;
    for (const auto &h : hints)
        w += ui.hintsWidth(h.first, h.second) + 22;
    if (!status.empty() && fresh)
        ui.text(24, 686, status, FS_SMALL, statusColor, 1220 - w - 24);
    int x = 1256 - w + 22;
    for (const auto &h : hints)
        x += ui.hint(x, 685, h.first, h.second) + 22;
}

void App::drawEmpty(const std::string &title, const std::string &sub) {
    ui.text(40, kTop + 20, title, FS_BODY, col::text);
    ui.text(40, kTop + 60, sub, FS_SMALL, col::dim, 1200);
}

void App::drawStations() {
    if (data.empty()) {
        drawEmpty("No hideout data loaded.", "Go to Settings and choose \"Download latest hideout data\".");
        drawFooter({{"+", "Quit"}});
        return;
    }
    const int rowH = 46;
    int visible = (kBottom - kTop) / rowH;
    stationList.clamp(data.stations.size(), visible);
    for (int i = 0; i < visible && stationList.scroll + i < (int)data.stations.size(); i++) {
        int idx = stationList.scroll + i;
        const Station &s = data.stations[idx];
        int y = kTop + i * rowH;
        bool on = idx == stationList.sel;
        if (on) {
            ui.fill(16, y, 424, rowH - 4, reqFocus ? col::panel : col::panelHi);
            ui.fill(16, y, 4, rowH - 4, col::accent);
        }
        int built = st.builtLevel(s);
        int maxLv = s.levels.empty() ? 0 : s.levels.back().level;
        bool maxed = built >= maxLv;
        ui.text(32, y + 4, s.name, FS_BODY, maxed ? col::dim : col::text, 280);
        // One segment per level.
        int segW = maxLv > 0 ? std::min(26, 120 / maxLv) : 0;
        for (int l = 1; l <= maxLv; l++)
            ui.fill(32 + (l - 1) * (segW + 3), y + 34, segW, 4, l <= built ? col::good : col::line);
        std::string lv = maxed ? "MAX" : std::to_string(built) + " / " + std::to_string(maxLv);
        ui.textRight(428, y + 10, lv, FS_SMALL, maxed ? col::good : col::dim);
    }
    drawScrollbar(446, kTop, visible * rowH, data.stations.size(), visible, stationList.scroll);

    const Station &s = data.stations[stationList.sel];
    int built = st.builtLevel(s);
    int lvl = shownLevel(st, s, viewLevel);
    const Level *L = levelOf(s, lvl);
    int px = 468;
    int pw = 1280 - px - 16;
    ui.fill(px, kTop, pw, kBottom - kTop, col::panel);
    if (!L) {
        ui.text(px + 20, kTop + 16, s.name + " has no levels", FS_BODY, col::dim);
        drawFooter({{"+", "Quit"}});
        return;
    }
    ui.text(px + 20, kTop + 12, s.name, FS_TITLE, col::text, 480);
    // Level chips; the shown one is outlined.
    int cx = px + pw - 20;
    for (int i = (int)s.levels.size() - 1; i >= 0; i--) {
        int l = s.levels[i].level;
        cx -= 38;
        SDL_Color c = l <= built ? col::good : (l == built + 1 ? col::accent : col::faint);
        ui.fill(cx, kTop + 16, 34, 30, l == lvl ? c : col::cell);
        std::string t = std::to_string(l);
        ui.text(cx + 17 - ui.measure(t, FS_SMALL) / 2, kTop + 18, t, FS_SMALL, l == lvl ? col::bg : c);
    }
    std::string tag = lvl <= built ? "BUILT" : (lvl == built + 1 ? "NEXT UPGRADE" : "LOCKED");
    int y = kTop + 58;
    int bw = drawBadge(px + 20, y, "Level " + std::to_string(lvl) + "  " + tag,
                       lvl <= built ? col::good : (lvl == built + 1 ? col::accent : col::faint));
    if (L->constructionTime > 0)
        ui.text(px + 36 + bw, y + 1, "Build time " + formatDuration(L->constructionTime), FS_SMALL, col::dim);
    y += 34;
    for (const auto &o : L->other) {
        ui.fill(px + 22, y + 9, 6, 6, col::dim);
        ui.text(px + 36, y, o, FS_SMALL, col::dim, pw - 60);
        y += 26;
    }
    y += 6;
    ui.fill(px + 16, y, pw - 32, 1, col::line);
    y += 10;

    int m = L->items.size();
    const int ih = 50;
    int rows = std::max(1, (kBottom - y - 6) / ih);
    if (reqFocus)
        reqList.clamp(m, rows);
    if (m == 0)
        ui.text(px + 20, y + 4, "No items needed", FS_BODY, col::dim);
    for (int i = 0; i < rows && reqList.scroll + i < m; i++) {
        int idx = reqList.scroll + i;
        const ReqItem &r = L->items[idx];
        int ry = y + i * ih;
        bool on = reqFocus && idx == reqList.sel;
        if (on) {
            ui.fill(px + 8, ry, pw - 24, ih - 4, col::panelHi);
            ui.fill(px + 8, ry, 4, ih - 4, col::accent);
        }
        int have = lvl <= built ? r.count : st.have(r);
        bool done = have >= r.count;
        ui.icon(data.icon(r.itemId), px + 20, ry + 2, 42);
        int w = ui.text(px + 74, ry + 2, r.name, FS_BODY, done ? col::good : col::text, 460);
        if (r.fir)
            drawBadge(px + 84 + w, ry + 4, "FIR", col::warn);
        std::string cnt = groupDigits(have) + " / " + groupDigits(r.count);
        ui.textRight(px + pw - 24, ry + 4, cnt, FS_BODY, done ? col::good : col::text);
        ui.progress(px + 74, ry + 36, pw - 98 - 74 + 74 - 24, 4, float(have) / std::max(1, r.count),
                    done ? col::good : col::accent);
    }
    drawScrollbar(px + pw - 8, y, rows * ih, m, rows, reqList.scroll);

    if (reqFocus)
        drawFooter({{"A", "+1"}, {"X", "-1"}, {"Y", "All/none"}, {"B", "Back"}});
    else
        drawFooter({{"A", "Build"}, {"X", "Undo"}, {"ZL/ZR", "Level"}, {">", "Items"}});
}

void App::drawItems() {
    const int rowH = 56;
    int top = kTop + 36;
    int visible = (kBottom - top) / rowH;
    itemList.clamp(needed.size(), visible);
    int fir = 0;
    for (const auto &n : needed)
        fir += n.fir;
    ui.text(28, kTop - 4, std::to_string(needed.size()) + " items still needed", FS_BODY, col::text);
    ui.text(320, kTop, std::to_string(fir) + " must be found in raid", FS_SMALL, col::warn);
    if (firOnly)
        drawBadge(1150, kTop - 2, "FIR only", col::warn);
    if (needed.empty()) {
        drawEmpty("", data.empty() ? "No hideout data loaded." : "Nothing left to collect. Hideout maxed!");
        drawFooter({{"Y", firOnly ? "Show all" : "FIR only"}});
        return;
    }
    for (int i = 0; i < visible && itemList.scroll + i < (int)needed.size(); i++) {
        int idx = itemList.scroll + i;
        const NeededItem &n = needed[idx];
        int y = top + i * rowH;
        bool on = idx == itemList.sel;
        ui.fill(16, y, 1240, rowH - 6, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 6, col::accent);
        ui.icon(data.icon(n.itemId), 28, y + 3, 44);
        int w = ui.text(88, y + 10, n.name, FS_BODY, col::text, 520);
        if (n.fir)
            drawBadge(98 + w, y + 12, "FIR", col::warn);
        ui.textRight(790, y + 10, "x" + groupDigits(n.need), FS_BODY, col::accent);
        std::string used;
        for (size_t k = 0; k < n.usedBy.size(); k++)
            used += (k ? ", " : "") + n.usedBy[k];
        ui.text(820, y + 14, used, FS_SMALL, col::dim, 420);
    }
    drawScrollbar(1262, top, visible * rowH, needed.size(), visible, itemList.scroll);
    drawFooter({{"Y", firOnly ? "Show all" : "FIR only"}, {"</>", "Page"}});
}

void App::drawCrafts() {
    auto list = visibleCrafts();
    std::string filter = craftFilter == 0 ? "Crafts at your built stations" : data.stations[craftFilter - 1].name;
    ui.text(28, kTop - 4, "<  " + filter + "  >", FS_BODY, col::accent);
    ui.textRight(1252, kTop, std::to_string(list.size()) + " recipes", FS_SMALL, col::dim);
    int top = kTop + 36;
    int rowH = 72;
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
        bool on = idx == craftList.sel;
        ui.fill(16, y, 1240, rowH - 6, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 6, col::accent);
        const Station *s = data.station(c.stationId);
        bool locked = s && st.builtLevel(*s) < c.level;
        ui.icon(c.rew.empty() ? -1 : data.icon(c.rew[0].itemId), 28, y + 4, 58);
        ui.text(100, y + 4, craftTitle(c), FS_BODY, locked ? col::dim : col::text, 700);
        std::string where = (s ? s->name : c.stationId) + " " + std::to_string(c.level);
        int x = 100 + ui.text(100, y + 38, where, FS_SMALL, locked ? col::bad : col::dim) + 18;
        // Inputs as small icons with counts.
        for (const auto &r : c.req) {
            if (x > 980)
                break;
            ui.icon(data.icon(r.itemId), x, y + 34, 28);
            x += 32;
            std::string label = (r.count > 1 ? "x" + std::to_string(r.count) + " " : "") + r.shortName;
            x += ui.text(x, y + 38, label, FS_SMALL, col::dim, 160) + 14;
        }
        Timer *t = timerFor(c.id);
        if (t) {
            int64_t left = t->endTime - now;
            ui.textRight(1240, y + 6, left <= 0 ? "READY" : formatDuration(left), FS_BODY,
                         left <= 0 ? col::good : col::accent);
            float frac = c.duration > 0 ? 1.f - float(left) / c.duration : 1.f;
            ui.progress(1100, y + 44, 140, 6, frac, left <= 0 ? col::good : col::accent);
        } else {
            ui.textRight(1240, y + 8, formatDuration(c.duration), FS_SMALL, col::dim);
        }
    }
    drawScrollbar(1262, top, visible * rowH, list.size(), visible, craftList.scroll);
    drawFooter({{"A", "Start/collect"}, {"X", "Cancel"}, {"</>", "Station"}});
}

void App::drawTimers() {
    std::sort(st.timers.begin(), st.timers.end(),
              [](const Timer &a, const Timer &b) { return a.endTime < b.endTime; });
    const int rowH = 72;
    int visible = (kBottom - kTop) / rowH;
    timerList.clamp(st.timers.size(), visible);
    if (st.timers.empty())
        drawEmpty("No crafts running.", "Start one from the Crafts tab and it will count down here, even while the app is closed.");
    int64_t now = time(nullptr);
    for (int i = 0; i < visible && timerList.scroll + i < (int)st.timers.size(); i++) {
        int idx = timerList.scroll + i;
        const Timer &t = st.timers[idx];
        const Craft *c = craftById(t.craftId);
        int y = kTop + i * rowH;
        bool on = idx == timerList.sel;
        ui.fill(16, y, 1240, rowH - 6, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 6, col::accent);
        std::string name = c ? craftTitle(*c) : t.craftId;
        const Station *s = c ? data.station(c->stationId) : nullptr;
        ui.icon(c && !c->rew.empty() ? data.icon(c->rew[0].itemId) : -1, 28, y + 4, 58);
        ui.text(100, y + 6, name, FS_BODY, col::text, 760);
        if (s)
            ui.text(100, y + 38, s->name, FS_SMALL, col::dim, 300);
        int64_t left = t.endTime - now;
        ui.textRight(1240, y + 8, left <= 0 ? "READY" : formatDuration(left), FS_TITLE,
                     left <= 0 ? col::good : col::accent);
        if (c && c->duration > 0)
            ui.progress(420, y + 54, 820, 6, 1.f - float(left) / c->duration, left <= 0 ? col::good : col::accent);
    }
    drawFooter({{"A", "Collect"}, {"X", "Cancel"}});
}

void App::drawTiles(const std::vector<SDL_Texture *> &tiles, int imgW, int imgH, float x, float y, float w, float h,
                    int top, int bottom) {
    // Tiles share rounded edges so no seams open between them.
    int cols = (imgW + kMapTile - 1) / kMapTile;
    float sx = w / imgW, sy = h / imgH;
    for (int i = 0; i < (int)tiles.size(); i++) {
        int px = i % cols * kMapTile, py = i / cols * kMapTile;
        int pw = std::min(kMapTile, imgW - px), ph = std::min(kMapTile, imgH - py);
        float x0 = std::round(x + px * sx), x1 = std::round(x + (px + pw) * sx);
        float y0 = std::round(y + py * sy), y1 = std::round(y + (py + ph) * sy);
        if (x1 < 0 || x0 > 1280 || y1 < top || y0 > bottom)
            continue;
        SDL_FRect dst{x0, y0, x1 - x0, y1 - y0};
        ui.image(tiles[i], nullptr, &dst);
    }
}

void App::drawMapPicker() {
    int n = maps.size();
    const int gap = 12, cw = (1248 - gap * (kPickCols - 1)) / kPickCols, thumbH = 96, ch = 140;
    int rows = (n + kPickCols - 1) / kPickCols;
    int visible = std::max(1, (kBottom - kTop + 8 + gap) / (ch + gap));
    int firstRow = std::clamp(pickSel / kPickCols - visible + 1, 0, std::max(0, rows - visible));
    for (int i = firstRow * kPickCols; i < n && i < (firstRow + visible) * kPickCols; i++) {
        const MapInfo &m = maps[i];
        int col = i % kPickCols, row = i / kPickCols - firstRow;
        int x = 16 + col * (cw + gap), y = kTop - 8 + row * (ch + gap);
        bool on = i == pickSel;
        ui.fill(x, y, cw, ch, on ? col::panelHi : col::panel);
        auto it = mapThumbs.find(m.thumb);
        if (it == mapThumbs.end() && !m.thumb.empty()) {
            std::string path = mapFile(m, m.thumb, true);
            it = mapThumbs.emplace(m.thumb, path.empty() ? nullptr : ui.loadImage(path)).first;
        }
        if (it != mapThumbs.end() && it->second) {
            SDL_FRect dst{float(x), float(y), float(cw), float(thumbH)};
            ui.image(it->second, nullptr, &dst);
        } else {
            ui.fill(x, y, cw, thumbH, col::cell);
        }
        if (on)
            ui.rect(x, y, cw, ch, col::accent);
        ui.fill(x, y + thumbH, cw, 2, on ? col::accent : col::line);
        ui.text(x + 12, y + thumbH + 6, m.name, FS_SMALL, on ? col::text : col::dim, cw - 24);
        std::string sub = m.duration ? std::to_string(m.duration) + " min" : "";
        if (!m.floors.empty())
            sub += (sub.empty() ? "" : "  ") + std::to_string(m.floors.size()) + " levels";
        ui.textRight(x + cw - 12, y + thumbH + 8, sub, FS_SMALL, col::faint);
    }
    drawScrollbar(1268, kTop - 8, kBottom - kTop + 8, rows, visible, firstRow);
    drawFooter({{"A", "Open"}, {"+", "Quit"}});
}

void App::drawMaps() {
    const MapInfo *m = currentMap();
    if (!m) {
        drawEmpty("No map data yet.", "Connect to the internet and use Settings > Download latest hideout data.");
        drawFooter({{"+", "Quit"}});
        return;
    }
    if (!mapPicked) {
        drawMapPicker();
        return;
    }
    int top, bottom;
    mapViewport(top, bottom);
    float viewW = 1280, viewH = bottom - top;
    float fit = std::min(viewW / m->w, viewH / m->h);
    float scale = fit * mapZoom;
    float dw = m->w * scale, dh = m->h * scale;
    // Keep the view inside the image when zoomed in, centered when it fits.
    float ox = dw <= viewW ? (viewW - dw) / 2 : std::clamp(viewW / 2 - mapU * dw, viewW - dw, 0.f);
    float oy = dh <= viewH ? (viewH - dh) / 2 : std::clamp(viewH / 2 - mapV * dh, viewH - dh, 0.f);
    oy += top;

    SDL_Rect clipR{0, top, 1280, bottom - top};
    ui.clip(&clipR);
    ui.fill(0, top, 1280, bottom - top, col::bg);
    if (ensureMapTexture()) {
        drawTiles(mapTiles, mapImgW, mapImgH, ox, oy, dw, dh, top, bottom);
        if (ensureFloorTexture()) {
            // Floor images carry the dimmed base inside their rect; dim the rest to match.
            const MapFloor &f = m->floors[mapFloor];
            const float *r = mapPlan && !f.alt.empty() ? f.ar : f.r;
            ui.fill(int(ox), int(oy), int(dw) + 1, int(dh) + 1, SDL_Color{0, 0, 0, 140});
            drawTiles(floorTiles, floorImgW, floorImgH, ox + r[0] * dw, oy + r[1] * dh, (r[2] - r[0]) * dw,
                      (r[3] - r[1]) * dh, top, bottom);
        }
    } else {
        ui.text(40, top + 80, "Map image unavailable offline. Connect to download it once.", FS_BODY, col::dim);
    }

    auto vis = visibleMarkers();
    int selIdx = markerSel >= 0 && markerSel < (int)vis.size() ? vis[markerSel] : -1;
    float r = std::clamp(4.f + mapZoom * 1.5f, 6.f, 12.f);
    for (int pass = 0; pass < 2; pass++) {
        for (int i : vis) {
            const MapMarker &mk = m->markers[i];
            bool sel = i == selIdx;
            if ((pass == 1) != sel)
                continue;
            float x = ox + mk.u * dw, y = oy + mk.v * dh;
            float rr = sel ? r + 4 : r;
            ui.disc(x, y, rr + 2.5f, col::shadow);
            ui.disc(x, y, rr, kMarkerColor[mk.type]);
            if (mk.type == MK_BOSS)
                ui.disc(x, y, rr * 0.4f, col::bg);
            if (sel || (mapLabels && mapZoom >= 1.8f) || (mapLabels && markerFilter >= 0)) {
                FontSize fs = sel ? FS_BODY : FS_SMALL;
                int tw = ui.measure(mk.name, fs);
                int tx = int(x + rr + 6), ty = int(y - ui.lineHeight(fs) / 2);
                ui.fill(tx - 4, ty, tw + 8, ui.lineHeight(fs), col::shadow);
                ui.text(tx, ty, mk.name, fs, sel ? col::text : kMarkerColor[mk.type]);
            }
        }
    }

    // Quest or key pins on top of everything.
    int shownPins = 0;
    for (const Pin &p : pins) {
        if (p.map != m->id)
            continue;
        float x = ox + p.u * dw, y = oy + p.v * dh;
        ui.disc(x, y, 15, col::shadow);
        ui.disc(x, y, 12, col::text);
        ui.disc(x, y, 7, col::warn);
        if (shownPins++ == 0 && !pinLabel.empty()) {
            int tw = ui.measure(pinLabel, FS_BODY);
            ui.fill(int(x) + 18, int(y) - 16, tw + 12, 32, col::shadow);
            ui.text(int(x) + 24, int(y) - 14, pinLabel, FS_BODY, col::text);
        }
    }

    // Floor list, top floor first, on the right edge.
    if (!m->floors.empty()) {
        int n = m->floors.size(), rowH = 34, w = 0;
        for (const auto &f : m->floors)
            w = std::max(w, ui.measure(f.name, FS_SMALL));
        w += 28;
        int x = 1264 - w, y = top + 12;
        ui.fill(x, y, w, n * rowH + 8, SDL_Color{14, 15, 13, 215});
        for (int i = n - 1; i >= 0; i--) {
            int ry = y + 4 + (n - 1 - i) * rowH;
            bool on = i == mapFloor;
            if (on)
                ui.fill(x + 4, ry, w - 8, rowH, col::panelHi);
            ui.fill(x + 4, ry, 3, rowH, on ? col::accent : (i == m->baseFloor ? col::faint : SDL_Color{0, 0, 0, 0}));
            ui.text(x + 16, ry + 4, m->floors[i].name, FS_SMALL, on ? col::text : col::dim);
        }
    }

    if (mapFull) {
        // Just a small name chip so the map gets the whole screen.
        std::string t = m->name + (m->floors.empty() ? "" : "  " + m->floors[mapFloor].name) + "   - Exit fullscreen";
        int tw = ui.measure(t, FS_SMALL);
        ui.fill(12, 12, tw + 20, 32, SDL_Color{14, 15, 13, 200});
        ui.text(22, 16, t, FS_SMALL, col::dim);
        ui.clip(nullptr);
        return;
    }

    // Title card and legend.
    ui.fill(16, kMapTop + 12, 360, 70, SDL_Color{14, 15, 13, 215});
    ui.fill(16, kMapTop + 12, 4, 70, col::accent);
    ui.text(32, kMapTop + 16, m->name, FS_TITLE, col::text, 330);
    std::string sub = std::to_string(mapSel % maps.size() + 1) + "/" + std::to_string(maps.size());
    if (m->duration)
        sub += "   " + std::to_string(m->duration) + " min";
    if (!m->players.empty())
        sub += "   " + m->players + " players";
    ui.text(32, kMapTop + 52, sub, FS_SMALL, col::dim);

    int counts[MK_COUNT] = {};
    for (const auto &mk : m->markers)
        counts[mk.type]++;
    int ly = kMapBottom - 12 - MK_COUNT * 28 - 8;
    ui.fill(16, ly, 250, MK_COUNT * 28 + 12, SDL_Color{14, 15, 13, 215});
    for (int t = 0; t < MK_COUNT; t++) {
        bool on = markerFilter < 0 || markerFilter == t;
        int y = ly + 8 + t * 28;
        ui.disc(34, y + 12, 7, on ? kMarkerColor[t] : col::faint);
        ui.text(52, y, std::string(kMarkerName[t]) + "  " + std::to_string(counts[t]), FS_SMALL,
                on ? col::text : col::faint);
    }
    ui.clip(nullptr);

    std::vector<std::pair<std::string, std::string>> hints = {{"B", pinReturn ? "Back" : "Maps"}};
    if (!m->floors.empty())
        hints.push_back({"ZL/ZR", "Floor"});
    if (!pinReturn)
        hints.insert(hints.end(), {{"</>", "Marker"}, {"^/v", "Filter"}});
    hints.insert(hints.end(), {{"A/X", "Zoom"}, {"Y", "Labels"}});
    if (!m->alt.empty())
        hints.push_back({"R3", mapPlan ? "Render" : "Plan"});
    hints.push_back({"-", "Full"});
    drawFooter(hints);
}


static SDL_Color penColor(int pen) {
    if (pen >= 60)
        return col::good;
    if (pen >= 50)
        return SDL_Color{160, 196, 96, 255};
    if (pen >= 40)
        return col::accent;
    if (pen >= 30)
        return col::warn;
    if (pen >= 20)
        return SDL_Color{214, 120, 72, 255};
    return col::bad;
}

void App::drawDbHeader() {
    int x = 24;
    for (int i = 0; i < DB_COUNT; i++) {
        bool on = i == dbSection;
        int w = ui.measure(kDbNames[i], FS_BODY) + 28;
        ui.fill(x, kTop - 8, w, 36, on ? col::accent : col::panel);
        ui.text(x + 14, kTop - 6, kDbNames[i], FS_BODY, on ? col::bg : col::dim);
        x += w + 6;
    }
    ui.hint(x + 6, kTop - 3, "ZL/ZR", "");
    x += 90;
    int nf = filterCount(dbSection, db);
    if (nf > 0) {
        std::string f = dbFilter == 0 ? (dbSection == DB_QUESTS ? "All traders" : "All calibers")
                                       : (dbSection == DB_QUESTS ? db.traders : db.calibers)[dbFilter - 1];
        ui.text(x, kTop - 4, "<  " + f + "  >", FS_BODY, col::accent, 300);
    }
    if (!dbSearch.empty())
        drawBadge(1256 - ui.measure("Search: " + dbSearch, FS_SMALL) - 14, kTop - 2, "Search: " + dbSearch, col::info);
}

void App::drawQuestList() {
    const int rowH = 58, top = kTop + 40;
    int visible = (kBottom - top) / rowH;
    dbList.clamp(dbRows.size(), visible);
    if (dbRows.empty())
        ui.text(28, top + 10, "No quests match.", FS_BODY, col::dim);
    for (int i = 0; i < visible && dbList.scroll + i < (int)dbRows.size(); i++) {
        int idx = dbList.scroll + i;
        const Quest &q = db.quests[dbRows[idx]];
        int y = top + i * rowH;
        bool on = idx == dbList.sel;
        bool done = st.tasksDone.count(q.id) > 0;
        ui.fill(16, y, 1240, rowH - 6, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 6, col::accent);
        ui.text(32, y + 2, q.name, FS_BODY, done ? col::dim : col::text, 640);
        std::string sub = q.trader + (q.level > 1 ? "   Level " + std::to_string(q.level) : "") +
                          (q.map.empty() ? "" : "   " + q.map);
        ui.text(32, y + 28, sub, FS_SMALL, col::dim, 640);
        int bx = 1240;
        auto badge = [&](const std::string &t, SDL_Color c) {
            int w = ui.measure(t, FS_SMALL) + 14;
            bx -= w + 8;
            drawBadge(bx, y + 14, t, c);
        };
        if (done)
            badge("DONE", col::good);
        if (q.kappa)
            badge("Kappa", col::accent);
        if (q.lightkeeper)
            badge("Lightkeeper", col::violet);
        bool pinned = false;
        for (const auto &o : q.objectives)
            pinned |= !o.pins.empty();
        if (pinned)
            badge("Map", col::warn);
    }
    drawScrollbar(1262, top, visible * rowH, dbRows.size(), visible, dbList.scroll);
    drawFooter({{"A", "Open"}, {"Y", "Search"}, {"</>", "Trader"}, {"ZL/ZR", "Section"}});
}

void App::drawAmmo() {
    const int rowH = 46, top = kTop + 76;
    static const char *cols[] = {"Damage", "Pen", "Armor dmg", "Frag", "Speed", "Flea"};
    static const int colX[] = {640, 740, 860, 960, 1060, 1240};
    ui.fill(16, kTop + 38, 1248, 34, col::panel);
    ui.text(84, kTop + 42, "Round", FS_SMALL, col::dim);
    for (int c = 0; c < 6; c++)
        ui.textRight(colX[c], kTop + 42, cols[c], FS_SMALL, col::dim);
    int visible = (kBottom - top) / rowH;
    dbList.clamp(dbRows.size(), visible);
    std::string lastCal;
    for (int i = 0; i < visible && dbList.scroll + i < (int)dbRows.size(); i++) {
        int idx = dbList.scroll + i;
        const Ammo &a = db.ammo[dbRows[idx]];
        int y = top + i * rowH;
        bool on = idx == dbList.sel;
        if (on)
            ui.fill(16, y, 1248, rowH - 4, col::panelHi);
        ui.icon(itemIcon(a.item), 28, y + 2, 38);
        std::string name = itemName(a.item);
        int w = ui.text(84, y + 8, name, FS_SMALL, col::text, 400);
        ui.text(84 + w + 10, y + 8, a.caliber, FS_SMALL, col::faint, 120);
        std::string dmg = a.projectiles > 1 ? std::to_string(a.projectiles) + "x" + std::to_string(a.damage)
                                            : std::to_string(a.damage);
        ui.textRight(colX[0], y + 8, dmg, FS_SMALL, col::text);
        ui.fill(colX[1] - 52, y + 6, 54, 28, SDL_Color{penColor(a.pen).r, penColor(a.pen).g, penColor(a.pen).b, 60});
        ui.textRight(colX[1], y + 8, std::to_string(a.pen), FS_SMALL, penColor(a.pen));
        ui.textRight(colX[2], y + 8, std::to_string(a.armorDamage), FS_SMALL, col::text);
        ui.textRight(colX[3], y + 8, std::to_string(a.frag) + "%", FS_SMALL, col::text);
        ui.textRight(colX[4], y + 8, std::to_string(a.speed), FS_SMALL, col::dim);
        const DbItem *it = db.item(a.item);
        std::string price = it && it->price ? groupDigits(it->price) : (it && it->noFlea ? "no flea" : "-");
        ui.textRight(colX[5], y + 8, price, FS_SMALL, col::accent);
    }
    drawScrollbar(1266, top, visible * rowH, dbRows.size(), visible, dbList.scroll);
    drawFooter({{"</>", "Caliber"}, {"Y", "Search"}, {"ZL/ZR", "Section"}});
}

void App::drawItemDetail(const std::string &id, int x, int y, int w) {
    const DbItem *it = db.item(id);
    if (!it)
        return;
    ui.fill(x, y, w, kBottom - y, col::panel);
    ui.icon(itemIcon(id), x + 16, y + 16, 96);
    ui.text(x + 128, y + 14, it->name, FS_BODY, col::text, w - 144);
    ui.text(x + 128, y + 46, it->category, FS_SMALL, col::dim, w - 144);
    int yy = y + 76;
    if (it->price)
        ui.text(x + 128, yy, "Flea  " + groupDigits(it->price) + " RUB", FS_SMALL, col::accent);
    else if (it->noFlea)
        ui.text(x + 128, yy, "Can't be sold on the flea market", FS_SMALL, col::warn);
    yy = y + 124;
    if (!it->trader.empty()) {
        ui.text(x + 16, yy, "Best trader: " + it->trader + "  " + groupDigits(it->traderPrice) + " RUB", FS_SMALL, col::text);
        yy += 28;
    }
    if (it->weight > 0) {
        char buf[32];
        snprintf(buf, sizeof buf, "Weight %.2f kg", it->weight);
        ui.text(x + 16, yy, buf, FS_SMALL, col::dim);
        yy += 28;
    }
    yy += 6;
    for (const auto &l : ui.wrap(it->desc, FS_SMALL, w - 32)) {
        if (yy > kBottom - 140)
            break;
        ui.text(x + 16, yy, l, FS_SMALL, col::dim);
        yy += 26;
    }
    if (usedForId != id) {
        usedForId = id;
        usedForCache = usedFor(id);
    }
    if (!usedForCache.empty()) {
        yy += 8;
        ui.text(x + 16, yy, "Needed for", FS_SMALL, col::accent);
        yy += 28;
        for (const auto &u : usedForCache) {
            if (yy > kBottom - 30) {
                ui.text(x + 16, yy, "...", FS_SMALL, col::dim);
                break;
            }
            ui.text(x + 16, yy, u, FS_SMALL, col::text, w - 32);
            yy += 26;
        }
    }
}

void App::drawKeys() {
    const int rowH = 48, top = kTop + 40;
    int visible = (kBottom - top) / rowH;
    dbList.clamp(dbRows.size(), visible);
    for (int i = 0; i < visible && dbList.scroll + i < (int)dbRows.size(); i++) {
        int idx = dbList.scroll + i;
        const KeyInfo &k = db.keys[dbRows[idx]];
        int y = top + i * rowH;
        bool on = idx == dbList.sel;
        ui.fill(16, y, 600, rowH - 4, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 4, col::accent);
        ui.icon(itemIcon(k.item), 26, y + 2, 40);
        ui.text(76, y + 8, itemName(k.item), FS_SMALL, col::text, 400);
        if (!k.pins.empty())
            drawBadge(490, y + 10, "Map", col::warn);
        if (!k.quests.empty())
            drawBadge(548, y + 10, "Quest", col::accent);
    }
    drawScrollbar(620, top, visible * rowH, dbRows.size(), visible, dbList.scroll);
    if (!dbRows.empty()) {
        const KeyInfo &k = db.keys[dbRows[dbList.sel]];
        drawItemDetail(k.item, 640, top, 624);
        int y = kBottom - 30 - 26 * std::min<int>(k.quests.size(), 3);
        if (k.uses)
            ui.textRight(1248, top + 124, std::to_string(k.uses) + " uses", FS_SMALL, col::dim);
        (void)y;
    }
    drawFooter({{"A", "Show lock"}, {"Y", "Search"}, {"</>", "Page"}, {"ZL/ZR", "Section"}});
}

void App::drawDbItems() {
    const int rowH = 44, top = kTop + 40;
    int visible = (kBottom - top) / rowH;
    dbList.clamp(dbItemRows.size(), visible);
    if (dbItemRows.empty())
        ui.text(28, top + 10, "No items match.", FS_BODY, col::dim);
    for (int i = 0; i < visible && dbList.scroll + i < (int)dbItemRows.size(); i++) {
        int idx = dbList.scroll + i;
        const std::string &id = dbItemRows[idx];
        const DbItem &it = db.items.at(id);
        int y = top + i * rowH;
        bool on = idx == dbList.sel;
        ui.fill(16, y, 600, rowH - 4, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, rowH - 4, col::accent);
        ui.icon(itemIcon(id), 26, y + 2, 36);
        ui.text(72, y + 6, it.name, FS_SMALL, col::text, 400);
        if (it.price)
            ui.textRight(604, y + 6, groupDigits(it.price), FS_SMALL, col::accent);
    }
    drawScrollbar(620, top, visible * rowH, dbItemRows.size(), visible, dbList.scroll);
    if (!dbItemRows.empty())
        drawItemDetail(dbItemRows[dbList.sel], 640, top, 624);
    drawFooter({{"Y", "Search"}, {"X", "Clear"}, {"</>", "Page"}, {"ZL/ZR", "Section"}});
}

void App::drawQuest() {
    const Quest &q = db.quests[openQuest];
    bool done = st.tasksDone.count(q.id) > 0;
    // Title strip.
    ui.text(24, kTop - 10, q.name, FS_TITLE, col::text, 760);
    std::string sub = q.trader + (q.level > 1 ? "   Level " + std::to_string(q.level) : "") +
                      (q.map.empty() ? "" : "   " + q.map) +
                      (q.faction.empty() ? "" : "   " + q.faction + " only");
    ui.text(24, kTop + 26, sub, FS_SMALL, col::dim, 760);
    int bx = 1256;
    auto badge = [&](const std::string &t, SDL_Color c) {
        int w = ui.measure(t, FS_SMALL) + 14;
        bx -= w + 8;
        drawBadge(bx, kTop + 4, t, c);
    };
    if (done)
        badge("DONE", col::good);
    if (q.kappa)
        badge("Kappa", col::accent);
    if (q.lightkeeper)
        badge("Lightkeeper", col::violet);

    int top = kTop + 60;
    int y = top;
    // Left (or full width): wiki guide.
    if (questView != 2) {
        int gw = questView == 1 ? 1248 : 608;
        ui.fill(16, top, gw, kBottom - top, col::panel);
        int lines = (kBottom - top - 50) / 26;
        y = top + 12;
        for (int i = 0; i < lines && textScroll + i < (int)questText.size(); i++) {
            const std::string &l = questText[textScroll + i];
            if (l.rfind("## ", 0) == 0)
                ui.text(32, y, l.substr(3), FS_SMALL, col::accent);
            else if (l.rfind("### ", 0) == 0)
                ui.text(32, y, l.substr(4), FS_SMALL, col::warn);
            else
                ui.text(32, y, l, FS_SMALL, col::text);
            y += 26;
        }
        drawScrollbar(16 + gw - 8, top + 8, lines * 26, questText.size(), lines, textScroll);
        ui.text(32, kBottom - 30, "Guide: Escape from Tarkov Wiki, CC BY-SA", FS_SMALL, col::faint);
    }

    // Right (or full width): structured details.
    static const char *kViewNames[3] = {"Guide + details", "Guide only", "Details only"};
    if (questView == 1) {
        drawFooter({{"B", "Back"}, {"Y", kViewNames[2]}, {"^/v", "Scroll"}, {"ZL/ZR", "Page"}});
        return;
    }
    int rx = questView == 2 ? 16 : 640, rw = questView == 2 ? 1248 : 624;
    ui.fill(rx, top, rw, kBottom - top, col::panel);
    SDL_Rect clipR{rx, top, rw, kBottom - top};
    // Keep the selected row in view.
    int ySel = 0, total = 0;
    for (int i = 0; i < (int)qrows.size(); i++) {
        if (i == qrowSel)
            ySel = total;
        total += qrows[i].h;
    }
    int viewH = kBottom - top - 16;
    if (qrowSel >= 0) {
        int h = qrows[qrowSel].h;
        if (ySel < qrowScroll)
            qrowScroll = ySel;
        if (ySel + h > qrowScroll + viewH)
            qrowScroll = ySel + h - viewH;
    }
    qrowScroll = std::clamp(qrowScroll, 0, std::max(0, total - viewH));
    ui.clip(&clipR);
    y = top + 8 - qrowScroll;
    for (int i = 0; i < (int)qrows.size(); i++) {
        const QRow &r = qrows[i];
        if (y > kBottom || y + r.h < top) {
            y += r.h;
            continue;
        }
        bool on = i == qrowSel;
        switch (r.kind) {
        case QRow::HEAD:
            ui.text(rx + 16, y + 8, r.text, FS_BODY, col::accent);
            ui.fill(rx + 16, y + 38, rw - 32, 1, col::line);
            break;
        case QRow::TEXT:
            for (size_t k = 0; k < r.lines.size(); k++)
                ui.text(rx + 24, y + 2 + k * 26, r.lines[k], FS_SMALL, r.color);
            break;
        case QRow::ITEM: {
            if (on)
                ui.fill(rx + 8, y, rw - 16, r.h - 2, col::panelHi);
            ui.icon(itemIcon(r.item), rx + 24, y + 2, 34);
            std::string t = itemName(r.item) + (r.count > 1 ? "  x" + groupDigits(r.count) : "");
            int w = ui.text(rx + 68, y + 6, t, FS_SMALL, col::text, rw - 244);
            if (!r.text.empty())
                ui.text(rx + 80 + w, y + 6, r.text, FS_SMALL, col::dim, rw - 100 - w - 60);
            break;
        }
        case QRow::OBJ: {
            const Objective &o = q.objectives[r.obj];
            if (on) {
                ui.fill(rx + 8, y, rw - 16, r.h - 4, col::panelHi);
                ui.fill(rx + 8, y, 4, r.h - 4, col::accent);
            }
            ui.disc(rx + 30, y + 15, 6, o.optional ? col::faint : col::accent);
            int ly = y + 2;
            for (const auto &l : r.lines) {
                ui.text(rx + 46, ly, l, FS_SMALL, o.optional ? col::dim : col::text);
                ly += 26;
            }
            int bx2 = rx + rw - 16;
            if (!o.pins.empty()) {
                bx2 -= ui.measure("Map", FS_SMALL) + 22;
                drawBadge(bx2, y + 4, "Map", col::warn);
            }
            if (o.fir) {
                bx2 -= ui.measure("FIR", FS_SMALL) + 22;
                drawBadge(bx2, y + 4, "FIR", col::warn);
            }
            if (o.count > 1) {
                std::string c = "x" + groupDigits(o.count);
                bx2 -= ui.measure(c, FS_SMALL) + 12;
                ui.text(bx2, y + 4, c, FS_SMALL, col::accent);
            }
            ly += 6;
            if (!o.items.empty()) {
                int ix = rx + 46;
                for (const auto &id : o.items) {
                    if (ix > rx + rw - 60)
                        break;
                    ui.icon(itemIcon(id), ix, ly, 38);
                    ix += 42;
                }
                if (o.more)
                    ui.text(ix + 6, ly + 8, "+" + std::to_string(o.more) + " more", FS_SMALL, col::dim);
                else if (o.items.size() == 1)
                    ui.text(ix + 6, ly + 8, itemName(o.items[0]), FS_SMALL, col::dim, rw - (ix - rx) - 30);
                ly += 44;
            }
            if (o.hasBuild) {
                for (const auto &a : o.build.attrs) {
                    ui.text(rx + 46, ly, a, FS_SMALL, col::info);
                    ly += 26;
                }
                if (!o.build.categories.empty()) {
                    std::string c = "Must include: ";
                    for (size_t k = 0; k < o.build.categories.size(); k++)
                        c += (k ? ", " : "") + o.build.categories[k];
                    ui.text(rx + 46, ly, c, FS_SMALL, col::info, rw - 70);
                    ly += 26;
                }
                if (!o.build.parts.empty()) {
                    int ix = rx + 46;
                    for (const auto &id : o.build.parts) {
                        if (ix > rx + rw - 60)
                            break;
                        ui.icon(itemIcon(id), ix, ly + 2, 38);
                        ix += 42;
                    }
                }
            }
            break;
        }
        }
        y += r.h;
    }
    ui.clip(nullptr);
    drawScrollbar(rx + rw - 6, top + 8, viewH, total, viewH, qrowScroll);

    bool hasPins = qrowSel >= 0 && qrows[qrowSel].kind == QRow::OBJ && !q.objectives[qrows[qrowSel].obj].pins.empty();
    bool onItem = qrowSel >= 0 && qrows[qrowSel].kind == QRow::ITEM;
    std::vector<std::pair<std::string, std::string>> hints = {{"B", "Back"}, {"Y", kViewNames[(questView + 1) % 3]}};
    if (hasPins)
        hints.push_back({"A", "Show on map"});
    if (onItem)
        hints.push_back({"A", "Item info"});
    if (questView == 0)
        hints.push_back({"R-stick", "Scroll guide"});
    drawFooter(hints);
}

void App::drawDatabase() {
    ensureDb();
    if (db.empty()) {
        drawEmpty("No database yet.", "Connect to the internet and use Settings > Download latest data.");
        drawFooter({{"+", "Quit"}});
        return;
    }
    if (openQuest >= 0) {
        drawQuest();
        return;
    }
    if (dbDirty)
        filterDb();
    drawDbHeader();
    switch (dbSection) {
    case DB_QUESTS: drawQuestList(); break;
    case DB_AMMO: drawAmmo(); break;
    case DB_KEYS: drawKeys(); break;
    case DB_ITEMS: drawDbItems(); break;
    }
}

void App::drawProfile() {
    if (st.profileId.empty()) {
        ui.text(40, kTop + 10, "Show your Tarkov profile", FS_TITLE, col::text);
        const char *lines[] = {
            "1. On your phone or PC, open tarkov.dev/players and search for your in-game name.",
            "2. Open your profile. The link ends with a number, for example tarkov.dev/players/pve/8560316.",
            "3. Press A here and type that number.",
            "",
            "tarkov.dev shows a public snapshot of your profile: level, raid stats and skills.",
            "The snapshot refreshes when someone looks the profile up on tarkov.dev, so it can be a few days old.",
        };
        int y = kTop + 64;
        for (const char *l : lines) {
            ui.text(40, y, l, FS_BODY, *l >= '1' && *l <= '3' ? col::text : col::dim, 1200);
            y += 40;
        }
        drawFooter({{"A", "Enter profile ID"}});
        return;
    }
    if (profile.empty()) {
        drawEmpty("Profile " + st.profileId + " not loaded yet.", "Press A to load it from tarkov.dev (needs internet).");
        drawFooter({{"A", "Load"}, {"X", "Change ID"}});
        return;
    }
    // Identity card.
    ui.fill(16, kTop, 1248, 96, col::panel);
    ui.fill(16, kTop, 4, 96, col::accent);
    int level = data.playerLevel(profile.exp);
    std::string lv = std::to_string(level);
    ui.fill(36, kTop + 14, 68, 68, col::cell);
    ui.rect(36, kTop + 14, 68, 68, col::accent);
    ui.text(70 - ui.measure(lv, FS_TITLE) / 2, kTop + 30, lv, FS_TITLE, col::accent);
    ui.text(124, kTop + 12, profile.nickname, FS_TITLE, col::text, 560);
    std::string sub = (profile.side.empty() ? "" : profile.side + "   ") + "Level " + lv + "   " +
                      groupDigits(profile.exp) + " exp";
    if (profile.prestige > 0)
        sub += "   Prestige " + std::to_string(profile.prestige);
    ui.text(124, kTop + 54, sub, FS_SMALL, col::dim, 700);
    // Progress to the next level.
    int64_t total = 0, prev = 0;
    for (int i = 0; i < (int)data.levelExp.size() && i < level; i++)
        total += data.levelExp[i];
    prev = total;
    if (level < (int)data.levelExp.size())
        total += data.levelExp[level];
    if (total > prev) {
        ui.progress(860, kTop + 60, 380, 8, float(profile.exp - prev) / (total - prev), col::accent);
        ui.textRight(1240, kTop + 26, groupDigits(total - profile.exp) + " exp to level " + std::to_string(level + 1),
                     FS_SMALL, col::dim);
    }

    auto statCol = [&](int x, const char *title, const std::vector<ProfileStat> &stats) {
        int y = kTop + 112;
        ui.fill(x, y, 400, kBottom - y, col::panel);
        ui.text(x + 16, y + 10, title, FS_BODY, col::accent);
        y += 50;
        for (const auto &s : stats) {
            ui.text(x + 16, y, s.label, FS_SMALL, col::dim);
            ui.textRight(x + 384, y, s.value, FS_SMALL, col::text);
            y += 32;
            if (y > kBottom - 30)
                break;
        }
    };
    statCol(16, "PMC", profile.pmc);
    statCol(432, "Scav", profile.scav);

    int x = 848, y = kTop + 112;
    ui.fill(x, y, 416, kBottom - y, col::panel);
    ui.text(x + 16, y + 10, "Skills", FS_BODY, col::accent);
    y += 50;
    int rows = 12;
    skillScroll = std::clamp(skillScroll, 0, std::max(0, (int)profile.skills.size() - rows));
    for (int i = 0; i < rows && skillScroll + i < (int)profile.skills.size(); i++) {
        const ProfileSkill &s = profile.skills[skillScroll + i];
        char lvbuf[16];
        snprintf(lvbuf, sizeof lvbuf, "%d", int(s.level));
        ui.text(x + 16, y, s.name, FS_SMALL, col::text, 250);
        ui.textRight(x + 400, y, lvbuf, FS_SMALL, s.level >= 51 ? col::good : col::accent);
        ui.progress(x + 270, y + 12, 100, 4, s.level / 51.f, s.level >= 51 ? col::good : col::accent);
        y += 30;
    }
    if (profile.skills.empty())
        ui.text(x + 16, y, "No skill progress yet", FS_SMALL, col::dim);
    drawScrollbar(x + 408, kTop + 162, rows * 30, profile.skills.size(), rows, skillScroll);

    std::string snap = profile.updated > 0 ? "Snapshot " + dateString(profile.updated) : "";
    drawFooter({{"A", "Refresh" + (snap.empty() ? "" : " (" + snap + ")")}, {"X", "Change ID"}});
}

void App::drawSettings() {
    std::string tok = st.token.empty() ? "not set" : "..." + st.token.substr(st.token.size() > 4 ? st.token.size() - 4 : 0);
    struct Row {
        std::string label, value;
    };
    Row rows[SET_COUNT] = {
        {"TarkovTracker token", tok},
        {"Sync from TarkovTracker now", st.lastSync.empty() ? "never synced" : "last " + st.lastSync},
        {"Sync on launch", st.syncOnLaunch ? "On" : "Off"},
        {"tarkov.dev profile ID", st.profileId.empty() ? "not set" : st.profileId},
        {"Game mode", st.mode == "pve" ? "PVE" : "PVP"},
        {"Download latest data, icons and maps", data.empty() ? "no data" : data.generated},
        {confirmReset ? "Clear local progress? Press A again" : "Clear local progress and timers", ""},
    };
    settingsList.clamp(SET_COUNT, SET_COUNT);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = kTop + i * 52;
        bool on = i == settingsList.sel;
        ui.fill(16, y, 820, 46, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, 46, col::accent);
        ui.text(36, y + 10, rows[i].label, FS_BODY, i == SET_RESET && confirmReset ? col::warn : col::text, 500);
        ui.textRight(816, y + 13, rows[i].value, FS_SMALL, col::dim);
    }
    int x = 860, y = kTop;
    ui.fill(x, y, 404, kBottom - y, col::panel);
    ui.text(x + 16, y + 10, "About", FS_BODY, col::accent);
    y += 50;
    auto info = [&](const std::string &k, const std::string &v) {
        ui.text(x + 16, y, k, FS_SMALL, col::dim);
        ui.textRight(x + 388, y, v, FS_SMALL, col::text);
        y += 30;
    };
    info("Account", st.account.empty() ? "-" : st.account);
    info("Stations", std::to_string(data.stations.size()));
    info("Crafts", std::to_string(data.crafts.size()));
    info("Item icons", ui.hasIcons() ? std::to_string(data.icons.size()) : "none");
    info("Maps", std::to_string(maps.size()));
    info("Version", APP_VERSION);
    y += 16;
    const char *notes[] = {
        "Get a token on tarkovtracker.org:",
        "Settings > API tokens, with the",
        "Get Progression permission.",
        "Sync only reads from TarkovTracker.",
        "",
        "Game data, icons and maps come",
        "from tarkov.dev, refreshed daily.",
    };
    for (const char *n : notes) {
        ui.text(x + 16, y, n, FS_SMALL, col::dim, 372);
        y += 26;
    }
    drawFooter({{"A", "Select"}, {"+", "Quit"}});
}

void App::draw() {
    if (needDirty)
        rebuildNeeded();
    ui.clear();
    if (!(tab == TAB_MAPS && mapFull && !maps.empty()))
        drawHeader();
    switch (tab) {
    case TAB_STATIONS: drawStations(); break;
    case TAB_ITEMS: drawItems(); break;
    case TAB_CRAFTS: drawCrafts(); break;
    case TAB_TIMERS: drawTimers(); break;
    case TAB_MAPS: drawMaps(); break;
    case TAB_DATABASE: drawDatabase(); break;
    case TAB_PROFILE: drawProfile(); break;
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
