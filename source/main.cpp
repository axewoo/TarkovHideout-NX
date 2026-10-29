#include "platform.h"

#include <sys/stat.h>
#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model.h"
#include "net.h"
#include "ui.h"

#ifndef APP_DIR
#define APP_DIR "sdmc:/switch/TarkovHideout"
#endif
#define STATE_PATH APP_DIR "/state.json"
#define DATA_URL "https://raw.githubusercontent.com/axewoo/TarkovHideout-NX/data/"
#define TRACKER_API "https://api.tarkovtracker.org"
#define PROFILE_API "https://players.tarkov.dev/"

enum Tab { TAB_STATIONS, TAB_ITEMS, TAB_CRAFTS, TAB_TIMERS, TAB_MAPS, TAB_AMMO, TAB_DATABASE, TAB_KAPPA, TAB_PROFILE,
           TAB_SETTINGS, TAB_COUNT };
static const char *kTabNames[TAB_COUNT] = {"Stations", "Needed", "Crafts", "Timers", "Maps", "Ammo", "Database", "Kappa",
                                           "Profile", "Settings"};

enum DbSection { DB_QUESTS, DB_KEYS, DB_ITEMS, DB_COUNT };
static const char *kDbNames[DB_COUNT] = {"Quests", "Keys", "Items"};
static const int kMaxIconPages = 8;

static const int kTop = 68;
static const int kBottom = 664;
static const int kMapTop = 50;
static const int kMapBottom = 676;
static const int kMapTile = 2048;

// Looked up per draw so they follow the theme.
static const struct {
    SDL_Color operator[](int t) const {
        const SDL_Color c[MK_COUNT] = {col::good, col::info, col::accent, col::violet, col::bad};
        return c[t];
    }
} kMarkerColor;
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

// stat rather than dirent's d_type, which Windows does not have.
static bool isDir(const std::string &path) {
    struct stat sb;
    return stat(path.c_str(), &sb) == 0 && S_ISDIR(sb.st_mode);
}

// Creates every missing folder above a file path.
static void makeParents(const std::string &path) {
    for (size_t i = path.find('/', 1); i != std::string::npos; i = path.find('/', i + 1))
        mkdir(path.substr(0, i).c_str(), 0777);
}

// Fetches map pieces and guide pictures that the app does not bundle (Lite builds, Android) on a
// background thread, saving each under APP_DIR, so the screen keeps moving while they arrive.
// The newest request goes first, since it is what is on screen now.
class Fetcher {
public:
    enum State { NONE, PENDING, DONE, FAILED };

    void start() {
        lock = SDL_CreateMutex();
        wake = SDL_CreateCond();
        if (lock && wake)
            thread = SDL_CreateThreadWithStackSize(run, "fetch", 512 * 1024, this);
    }

    void stop() {
        if (!thread)
            return;
        SDL_LockMutex(lock);
        quit = true;
        SDL_CondSignal(wake);
        SDL_UnlockMutex(lock);
        SDL_WaitThread(thread, nullptr);
        thread = nullptr;
    }

    State state(const std::string &path) {
        SDL_LockMutex(lock);
        auto it = states.find(path);
        State st = it == states.end() ? NONE : it->second;
        SDL_UnlockMutex(lock);
        return st;
    }

    // Queues a download unless one is known; gives up right away after repeated network errors.
    State request(const std::string &path) {
        if (!thread)
            return FAILED;
        SDL_LockMutex(lock);
        State st;
        auto it = states.find(path);
        if (it != states.end()) {
            st = it->second;
        } else if (netFails >= 3) {
            st = FAILED;
        } else {
            st = states[path] = PENDING;
            todo.push_front(path);
            // Pieces that scrolled away long ago are dropped; they are asked for again if seen.
            while (todo.size() > 48) {
                states.erase(todo.back());
                todo.pop_back();
            }
            SDL_CondSignal(wake);
        }
        SDL_UnlockMutex(lock);
        return st;
    }

    // Forgets queued requests and what is known, for a new map or after the data was replaced.
    void reset() {
        if (!thread)
            return;
        SDL_LockMutex(lock);
        todo.clear();
        states.clear();
        netFails = 0;
        SDL_UnlockMutex(lock);
    }

private:
    static int run(void *self) {
        static_cast<Fetcher *>(self)->loop();
        netThreadExit();
        return 0;
    }

    void loop() {
        SDL_LockMutex(lock);
        while (!quit) {
            if (todo.empty()) {
                SDL_CondWait(wake, lock);
                continue;
            }
            std::string path = todo.front();
            todo.pop_front();
            SDL_UnlockMutex(lock);
            HttpResult r = httpGet(DATA_URL + path, {}, &quit);
            std::string file = std::string(APP_DIR "/") + path;
            bool ok = r.ok && r.status == 200;
            if (ok) {
                makeParents(file);
                ok = writeFile(file, r.body);
            }
            SDL_LockMutex(lock);
            // A reset while downloading forgot this path; the file is kept either way.
            auto it = states.find(path);
            if (it != states.end())
                it->second = ok ? DONE : FAILED;
            netFails = r.ok ? 0 : netFails + 1;
        }
        SDL_UnlockMutex(lock);
    }

    SDL_mutex *lock = nullptr;
    SDL_cond *wake = nullptr;
    SDL_Thread *thread = nullptr;
    std::atomic<bool> quit{false};
    std::deque<std::string> todo;
    std::map<std::string, State> states;
    int netFails = 0;
};

// Reads and decodes map pieces on a worker thread, so panning never waits on the SD card or the
// JPEG decoder; the main thread only uploads finished pictures to the GPU.
class Decoder {
public:
    enum State { NONE, QUEUED, READY, MISSING };

    void start(Uint32 format) {
        pixelFormat = format;
        lock = SDL_CreateMutex();
        wake = SDL_CreateCond();
        if (lock && wake)
            thread = SDL_CreateThreadWithStackSize(run, "decode", 256 * 1024, this);
    }

    void stop() {
        if (!thread)
            return;
        SDL_LockMutex(lock);
        quit = true;
        SDL_CondSignal(wake);
        SDL_UnlockMutex(lock);
        SDL_WaitThread(thread, nullptr);
        thread = nullptr;
        clear();
    }

    // Queues a piece; the newest requests are decoded first since they are what is on screen.
    State request(const std::string &path) {
        if (!thread)
            return MISSING;
        SDL_LockMutex(lock);
        State st;
        auto it = states.find(path);
        if (it != states.end()) {
            st = it->second;
        } else {
            st = states[path] = QUEUED;
            todo.push_front(path);
            while (todo.size() > 24) {
                states.erase(todo.back());
                todo.pop_back();
            }
            SDL_CondSignal(wake);
        }
        SDL_UnlockMutex(lock);
        return st;
    }

    // Takes one decoded piece, if any; the caller owns the surface.
    bool take(std::string &path, SDL_Surface *&surf) {
        if (!thread)
            return false;
        SDL_LockMutex(lock);
        bool any = !done.empty();
        if (any) {
            path = done.front().first;
            surf = done.front().second;
            done.pop_front();
            states.erase(path);
        }
        SDL_UnlockMutex(lock);
        return any;
    }

    // Forgets a piece reported MISSING, so it is looked for again (after a download).
    void forget(const std::string &path) {
        if (!thread)
            return;
        SDL_LockMutex(lock);
        states.erase(path);
        SDL_UnlockMutex(lock);
    }

    void clear() {
        if (!lock)
            return;
        SDL_LockMutex(lock);
        todo.clear();
        states.clear();
        for (auto &d : done)
            SDL_FreeSurface(d.second);
        done.clear();
        SDL_UnlockMutex(lock);
    }

private:
    static int run(void *self) {
        static_cast<Decoder *>(self)->loop();
        return 0;
    }

    void loop() {
        SDL_LockMutex(lock);
        while (!quit) {
            if (todo.empty()) {
                SDL_CondWait(wake, lock);
                continue;
            }
            std::string path = todo.front();
            todo.pop_front();
            SDL_UnlockMutex(lock);
            std::string file = ROMFS_ROOT + path;
            if (!fileExists(file))
                file = std::string(APP_DIR "/") + path;
            SDL_Surface *surf = nullptr;
            if (fileExists(file)) {
                if (SDL_Surface *raw = IMG_Load(file.c_str())) {
                    surf = SDL_ConvertSurfaceFormat(raw, pixelFormat, 0);
                    SDL_FreeSurface(raw);
                }
            }
            SDL_LockMutex(lock);
            auto it = states.find(path);
            if (it == states.end()) {
                // Cleared while decoding.
                if (surf)
                    SDL_FreeSurface(surf);
            } else if (surf) {
                it->second = READY;
                done.push_back({path, surf});
            } else {
                it->second = MISSING;
            }
        }
        SDL_UnlockMutex(lock);
    }

    SDL_mutex *lock = nullptr;
    SDL_cond *wake = nullptr;
    SDL_Thread *thread = nullptr;
    std::atomic<bool> quit{false};
    Uint32 pixelFormat = SDL_PIXELFORMAT_ARGB8888;
    std::deque<std::string> todo;
    std::deque<std::pair<std::string, SDL_Surface *>> done;
    std::map<std::string, State> states;
};

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

// Directional buttons with hold-to-repeat, plus sticks and touch. On the desktop the mouse is the
// touch screen, so a tap is a click.
struct Input {
    PadState pad;
    u64 down = 0, held = 0;
    u64 heldSince = 0, lastRepeat = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, y up
    bool touching = false;
    int touchX = 0, touchY = 0, touchDX = 0, touchDY = 0;
    // A touch that ends close to where it started is a tap; two quick taps in one spot count 2.
    int tapStartX = 0, tapStartY = 0, tapMoved = 0;
    bool tapped = false;
    int tapX = 0, tapY = 0, tapCount = 0;
    u64 lastTap = 0;
    float wheel = 0;  // desktop mouse wheel notches, up is positive

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
        tapped = false;
        if (touching) {
            int x = ts.touches[0].x, y = ts.touches[0].y;
            if (was) {
                touchDX = x - touchX;
                touchDY = y - touchY;
            } else {
                tapStartX = x;
                tapStartY = y;
                tapMoved = 0;
            }
            touchX = x;
            touchY = y;
            tapMoved = std::max(tapMoved, std::abs(x - tapStartX) + std::abs(y - tapStartY));
        } else if (was && !nxTouchCancelled() && tapMoved < 12) {
            bool again = now - lastTap < freq * 45 / 100 && std::abs(tapStartX - tapX) + std::abs(tapStartY - tapY) < 12;
            tapCount = again ? tapCount + 1 : 1;
            tapped = true;
            tapX = tapStartX;
            tapY = tapStartY;
            lastTap = now;
        }
        wheel = nxWheel();
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
    void downloadPictures();
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
    void inputAmmo();
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
    void drawAmmoChart(int x, int y, int w, int h);
    void drawAmmoDetail(const Ammo &a, int x, int y, int w);
    void filterAmmo();
    void drawKeys();
    void drawDbItems();
    void drawItemDetail(const std::string &id, int x, int y, int w);
    void drawQuest();
    void drawProfile();
    void drawSettings();
    void drawScrollbar(int x, int y, int h, int count, int visible, int scroll);
    int drawBadge(int x, int y, const std::string &s, SDL_Color c);

    // Kappa: the Collector quest's items
    void ensureKappa();
    std::vector<int> kappaRows() const;
    void inputKappa();
    void drawKappa();
    std::vector<std::string> kappaItems;  // item ids, in quest order
    std::string kappaQuestId;
    int kappaSel = 0, kappaScroll = 0;
    int kappaFilter = 0;  // 0 all, 1 still needed, 2 found

    // Theme picker (Settings)
    void inputThemePicker();
    void drawThemePicker();
    bool themePicking = false;
    int themeSel = 0, themeBefore = 0;

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
    void drawGuidePicture(const std::string &line, int y);
    std::vector<std::string> guideLines(const std::string &text, int width);
    SDL_Texture *wikiPicture(const std::string &path, bool &missing);
    void dropWikiPictures();
    void showPins(const std::string &label, const std::vector<Pin> &pins);
    std::string itemName(const std::string &id) const;
    int itemIcon(const std::string &id) const;
    std::vector<std::string> usedFor(const std::string &id) const;

    // Maps
    const MapInfo *currentMap() const { return maps.empty() ? nullptr : &maps[mapSel % maps.size()]; }
    bool ensureMapTexture();
    bool ensureFloorTexture();
    std::string mapFile(const std::string &image, bool fetch, bool *pending = nullptr);
    void openMap(int idx);
    void inputMapPicker();
    void drawMapPicker();
    void drawTiles(const std::vector<SDL_Texture *> &tiles, int imgW, int imgH, float x, float y, float w, float h,
                   int top, int bottom);
    void drawLevels(const std::string &image, const std::vector<MapLevel> &levels, int overviewW, float x, float y,
                    float w, float h, int top, int bottom);
    SDL_Texture *mapChunk(const std::string &path);
    void freeChunks();
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
    // Sharp map pieces, kept while recently drawn; a few load per frame so panning stays smooth.
    struct Chunk {
        SDL_Texture *tex;
        int used;
    };
    std::map<std::string, Chunk> mapChunks;
    std::set<std::string> chunkMissing;
    Fetcher fetcher;  // pieces and pictures that are neither bundled nor saved yet
    Decoder decoder;  // map pieces read and decoded off the main thread
    void pumpChunks();
    int mapFrame = 0;
    uint32_t mapFrameStart = 0;
    bool mapFailed = false;
    bool mapPending = false;  // the map image is downloading
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

    // Ammo tab
    ListView ammoList;
    std::vector<int> ammoRows;    // indices into db.ammo
    std::vector<int> ammoPicked;  // rounds pinned to the chart, in pick order
    int ammoCal = 0;              // 0 = all calibers, else db.calibers index + 1
    std::string ammoSearch;
    bool ammoDirty = true;
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
    std::map<std::string, SDL_Texture *> wikiPics; // guide pictures of the open quest; null = unavailable
    std::vector<QRow> qrows;
    int qrowSel = -1;
    int qrowScroll = 0;

    std::string pinLabel;
    std::vector<Pin> pins;
    bool pinReturn = false;

    std::string status;
    SDL_Color statusColor = col::dim;
    u64 statusTick = 0;

    // Taps and clicks: areas drawn last frame that react, topmost last. The handler gets the
    // tap count (2 for a double click).
    struct Hit {
        SDL_Rect r;
        std::function<void(int)> fn;
    };
    std::vector<Hit> hits;
    float wheelAcc = 0;
    int wheelSteps = 0;   // pending Up/Down presses from the mouse wheel
    float wheelGuide = 0; // wiki guide lines to scroll
    float dragAcc = 0;    // list rows dragged by touch
    void onTap(int x, int y, int w, int h, std::function<void(int)> fn) { hits.push_back({{x, y, w, h}, std::move(fn)}); }
    // Acts as if the buttons were pressed this frame.
    void press(u64 buttons) { in.down |= buttons; }
    void setTab(int t);
    void pointerInput();
    bool wheelList();
    void zoomMap(float factor, float ax, float ay);
    void listTap(ListView &lv, int idx, int taps, u64 doubleTap = HidNpadButton_A);
    void filterTaps(int x, int y, int w, int h);
};

bool App::init() {
    romfsInit();
    plInitialize(PlServiceType_User);
    socketInitializeDefault();
    netInit();
    fetcher.start();
    mkdir(APP_DIR, 0777);
    mkdir(APP_DIR "/maps", 0777);
    mkdir(APP_DIR "/wiki", 0777);
    in.init();
    if (!ui.init())
        return false;
    decoder.start(ui.textureFormat());

    loadState(STATE_PATH, st);
    applyTheme(themeIndex(st.theme));
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
    fetcher.stop();
    decoder.stop();
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
    for (const std::string &root : {std::string(APP_DIR "/"), std::string(ROMFS_ROOT)}) {
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
    for (const std::string &root : {std::string(APP_DIR "/"), std::string(ROMFS_ROOT)}) {
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
    // Map images can change with the data; drop downloaded overviews so they refetch on view.
    // Sharp pieces stay: they only change when the source renders do.
    std::vector<std::string> dirs = {"/maps/", "/maps/floors/", "/maps/thumbs/"};
    if (DIR *dir = opendir(APP_DIR "/maps/")) {
        while (struct dirent *e = readdir(dir))
            if (e->d_name[0] != '.' && isDir(std::string(APP_DIR "/maps/") + e->d_name))
                dirs.push_back(std::string("/maps/") + e->d_name + "/");
        closedir(dir);
    }
    for (const std::string &sub : dirs) {
        std::string d = std::string(APP_DIR) + sub;
        if (DIR *dir = opendir(d.c_str())) {
            while (struct dirent *e = readdir(dir))
                if (e->d_name[0] != '.' && !isDir(d + e->d_name))
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

// Saves every wiki guide picture the app does not have yet, for use without internet.
void App::downloadPictures() {
    ensureWiki();
    std::set<std::string> paths;
    for (const auto &kv : wiki) {
        const std::string &t = kv.second;
        for (size_t at = t.find("@img "); at != std::string::npos; at = t.find("@img ", at + 5)) {
            size_t end = t.find(' ', at + 5);
            if (end != std::string::npos)
                paths.insert(t.substr(at + 5, end - at - 5));
        }
    }
    std::vector<std::string> todo;
    for (const std::string &p : paths)
        if (!fileExists(ROMFS_ROOT + p) && !fileExists(std::string(APP_DIR "/") + p))
            todo.push_back(p);
    if (todo.empty()) {
        setStatus(paths.empty() ? "No wiki pictures in the data yet" :
                  "All " + std::to_string(paths.size()) + " wiki pictures are already saved", col::good);
        return;
    }
    int done = 0, failed = 0;
    for (const std::string &p : todo) {
        if (done % 8 == 0)
            busy("Downloading wiki pictures " + std::to_string(done) + " / " + std::to_string(todo.size()) + "...");
        HttpResult r = httpGet(DATA_URL + p);
        std::string sd = std::string(APP_DIR "/") + p;
        makeParents(sd);
        if (r.ok && r.status == 200 && writeFile(sd, r.body)) {
            done++;
            continue;
        }
        if (!r.ok || r.status == 429) {
            // Offline, or GitHub asks to slow down: stop and keep what is saved.
            setStatus("Stopped after " + std::to_string(done) + " pictures: " +
                      (r.ok ? "GitHub is rate limiting, try again later" : r.error), col::bad);
            return;
        }
        failed++;
        done++;
    }
    dropWikiPictures();
    setStatus("Saved " + std::to_string(done - failed) + " wiki pictures" +
              (failed ? " (" + std::to_string(failed) + " missing on the server)" : ""), failed ? col::warn : col::good);
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
    int trackerLevels = 0;
    if (!applyTrackerProgress(r.body, data, st, trackerLevels, err)) {
        setStatus("Sync failed: " + err, col::bad);
        return;
    }
    st.lastSync = nowString();
    needDirty = true;
    save();
    std::string who = st.account.empty() ? std::string("progress") : st.account;
    std::string mode = st.mode == "pve" ? "PVE" : "PVP";
    if (trackerLevels == 0)
        setStatus("Synced " + who + " (" + mode + "). TarkovTracker has no hideout levels, so yours were kept",
                  col::warn);
    else
        setStatus("Synced " + who + " (" + mode + "): " + std::to_string(trackerLevels) +
                      " hideout levels from TarkovTracker, merged with yours", col::good);
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
    pointerInput();
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
    case TAB_AMMO: inputAmmo(); break;
    case TAB_DATABASE: inputDatabase(); break;
    case TAB_KAPPA: inputKappa(); break;
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
    if (mapTexId != image) {
        freeMapTiles();
        mapTexId = image;
        mapFailed = false;
    }
    mapPending = false;
    if (!mapTiles.empty())
        return true;
    if (mapFailed)
        return false;
    std::string path = mapFile(image, true, &mapPending);
    if (mapPending)
        return false;
    if (!path.empty() && !ui.loadImageTiles(path, kMapTile, mapTiles, mapImgW, mapImgH))
        setStatus("Not enough memory for the " + m->name + " map", col::bad);
    mapFailed = mapTiles.empty();
    return !mapFailed;
}

// Finds a map image on the SD card or in romfs. With fetch, one that neither has is downloaded
// in the background and saved for offline use: "" with *pending set until it has arrived, and
// "" alone when it is unavailable.
std::string App::mapFile(const std::string &image, bool fetch, bool *pending) {
    if (pending)
        *pending = false;
    Fetcher::State state = fetch ? fetcher.state(image) : Fetcher::NONE;
    if (state == Fetcher::PENDING) {
        *pending = true;
        return "";
    }
    std::string sd = std::string(APP_DIR "/") + image;
    if (state == Fetcher::DONE || fileExists(sd))
        return sd;
    if (fileExists(ROMFS_ROOT + image))
        return ROMFS_ROOT + image;
    if (fetch && fetcher.request(image) == Fetcher::PENDING)
        *pending = true;
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
    bool pending;
    std::string path = mapFile(image, true, &pending);
    if (pending)
        return false;
    for (SDL_Texture *t : floorTiles)
        SDL_DestroyTexture(t);
    floorTiles.clear();
    floorTexId = image;
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

// A desktop window can show more of a sharp level at once and has the memory for it.
static const int kMaxChunks = nxDesktop() ? 160 : 32;

// A sharp piece from the SD card or romfs, downloaded in the background when neither has it.
// Returns nullptr when it is not ready yet (try again next frame) or does not exist.
// A sharp piece if it is ready. Otherwise it is queued for the decoder (or a download when neither
// romfs nor the SD card has it) and nullptr is returned; the overview shows until it arrives.
SDL_Texture *App::mapChunk(const std::string &path) {
    auto it = mapChunks.find(path);
    if (it != mapChunks.end()) {
        it->second.used = mapFrame;
        return it->second.tex;
    }
    if (chunkMissing.count(path) || fetcher.state(path) == Fetcher::PENDING)
        return nullptr;
    if (decoder.request(path) == Decoder::MISSING) {
        decoder.forget(path);
        if (fetcher.state(path) == Fetcher::DONE || fetcher.request(path) == Fetcher::FAILED)
            chunkMissing.insert(path);
    }
    return nullptr;
}

// Uploads decoded pieces: one per frame, more while the frame is still young, so a burst after
// zooming spreads over several frames.
void App::pumpChunks() {
    std::string path;
    SDL_Surface *surf;
    for (int n = 0; n < 4 && (n == 0 || SDL_GetTicks() - mapFrameStart < 4) && decoder.take(path, surf); n++) {
        SDL_Texture *tex = ui.texture(surf);
        SDL_FreeSurface(surf);
        if (!tex)
            continue;
        while ((int)mapChunks.size() >= kMaxChunks) {
            auto old = mapChunks.end();
            for (auto c = mapChunks.begin(); c != mapChunks.end(); ++c)
                if (old == mapChunks.end() || c->second.used < old->second.used)
                    old = c;
            SDL_DestroyTexture(old->second.tex);
            mapChunks.erase(old);
        }
        auto prev = mapChunks.find(path);
        if (prev != mapChunks.end())
            SDL_DestroyTexture(prev->second.tex);
        mapChunks[path] = {tex, mapFrame};
    }
}

void App::freeChunks() {
    for (auto &kv : mapChunks)
        SDL_DestroyTexture(kv.second.tex);
    mapChunks.clear();
    chunkMissing.clear();
    fetcher.reset();
    decoder.clear();
}

void App::freeMapTiles() {
    freeChunks();
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
    mapZoom = std::clamp(mapZoom * (1 + zoomStep), 1.f, 16.f);

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
    // Keep the view center where the view can actually go: past the point where the map edge
    // reaches the screen edge the drawing stops moving, and panning back felt stuck.
    float hu = std::min(0.5f, viewW / 2 / (m->w * scale)), hv = std::min(0.5f, viewH / 2 / (m->h * scale));
    mapU = std::clamp(mapU, hu, 1 - hu);
    mapV = std::clamp(mapV, hv, 1 - hv);
}

// Zooms the open map by factor, keeping the map point under (ax, ay) where it is.
void App::zoomMap(float factor, float ax, float ay) {
    const MapInfo *m = currentMap();
    if (!m)
        return;
    int top, bottom;
    mapViewport(top, bottom);
    float viewW = 1280, viewH = bottom - top;
    float fit = std::min(viewW / m->w, viewH / m->h);
    auto origin = [&](float zoom, float &ox, float &oy) {
        float dw = m->w * fit * zoom, dh = m->h * fit * zoom;
        ox = dw <= viewW ? (viewW - dw) / 2 : std::clamp(viewW / 2 - mapU * dw, viewW - dw, 0.f);
        oy = top + (dh <= viewH ? (viewH - dh) / 2 : std::clamp(viewH / 2 - mapV * dh, viewH - dh, 0.f));
    };
    float ox, oy;
    origin(mapZoom, ox, oy);
    float u = (ax - ox) / (m->w * fit * mapZoom), v = (ay - oy) / (m->h * fit * mapZoom);
    float zoom = std::clamp(mapZoom * factor, 1.f, 16.f);
    float dw = m->w * fit * zoom, dh = m->h * fit * zoom;
    mapU = (viewW / 2 - ax) / dw + u;
    mapV = (top + viewH / 2 - ay) / dh + v;
    mapZoom = zoom;
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

enum SettingRow { SET_TOKEN, SET_SYNC, SET_SYNC_LAUNCH, SET_PROFILE, SET_MODE, SET_THEME, SET_DOWNLOAD, SET_PICTURES,
                  SET_RESET, SET_COUNT };

void App::inputSettings() {
    if (themePicking) {
        inputThemePicker();
        return;
    }
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
    case SET_PICTURES: downloadPictures(); break;
    case SET_THEME:
        themePicking = true;
        themeBefore = themeSel = themeIndex(st.theme);
        break;
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

void App::setTab(int t) {
    tab = Tab(t);
    reqFocus = false;
    confirmReset = false;
}

// Selects a list row on a tap; a double tap also presses the row's action button.
void App::listTap(ListView &lv, int idx, int taps, u64 doubleTap) {
    lv.sel = idx;
    if (taps >= 2)
        press(doubleTap);
}

// A "<  filter  >" label: the left half steps back, the right half forward.
void App::filterTaps(int x, int y, int w, int h) {
    onTap(x, y, w / 2, h, [this](int) { press(HidNpadButton_Left); });
    onTap(x + w / 2, y, w - w / 2, h, [this](int) { press(HidNpadButton_Right); });
}

// Moves the selection of the list under the wheel without wrapping around. False when the tab
// has no such list.
bool App::wheelList() {
    ListView *lv = nullptr;
    int n = 0;
    switch (tab) {
    case TAB_STATIONS:
        if (data.stations.empty())
            break;
        stationList.clampSel(data.stations.size());
        if (reqFocus) {
            const Station &s = data.stations[stationList.sel];
            const Level *L = levelOf(s, shownLevel(st, s, viewLevel));
            lv = &reqList;
            n = L ? L->items.size() : 0;
        } else {
            int before = stationList.sel;
            stationList.sel = std::clamp(before + wheelSteps, 0, (int)data.stations.size() - 1);
            if (stationList.sel != before) {
                viewLevel = -1;
                reqList = ListView();
            }
            wheelSteps = 0;
            return true;
        }
        break;
    case TAB_ITEMS: lv = &itemList; n = needed.size(); break;
    case TAB_CRAFTS: lv = &craftList; n = visibleCrafts().size(); break;
    case TAB_TIMERS: lv = &timerList; n = st.timers.size(); break;
    case TAB_AMMO: lv = &ammoList; n = ammoRows.size(); break;
    case TAB_SETTINGS: lv = &settingsList; n = SET_COUNT; confirmReset = false; break;
    case TAB_DATABASE:
        if (openQuest < 0) {
            lv = &dbList;
            n = dbSection == DB_ITEMS ? dbItemRows.size() : dbRows.size();
        }
        break;
    default: break;
    }
    if (!lv)
        return false;
    if (n > 0)
        lv->sel = std::clamp(lv->sel + wheelSteps, 0, n - 1);
    wheelSteps = 0;
    return true;
}

// Mouse and touch on top of the pad: number keys pick a tab, taps run the area under them and
// the wheel scrolls lists, the wiki guide, or zooms the map around the pointer.
void App::pointerInput() {
    int key = nxTabKey();
    if (key >= 0 && key < TAB_COUNT)
        setTab(key);
    if (in.tapped) {
        SDL_Point pt{in.tapX, in.tapY};
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (SDL_PointInRect(&pt, &it->r)) {
                // The handler may redraw (busy messages), which rebuilds the list.
                auto fn = it->fn;
                fn(in.tapCount);
                break;
            }
    }
    if (in.wheel != 0) {
        int px = 640, py = 360;
        bool over = nxPointer(px, py);
        if (tab == TAB_MAPS && mapPicked && !maps.empty()) {
            zoomMap(std::pow(1.25f, in.wheel), over ? px : 640, over ? py : 360);
        } else if (tab == TAB_DATABASE && openQuest >= 0 && questView != 2 && px < (questView == 1 ? 1264 : 624)) {
            wheelGuide -= in.wheel * 3;
        } else {
            wheelAcc -= in.wheel * 3;
            int steps = (int)wheelAcc;
            wheelAcc -= steps;
            wheelSteps = std::clamp(wheelSteps + steps, -30, 30);
        }
    }
    // Dragging a finger (or the mouse) over a list scrolls it like the wheel; maps pan instead.
    if (in.touching && in.touchDY && !(tab == TAB_MAPS && mapPicked)) {
        if (tab == TAB_DATABASE && openQuest >= 0 && questView != 2 && in.touchX < (questView == 1 ? 1264 : 624)) {
            wheelGuide -= in.touchDY / 26.f;
        } else {
            dragAcc -= in.touchDY / (tab == TAB_MAPS ? 150.f : 46.f);
            int steps = (int)dragAcc;
            dragAcc -= steps;
            wheelSteps = std::clamp(wheelSteps + steps, -30, 30);
        }
    } else if (!in.touching) {
        dragAcc = 0;
    }
    if (wheelSteps != 0 && !wheelList()) {
        // Screens without a plain list (map picker, quest details, profile) get arrow presses.
        press(wheelSteps > 0 ? HidNpadButton_Down : HidNpadButton_Up);
        wheelSteps += wheelSteps > 0 ? -1 : 1;
    }
}


// ---------------------------------------------------------------- themes

static const int kThemeCols = 4;

void App::inputThemePicker() {
    int n = themes().size();
    int before = themeSel;
    if (in.left() && themeSel % kThemeCols > 0)
        themeSel--;
    if (in.right() && themeSel % kThemeCols < kThemeCols - 1 && themeSel + 1 < n)
        themeSel++;
    if (in.up() && themeSel >= kThemeCols)
        themeSel -= kThemeCols;
    if (in.downDir() && themeSel + kThemeCols < n)
        themeSel += kThemeCols;
    // The whole app previews the highlighted theme; B puts the old one back.
    if (themeSel != before)
        applyTheme(themeSel);
    if (in.pressed(HidNpadButton_A)) {
        st.theme = themes()[themeSel].name;
        save();
        themePicking = false;
        setStatus(std::string("Theme set to ") + themes()[themeSel].name, col::good);
    } else if (in.pressed(HidNpadButton_B)) {
        applyTheme(themeBefore);
        themePicking = false;
    }
}

void App::drawThemePicker() {
    const auto &list = themes();
    int n = list.size();
    const int gap = 12, cw = (1248 - gap * (kThemeCols - 1)) / kThemeCols, ch = 138;
    for (int i = 0; i < n; i++) {
        const Theme &t = list[i];
        int x = 16 + i % kThemeCols * (cw + gap), y = kTop - 8 + i / kThemeCols * (ch + gap);
        bool on = i == themeSel;
        onTap(x, y, cw, ch, [this, i](int taps) {
            if (themeSel != i) {
                themeSel = i;
                applyTheme(i);
            }
            if (taps >= 2)
                press(HidNpadButton_A);
        });
        // A small mock of the app in the theme's own colors.
        ui.fill(x, y, cw, ch, t.bg);
        ui.fill(x, y, cw, 20, t.bgTop);
        ui.fill(x, y, cw, 2, t.accent);
        ui.fill(x + 12, y + 17, 46, 3, t.accent);
        int pw = cw * 55 / 100;
        ui.fill(x + 10, y + 28, pw, 70, t.panel);
        ui.fill(x + 10, y + 34, pw, 26, t.panelHi);
        ui.fill(x + 10, y + 34, 3, 26, t.accent);
        ui.text(x + 20, y + 35, "Stations", FS_SMALL, t.text, pw - 20);
        ui.text(x + 20, y + 66, "Level 2  0 / 5", FS_SMALL, t.dim, pw - 20);
        const SDL_Color sw[] = {t.accent, t.good, t.warn, t.bad, t.info, t.violet};
        int sx = x + 22 + pw, sz = (cw - pw - 40) / 3;
        for (int k = 0; k < 6; k++)
            ui.fill(sx + k % 3 * (sz + 4), y + 30 + k / 3 * (sz + 4), sz, sz, sw[k]);
        ui.fill(x, y + ch - 34, cw, 34, t.panel);
        ui.text(x + 12, y + ch - 30, t.name, FS_SMALL, t.text, cw - 24);
        if (on) {
            ui.rect(x, y, cw, ch, col::accent);
            ui.rect(x + 1, y + 1, cw - 2, ch - 2, col::accent);
        } else {
            ui.rect(x, y, cw, ch, t.line);
        }
    }
    drawFooter({{"A", "Use theme"}, {"B", "Cancel"}});
}

// ---------------------------------------------------------------- kappa

static const int kKappaCols = 10;

void App::ensureKappa() {
    ensureDb();
    if (!kappaItems.empty() || db.quests.empty())
        return;
    for (const Quest &q : db.quests) {
        if (q.name != "Collector")
            continue;
        kappaQuestId = q.id;
        for (const Objective &o : q.objectives)
            if (o.type == "giveItem" && !o.items.empty() &&
                std::find(kappaItems.begin(), kappaItems.end(), o.items[0]) == kappaItems.end())
                kappaItems.push_back(o.items[0]);
    }
}

std::vector<int> App::kappaRows() const {
    std::vector<int> rows;
    bool done = st.tasksDone.count(kappaQuestId) > 0;
    for (int i = 0; i < (int)kappaItems.size(); i++) {
        bool found = done || st.kappaFound.count(kappaItems[i]);
        if (kappaFilter == 0 || (kappaFilter == 1) != found)
            rows.push_back(i);
    }
    return rows;
}

void App::inputKappa() {
    ensureKappa();
    if (in.pressed(HidNpadButton_Y)) {
        kappaFilter = (kappaFilter + 1) % 3;
        kappaSel = kappaScroll = 0;
    }
    auto rows = kappaRows();
    int n = rows.size();
    if (n == 0)
        return;
    kappaSel = std::clamp(kappaSel, 0, n - 1);
    if (in.left() && kappaSel > 0)
        kappaSel--;
    if (in.right() && kappaSel + 1 < n)
        kappaSel++;
    if (in.up() && kappaSel >= kKappaCols)
        kappaSel -= kKappaCols;
    if (in.downDir() && kappaSel + kKappaCols < n)
        kappaSel += kKappaCols;
    if (in.pressed(HidNpadButton_A)) {
        const std::string &id = kappaItems[rows[kappaSel]];
        if (st.kappaFound.count(id))
            st.kappaFound.erase(id);
        else
            st.kappaFound.insert(id);
        save();
    }
}

void App::drawKappa() {
    ensureKappa();
    if (kappaItems.empty()) {
        drawEmpty("No Collector data yet.", "Connect to the internet and use Settings > Download latest hideout data.");
        drawFooter({{"+", "Quit"}});
        return;
    }
    bool done = st.tasksDone.count(kappaQuestId) > 0;
    int found = 0;
    for (const auto &id : kappaItems)
        found += done || st.kappaFound.count(id);
    int total = kappaItems.size();
    int kq = 0, kqDone = 0;
    for (const Quest &q : db.quests)
        if (q.kappa) {
            kq++;
            kqDone += st.tasksDone.count(q.id) > 0;
        }

    // Progress strip: Collector items, then the quests the Kappa container needs.
    ui.fill(16, kTop - 8, 1248, 56, col::panel);
    ui.fill(16, kTop - 8, 4, 56, col::accent);
    int w = ui.text(34, kTop - 2, "Collector", FS_BODY, col::text);
    ui.text(34 + w + 14, kTop + 2, done ? "complete on TarkovTracker" : "items found in raid", FS_SMALL, col::dim);
    std::string cnt = std::to_string(found) + " / " + std::to_string(total);
    ui.textRight(760, kTop - 2, cnt, FS_BODY, found == total ? col::good : col::accent);
    ui.progress(34, kTop + 34, 726, 6, total ? float(found) / total : 0, found == total ? col::good : col::accent);
    ui.text(800, kTop - 2, "Kappa quests", FS_SMALL, col::dim);
    std::string kqs = st.tasksDone.empty() ? "sync to see" : std::to_string(kqDone) + " / " + std::to_string(kq);
    ui.textRight(1248, kTop - 2, kqs, FS_SMALL, col::text);
    ui.progress(800, kTop + 34, 448, 6, kq ? float(kqDone) / kq : 0, col::info);
    static const char *filters[] = {"All items", "Still needed", "Found"};

    auto rows = kappaRows();
    const int gap = 8, cw = (1248 - gap * (kKappaCols - 1)) / kKappaCols, ch = 112, top = kTop + 58;
    const int detailH = 70;
    int visible = std::max(1, (kBottom - top - detailH + gap) / (ch + gap));
    int nrows = (rows.size() + kKappaCols - 1) / kKappaCols;
    kappaSel = std::clamp(kappaSel, 0, std::max(0, (int)rows.size() - 1));
    int selRow = kappaSel / kKappaCols;
    if (selRow < kappaScroll)
        kappaScroll = selRow;
    if (selRow >= kappaScroll + visible)
        kappaScroll = selRow - visible + 1;
    kappaScroll = std::clamp(kappaScroll, 0, std::max(0, nrows - visible));
    if (rows.empty())
        ui.text(34, top + 20, kappaFilter == 1 ? "Nothing left to find. Go get that Kappa!" : "No items marked found yet.",
                FS_BODY, col::dim);
    for (int k = kappaScroll * kKappaCols; k < (int)rows.size() && k < (kappaScroll + visible) * kKappaCols; k++) {
        const std::string &id = kappaItems[rows[k]];
        int x = 16 + k % kKappaCols * (cw + gap), y = top + (k / kKappaCols - kappaScroll) * (ch + gap);
        bool on = k == kappaSel, have = done || st.kappaFound.count(id);
        onTap(x, y, cw, ch, [this, k](int taps) {
            kappaSel = k;
            if (taps >= 2)
                press(HidNpadButton_A);
        });
        ui.fill(x, y, cw, ch, on ? col::panelHi : col::panel);
        if (have)
            ui.fill(x, y, cw, 3, col::good);
        ui.icon(itemIcon(id), x + (cw - 64) / 2, y + 10, 64);
        std::string name = itemName(id);
        int tw = std::min(ui.measure(name, FS_SMALL), cw - 12);
        ui.text(x + (cw - tw) / 2, y + 80, name, FS_SMALL, have ? col::good : (on ? col::text : col::dim), cw - 12);
        if (have) {
            ui.disc(x + cw - 16, y + 18, 10, col::good);
            ui.fill(x + cw - 21, y + 18, 4, 2, col::bg);
            ui.fill(x + cw - 19, y + 20, 2, 2, col::bg);
            ui.fill(x + cw - 17, y + 16, 2, 4, col::bg);
            ui.fill(x + cw - 15, y + 14, 2, 2, col::bg);
            ui.fill(x + cw - 13, y + 12, 2, 2, col::bg);
        }
        if (on)
            ui.rect(x, y, cw, ch, col::accent);
    }
    drawScrollbar(1268, top, visible * (ch + gap) - gap, nrows, visible, kappaScroll);

    // The highlighted item.
    int dy = kBottom - detailH + 6;
    ui.fill(16, dy, 1248, detailH - 6, col::panel);
    if (!rows.empty()) {
        const std::string &id = kappaItems[rows[kappaSel]];
        bool have = done || st.kappaFound.count(id);
        ui.icon(itemIcon(id), 26, dy + 6, 52);
        ui.text(92, dy + 6, itemName(id), FS_BODY, col::text, 700);
        const DbItem *it = db.item(id);
        std::string price = it && it->price ? "Flea " + groupDigits(it->price) + " RUB" : "Not sold on flea";
        if (it && !it->trader.empty())
            price += "   " + it->trader + " pays " + groupDigits(it->traderPrice) + " RUB";
        ui.text(92, dy + 36, price, FS_SMALL, col::dim, 800);
        drawBadge(1248 - ui.measure(have ? "Found in raid" : "Still needed", FS_SMALL) - 14, dy + 18,
                  have ? "Found in raid" : "Still needed", have ? col::good : col::warn);
    }
    bool have = !rows.empty() && (done || st.kappaFound.count(kappaItems[rows[kappaSel]]));
    drawFooter({{"A", have ? "Unmark" : "Mark found"}, {"Y", std::string("Showing: ") + filters[kappaFilter]}, {"+", "Quit"}});
}

// ---------------------------------------------------------------- database

void App::ensureDb() {
    if (dbLoaded)
        return;
    dbLoaded = true;
    std::string name = "db-" + modeName() + ".json";
    std::string err;
    std::vector<std::string> roots = {dataRoot, std::string(APP_DIR "/"), std::string(ROMFS_ROOT)};
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
    for (const std::string &root : {std::string(APP_DIR "/"), std::string(ROMFS_ROOT)})
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
    return section == DB_QUESTS ? db.traders.size() : 0;
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
    dropWikiPictures();
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
                             : guideLines(text, questView == 1 ? 1200 : 560);
    buildQuestRows(q);
}

// Wraps the guide text. A picture ('@img path w h') becomes one '@img path w h' line, sized to the
// column, followed by '@cont' lines for the rows it covers; '@cap' captions wrap with their prefix.
std::vector<std::string> App::guideLines(const std::string &text, int width) {
    std::vector<std::string> out;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.rfind("@img ", 0) == 0) {
            char path[256];
            int w = 0, h = 0;
            if (sscanf(line.c_str() + 5, "%255s %d %d", path, &w, &h) != 3 || w <= 0 || h <= 0)
                continue;
            int dw = std::min(w, width), dh = h * dw / w;
            out.push_back("@img " + std::string(path) + " " + std::to_string(dw) + " " + std::to_string(dh));
            for (int k = 1; k < (dh + 8 + 25) / 26; k++)
                out.push_back("@cont");
        } else if (line.rfind("@cap ", 0) == 0) {
            for (const auto &l : ui.wrap(line.substr(5), FS_SMALL, width))
                out.push_back("@cap " + l);
        } else {
            for (auto &l : ui.wrap(line, FS_SMALL, width))
                out.push_back(std::move(l));
        }
    }
    return out;
}

// Loads a guide picture from the SD card or romfs, downloading it from the data branch once.
SDL_Texture *App::wikiPicture(const std::string &path, bool &missing) {
    missing = false;
    auto it = wikiPics.find(path);
    if (it != wikiPics.end()) {
        missing = !it->second;
        return it->second;
    }
    Fetcher::State fetch = fetcher.state(path);
    if (fetch == Fetcher::PENDING)
        return nullptr; // still downloading
    std::string sd = std::string(APP_DIR "/") + path, file;
    if (fetch == Fetcher::DONE || fileExists(sd))
        file = sd;
    else if (fileExists(ROMFS_ROOT + path))
        file = ROMFS_ROOT + path;
    else if (fetcher.request(path) == Fetcher::PENDING)
        return nullptr;
    SDL_Texture *t = file.empty() ? nullptr : ui.loadImage(file);
    wikiPics[path] = t;
    missing = !t;
    return t;
}

void App::drawGuidePicture(const std::string &line, int y) {
    char path[256];
    int w = 0, h = 0;
    if (sscanf(line.c_str() + 5, "%255s %d %d", path, &w, &h) != 3)
        return;
    bool missing;
    SDL_Texture *t = wikiPicture(path, missing);
    if (t) {
        SDL_FRect dst{32.f, float(y + 2), float(w), float(h)};
        ui.image(t, nullptr, &dst);
    } else {
        ui.fill(32, y + 2, w, h, col::cell);
        ui.text(48, y + 14, missing ? "Picture unavailable offline" : "Loading picture...", FS_SMALL, col::faint);
    }
}

void App::dropWikiPictures() {
    for (auto &kv : wikiPics)
        if (kv.second)
            SDL_DestroyTexture(kv.second);
    wikiPics.clear();
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
        dropWikiPictures();
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
    acc += -in.ry * 0.6f + wheelGuide;
    wheelGuide = 0;
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
            dropWikiPictures();
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
    onTap(0, 0, x, 46, [this](int) { press(HidNpadButton_L); });
    // Tabs get narrower padding when a long player name would push them past the R hint.
    int textW = 0;
    for (int i = 0; i < TAB_COUNT; i++)
        textW += ui.measure(i == TAB_PROFILE ? who : kTabNames[i], FS_SMALL) + 2;
    textW += ui.measure(mode, FS_SMALL) + 22;
    int pad = std::clamp((1280 - x - 60 - textW) / TAB_COUNT, 14, 28);
    if (ui.measure(who, FS_SMALL) > 200)
        who = who.substr(0, 16) + "...";
    for (int i = 0; i < TAB_COUNT; i++) {
        bool on = i == tab;
        std::string label = i == TAB_PROFILE ? who : kTabNames[i];
        int lw = ui.measure(label, FS_SMALL);
        int w = lw + pad + (i == TAB_PROFILE ? ui.measure(mode, FS_SMALL) + 22 : 0);
        onTap(x, 0, w + 2, 46, [this, i](int) { setTab(i); });
        if (on) {
            ui.fill(x, 4, w, 42, col::panel);
            ui.fill(x, 43, w, 3, col::accent);
        }
        ui.text(x + pad / 2, 12, label, FS_SMALL, on ? col::text : col::dim);
        if (i == TAB_PROFILE)
            drawBadge(x + pad / 2 + 8 + lw, 11, mode, pve ? col::good : col::bad);
        x += w + 2;
    }
    ui.hint(x + 10, 10, "R", "");
    onTap(x, 0, ui.hintsWidth("R", "") + 30, 46, [this](int) { press(HidNpadButton_R); });
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
    for (const auto &h : hints) {
        int hw = ui.hint(x, 685, h.first, h.second);
        // Every hint is also a button; a pair like "ZL/ZR" splits into its two halves.
        static const std::map<std::string, std::pair<u64, u64>> kButtons = {
            {"A", {HidNpadButton_A, 0}},        {"B", {HidNpadButton_B, 0}},
            {"X", {HidNpadButton_X, 0}},        {"Y", {HidNpadButton_Y, 0}},
            {"+", {HidNpadButton_Plus, 0}},     {"-", {HidNpadButton_Minus, 0}},
            {"R3", {HidNpadButton_StickR, 0}},  {">", {HidNpadButton_Right, 0}},
            {"ZL/ZR", {HidNpadButton_ZL, HidNpadButton_ZR}},
            {"</>", {HidNpadButton_Left, HidNpadButton_Right}},
            {"^/v", {HidNpadButton_Up, HidNpadButton_Down}},
        };
        auto it = kButtons.find(h.first);
        if (h.first == "A/X") {
            onTap(x - 8, 678, hw / 2 + 8, 42, [this](int) { zoomMap(1.5f, 640, 363); });
            onTap(x + hw / 2, 678, hw - hw / 2 + 8, 42, [this](int) { zoomMap(1 / 1.5f, 640, 363); });
        } else if (it != kButtons.end() && it->second.second) {
            u64 a = it->second.first, b = it->second.second;
            onTap(x - 8, 678, hw / 2 + 8, 42, [this, a](int) { press(a); });
            onTap(x + hw / 2, 678, hw - hw / 2 + 8, 42, [this, b](int) { press(b); });
        } else if (it != kButtons.end()) {
            u64 a = it->second.first;
            onTap(x - 8, 678, hw + 16, 42, [this, a](int) { press(a); });
        }
        x += hw + 22;
    }
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
        onTap(16, y, 424, rowH - 4, [this, idx](int) {
            if (stationList.sel != idx) {
                stationList.sel = idx;
                viewLevel = -1;
                reqList = ListView();
            }
            reqFocus = false;
        });
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
        onTap(cx, kTop + 16, 34, 30, [this, l](int) {
            viewLevel = l;
            reqList = ListView();
        });
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
        onTap(px + 8, ry, pw - 24, ih - 4, [this, idx](int n) {
            reqFocus = true;
            listTap(reqList, idx, n);
        });
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
        onTap(16, y, 1240, rowH - 6, [this, idx](int n) { listTap(itemList, idx, n, 0); });
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
    int fw = ui.text(28, kTop - 4, "<  " + filter + "  >", FS_BODY, col::accent);
    filterTaps(20, kTop - 8, fw + 16, 36);
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
        onTap(16, y, 1240, rowH - 6, [this, idx](int n) { listTap(craftList, idx, n); });
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
        onTap(16, y, 1240, rowH - 6, [this, idx](int n) { listTap(timerList, idx, n); });
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

// Draws the pieces of the sharpest needed level over the overview. x, y, w, h is the whole map on screen.
void App::drawLevels(const std::string &image, const std::vector<MapLevel> &levels, int overviewW, float x, float y,
                     float w, float h, int top, int bottom) {
    if (levels.empty() || w * ui.pixelScale() <= overviewW * 1.1f || image.size() < 4)
        return;
    int k = 0;
    while (k + 1 < (int)levels.size() && levels[k].w < w * 0.9f * ui.pixelScale())
        k++;
    const MapLevel &lv = levels[k];
    std::string stem = image.substr(0, image.size() - 4) + "/" + std::to_string(k) + "/";
    int cols = (lv.w + kMapChunk - 1) / kMapChunk, rows = (lv.h + kMapChunk - 1) / kMapChunk;
    float sx = w / lv.w, sy = h / lv.h;
    int cx0 = std::max(0, int((0 - x) / sx) / kMapChunk), cx1 = std::min(cols - 1, int((1280 - x) / sx) / kMapChunk);
    int cy0 = std::max(0, int((top - y) / sy) / kMapChunk), cy1 = std::min(rows - 1, int((bottom - y) / sy) / kMapChunk);
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            if (!lv.have[cy * cols + cx])
                continue;
            SDL_Texture *tex = mapChunk(stem + std::to_string(cx) + "_" + std::to_string(cy) + ".jpg");
            if (!tex)
                continue;
            int px = cx * kMapChunk, py = cy * kMapChunk;
            int pw = std::min(kMapChunk, lv.w - px), ph = std::min(kMapChunk, lv.h - py);
            float x0 = std::round(x + px * sx), x1 = std::round(x + (px + pw) * sx);
            float y0 = std::round(y + py * sy), y1 = std::round(y + (py + ph) * sy);
            SDL_FRect dst{x0, y0, x1 - x0, y1 - y0};
            SDL_Rect src{0, 0, pw, ph};
            ui.image(tex, &src, &dst);
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
        onTap(x, y, cw, ch, [this, i](int) {
            pickSel = i;
            openMap(i);
        });
        ui.fill(x, y, cw, ch, on ? col::panelHi : col::panel);
        auto it = mapThumbs.find(m.thumb);
        if (it == mapThumbs.end() && !m.thumb.empty()) {
            std::string path = mapFile(m.thumb, false);
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
        // Level count on the right; the name gets the rest of the line.
        std::string sub = m.floors.empty() ? "" : std::to_string(m.floors.size()) + " levels";
        int subW = sub.empty() ? 0 : ui.measure(sub, FS_SMALL) + 12;
        ui.text(x + 12, y + thumbH + 6, m.name, FS_SMALL, on ? col::text : col::dim, cw - 24 - subW);
        if (!sub.empty())
            ui.textRight(x + cw - 12, y + thumbH + 8, sub, FS_SMALL, col::faint);
    }
    drawScrollbar(1268, kTop - 8, kBottom - kTop + 8, rows, visible, firstRow);
    drawFooter({{"A", "Open"}, {"+", "Quit"}});
}

// Map overlays sit on dark map art, so light themes draw them with the default theme's ink.
struct MapInk {
    SDL_Color *slots[7] = {&col::text, &col::dim, &col::faint, &col::shadow, &col::panel, &col::panelHi, &col::line};
    SDL_Color saved[7];
    bool active;
    MapInk() : active(col::bg.r + col::bg.g + col::bg.b > 384) {
        if (!active)
            return;
        const Theme &d = themes()[0];
        const SDL_Color ink[7] = {d.text, d.dim, d.faint, d.shadow, d.panel, d.panelHi, d.line};
        for (int i = 0; i < 7; i++) {
            saved[i] = *slots[i];
            *slots[i] = ink[i];
        }
    }
    void restore() {
        for (int i = 0; active && i < 7; i++)
            *slots[i] = saved[i];
        active = false;
    }
    ~MapInk() { restore(); }
};

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
    MapInk ink;
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
    mapFrame++;
    mapFrameStart = SDL_GetTicks();
    pumpChunks();
    if (ensureMapTexture()) {
        bool plan = mapPlan && !m->alt.empty();
        drawTiles(mapTiles, mapImgW, mapImgH, ox, oy, dw, dh, top, bottom);
        drawLevels(plan ? m->alt : m->image, plan ? m->altLevels : m->levels, mapImgW, ox, oy, dw, dh, top, bottom);
        if (ensureFloorTexture()) {
            // Floor images carry the dimmed base inside their rect; dim the rest to match.
            const MapFloor &f = m->floors[mapFloor];
            bool fplan = mapPlan && !f.alt.empty();
            const float *r = fplan ? f.ar : f.r;
            ui.fill(int(ox), int(oy), int(dw) + 1, int(dh) + 1, SDL_Color{0, 0, 0, 140});
            drawTiles(floorTiles, floorImgW, floorImgH, ox + r[0] * dw, oy + r[1] * dh, (r[2] - r[0]) * dw,
                      (r[3] - r[1]) * dh, top, bottom);
            drawLevels(fplan ? f.alt : f.image, fplan ? f.altLevels : f.levels, mapImgW, ox, oy, dw, dh, top, bottom);
        }
    } else {
        ui.text(40, top + 80,
                mapPending ? "Downloading the " + m->name + " map..." :
                             "Map image unavailable offline. Connect to download it once.",
                FS_BODY, col::dim);
    }

    auto vis = visibleMarkers();
    int selIdx = markerSel >= 0 && markerSel < (int)vis.size() ? vis[markerSel] : -1;
    float r = std::clamp(4.f + mapZoom * 1.5f, 6.f, 12.f);
    for (int pass = 0; pass < 2; pass++) {
        for (int pos = 0; pos < (int)vis.size(); pos++) {
            int i = vis[pos];
            const MapMarker &mk = m->markers[i];
            bool sel = i == selIdx;
            if ((pass == 1) != sel)
                continue;
            float x = ox + mk.u * dw, y = oy + mk.v * dh;
            float rr = sel ? r + 4 : r;
            if (pass == 0 && y >= top && y < bottom) {
                int hr = int(rr) + 6;
                onTap(int(x) - hr, int(y) - hr, hr * 2, hr * 2, [this, pos](int) { markerSel = pos; });
            }
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
            onTap(x, ry, w, rowH, [this, i](int) { mapFloor = i; });
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
        onTap(12, 12, tw + 20, 32, [this](int) { mapFull = false; });
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
        onTap(16, y, 250, 28, [this, t](int) {
            markerFilter = markerFilter == t ? -1 : t;
            markerSel = -1;
        });
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
    ink.restore();
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
        onTap(x, kTop - 8, w, 36, [this, i](int) {
            if (dbSection == i)
                return;
            dbSection = i;
            dbFilter = 0;
            dbList = ListView();
            dbDirty = true;
        });
        ui.text(x + 14, kTop - 6, kDbNames[i], FS_BODY, on ? col::bg : col::dim);
        x += w + 6;
    }
    ui.hint(x + 6, kTop - 3, "ZL/ZR", "");
    x += std::max(90, ui.hintsWidth("ZL/ZR", "") + 24);
    int nf = filterCount(dbSection, db);
    if (nf > 0) {
        std::string f = dbFilter == 0 ? "All traders" : db.traders[dbFilter - 1];
        int fw = ui.text(x, kTop - 4, "<  " + f + "  >", FS_BODY, col::accent, 300);
        filterTaps(x - 8, kTop - 8, fw + 16, 36);
    }
    if (!dbSearch.empty()) {
        int bw = drawBadge(1256 - ui.measure("Search: " + dbSearch, FS_SMALL) - 14, kTop - 2, "Search: " + dbSearch,
                           col::info);
        onTap(1256 - bw, kTop - 6, bw, 32, [this](int) { press(HidNpadButton_X); });
    }
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
        onTap(16, y, 1240, rowH - 6, [this, idx](int n) { listTap(dbList, idx, n); });
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

// Colors for rounds pinned to the chart.
static const struct {
    SDL_Color operator[](size_t k) const {
        const SDL_Color c[] = {col::info, col::good,          col::violet,         col::warn,
                               col::bad,  {96, 200, 190, 255}, {226, 120, 170, 255}, col::text};
        return c[k];
    }
} kPickColors;
static const int kMaxPicks = 8;

void App::filterAmmo() {
    ammoDirty = false;
    ammoRows.clear();
    std::string q = lower(ammoSearch);
    for (int i = 0; i < (int)db.ammo.size(); i++) {
        const Ammo &a = db.ammo[i];
        if (ammoCal > 0 && a.caliber != db.calibers[ammoCal - 1])
            continue;
        if (matches(itemName(a.item), q) || matches(a.caliber, q))
            ammoRows.push_back(i);
    }
}

void App::inputAmmo() {
    ensureDb();
    if (in.pressed(HidNpadButton_Y)) {
        std::string q;
        if (keyboard("Search ammo", "Part of a name or caliber", ammoSearch, false, q)) {
            ammoSearch = q;
            ammoList = ListView();
            ammoDirty = true;
        }
    }
    if (in.pressed(HidNpadButton_X)) {
        if (!ammoSearch.empty()) {
            ammoSearch.clear();
            ammoList = ListView();
            ammoDirty = true;
        } else if (!ammoPicked.empty()) {
            ammoPicked.clear();
            setStatus("Chart cleared");
        }
    }
    int nc = db.calibers.size();
    if (nc > 0 && (in.left() || in.right())) {
        ammoCal = (ammoCal + (in.right() ? 1 : -1) + nc + 1) % (nc + 1);
        ammoList = ListView();
        ammoDirty = true;
    }
    if (ammoDirty)
        filterAmmo();
    int n = ammoRows.size();
    if (in.up())
        ammoList.move(-1, n);
    if (in.downDir())
        ammoList.move(1, n);
    if (in.pressed(HidNpadButton_ZL))
        ammoList.move(-10, n);
    if (in.pressed(HidNpadButton_ZR))
        ammoList.move(10, n);
    ammoList.clampSel(n);
    if (n > 0 && in.pressed(HidNpadButton_A)) {
        int idx = ammoRows[ammoList.sel];
        auto it = std::find(ammoPicked.begin(), ammoPicked.end(), idx);
        if (it != ammoPicked.end())
            ammoPicked.erase(it);
        else if ((int)ammoPicked.size() >= kMaxPicks)
            setStatus("The chart holds " + std::to_string(kMaxPicks) + " rounds; unpin one with A first", col::warn);
        else
            ammoPicked.push_back(idx);
    }
}

void App::drawAmmo() {
    ensureDb();
    if (ammoDirty)
        filterAmmo();
    if (db.ammo.empty()) {
        drawEmpty("No ammo data yet.", "Connect to the internet and use Settings > Download latest hideout data.");
        drawFooter({{"+", "Quit"}});
        return;
    }
    // Caliber filter and search above the list.
    std::string cal = ammoCal == 0 ? "All calibers" : db.calibers[ammoCal - 1];
    int fw = ui.text(24, kTop - 6, "<  " + cal + "  >", FS_BODY, col::accent, 420);
    filterTaps(16, kTop - 10, fw + 16, 36);
    if (!ammoSearch.empty()) {
        int bw = drawBadge(460, kTop - 4, "Search: " + ammoSearch, col::info);
        onTap(460, kTop - 8, bw, 32, [this](int) { press(HidNpadButton_X); });
    }

    const int lx = 16, lw = 620, rowH = 40, top = kTop + 66;
    static const char *cols[] = {"Dmg", "Pen", "Arm", "Frag"};
    static const int colX[] = {440, 508, 568, 628};
    ui.fill(lx, kTop + 30, lw, 32, col::panel);
    ui.text(lx + 52, kTop + 34, "Round", FS_SMALL, col::dim);
    for (int c = 0; c < 4; c++)
        ui.textRight(colX[c], kTop + 34, cols[c], FS_SMALL, col::dim);
    int visible = (kBottom - top) / rowH;
    ammoList.clamp(ammoRows.size(), visible);
    for (int i = 0; i < visible && ammoList.scroll + i < (int)ammoRows.size(); i++) {
        int idx = ammoList.scroll + i;
        const Ammo &a = db.ammo[ammoRows[idx]];
        int y = top + i * rowH;
        bool on = idx == ammoList.sel;
        onTap(lx, y, lw, rowH - 4, [this, idx](int n) { listTap(ammoList, idx, n); });
        if (on) {
            ui.fill(lx, y, lw, rowH - 4, col::panelHi);
            ui.fill(lx, y, 4, rowH - 4, col::accent);
        }
        auto pk = std::find(ammoPicked.begin(), ammoPicked.end(), ammoRows[idx]);
        if (pk != ammoPicked.end())
            ui.disc(lx + 12, y + 18, 5, kPickColors[pk - ammoPicked.begin()]);
        ui.icon(itemIcon(a.item), lx + 20, y + 2, 32);
        std::string name = itemName(a.item);
        int w = ui.text(lx + 60, y + 6, name, FS_SMALL, on ? col::text : col::dim, ammoCal ? 300 : 230);
        if (!ammoCal)
            ui.text(lx + 66 + w, y + 6, a.caliber, FS_SMALL, col::faint, 290 - w);
        std::string dmg = a.projectiles > 1 ? std::to_string(a.projectiles) + "x" + std::to_string(a.damage)
                                            : std::to_string(a.damage);
        ui.textRight(colX[0], y + 6, dmg, FS_SMALL, col::text);
        SDL_Color pc = penColor(a.pen);
        ui.fill(colX[1] - 44, y + 4, 46, 28, SDL_Color{pc.r, pc.g, pc.b, 60});
        ui.textRight(colX[1], y + 6, std::to_string(a.pen), FS_SMALL, pc);
        ui.textRight(colX[2], y + 6, std::to_string(a.armorDamage), FS_SMALL, col::dim);
        ui.textRight(colX[3], y + 6, std::to_string(a.frag) + "%", FS_SMALL, col::dim);
    }
    drawScrollbar(lx + lw + 2, top, visible * rowH, ammoRows.size(), visible, ammoList.scroll);

    drawAmmoChart(652, kTop - 8, 612, 400);
    if (!ammoRows.empty())
        drawAmmoDetail(db.ammo[ammoRows[ammoList.sel]], 652, kTop + 400, 612);
    bool picked = !ammoRows.empty() &&
                  std::find(ammoPicked.begin(), ammoPicked.end(), ammoRows[ammoList.sel]) != ammoPicked.end();
    std::vector<std::pair<std::string, std::string>> hints = {{"A", picked ? "Unpin" : "Pin to chart"},
                                                               {"</>", "Caliber"}, {"Y", "Search"}};
    if (!ammoSearch.empty())
        hints.push_back({"X", "Clear search"});
    else if (!ammoPicked.empty())
        hints.push_back({"X", "Clear chart"});
    drawFooter(hints);
}

// Damage against penetration, like eft-ammo.com: armor class columns, the listed rounds as faint dots,
// pinned rounds in color and the highlighted round with a ring.
void App::drawAmmoChart(int x, int y, int w, int h) {
    ui.fill(x, y, w, h, col::panel);
    const int ml = 46, mr = 14, mt = 34, mb = 34;
    int px = x + ml, py = y + mt, pw = w - ml - mr, ph = h - mt - mb;
    std::vector<int> pts = ammoRows;
    for (int i : ammoPicked)
        if (std::find(pts.begin(), pts.end(), i) == pts.end())
            pts.push_back(i);
    int maxPen = 70, maxDmg = 100;
    for (int i : pts) {
        maxPen = std::max(maxPen, db.ammo[i].pen);
        maxDmg = std::max(maxDmg, db.ammo[i].damage);
    }
    maxPen = (maxPen + 9) / 10 * 10;
    int step = maxDmg > 300 ? 100 : maxDmg > 160 ? 50 : 25;
    maxDmg = (maxDmg + step - 1) / step * step;
    auto sx = [&](float pen) { return px + pen / maxPen * pw; };
    auto sy = [&](float dmg) { return py + ph - dmg / maxDmg * ph; };

    ui.text(x + 14, y + 6, "Damage vs penetration", FS_SMALL, col::accent);
    // Armor classes: a round needs roughly 10 x class penetration to go through reliably.
    for (int c = 1; c <= 6 && c * 10 <= maxPen; c++) {
        int cx0 = int(sx(c * 10)), cx1 = int(sx(std::min(maxPen, c * 10 + 10)));
        if (c % 2)
            ui.fill(cx0, py, cx1 - cx0, ph, col::cell);
        ui.fill(cx0, py, 1, ph, col::line);
        std::string lbl = "Class " + std::to_string(c);
        int lw = ui.measure(lbl, FS_SMALL);
        if (cx1 - cx0 > lw + 4)
            ui.text((cx0 + cx1 - lw) / 2, py + 2, lbl, FS_SMALL, col::faint);
        else
            ui.text(cx0 + 4, py + 2, std::to_string(c), FS_SMALL, col::faint);
    }
    for (int d = 0; d <= maxDmg; d += step) {
        int yy = int(sy(d));
        ui.fill(px, yy, pw, 1, d ? SDL_Color{58, 58, 50, 120} : col::line);
        ui.textRight(px - 6, yy - 12, std::to_string(d), FS_SMALL, col::faint);
    }
    for (int p = 0; p <= maxPen; p += 10)
        ui.text(int(sx(p)) - (p ? 10 : 4), py + ph + 4, std::to_string(p), FS_SMALL, col::faint);
    ui.textRight(x + w - 14, y + 6, "x: penetration   y: damage", FS_SMALL, col::faint);

    SDL_Rect clipR{px - 8, py - 8, pw + 16, ph + 16};
    ui.clip(&clipR);
    for (int i : ammoRows)
        ui.disc(sx(db.ammo[i].pen), sy(db.ammo[i].damage), 4, col::dim);
    int cur = ammoRows.empty() ? -1 : ammoRows[ammoList.sel];
    // Labels go right of the dot, or left near the right edge; a placed label pushes later ones down.
    std::vector<SDL_Rect> placed;
    auto label = [&](int i, SDL_Color c, FontSize fs) {
        const Ammo &a = db.ammo[i];
        std::string name = itemName(a.item);
        int tw = std::min(ui.measure(name, fs), 220), th = ui.lineHeight(fs);
        float dx = sx(a.pen), dy = sy(a.damage);
        int tx = dx + 10 + tw > px + pw ? int(dx) - 10 - tw - 8 : int(dx) + 10;
        int ty = int(dy) - th / 2;
        for (bool moved = true; moved;) {
            moved = false;
            for (const SDL_Rect &r : placed)
                if (tx < r.x + r.w && tx + tw + 8 > r.x && ty < r.y + r.h && ty + th > r.y) {
                    ty = r.y + r.h + 1;
                    moved = true;
                }
        }
        placed.push_back({tx - 4, ty, tw + 8, th});
        ui.fill(tx - 4, ty, tw + 8, th, col::shadow);
        ui.text(tx, ty, name, fs, c, tw);
    };
    for (size_t k = 0; k < ammoPicked.size(); k++) {
        const Ammo &a = db.ammo[ammoPicked[k]];
        ui.disc(sx(a.pen), sy(a.damage), 8, col::shadow);
        ui.disc(sx(a.pen), sy(a.damage), 6, kPickColors[k]);
    }
    if (cur >= 0) {
        const Ammo &a = db.ammo[cur];
        ui.disc(sx(a.pen), sy(a.damage), 10, col::accent);
        ui.disc(sx(a.pen), sy(a.damage), 7, col::bg);
        ui.disc(sx(a.pen), sy(a.damage), 5, col::accent);
        label(cur, col::text, FS_SMALL);
    }
    for (size_t k = 0; k < ammoPicked.size(); k++)
        if (ammoPicked[k] != cur)
            label(ammoPicked[k], kPickColors[k], FS_SMALL);
    ui.clip(nullptr);
}

void App::drawAmmoDetail(const Ammo &a, int x, int y, int w) {
    ui.fill(x, y + 8, w, kBottom - y - 8, col::panel);
    ui.icon(itemIcon(a.item), x + 14, y + 20, 48);
    ui.text(x + 74, y + 18, itemName(a.item), FS_BODY, col::text, w - 90);
    const DbItem *it = db.item(a.item);
    std::string price = it && it->price ? groupDigits(it->price) + " RUB on flea" :
                        (it && it->noFlea ? "Not on flea" : "No flea price");
    ui.text(x + 74, y + 48, a.caliber + "   " + std::to_string(a.speed) + " m/s   " + price + (a.tracer ? "   Tracer" : ""),
            FS_SMALL, col::dim, w - 90);
    // Rough armor class effectiveness from penetration alone.
    int cx = x + 14, cy = y + 84;
    ui.text(cx, cy + 4, "vs armor class", FS_SMALL, col::dim);
    cx += 150;
    int cw = (x + w - 14 - cx) / 6;
    for (int c = 1; c <= 6; c++) {
        int margin = a.pen - c * 10;
        SDL_Color k = margin >= 10 ? col::good : margin >= 0 ? SDL_Color{160, 196, 96, 255} :
                      margin >= -5 ? col::warn : col::bad;
        ui.fill(cx + (c - 1) * cw, cy, cw - 4, 30, SDL_Color{k.r, k.g, k.b, 70});
        std::string t = std::to_string(c);
        ui.text(cx + (c - 1) * cw + (cw - 4 - ui.measure(t, FS_SMALL)) / 2, cy + 3, t, FS_SMALL, k);
    }
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
        onTap(16, y, 600, rowH - 4, [this, idx](int n) { listTap(dbList, idx, n); });
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
        onTap(16, y, 600, rowH - 4, [this, idx](int n) { listTap(dbList, idx, n, 0); });
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
        SDL_Rect clipG{16, top + 4, gw, lines * 26 + 8};
        ui.clip(&clipG);
        // A picture that starts above the first visible row still shows its lower part.
        if (textScroll < (int)questText.size() && questText[textScroll] == "@cont") {
            int j = textScroll;
            while (j > 0 && questText[j] == "@cont")
                j--;
            drawGuidePicture(questText[j], y - (textScroll - j) * 26);
        }
        for (int i = 0; i < lines && textScroll + i < (int)questText.size(); i++) {
            const std::string &l = questText[textScroll + i];
            if (l == "@cont")
                ;
            else if (l.rfind("@img ", 0) == 0)
                drawGuidePicture(l, y);
            else if (l.rfind("@cap ", 0) == 0)
                ui.text(32, y, l.substr(5), FS_SMALL, col::dim);
            else if (l.rfind("## ", 0) == 0)
                ui.text(32, y, l.substr(3), FS_SMALL, col::accent);
            else if (l.rfind("### ", 0) == 0)
                ui.text(32, y, l.substr(4), FS_SMALL, col::warn);
            else
                ui.text(32, y, l, FS_SMALL, col::text);
            y += 26;
        }
        ui.clip(nullptr);
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
        if (r.kind == QRow::OBJ || r.kind == QRow::ITEM) {
            int y0 = std::max(y, top), y1 = std::min(y + r.h, kBottom);
            if (y1 > y0)
                onTap(rx + 8, y0, rw - 16, y1 - y0, [this, i](int n) {
                    qrowSel = i;
                    if (n >= 2)
                        press(HidNpadButton_A);
                });
        }
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
    if (themePicking) {
        drawThemePicker();
        return;
    }
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
        {"Theme", themes()[themeIndex(st.theme)].name},
        {"Download latest data, icons and maps", data.empty() ? "no data" : data.generated},
        {"Download all wiki pictures", "for offline guides"},
        {confirmReset ? "Clear local progress? Press A again" : "Clear local progress and timers", ""},
    };
    settingsList.clamp(SET_COUNT, SET_COUNT);
    for (int i = 0; i < SET_COUNT; i++) {
        int y = kTop + i * 52;
        bool on = i == settingsList.sel;
        onTap(16, y, 820, 46, [this, i](int) {
            if (settingsList.sel != i)
                confirmReset = false;
            settingsList.sel = i;
            press(HidNpadButton_A);
        });
        ui.fill(16, y, 820, 46, on ? col::panelHi : col::panel);
        if (on)
            ui.fill(16, y, 4, 46, col::accent);
        ui.text(36, y + 10, rows[i].label, FS_BODY, i == SET_RESET && confirmReset ? col::warn : col::text, 500);
        ui.textRight(816, y + 13, rows[i].value, FS_SMALL, col::dim);
    }
    if (settingsList.sel == SET_PICTURES) {
        int ny = kTop + SET_COUNT * 52 + 8;
        const char *warn[] = {
            "Heads up: this makes about a thousand downloads in a row.",
            "They come from this app's GitHub copy of the pictures, never",
            "from the wiki, so Fandom does not see them. GitHub can still",
            "rate limit you for a while if you repeat it often.",
        };
        for (const char *w : warn) {
            ui.text(36, ny, w, FS_SMALL, col::bad, 780);
            ny += 24;
        }
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
    hits.clear();
    ui.clear();
    if (!(tab == TAB_MAPS && mapFull && !maps.empty()))
        drawHeader();
    switch (tab) {
    case TAB_STATIONS: drawStations(); break;
    case TAB_ITEMS: drawItems(); break;
    case TAB_CRAFTS: drawCrafts(); break;
    case TAB_TIMERS: drawTimers(); break;
    case TAB_MAPS: drawMaps(); break;
    case TAB_AMMO: drawAmmo(); break;
    case TAB_DATABASE: drawDatabase(); break;
    case TAB_KAPPA: drawKappa(); break;
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
