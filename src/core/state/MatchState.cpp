#include "core/state/MatchState.hpp"

#include "core/state/SessionState.hpp"

#include <algorithm>
#include <cmath>

void MatchState::ResetMatch(const std::string& newArena, const std::string& newArenaAsset) {
    static_cast<ActiveMatchState&>(*this) = {};
    inMatch = true;
    arenaName = newArena;
    arenaAsset = newArenaAsset;
    lastMatchWasVoid = false;
    lastMatchVoidReason.clear();
    matchSummaryScore = {0, 0};
    matchSummaryMyTeam = -1;
    matchSummaryWinnerTeam = -1;
    matchSummaryOvertimeSeconds = 0.0f;
    const int64_t savedSummaryStartMs = matchSummaryStartMs;
    static_cast<MatchTimeline&>(*this) = {};
    matchSummaryStartMs = savedSummaryStartMs;
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

    static_cast<MatchSessionState&>(*this) = {};
    sessionGeneration++;
    if (history) {
        history->ResetSessionMmr();
    }
    return captureRecap;
}

void MatchState::ClearActiveMatchOnDisconnect() {
    static_cast<ActiveMatchState&>(*this) = {};
    matchGuid.clear();
    matchSummaryOvertimeSeconds = 0.0f;
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
