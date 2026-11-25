#include "core/reducer/TelemetryReducer.hpp"

#include <iostream>
#include <utility>

#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"
#include "core/state/SessionState.hpp"

#include <ctime>

using namespace TelemetryReducerDetail;

ReducerContext MakeReducerContext(const UIState& ui) {
    return MakeReducerContext(ui, ReducerSettings(Config::Read()));
}

ReducerContext MakeReducerContext(const UIState& ui, const ReducerSettings& settings) {
    const auto steadyNow = std::chrono::steady_clock::now();
    const auto systemNow = std::chrono::system_clock::now();
    const int64_t nowUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(systemNow.time_since_epoch()).count();
    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(steadyNow.time_since_epoch()).count();
    return ReducerContext{
        .nowMs = nowMs > 0 ? nowMs : 1,
        .nowSeconds = std::chrono::duration<double>(steadyNow.time_since_epoch()).count(),
        .nowUnix = nowUnixMs / 1000,
        .nowUnixMs = nowUnixMs,
        .config = settings,
        .rosterMmrCategory = ui.rosterMmrCategory.load(std::memory_order_relaxed),
        .rankProgressMmrCategory = ui.rankProgressMmrCategory.load(std::memory_order_relaxed),
        .graphMmrCategory = ui.graphMmrCategory.load(std::memory_order_relaxed)};
}

namespace TelemetryReducerDetail {

    bool AcceptsUiMatchEvent(MatchState& state, const std::string& incomingGuid, int64_t nowMs) {
        if (state.nonLiveReplayActive) return false;
        if (!incomingGuid.empty()) {
            if (state.finalizedMatchGuids.count(incomingGuid) > 0 ||
                state.pendingDestroyedMatches.count(incomingGuid) > 0) {
                return false;
            }
            if (!state.matchGuid.empty() && state.inMatch &&
                !state.matchFinalized && incomingGuid != state.matchGuid) {
                return false;
            }
            if ((!state.uiMatchGuid.empty() && incomingGuid != state.uiMatchGuid) ||
                state.matchFinalized) {
                state.lastMatchStartMs = nowMs;
                state.firstCountdownOfMatchMs = 0;
                state.lastCountdownMs = 0;
                state.lastGoalMs = 0;
                state.lastMatchEndMs = 0;
                state.lastPodiumMs = 0;
                state.inGoalReplay = false;
                state.countdownSeenThisRound = false;
            }
            state.uiMatchGuid = incomingGuid;
            return true;
        }
        if (state.matchFinalized) {
            return false;
        }
        return true;
    }

    bool TargetsCurrentActiveMatchUi(MatchState& state, const std::string& incomingGuid) {
        if (!state.inMatch || state.nonLiveReplayActive) {
            return false;
        }
        if (!incomingGuid.empty()) {
            if ((!state.matchGuid.empty() && incomingGuid != state.matchGuid) ||
                (!state.uiMatchGuid.empty() && incomingGuid != state.uiMatchGuid) ||
                state.pendingDestroyedMatches.count(incomingGuid) > 0) {
                return false;
            }
            if (state.matchGuid.empty() && state.uiMatchGuid.empty() &&
                state.finalizedMatchGuids.count(incomingGuid) > 0) {
                return false;
            }
            if (state.uiMatchGuid.empty()) state.uiMatchGuid = incomingGuid;
            return true;
        }
        if (!state.pendingDestroyedMatches.empty() || state.missingGuidAssociationBlockedByReconnect) {
            return false;
        }
        return true;
    }

    void RequeueEncounterRecords(MatchState& state, SideEffects& effects) {
        for (auto& [pid, player] : state.roster) {
            player.lifetimeWinsWith = 0;
            player.lifetimeLossesWith = 0;
            player.lifetimeWinsAgainst = 0;
            player.lifetimeLossesAgainst = 0;
            player.hasLifetimeData = false;
            if (pid.rfind("Unknown|", 0) != 0) effects.fetchEncounterQueue.push_back(pid);
        }
    }

} // namespace TelemetryReducerDetail

void TelemetryReducer::OnTelemetryDisconnected(MatchState& state) {
    if (state.inMatch && !state.matchFinalized) {
        state.missingGuidAssociationBlockedByReconnect = true;
        std::cout
            << "[TelemetryReducer] Telemetry disconnected: "
            << "currentGeneration="
            << state.activeMatchGeneration
            << ", action=block-missing-guid-match-end.\n";
    }
}

DiscordPresenceSnapshot TelemetryReducer::BuildDiscordSnapshot(const MatchState& state) {
    DiscordPresenceSnapshot snapshot;
    snapshot.showPresence = true;
    snapshot.inMatch = state.inMatch;
    snapshot.matchGuid = state.matchGuid;
    snapshot.arenaName = state.arenaName;
    snapshot.score[0] = state.score[0];
    snapshot.score[1] = state.score[1];
    snapshot.myTeam = state.myTeam;
    snapshot.sessionWins = state.sessionTotals.wins;
    snapshot.sessionLosses = state.sessionTotals.losses;
    return snapshot;
}

ReducerResult TelemetryReducer::Reduce(MatchState state,
                                       const TelemetryEvents::TelemetryEvent& event,
                                       const ReducerContext& context) {
    SideEffects effects = ReduceInPlace(state, event, context);
    return {std::move(state), std::move(effects)};
}

SideEffects TelemetryReducer::ReduceInPlace(MatchState& state,
                                            const TelemetryEvents::TelemetryEvent& event,
                                            const ReducerContext& context) {
    SideEffects effects;
    state.currentReducerNowMs = context.nowMs;
    state.currentReducerNowUnixMs =
        context.nowUnixMs != 0 && (context.nowUnix == 0 || context.nowUnixMs / 1000 == context.nowUnix)
            ? context.nowUnixMs
            : context.nowUnix * 1000;

    std::visit([&](const auto& ev) {
        using T = std::decay_t<decltype(ev)>;

        if constexpr (std::is_same_v<T, TelemetryEvents::ReplayCreatedEvent>) {
            state.nonLiveReplayActive = true;
            state.inReplay = true;
            state.inGoalReplay = false;
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchCreatedEvent>) {
            const int64_t nowMs = context.nowMs;
            const std::string incomingGuid = ev.matchGuid;
            const bool wasInMatch = state.inMatch;
            const bool wasFinalized = state.matchFinalized;
            const bool hadPriorUiCountdown =
                state.firstCountdownOfMatchMs > 0 || state.lastMatchEndMs > 0;
            const bool hasIncomingGuid = !incomingGuid.empty();
            const bool hadCurrentGuid = !state.matchGuid.empty();
            const bool isSameActiveUiGuid =
                hasIncomingGuid && incomingGuid == state.uiMatchGuid &&
                wasInMatch && !wasFinalized;
            const uint64_t currentGeneration = state.activeMatchGeneration;
            const bool currentIsNonRecordable =
                wasInMatch &&
                (state.fallbackNonRecordableContext ||
                 PlaylistMetadata::IsNonRecordable(state.playlistId));
            const bool startsNewLifecycle =
                !wasInMatch ||
                (hasIncomingGuid && currentIsNonRecordable) ||
                (hasIncomingGuid && hadCurrentGuid && incomingGuid != state.matchGuid);
            const bool attachesGuid =
                wasInMatch && hasIncomingGuid &&
                !hadCurrentGuid && !currentIsNonRecordable;

            const char* action = "preserve-current";
            const char* reason = hasIncomingGuid ? "same-explicit-guid" : "missing-guid-active-lifecycle";

            if (startsNewLifecycle) {
                action = "start-new";
                reason = wasInMatch
                             ? (currentIsNonRecordable ? "non-recordable-transition" : "different-explicit-guid")
                             : "no-active-match";

                if (!context.config.reset_session_on_close) {
                    effects.closeSessionIfInactiveAtUnix = context.nowUnix;
                }

                LocalPreMatchMmrSnapshot initialMmrSnapshot;
                bool hasInitialMmrSnapshot = false;
                if (!state.myPrimaryId.empty()) {
                    const auto playerIt = state.roster.find(state.myPrimaryId);
                    if (playerIt != state.roster.end() && !playerIt->second.playlists.empty()) {
                        initialMmrSnapshot.playlistMmrs = playerIt->second.playlists;
                        initialMmrSnapshot.playlistMatches = playerIt->second.playlistMatches;
                        hasInitialMmrSnapshot = true;
                    }
                }

                const std::string currentArena = state.arenaName;
                state.ResetMatch(currentArena);
                state.activeMatchGeneration = ++state.nextMatchGeneration;
                state.matchGuid = incomingGuid;
                state.missingGuidAssociationBlockedByReconnect = false;
                if (hasInitialMmrSnapshot) {
                    state.preMatchMmrByGuid.emplace(incomingGuid, std::move(initialMmrSnapshot));
                }
                state.roundActive = false;
                state.autoSwitchedPlaylistCategory = MmrCategory::Best;
                state.followedGraphPlaylistCategory = MmrCategory::Best;
                static_cast<MatchLedger&>(state) = {};
                state.lastPlayerBoost.clear();
                state.boostRespawnPending.clear();
                state.lastPlayerSeenMs.clear();
                state.ResetTiming();
            } else if (attachesGuid) {
                action = "attach-guid";
                reason = "late-guid-enrichment";
                auto guidlessSnapshot = state.preMatchMmrByGuid.extract("");
                if (!guidlessSnapshot.empty()) {
                    guidlessSnapshot.key() = incomingGuid;
                    state.preMatchMmrByGuid.insert(std::move(guidlessSnapshot));
                }
                state.matchGuid = incomingGuid;
                CapturePreMatchMmr(state);
            }

            if (!isSameActiveUiGuid) {
                if (startsNewLifecycle || wasFinalized || hadPriorUiCountdown ||
                    (hasIncomingGuid && !state.uiMatchGuid.empty() && incomingGuid != state.uiMatchGuid)) {
                    state.lastMatchStartMs = nowMs;
                    state.firstCountdownOfMatchMs = 0;
                    state.lastCountdownMs = 0;
                    state.lastGoalMs = 0;
                    state.lastMatchEndMs = 0;
                    state.lastPodiumMs = 0;
                    state.inGoalReplay = false;
                    state.countdownSeenThisRound = false;
                } else {
                    state.lastMatchStartMs = nowMs;
                    state.inGoalReplay = false;
                }
                state.uiMatchGuid = incomingGuid;
            } else {
                if (state.lastMatchStartMs <= 0) {
                    state.lastMatchStartMs = nowMs;
                }
                state.inGoalReplay = false;
            }

            std::cout
                << "[TelemetryReducer] MatchCreated: "
                << "incomingGuid=" << (hasIncomingGuid ? "present" : "missing")
                << ", currentGuid=" << (hadCurrentGuid ? "present" : "missing")
                << ", currentGeneration=" << currentGeneration
                << ", inMatch=" << (wasInMatch ? "true" : "false")
                << ", action=" << action
                << ", reason=" << reason << ".\n";

            if (startsNewLifecycle || attachesGuid) {
                effects.pushDiscord = true;
                effects.discordSnapshot = BuildDiscordSnapshot(state);
            }
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchInitializedEvent>) {
            const int64_t nowMs = context.nowMs;
            if (AcceptsUiMatchEvent(state, ev.matchGuid, nowMs)) {
                if (state.lastMatchStartMs <= 0) {
                    state.lastMatchStartMs = nowMs;
                }
                if (state.firstCountdownOfMatchMs <= 0) {
                    state.firstCountdownOfMatchMs = nowMs;
                }
                if (state.lastCountdownMs <= 0) {
                    state.lastCountdownMs = nowMs;
                }
                state.inGoalReplay = false;
                state.countdownSeenThisRound = true;
            }
        } else if constexpr (std::is_same_v<T, TelemetryEvents::CountdownBeginEvent>) {
            const int64_t nowMs = context.nowMs;
            if (AcceptsUiMatchEvent(state, ev.matchGuid, nowMs)) {
                if (state.lastMatchStartMs <= 0) state.lastMatchStartMs = nowMs;
                if (state.firstCountdownOfMatchMs <= 0) state.firstCountdownOfMatchMs = nowMs;
                state.lastCountdownMs = nowMs;
                state.inGoalReplay = false;
                state.countdownSeenThisRound = true;
                state.roundActive = false;
                state.lastPlayerBoost.clear();
            }
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnCountdownBegin();
        } else if constexpr (std::is_same_v<T, TelemetryEvents::RoundStartedEvent>) {
            const int64_t nowMs = context.nowMs;
            if (AcceptsUiMatchEvent(state, ev.matchGuid, nowMs)) {
                if (state.lastMatchStartMs <= 0) state.lastMatchStartMs = nowMs;
                if (state.firstCountdownOfMatchMs <= 0) state.firstCountdownOfMatchMs = nowMs;
                if (!state.countdownSeenThisRound) state.lastCountdownMs = nowMs;
                state.inGoalReplay = false;
                state.countdownSeenThisRound = false;
            }
            if (!state.roundActive) {
                state.lastPlayerBoost.clear();
                state.boostRespawnPending.clear();
            }
            state.roundActive = true;
            state.roundEverStarted = true;
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnRoundStarted(context.nowSeconds);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchPausedEvent>) {
            if (!state.currentPauseStartMs.has_value()) {
                state.currentPauseStartMs = context.nowMs;
            }
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnPaused(context.nowSeconds);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchUnpausedEvent>) {
            const int64_t nowMs = context.nowMs;
            if (state.currentPauseStartMs.has_value()) {
                state.pauseIntervals.push_back({*state.currentPauseStartMs, nowMs});
                state.currentPauseStartMs.reset();
            }
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnUnpaused(context.nowSeconds);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::GoalReplayStartEvent>) {
            if (TargetsCurrentActiveMatchUi(state, ev.matchGuid)) state.inGoalReplay = true;
        } else if constexpr (std::is_same_v<T, TelemetryEvents::GoalReplayWillEndEvent>) {
            // Fires when the ball explodes while the replay is still on screen; inGoalReplay stays set.
        } else if constexpr (std::is_same_v<T, TelemetryEvents::GoalReplayEndEvent>) {
            const std::string& incomingGuid = ev.matchGuid;
            if (!incomingGuid.empty() &&
                ((!state.matchGuid.empty() && incomingGuid != state.matchGuid) ||
                 (!state.uiMatchGuid.empty() && incomingGuid != state.uiMatchGuid) ||
                 state.pendingDestroyedMatches.count(incomingGuid) > 0)) {
                return;
            }
            state.inGoalReplay = false;
        } else if constexpr (std::is_same_v<T, TelemetryEvents::PodiumStartEvent>) {
            if (TargetsCurrentActiveMatchUi(state, ev.matchGuid)) {
                state.inGoalReplay = false;
                if (state.lastPodiumMs <= 0) {
                    const int64_t nowMs = context.nowMs;
                    state.lastPodiumMs = nowMs;
                    if (state.showMatchSummary || state.matchFinalized) {
                        state.matchSummaryStartMs = nowMs;
                    }
                }
            }
        } else if constexpr (std::is_same_v<T, TelemetryEvents::UpdateStateEvent>) {
            const std::string preArena = state.arenaName;
            const std::array<int, 2> preScore = state.score;
            const bool preInMatch = state.inMatch;
            HandleUpdateState(state, ev, context, effects);
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnUpdateState(ev, context.nowSeconds);
            if (effects.pushDiscord ||
                state.arenaName != preArena || state.score != preScore || state.inMatch != preInMatch) {
                effects.pushDiscord = true;
                effects.discordSnapshot = BuildDiscordSnapshot(state);
            }
        } else if constexpr (std::is_same_v<T, TelemetryEvents::StatfeedEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnStatfeed(ev);
            HandleStatfeed(state, ev, context);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::GoalScoredEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid, true)) state.analytics.OnGoalScored(ev);
            HandleGoalScored(state, ev, context, effects);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::BallHitEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) {
                state.analytics.OnBallHit(ev, context.nowSeconds);
                const auto firstTouchSeconds = state.analytics.LatestKickoffFirstTouchSeconds();
                auto& fastest = state.currentMatch.kickoffFirstTouchSeconds;
                if (firstTouchSeconds && (!fastest || *firstTouchSeconds < *fastest)) fastest = firstTouchSeconds;
            }
            HandleBallHit(state, ev, context);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::BoostPickupEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnBoostPickup(ev);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::PlayerLeftEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnPlayerLeft(ev);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::CrossbarHitEvent>) {
            HandleCrossbarHit(state, ev, context);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchEndedEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnMatchEnded();
            HandleMatchEnded(state, ev, context, effects);
        } else if constexpr (std::is_same_v<T, TelemetryEvents::MatchDestroyedEvent>) {
            if (AcceptsAnalyticsEvent(state, ev.matchGuid)) state.analytics.OnMatchEnded();
            HandleMatchDestroyed(state, ev, context, effects);
        }
    },
               event);

    TelemetryReducerDetail::PublishUiTimestamps(state, effects);
    return effects;
}
