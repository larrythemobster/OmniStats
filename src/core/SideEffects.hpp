#pragma once
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "MatchSaveSnapshot.hpp"
#include "DiscordPresenceSnapshot.hpp"

struct SideEffects {
    bool pushDiscord = false;
    DiscordPresenceSnapshot discordSnapshot;
    bool fetchLifetimeHistory = false;
    std::string lifetimePrimaryId;
    std::string lifetimeCategory;
    bool refreshDbStats = false;
    std::string refreshStatsPrimaryId;
    std::vector<std::pair<std::string, std::string>> fetchMmrQueue;
    std::vector<std::string> fetchEncounterQueue;
    bool saveMatch = false;
    nlohmann::json matchRecord;
    MatchSaveSnapshot saveSnapshot;
    int replayKeyToPress = -1;
};
