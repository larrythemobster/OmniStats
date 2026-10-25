#pragma once

// Stateless helpers shared by the TelemetryReducer translation units.

#include <array>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>

#include "core/effects/SideEffects.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/LiveMatchSnapshot.hpp"
#include "core/state/MatchState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

namespace TelemetryReducerDetail {
    MmrCategory CategoryFromTeamCounts(const std::array<int, 2>& teamCounts, bool roundStarted);
    MmrCategory CategoryFromMode(const std::string& mode);
    std::string InferModeFromMatchState(const MatchState& match, MmrCategory rosterCategory);
    std::string InferModeFromMatchState(const LiveMatchSnapshot& match, MmrCategory rosterCategory);
    MmrCategory CategoryFromMatchContext(const MatchState& match);
    MmrCategory CategoryFromMatchContext(const LiveMatchSnapshot& match);
    bool IsSupportedMmrCategory(MmrCategory category);
    bool IsTrackedRankedEarlyExitMode(const std::string& mode);
    std::string FormatPendingMatchHistoryMode(const std::string& mode);
    std::string Lowercase(std::string value);

    inline int64_t SteadyNowMs() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        return ms > 0 ? ms : 1;
    }

    bool AcceptsUiMatchEvent(MatchState& state, const std::string& incomingGuid, int64_t nowMs, SideEffects& effects);
    bool TargetsCurrentActiveMatchUi(MatchState& state, const std::string& incomingGuid);
    void RequeueEncounterRecords(MatchState& state, SideEffects& effects);

    void HandleUpdateState(MatchState& state, const TelemetryEvents::UpdateStateEvent& ev, const ReducerContext& ctx, SideEffects& effects);
    void HandleStatfeed(MatchState& state, const TelemetryEvents::StatfeedEvent& ev, const ReducerContext& ctx);
    void HandleGoalScored(MatchState& state, const TelemetryEvents::GoalScoredEvent& ev, const ReducerContext& ctx, SideEffects& effects);
    void HandleBallHit(MatchState& state, const TelemetryEvents::BallHitEvent& ev, const ReducerContext& ctx);
    void HandleCrossbarHit(MatchState& state, const TelemetryEvents::CrossbarHitEvent& ev, const ReducerContext& ctx);
    // Analytics only follows live, recordable play of the current match; replays are excluded unless duringReplay.
    bool AcceptsAnalyticsEvent(const MatchState& state, const std::string& eventMatchGuid, bool duringReplay = false);
    void HandleMatchEnded(MatchState& state, const TelemetryEvents::MatchEndedEvent& ev, const ReducerContext& ctx, SideEffects& effects);
    void HandleMatchDestroyed(MatchState& state, const TelemetryEvents::MatchDestroyedEvent& ev, const ReducerContext& ctx, SideEffects& effects);

    void CapturePreMatchMmr(MatchState& state);
    bool AttachTerminalGuidToCurrent(MatchState& state, const std::string& eventMatchGuid);
    void UpdateLifecycleSignals(MatchState& state, const TelemetryEvents::UpdateStateEvent& ev);
    void MarkDestroyedMatchVoid(MatchState& state, const std::string& reason, SideEffects& effects);
    void RecordTerminalMatchGuid(MatchState& state, const std::string& matchGuid);
    void FinalizeCapturedMatch(MatchState& state, CapturedMatch match, int winnerTeam, TelemetryReducer::MatchFinalizeSource source, bool enqueueMmrRefresh, SideEffects& effects, const ReducerContext& ctx);
    bool BuildPostMatchMmrRefresh(const MatchState& state, const CapturedMatch& match, bool won, PostMatchMmrRefresh& refresh);
    int LegacyExpectedTeamSizeForMode(const std::string& mode);
    // MatchState owns the overlay visibility timestamps; the UI mirror is rewritten from it after every event.
    void PublishUiTimestamps(MatchState& state, SideEffects& effects);
}
