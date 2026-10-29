#pragma once

#include <cstdint>
#include <map>
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
    std::string lastSync;
    std::string account;
    std::string profileId;  // tarkov.dev account id

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

struct MapInfo {
    std::string id;
    std::string name;
    std::string image;  // path relative to the data root, e.g. maps/customs.jpg
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
bool applyTrackerProgress(const std::string &text, const HideoutData &data, State &st, std::string &err);

bool parseMaps(const std::string &text, std::vector<MapInfo> &out, std::string &err);

// Parses a players.tarkov.dev profile snapshot.
bool parseProfile(const std::string &text, const HideoutData &data, Profile &out, std::string &err);

std::string formatDuration(int64_t seconds);
