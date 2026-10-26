#include "core/state/MatchState.hpp"

#include "core/state/SessionState.hpp"

#include <algorithm>
#include <cmath>

void MatchState::ResetMatch(const std::string& newArena, const std::string& newArenaAsset) {
    inMatch = true;
    inReplay = false;
    myTeam = -1;
    arenaName = newArena;
    arenaAsset = newArenaAsset;
    playlistId = -1;
    score = {0, 0};
    legacyMaxPlayersSeen = 0;
    roundEverStarted = false;
    localPlayerWasActive = false;
    localPlayerWasSpectator = false;
    localPlayerPresenceObserved = false;
    localPlayerPresentInLatestUpdate = false;
    explicitLocalForfeit = false;
    excludedEarlyExitContext = false;
    earlyExitExclusionReason.clear();
    fallbackCasualContext = false;
    fallbackNonRecordableContext = false;
    fallbackNonRecordableReason.clear();
    legacyLobbyWasEverFull = false;
    legacyCurrentTeamPlayersSeen = {0, 0};
    legacyMaxTeamPlayersSeen = {0, 0};
    lastMatchWasVoid = false;
    lastMatchVoidReason.clear();
    matchSummaryScore = {0, 0};
    matchSummaryMyTeam = -1;
    matchSummaryWinnerTeam = -1;
    matchSummaryOvertimeSeconds = 0.0f;
    timeSeconds = 0.0f;
    bOvertime = false;
    currentMatch = MatchStats{};
    roster.clear();
    matchRoster.clear();
    preMatchMmrByGuid.clear();
    matchFinalized = false;
    showMatchSummary = false;
    lastMatchStartMs = 0;
    firstCountdownOfMatchMs = 0;
    lastCountdownMs = 0;
    lastGoalMs = 0;
    lastMatchEndMs = 0;
    lastPodiumMs = 0;
    inGoalReplay = false;
    hideOverlayRequested = true;
}

bool MatchState::StartNewSession(int64_t endedAtUnix, database::store::HistoryData* history) {
    const bool captureRecap = sessionTotals.wins + sessionTotals.losses > 0;
    if (captureRecap) {
        std::vector<std::string> matchGuids = std::move(sessionMatchGuids);
        if (history) {
            for (const auto& [playlist, points] : history->playlistMatchPoints) {
                for (const auto& pt : points) {
                    if (!pt.matchGuid.empty() &&
                        std::find(matchGuids.begin(), matchGuids.end(), pt.matchGuid) == matchGuids.end()) {
                        matchGuids.push_back(pt.matchGuid);
                    }
                }
            }
        }
        const std::string account = !myPrimaryId.empty()
                                        ? myPrimaryId
                                        : (history ? history->mmrOwnerPrimaryId : std::string{});
        const int64_t startedAt = sessionStartedAtUnix > 0 ? (std::min)(sessionStartedAtUnix, endedAtUnix) : 0;
        lastSessionRecap = {
            .valid = true,
            .endedAtUnix = endedAtUnix,
            .totals = std::move(sessionTotals),
            .gamemodes = std::move(sessionGamemodes),
            .sessionGeneration = sessionGeneration,
            .mmrOwnerPrimaryId = (history && !history->mmrOwnerPrimaryId.empty()) ? history->mmrOwnerPrimaryId : account,
            .playlistInitialMmr = history ? history->playlistInitialMmr : std::map<std::string, int>{},
            .id = 0,
            .account = account,
            .startedAtUnix = startedAt,
            .source = "live",
            .matchGuids = std::move(matchGuids)};
    }

    sessionTotals = SessionTotals();
    lastGoalSpeed = 0.0f;
    lastGoalSpeedSelf = 0.0f;
    sessionGamemodes.clear();
    sessionStartedAtUnix = 0;
    lastMatchEndedAtUnix = 0;
    sessionMatchGuids.clear();
    sessionGeneration++;
    if (history) {
        history->ResetSessionMmr();
    }
    return captureRecap;
}

void MatchState::ClearActiveMatchOnDisconnect() {
    inMatch = false;
    inReplay = false;
    myTeam = -1;
    arenaName.clear();
    arenaAsset.clear();
    matchGuid.clear();
    playlistId = -1;
    score = {0, 0};
    legacyMaxPlayersSeen = 0;
    roundEverStarted = false;
    localPlayerWasActive = false;
    localPlayerWasSpectator = false;
    localPlayerPresenceObserved = false;
    localPlayerPresentInLatestUpdate = false;
    explicitLocalForfeit = false;
    excludedEarlyExitContext = false;
    earlyExitExclusionReason.clear();
    fallbackCasualContext = false;
    fallbackNonRecordableContext = false;
    fallbackNonRecordableReason.clear();
    legacyLobbyWasEverFull = false;
    legacyCurrentTeamPlayersSeen = {0, 0};
    legacyMaxTeamPlayersSeen = {0, 0};
    timeSeconds = 0.0f;
    bOvertime = false;
    matchSummaryOvertimeSeconds = 0.0f;
    currentMatch = MatchStats{};
    roster.clear();
    matchRoster.clear();
    preMatchMmrByGuid.clear();
    matchFinalized = false;
    inGoalReplay = false;
    showMatchSummary = false;
    matchEvents.clear();
    ResetTiming();
}

void MatchState::ResetTiming() {
    regulationClockStart.reset();
    regulationClockLatest = 0.0f;
    overtimeStartedAtMs.reset();
    currentPauseStartMs.reset();
    pauseIntervals.clear();
    hasObservedMatchClock = false;
    lastMatchClockSampleAtMs = 0;
}

float MatchState::PausedSecondsWithin(int64_t windowStartMs, int64_t windowEndMs) const {
    if (windowEndMs <= windowStartMs) return 0.0f;
    int64_t totalPausedMs = 0;
    for (const auto& interval : pauseIntervals) {
        const int64_t overlapStart = (std::max)(windowStartMs, interval.startMs);
        const int64_t overlapEnd = (std::min)(windowEndMs, interval.endMs);
        if (overlapEnd > overlapStart) {
            totalPausedMs += (overlapEnd - overlapStart);
        }
    }
    if (currentPauseStartMs.has_value()) {
        const int64_t overlapStart = (std::max)(windowStartMs, *currentPauseStartMs);
        const int64_t overlapEnd = windowEndMs;
        if (overlapEnd > overlapStart) {
            totalPausedMs += (overlapEnd - overlapStart);
        }
    }
    return static_cast<float>(totalPausedMs) / 1000.0f;
}
