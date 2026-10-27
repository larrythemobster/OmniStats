#include "core/reducer/TelemetryReducer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <utility>

#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"
#include "utils/PrivacyLog.hpp"

using namespace TelemetryReducerDetail;

namespace TelemetryReducerDetail {

    namespace {

        bool IsSelf(const MatchState& state, const std::string& name) {
            if (name.empty() || state.myPrimaryId.empty()) return false;
            for (const auto& [pid, player] : state.roster) {
                if (player.name == name) return pid == state.myPrimaryId;
            }
            return false;
        }

        bool IsSelfById(const MatchState& state, const std::string& pid) {
            return !pid.empty() && pid == state.myPrimaryId;
        }

        std::string ResolveRosterPlayerId(const MatchState& state, const TelemetryEvents::PlayerRef& target) {
            if (!target.primaryId.empty()) {
                return target.primaryId;
            }
            if (target.name.empty()) return {};
            std::string match;
            for (const auto& [pid, player] : state.roster) {
                if (player.name != target.name || (target.team != -1 && player.team != target.team)) continue;
                if (!match.empty()) return {};
                match = pid;
            }
            return match;
        }

        void RecordJournalEvent(MatchState& state,
                                const std::string& type,
                                const std::string& guid,
                                const std::string& actorId,
                                const std::string& actorName,
                                const std::string& secondaryId,
                                const std::string& secondaryName,
                                const char* source,
                                std::optional<double> matchClockSeconds = std::nullopt,
                                std::optional<double> speed = std::nullopt,
                                std::optional<double> value = std::nullopt,
                                const std::string& eventId = "") {
            if (!state.inMatch || state.matchFinalized || state.inReplay || state.nonLiveReplayActive ||
                state.fallbackNonRecordableContext || PlaylistMetadata::IsNonRecordable(state.playlistId) ||
                state.matchEvents.size() >= 512) return;
            if (state.missingGuidAssociationBlockedByReconnect && guid.empty() && state.matchGuid.empty()) return;
            if (!guid.empty() && !state.matchGuid.empty() && guid != state.matchGuid) return;
            if (!guid.empty() && (state.finalizedMatchGuids.count(guid) || state.pendingDestroyedMatches.count(guid))) return;

            MatchJournalEvent event;
            event.type = type;
            event.source = source;
            event.observedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count();
            event.actorId = actorId;
            event.actorName = actorName;
            event.secondaryId = secondaryId;
            event.secondaryName = secondaryName;

            const auto rosterIt = state.roster.find(actorId);
            if (rosterIt != state.roster.end() && (rosterIt->second.team == 0 || rosterIt->second.team == 1)) {
                event.team = rosterIt->second.team;
            }
            if (matchClockSeconds.has_value()) {
                event.matchClockSeconds = *matchClockSeconds;
            } else if (state.hasObservedMatchClock) {
                event.matchClockSeconds = static_cast<double>(state.timeSeconds);
            }
            event.ballSpeed = speed;
            event.value = value;

            if (!eventId.empty()) {
                event.eventKey = std::string(source) + ":" + type + ":" + eventId;
                if (!state.eventNativeKeys.insert(event.eventKey).second) return;
            }
            state.matchEvents.push_back(std::move(event));
        }

    } // namespace

    void HandleStatfeed(MatchState& state,
                        const TelemetryEvents::StatfeedEvent& ev,
                        const ReducerContext& ctx) {
        (void)ctx;
        if (state.inReplay) return;
        const std::string& feedEvent = ev.eventName;
        if (feedEvent.empty()) return;

        std::string mainName = ev.mainTarget.name;
        std::string mainId = ResolveRosterPlayerId(state, ev.mainTarget);
        std::string secName;
        std::string secId;
        if (ev.secondaryTarget.has_value()) {
            secName = ev.secondaryTarget->name;
            secId = ResolveRosterPlayerId(state, *ev.secondaryTarget);
        }

        const bool isMainSelf = !mainId.empty() ? IsSelfById(state, mainId) : IsSelf(state, mainName);
        const bool isSecSelf = !secId.empty() ? IsSelfById(state, secId) : IsSelf(state, secName);

        const std::string journalType =
            (feedEvent == "EpicSave" || feedEvent == "Epic Save") ? "epic_save" : feedEvent == "Save"   ? "save"
                                                                              : feedEvent == "Shot"     ? "shot"
                                                                              : feedEvent == "Assist"   ? "assist"
                                                                              : feedEvent == "OwnGoal"  ? "own_goal"
                                                                              : feedEvent == "Demolish" ? "demolition"
                                                                                                        : "";
        if (!journalType.empty()) {
            RecordJournalEvent(state,
                               journalType,
                               ev.matchGuid,
                               mainId,
                               mainName,
                               secId,
                               secName,
                               "statfeed",
                               std::nullopt,
                               std::nullopt,
                               std::nullopt,
                               ev.eventId);
        }

        if (feedEvent == "Save" || feedEvent == "EpicSave" || feedEvent == "Epic Save") {
            state.currentMatch.saves++;
            if (isMainSelf) state.currentMatch.savesSelf++;
            if (!mainId.empty() && state.roster.count(mainId)) state.roster[mainId].saves++;
        } else if (feedEvent == "Shot") {
            state.currentMatch.shots++;
            if (isMainSelf) state.currentMatch.shotsSelf++;
            if (!mainId.empty() && state.roster.count(mainId)) state.roster[mainId].shots++;
        } else if (feedEvent == "Demolish") {
            state.currentMatch.demos++;
            if (isMainSelf) state.currentMatch.demosSelf++;
            if (isSecSelf) state.currentMatch.demoedSelf++;
            if (!mainId.empty() && state.roster.count(mainId)) state.roster[mainId].demos++;
            if (!secId.empty()) state.boostRespawnPending.insert(secId);
        } else if (feedEvent == "OwnGoal") {
            state.currentMatch.ownGoals++;
            if (isMainSelf) state.currentMatch.ownGoalsSelf++;
            std::cout << "[Event] OWN GOAL by " << PrivacyLog::Sensitive(mainName, "player name") << "!\n";
        } else if (feedEvent == "Assist") {
            state.currentMatch.assists++;
            if (isMainSelf) state.currentMatch.assistsSelf++;
            if (!mainId.empty() && state.roster.count(mainId)) state.roster[mainId].assists++;
        }
    }

    void HandleGoalScored(MatchState& state,
                          const TelemetryEvents::GoalScoredEvent& ev,
                          const ReducerContext& ctx,
                          SideEffects& effects) {
        // Skipped replays can deliver the replay-end echo outside replay state; it is not a new goal.
        if (ev.isEcho) return;
        if (state.inReplay) return;
        const int64_t nowMs = ctx.nowMs > 0 ? ctx.nowMs : SteadyNowMs();
        if (AcceptsUiMatchEvent(state, ev.matchGuid, nowMs, effects)) {
            state.lastGoalMs = nowMs;
            state.countdownSeenThisRound = false;
            effects.uiUpdate = UiStateUpdate{.lastGoalMs = nowMs};
        }

        const std::string& goalGuid = ev.matchGuid;
        const bool staleGoal =
            state.nonLiveReplayActive ||
            (!goalGuid.empty() &&
             (state.finalizedMatchGuids.count(goalGuid) > 0 || state.pendingDestroyedMatches.count(goalGuid) > 0 ||
              (state.inMatch && !state.matchFinalized && !state.matchGuid.empty() && goalGuid != state.matchGuid)));
        if (staleGoal) return;

        state.roundActive = false;
        state.currentMatch.goals++;

        std::string scorerName = ev.scorer.name;
        std::string scorerId = ResolveRosterPlayerId(state, ev.scorer);
        const bool isScorerSelf = !scorerId.empty() ? IsSelfById(state, scorerId) : IsSelf(state, scorerName);

        RecordJournalEvent(state,
                           "goal",
                           goalGuid,
                           scorerId,
                           scorerName,
                           ev.assister.has_value() ? ResolveRosterPlayerId(state, *ev.assister) : std::string{},
                           ev.assister.has_value() ? ev.assister->name : std::string{},
                           "goal",
                           ev.roundSeconds,
                           ev.goalSpeed && *ev.goalSpeed > 0.0 ? ev.goalSpeed : std::nullopt,
                           std::nullopt,
                           ev.eventId);

        if (isScorerSelf) state.currentMatch.goalsSelf++;
        if (!scorerId.empty() && state.roster.count(scorerId)) {
            state.roster[scorerId].goals++;
        }

        if (ev.goalSpeed.has_value()) {
            const float currentSpeed = static_cast<float>(*ev.goalSpeed);
            if (std::isfinite(currentSpeed) && currentSpeed > 0.0f) {
                state.currentMatch.maxGoalSpeed = (std::max)(state.currentMatch.maxGoalSpeed, currentSpeed);
                if (isScorerSelf) {
                    state.currentMatch.maxGoalSpeedSelf = (std::max)(state.currentMatch.maxGoalSpeedSelf, currentSpeed);
                }
                if (!scorerId.empty() && state.roster.count(scorerId)) {
                    state.roster[scorerId].maxGoalSpeed = (std::max)(state.roster[scorerId].maxGoalSpeed, currentSpeed);
                }
                state.lastGoalSpeed = currentSpeed;
                if (isScorerSelf) state.lastGoalSpeedSelf = currentSpeed;
            }
        }

        if (ev.roundSeconds.has_value()) {
            const float time = *ev.roundSeconds;
            if (time > 0) {
                if (state.currentMatch.fastestGoalTime == 0.0f || time < state.currentMatch.fastestGoalTime) {
                    state.currentMatch.fastestGoalTime = time;
                }
                if (isScorerSelf) {
                    if (state.currentMatch.fastestGoalTimeSelf == 0.0f || time < state.currentMatch.fastestGoalTimeSelf) {
                        state.currentMatch.fastestGoalTimeSelf = time;
                    }
                }
                if (!scorerId.empty() && state.roster.count(scorerId)) {
                    if (state.roster[scorerId].fastestGoalTime == 0.0f || time < state.roster[scorerId].fastestGoalTime) {
                        state.roster[scorerId].fastestGoalTime = time;
                    }
                }
            }
        }

        std::cout << "[Event] GOAL SCORED!\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
    }

    void HandleBallHit(MatchState& state,
                       const TelemetryEvents::BallHitEvent& ev,
                       const ReducerContext& ctx) {
        (void)ctx;
        const float speed = ev.postHitSpeed;
        state.currentMatch.maxBallSpeed = (std::max)(state.currentMatch.maxBallSpeed, speed);

        for (const auto& h : ev.hitters) {
            const std::string pid = ResolveRosterPlayerId(state, h);
            if (!pid.empty() ? IsSelfById(state, pid) : IsSelf(state, h.name)) {
                state.currentMatch.maxBallSpeedSelf = (std::max)(state.currentMatch.maxBallSpeedSelf, speed);
                break;
            }
        }
    }

    void HandleCrossbarHit(MatchState& state,
                           const TelemetryEvents::CrossbarHitEvent& ev,
                           const ReducerContext& ctx) {
        (void)ctx;
        const std::string toucherId = ResolveRosterPlayerId(state, ev.lastTouch);
        const bool isToucherSelf =
            !toucherId.empty() ? IsSelfById(state, toucherId) : IsSelf(state, ev.lastTouch.name);
        state.currentMatch.crossbars++;
        if (isToucherSelf) state.currentMatch.crossbarsSelf++;
        if (ev.impactForce.has_value()) {
            const float impactForce = static_cast<float>(*ev.impactForce);
            state.currentMatch.maxImpactForce = (std::max)(state.currentMatch.maxImpactForce, impactForce);
            if (isToucherSelf) {
                state.currentMatch.maxImpactForceSelf = (std::max)(state.currentMatch.maxImpactForceSelf, impactForce);
            }
        }
        RecordJournalEvent(state,
                           "crossbar",
                           ev.matchGuid,
                           toucherId,
                           ev.lastTouch.name,
                           "",
                           "",
                           "crossbar",
                           std::nullopt,
                           std::nullopt,
                           ev.impactForce,
                           ev.eventId);
    }
} // namespace TelemetryReducerDetail
