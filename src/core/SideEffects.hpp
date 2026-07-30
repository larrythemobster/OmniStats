#pragma once
#include <string>
#include <optional>
#include <vector>
#include <array>
#include <cstdint>
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

struct PendingDestroyedMatchMmrRefresh {
    std::string primaryId;
    std::string name;
    std::string matchGuid;
    std::string playlist;
    int localTeam = -1;
    std::array<int, 2> score{};
    int previousMmr = 0;
    int previousMatches = -1;
    bool previousMmrIsPlaylistSpecific = false;
    bool localPlayerDisappeared = false;
    bool explicitLocalForfeit = false;
    int64_t destroyedAtUnixMs = 0;
    bool validCompetitiveMatch = false;
};

struct ResolvedDestroyedMatch {
    std::string matchGuid;
    bool won = false;
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
    std::optional<PendingDestroyedMatchMmrRefresh> pendingDestroyedMatch;
    std::optional<ResolvedDestroyedMatch> resolvedDestroyedMatch;
    std::vector<std::string> fetchEncounterQueue;
    bool saveMatch = false;
    nlohmann::json matchRecord;
    MatchSaveSnapshot saveSnapshot;
    int replayKeyToPress = -1;
};
