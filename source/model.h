#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct ReqItem {
    std::string id;      // requirement id, matches TarkovTracker hideoutParts ids
    std::string itemId;
    std::string name;
    std::string shortName;
    int count = 1;
    bool fir = false;
};

struct Level {
    std::string id;      // level id, matches TarkovTracker hideoutModules ids
    int level = 0;
    int constructionTime = 0;
    std::vector<ReqItem> items;
    std::vector<std::string> other;
};

struct Station {
    std::string id;
    std::string name;
    std::vector<Level> levels;
};

struct CraftItem {
    std::string itemId;
    std::string name;
    std::string shortName;
    int count = 1;
};

struct Craft {
    std::string id;
    std::string stationId;
    int level = 0;
    int duration = 0;
    std::vector<CraftItem> req;
    std::vector<CraftItem> rew;
};

struct HideoutData {
    std::string mode;
    std::string generated;
    std::vector<Station> stations;
    std::vector<Craft> crafts;
    std::map<std::string, int> icons;          // item id -> index in icons.png
    std::map<std::string, std::string> skills; // skill id -> display name
    std::vector<int> levelExp;                 // total exp needed for level i + 1

    const Station *station(const std::string &id) const;
    int icon(const std::string &itemId) const;
    int playerLevel(int64_t exp) const;
    bool empty() const { return stations.empty(); }
};

// ---- Database tab (db-<mode>.json)

struct Pin {
    std::string map;  // map id as in maps-<mode>.json (normalized name)
    float u, v;
};

struct DbItem {
    std::string name, shortName, category, desc;
    int price = 0;            // flea average, 0 if unknown
    std::string trader;       // best trader to sell to
    int traderPrice = 0;
    float weight = 0;
    bool noFlea = false, questItem = false;
    int icon = -1;
};

struct Ammo {
    std::string item, caliber;
    int damage = 0, pen = 0, armorDamage = 0, frag = 0, speed = 0, projectiles = 1;
    bool tracer = false;
};

struct KeyInfo {
    std::string item;
    int uses = 0;
    std::vector<std::string> quests;
    std::vector<Pin> pins;
};

struct BuildSpec {
    std::string weapon;
    std::vector<std::string> attrs;  // "Ergonomics >= 37"
    std::vector<std::string> parts;  // item ids
    std::vector<std::string> categories;
};

struct Objective {
    std::string desc, type;
    int count = 0, more = 0;
    bool optional = false, fir = false;
    std::vector<std::string> items;
    std::vector<Pin> pins;
    bool hasBuild = false;
    BuildSpec build;
};

struct ItemCount {
    std::string item;
    int count = 1;
};

struct Unlock {
    std::string item, trader;
    int level = 1;
};

struct Rewards {
    std::vector<std::pair<std::string, float>> rep;
    std::vector<ItemCount> items;
    std::vector<Unlock> unlocks;
    std::vector<std::pair<std::string, int>> skills;
    std::vector<std::string> traders;
};

struct Quest {
    std::string id, name, trader, map, faction, wiki;
    int level = 0, exp = 0;
    bool kappa = false, lightkeeper = false;
    std::vector<std::string> prereqs, traderReqs, keys;
    std::vector<Objective> objectives;
    Rewards rewards;
    std::vector<ItemCount> startItems;
};

struct Database {
    std::map<std::string, DbItem> items;
    std::vector<Ammo> ammo;
    std::vector<KeyInfo> keys;
    std::vector<Quest> quests;
    std::vector<std::string> calibers;
    std::vector<std::string> traders;

    const DbItem *item(const std::string &id) const;
    bool empty() const { return quests.empty() && items.empty(); }
};

bool parseDatabase(const std::string &text, Database &out, std::string &err);
// wiki.json: {"pages": {title: text}}
bool parseWiki(const std::string &text, std::map<std::string, std::string> &out);

struct Timer {
    std::string craftId;
    int64_t endTime = 0;
};

struct State {
    std::string token;
    std::string mode = "pve";
    bool syncOnLaunch = true;
    std::map<std::string, bool> modules;
    std::map<std::string, int> parts;
    std::vector<Timer> timers;
    std::set<std::string> tasksDone;  // from TarkovTracker
    std::string lastSync;
    std::string account;
    std::string profileId;  // tarkov.dev account id
    std::string theme;      // Theme name, empty for the default
    std::set<std::string> kappaFound;  // Collector items handed in or set aside, by item id

    // Number of consecutive levels built from level 1.
    int builtLevel(const Station &st) const;
    int have(const ReqItem &r) const;
};

enum MarkerType { MK_PMC, MK_SCAV, MK_SHARED, MK_TRANSIT, MK_BOSS, MK_COUNT };

struct MapMarker {
    MarkerType type;
    std::string name;
    float u, v;  // 0..1 across the map image
};

// One sharper copy of a map image, cut in kMapChunk squares stored next to the image:
// "maps/x/render.jpg" has its level 2 piece (3, 1) at "maps/x/render/2/3_1.jpg".
struct MapLevel {
    int w = 0, h = 0;          // full-map size at this level
    std::vector<bool> have;    // cy * cols + cx; missing pieces are plain background
};

static const int kMapChunk = 1024;

// One floor of a map. The image is the dimmed base with the floor drawn on top, covering r (0..1 of the map).
struct MapFloor {
    std::string name;
    std::string image, alt;  // per style, like MapInfo::image / alt; empty for the base level
    float r[4] = {0, 0, 1, 1}, ar[4] = {0, 0, 1, 1};
    std::vector<MapLevel> levels, altLevels;  // pieces only where the floor is
};

struct MapInfo {
    std::string id;
    std::string name;
    std::string image;  // path relative to the data root, e.g. maps/customs.jpg
    std::string alt;    // optional second style (vector plan), same framing as image
    std::string thumb;  // small preview for the map picker
    std::vector<MapLevel> levels, altLevels;  // coarsest first; empty for data without them
    std::vector<MapFloor> floors;  // bottom to top; empty when the map has a single level
    int baseFloor = 0;             // index of the ground level in floors
    int w = 0, h = 0;
    int duration = 0;   // raid minutes
    std::string players;
    std::vector<MapMarker> markers;
};

struct ProfileStat {
    std::string label;
    std::string value;
};

struct ProfileSkill {
    std::string name;
    float level;  // 0..51
};

struct Profile {
    std::string nickname;
    std::string side;
    int64_t exp = 0;
    int prestige = 0;
    int64_t updated = 0;  // unix seconds of the snapshot
    std::vector<ProfileStat> pmc;
    std::vector<ProfileStat> scav;
    std::vector<ProfileSkill> skills;
    bool empty() const { return nickname.empty(); }
};

// Parses a compact hideout file written by tools/build_data.py.
bool parseHideout(const std::string &text, HideoutData &out, std::string &err);

bool loadState(const std::string &path, State &st);
bool saveState(const std::string &path, const State &st);

// Applies a TarkovTracker /progress response to the local state.
// Merges TarkovTracker progress into st: built levels and item counts only ever go up, so levels set in the
// app survive a sync when TarkovTracker has less (or no hideout progress). trackerLevels is how many built
// levels TarkovTracker reported.
bool applyTrackerProgress(const std::string &text, const HideoutData &data, State &st, int &trackerLevels,
                          std::string &err);

bool parseMaps(const std::string &text, std::vector<MapInfo> &out, std::string &err);

// Parses a players.tarkov.dev profile snapshot.
bool parseProfile(const std::string &text, const HideoutData &data, Profile &out, std::string &err);

std::string formatDuration(int64_t seconds);
