#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/game/GamemodeUtils.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/game/MatchAnalyticsTracker.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/state/MatchSaveSnapshot.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

namespace database::store {
    class HistoryStore;
}
using HistoryState = database::store::HistoryStore;

struct CapturedMatch {
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int playlistId = -1;
    uint64_t matchGeneration = 0;
    uint64_t sessionGeneration = 0;
    std::string myPrimaryId;
    int myTeam = -1;
    std::array<int, 2> score{};
    int legacyMaxPlayersSeen = 0;
    std::array<int, 2> legacyMaxTeamPlayersSeen{};
    bool roundEverStarted = false;
    bool localPlayerWasActive = false;
    bool localPlayerWasSpectator = false;
    bool legacyLobbyWasEverFull = false;
    bool localPlayerDisappeared = false;
    bool explicitLocalForfeit = false;
    bool fallbackCasualContext = false;
    bool fallbackNonRecordableContext = false;
    std::string fallbackNonRecordableReason;
    bool nonLiveReplay = false;
    MatchStats stats;
    std::unordered_map<std::string, PlayerData> roster;
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    std::string mode;
    LocalPreMatchMmrSnapshot preMatchMmr;
    bool hasPreMatchMmr = false;
    int64_t endedAtUnixMs = 0;
    float durationSeconds = 0.0f;
    float overtimeSeconds = 0.0f;
    std::vector<MatchJournalEvent> events;
    std::optional<MatchAnalytics> analytics;
};

struct MatchState {
    // Version incremented on every change published to snapshots
    uint64_t version = 1;

    // Match Lifecycle State
    bool inMatch = false;
    bool inReplay = false;
    int myTeam = -1;
    std::string myPrimaryId;
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int playlistId = -1;
    uint64_t activeMatchGeneration = 0;
    std::array<int, 2> score{};
    int legacyMaxPlayersSeen = 0;

    bool roundEverStarted = false;
    bool localPlayerWasActive = false;
    bool localPlayerWasSpectator = false;
    bool localPlayerPresenceObserved = false;
    bool localPlayerPresentInLatestUpdate = false;
    bool explicitLocalForfeit = false;
    bool excludedEarlyExitContext = false;
    std::string earlyExitExclusionReason;
    bool fallbackCasualContext = false;
    bool fallbackNonRecordableContext = false;
    std::string fallbackNonRecordableReason;
    bool legacyLobbyWasEverFull = false;
    std::array<int, 2> legacyCurrentTeamPlayersSeen{0, 0};
    std::array<int, 2> legacyMaxTeamPlayersSeen{0, 0};

    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    std::array<int, 2> matchSummaryScore{};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;
    float matchSummaryOvertimeSeconds = 0.0f;
    float timeSeconds = 0.0f;
    bool bOvertime = false;

    // Collections
    MatchStats currentMatch;
    SessionTotals sessionTotals;
    float lastGoalSpeed = 0.0f;
    float lastGoalSpeedSelf = 0.0f;
    std::unordered_map<std::string, PlayerData> roster;
    std::unordered_map<std::string, PlayerData> matchRoster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    std::unordered_map<std::string, LocalPreMatchMmrSnapshot> preMatchMmrByGuid;
    bool matchFinalized = false;
    int64_t sessionStartedAtUnix = 0;
    int64_t lastMatchEndedAtUnix = 0;
    std::vector<std::string> sessionMatchGuids;
    SessionRecap lastSessionRecap;
    uint64_t sessionGeneration = 0;

    // Hidden members from TelemetryReducer
    std::string lastSavedMatchGuid;
    std::vector<MatchJournalEvent> matchEvents;
    std::unordered_set<std::string> eventNativeKeys;
    bool hasObservedMatchClock = false;
    int64_t lastMatchClockSampleAtMs = 0;
    std::string lastEventSignature;
    int64_t lastEventAtMs = 0;
    std::unordered_map<std::string, int> lastPlayerBoost;
    std::unordered_set<std::string> boostRespawnPending;
    std::unordered_map<std::string, CapturedMatch> pendingDestroyedMatches;
    std::unordered_set<std::string> finalizedMatchGuids;
    uint64_t nextMatchGeneration = 0;
    bool missingGuidAssociationBlockedByReconnect = false;
    bool nonLiveReplayActive = false;
    std::unordered_map<std::string, int64_t> lastPlayerSeenMs;

    bool roundActive = true;
    std::string uiMatchGuid;
    bool countdownSeenThisRound = false;
    std::unordered_set<std::string> identityCandidates;
    int missedMyIdCount = 0;
    std::string lastKnownMyId;
    std::string lastConfirmedLocalAccount;
    MmrCategory autoSwitchedPlaylistCategory = MmrCategory::Best;
    MmrCategory followedGraphPlaylistCategory = MmrCategory::Best;
    std::optional<float> regulationClockStart;
    float regulationClockLatest = 0.0f;
    std::optional<int64_t> overtimeStartedAtMs;
    std::optional<int64_t> currentPauseStartMs;
    int64_t currentReducerNowMs = 0;
    struct PauseInterval {
        int64_t startMs = 0;
        int64_t endMs = 0;
    };
    std::vector<PauseInterval> pauseIntervals;

    // UI state mirror (owned by live match store, surfaced via side effects)
    int64_t lastMatchStartMs = 0;
    int64_t firstCountdownOfMatchMs = 0;
    int64_t lastCountdownMs = 0;
    int64_t lastGoalMs = 0;
    int64_t lastMatchEndMs = 0;
    int64_t lastPodiumMs = 0;
    bool inGoalReplay = false;
    bool showMatchSummary = false;
    int64_t matchSummaryStartMs = 0;
    // Set by ResetMatch; the next published UI update hides the held overlay once.
    bool hideOverlayRequested = false;

    MatchAnalyticsTracker analytics;

    void ResetMatch(const std::string& newArena, const std::string& newArenaAsset = "");
    void ClearActiveMatchOnDisconnect();
    // Returns whether a recap was captured.
    bool StartNewSession(int64_t endedAtUnix, HistoryState* history);
    void ResetTiming();
    float PausedSecondsWithin(int64_t windowStartMs, int64_t windowEndMs) const;
};
