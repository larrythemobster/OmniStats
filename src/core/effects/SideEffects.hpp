#pragma once
#include <string>
#include <optional>
#include <vector>
#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "core/state/MatchSaveSnapshot.hpp"
#include "core/state/DiscordPresenceSnapshot.hpp"

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

struct ConfirmedLocalAccount {
    std::string primaryId;
    std::string name;
};

struct UiStateUpdate {
    std::optional<int64_t> lastMatchStartMs;
    std::optional<int64_t> firstCountdownOfMatchMs;
    std::optional<int64_t> lastCountdownMs;
    std::optional<int64_t> lastGoalMs;
    std::optional<int64_t> lastMatchEndMs;
    std::optional<int64_t> lastPodiumMs;
    std::optional<bool> inGoalReplay;
    std::optional<bool> showMatchSummary;
    std::optional<int64_t> matchSummaryStartMs;
    std::optional<MmrCategory> rosterMmrCategory;
    std::optional<MmrCategory> graphMmrCategory;
    std::optional<MmrCategory> rankProgressMmrCategory;
    bool resetTimestamps = false;
    int64_t resetMatchStartMs = 0;
    // A new match lifecycle hides the held overlay and the previous match summary.
    bool matchReset = false;
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
    std::optional<SessionMatchSummary> pendingMatchHistory;
    std::optional<ResolvedDestroyedMatch> resolvedDestroyedMatch;
    std::vector<std::string> fetchEncounterQueue;
    bool saveMatch = false;
    MatchSaveSnapshot saveSnapshot;
    std::optional<MatchSaveSnapshot> provisionalSaveSnapshot;
    std::optional<SessionRecap> persistSession;
    // Unix time at which a new match lifecycle started; closes the session if it went inactive.
    std::optional<int64_t> closeSessionIfInactiveAtUnix;
    std::optional<SessionRecap> updateSession;
    std::optional<ConfirmedLocalAccount> confirmedLocalAccount;
    // Identity changes: the saved local id (empty clears it) and the account whose MMR history is shown.
    std::optional<std::string> persistLocalPrimaryId;
    std::optional<std::string> selectMmrOwner;
    std::optional<UiStateUpdate> uiUpdate;
};
