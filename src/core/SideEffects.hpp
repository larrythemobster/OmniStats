#pragma once
#include <string>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>
#include "MatchSaveSnapshot.hpp"
#include "DiscordPresenceSnapshot.hpp"

struct PostMatchMmrRefresh {
    std::string primaryId;
    std::string name;
    std::string matchGuid;
    std::string playlist;
    int previousMmr = 0;
    int previousMatches = -1;
    bool previousMmrIsPlaylistSpecific = false;
    bool won = false;
    bool provisionalImmediately = false;
};


struct SideEffects {
    bool pushDiscord = false;
    DiscordPresenceSnapshot discordSnapshot;
    bool fetchLifetimeHistory = false;
    std::string lifetimePrimaryId;
    std::string lifetimeCategory;
    bool refreshDbStats = false;
    std::string refreshStatsPrimaryId;
    std::vector<std::pair<std::string, std::string>> fetchMmrQueue;
    std::optional<PostMatchMmrRefresh> postMatchMmrRefresh;
    std::vector<std::string> fetchEncounterQueue;
    bool saveMatch = false;
    nlohmann::json matchRecord;
    MatchSaveSnapshot saveSnapshot;
    int replayKeyToPress = -1;
};
