#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <array>
#include "core/effects/SideEffects.hpp"
#include "core/game/RankProgress.hpp"

enum class MMRRequestReason {
    Roster,
    PostMatch,
    TrackedAccount
};

struct MMRRequest {
    std::string primaryId;
    std::string name;
    MMRRequestReason reason = MMRRequestReason::Roster;
    std::string matchGuid;
    std::string playlist;
    int previousMmr = 0;
    int previousMatches = -1;
    bool previousMmrIsPlaylistSpecific = false;
    bool won = false;
    bool resultKnown = true;
    int retriesRemaining = 2;
    std::chrono::steady_clock::time_point notBefore{};
    // Stamped at dequeue.
    uint64_t sessionGeneration = 0;
};

struct MMRProfileTotals {
    int totalWins = -1;
};

enum class CustomApiFetchResult {
    SuccessFinished,
    SuccessRequeued,
    AuthFailure,
    TransientError,
    UnusableData,
    DisabledOrNotReady
};

struct NormalizedProfileResult {
    int bestMmr = 0;
    std::string bestTier = "Unranked";
    std::string bestPlaylistName = "best";
    std::map<std::string, int> playlistMMRs;
    std::map<std::string, std::string> playlistTiers;
    std::map<std::string, int> playlistMatches;
    int totalWins = -1;
    std::string rankVerificationSource = "Tracker";
    std::string displayName;
};

enum class TrackedAccountFetchStatus {
    Fetching,
    Retrying,
    RateLimited,
    Success,
    NotFound,
    NoRankedData,
    AuthRequired,
    ProviderUnavailable,
    Unsupported,
    Failed
};

struct TrackedAccountFetchEvent {
    std::string primaryId;
    TrackedAccountFetchStatus status = TrackedAccountFetchStatus::Failed;
    NormalizedProfileResult profile;
    std::chrono::system_clock::time_point retryAt{};
    std::string detail;
};

enum class TrackedEnqueueResult {
    Queued,
    AlreadyQueued,
    ProviderUnavailable,
    InvalidIdentity
};

enum class PostMatchReconciliationState {
    AwaitingTracker,
    Provisional,
    Confirmed
};

struct PendingPostMatchRecord {
    std::string matchGuid;
    std::string primaryId;
    std::string playlist;
    int preMatchMmr = 0;
    int preMatchMatchesPlayed = -1;
    bool preMatchMmrIsPlaylistSpecific = false;
    int firstObservedMmr = 0;
    int firstObservedMatchesPlayed = -1;
    bool won = false;
    bool resultKnown = true;
    bool destroyedMatch = false;
    bool localPlayerDisappeared = false;
    bool explicitLocalForfeit = false;
    int localTeam = -1;
    std::array<int, 2> score{};
    int64_t destroyedAtUnixMs = 0;
    bool validCompetitiveMatch = false;
    bool databaseMatchFinalized = false;
    int provisionalMmr = 0;
    bool graphPointAppended = false;
    bool databaseRowUpdated = false;
    bool trackerCovered = false;
    bool valueEstimated = true;
    PostMatchReconciliationState reconciliationState = PostMatchReconciliationState::AwaitingTracker;
    uint64_t sessionGeneration = 0;
    // preMatchMmr may predate an unresolved ended-session match.
    bool followsPreviousSessionMatch = false;
};
