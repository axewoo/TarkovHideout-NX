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

    const Station *station(const std::string &id) const;
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

    // Number of consecutive levels built from level 1.
    int builtLevel(const Station &st) const;
    int have(const ReqItem &r) const;
};

// Parses a compact hideout file written by tools/build_data.py.
bool parseHideout(const std::string &text, HideoutData &out, std::string &err);

bool loadState(const std::string &path, State &st);
bool saveState(const std::string &path, const State &st);

// Applies a TarkovTracker /progress response to the local state.
bool applyTrackerProgress(const std::string &text, const HideoutData &data, State &st, std::string &err);

std::string formatDuration(int64_t seconds);
