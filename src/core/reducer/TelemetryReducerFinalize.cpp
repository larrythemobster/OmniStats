#include "core/reducer/TelemetryReducer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <type_traits>
#include <utility>

#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/Insights.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"
#include "utils/PrivacyLog.hpp"

using namespace TelemetryReducerDetail;

namespace TelemetryReducerDetail {

    void RecordTerminalMatchGuid(MatchState& state, const std::string& matchGuid) {
        if (!matchGuid.empty()) {
            state.finalizedMatchGuids.insert(matchGuid);
        }
    }

    void MarkDestroyedMatchVoid(MatchState& state,
                                const std::string& reason,
                                SideEffects& effects) {
        RecordTerminalMatchGuid(state, state.matchGuid);
        state.lastMatchWasVoid = true;
        state.lastMatchVoidReason = reason;
        state.matchSummaryScore = state.score;
        state.matchSummaryMyTeam = state.myTeam;
        state.matchSummaryWinnerTeam = -1;
        state.matchSummaryOvertimeSeconds = 0.0f;
        state.matchFinalized = true;
        std::cout << "[Event] MATCH VOIDED: " << reason << "\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
    }

    void AddMatchToSessionTotals(const CapturedMatch& match,
                                 bool iWon,
                                 SessionTotals& sessionTotals,
                                 std::map<std::string, GamemodeStat>& sessionGamemodes) {
        if (iWon) {
            sessionTotals.wins++;
        } else {
            sessionTotals.losses++;
        }

        const auto& currentMatch = match.stats;
        sessionTotals.goals += currentMatch.goalsSelf;
        sessionTotals.saves += currentMatch.savesSelf;
        sessionTotals.savesTotal += currentMatch.saves;
        sessionTotals.shots += currentMatch.shotsSelf;
        sessionTotals.shotsTotal += currentMatch.shots;
        sessionTotals.demos += currentMatch.demosSelf;
        sessionTotals.demosTotal += currentMatch.demos;
        sessionTotals.demoed += currentMatch.demoedSelf;
        sessionTotals.crossbars += currentMatch.crossbarsSelf;
        sessionTotals.crossbarsTotal += currentMatch.crossbars;
        sessionTotals.assists += currentMatch.assistsSelf;
        sessionTotals.assistsTotal += currentMatch.assists;
        sessionTotals.boostPickedUp += currentMatch.boostPickedUpSelf;
        sessionTotals.maxGoalSpeed = (std::max)(sessionTotals.maxGoalSpeed, currentMatch.maxGoalSpeed);
        sessionTotals.maxGoalSpeedSelf = (std::max)(sessionTotals.maxGoalSpeedSelf, currentMatch.maxGoalSpeedSelf);
        sessionTotals.maxBallSpeed = (std::max)(sessionTotals.maxBallSpeed, currentMatch.maxBallSpeed);
        sessionTotals.maxBallSpeedSelf = (std::max)(sessionTotals.maxBallSpeedSelf, currentMatch.maxBallSpeedSelf);
        sessionTotals.maxImpactForce = (std::max)(sessionTotals.maxImpactForce, currentMatch.maxImpactForce);
        sessionTotals.maxImpactForceSelf = (std::max)(sessionTotals.maxImpactForceSelf, currentMatch.maxImpactForceSelf);
        if (currentMatch.fastestGoalTime > 0.0f &&
            (sessionTotals.fastestGoalTime == 0.0f || currentMatch.fastestGoalTime < sessionTotals.fastestGoalTime)) {
            sessionTotals.fastestGoalTime = currentMatch.fastestGoalTime;
        }
        if (currentMatch.fastestGoalTimeSelf > 0.0f &&
            (sessionTotals.fastestGoalTimeSelf == 0.0f || currentMatch.fastestGoalTimeSelf < sessionTotals.fastestGoalTimeSelf)) {
            sessionTotals.fastestGoalTimeSelf = currentMatch.fastestGoalTimeSelf;
        }
        sessionTotals.ownGoals += currentMatch.ownGoals;
        sessionTotals.ownGoalsSelf += currentMatch.ownGoalsSelf;

        if (match.mode != "1v1") {
            int teamGoalsThisMatch = 0;
            if (match.myTeam == 0 || match.myTeam == 1) {
                teamGoalsThisMatch = match.score[match.myTeam];
            }
            if (teamGoalsThisMatch <= 0) {
                for (const auto& [primaryId, player] : match.roster) {
                    if (player.team == match.myTeam) {
                        teamGoalsThisMatch += player.goals;
                    }
                }
            }
            const int participationThisMatch = std::clamp(
                currentMatch.goalsSelf + currentMatch.assistsSelf,
                0,
                (std::max)(0, teamGoalsThisMatch));
            sessionTotals.teamGoals += (std::max)(0, teamGoalsThisMatch);
            sessionTotals.goalParticipations += participationThisMatch;
        }

        if (!PlaylistMetadata::IsCasual(match.playlistId) &&
            !(!PlaylistMetadata::HasAuthoritativeId(match.playlistId) &&
              (match.fallbackCasualContext || match.fallbackNonRecordableContext)) &&
            GamemodeUtils::IsTrackedCompetitiveMode(match.mode)) {
            auto& gamemode = sessionGamemodes[match.mode];
            if (iWon) {
                gamemode.wins++;
            } else {
                gamemode.losses++;
            }
            gamemode.total++;
        }
    }

    void FinalizeCapturedMatch(MatchState& state,
                               CapturedMatch match,
                               int winnerTeam,
                               TelemetryReducer::MatchFinalizeSource source,
                               bool enqueueMmrRefresh,
                               SideEffects& effects,
                               const ReducerContext& ctx) {
        if (!match.matchGuid.empty() && state.finalizedMatchGuids.count(match.matchGuid) != 0) {
            std::cout << "[TelemetryReducer] Skipping duplicate match save for guid.\n";
            return;
        }
        RecordTerminalMatchGuid(state, match.matchGuid);
        const auto decision = TelemetryReducer::ClassifyMatchEnd(match, winnerTeam, ctx.config);
        const bool isCurrentMatch =
            match.matchGeneration == state.activeMatchGeneration &&
            match.matchGuid == state.matchGuid;
        if (isCurrentMatch) {
            state.matchFinalized = true;
            state.lastMatchWasVoid = !decision.shouldCount;
            state.lastMatchVoidReason = decision.voidReason;
            state.matchSummaryScore = match.score;
            state.matchSummaryMyTeam = match.myTeam;
            state.matchSummaryWinnerTeam = winnerTeam;
            state.matchSummaryOvertimeSeconds = match.overtimeSeconds;
            const int64_t nowMs = ctx.nowMs > 0 ? ctx.nowMs : SteadyNowMs();
            if (state.lastMatchEndMs <= 0) {
                state.lastMatchEndMs = nowMs;
            }
            state.inGoalReplay = false;
            state.showMatchSummary = true;
            const int64_t podiumMs = state.lastPodiumMs;
            const int64_t matchStartMs = state.lastMatchStartMs;
            const int64_t summaryStartMs =
                (podiumMs > 0 && (matchStartMs <= 0 || podiumMs >= matchStartMs)) ? podiumMs : nowMs;
            state.matchSummaryStartMs = summaryStartMs;
            effects.uiUpdate = UiStateUpdate{
                .lastMatchEndMs = state.lastMatchEndMs,
                .inGoalReplay = false,
                .showMatchSummary = true,
                .matchSummaryStartMs = summaryStartMs};
        }

        if (!decision.shouldPersist) {
            if (!decision.voidReason.empty()) {
                std::cout << "[Event] MATCH VOIDED: " << decision.voidReason << "\n";
            }
            if (isCurrentMatch) {
                effects.pushDiscord = true;
                effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
            }
            state.ResetTiming();
            return;
        }

        const bool iWon = decision.iWon;

        const int64_t matchEndedUnix = match.endedAtUnixMs > 0 ? match.endedAtUnixMs / 1000 : 0;

        auto& recap = state.lastSessionRecap;
        if (match.sessionGeneration == state.sessionGeneration) {
            AddMatchToSessionTotals(match, iWon, state.sessionTotals, state.sessionGamemodes);
            if (matchEndedUnix > 0) {
                if (state.sessionStartedAtUnix <= 0 || matchEndedUnix < state.sessionStartedAtUnix) {
                    state.sessionStartedAtUnix = matchEndedUnix;
                }
                state.lastMatchEndedAtUnix = (std::max)(state.lastMatchEndedAtUnix, matchEndedUnix);
            }
            if (!match.matchGuid.empty() &&
                std::find(state.sessionMatchGuids.begin(), state.sessionMatchGuids.end(), match.matchGuid) ==
                    state.sessionMatchGuids.end()) {
                state.sessionMatchGuids.push_back(match.matchGuid);
            }
        } else if (recap.valid && recap.sessionGeneration == match.sessionGeneration) {
            AddMatchToSessionTotals(match, iWon, recap.totals, recap.gamemodes);
            if (!match.matchGuid.empty() &&
                std::find(recap.matchGuids.begin(), recap.matchGuids.end(), match.matchGuid) ==
                    recap.matchGuids.end()) {
                recap.matchGuids.push_back(match.matchGuid);
            }
            if (recap.account.empty() && !match.myPrimaryId.empty()) {
                recap.account = match.myPrimaryId;
            }
            effects.updateSession = recap;
        }

        for (const auto& [primaryId, player] : match.roster) {
            if (primaryId == match.myPrimaryId || primaryId.rfind("Unknown|", 0) == 0) continue;
            effects.fetchEncounterQueue.push_back(primaryId);
        }

        bool hasLocalRefresh = false;
        if (enqueueMmrRefresh) {
            PostMatchMmrRefresh refresh;
            hasLocalRefresh = BuildPostMatchMmrRefresh(state, match, iWon, refresh);
            if (hasLocalRefresh) {
                refresh.provisionalImmediately = (source == TelemetryReducer::MatchFinalizeSource::LocalForfeit);
                effects.postMatchMmrRefresh = std::move(refresh);
            }
            for (const auto& [primaryId, player] : match.roster) {
                if (player.team != match.myTeam) continue;
                if (hasLocalRefresh && primaryId == match.myPrimaryId) continue;
                effects.fetchMmrQueue.emplace_back(primaryId, player.name);
            }
        }

        MatchSaveSnapshot snapshot = TelemetryReducer::BuildMatchSaveSnapshot(match, winnerTeam, decision);
        snapshot.roster = std::move(match.roster);
        snapshot.localMmrNeedsReconciliation =
            hasLocalRefresh && (ctx.config.enable_mmr_tracking ||
                                (ctx.config.custom_api_enabled && !ctx.config.account_refresh_token.empty()));

        effects.saveMatch = true;
        effects.saveSnapshot = std::move(snapshot);

        if (isCurrentMatch) {
            effects.pushDiscord = true;
            effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
        }
        state.ResetTiming();
    }

    namespace {
        template <typename T>
        bool HasExplicitLocalForfeit(const T& event, int localTeam) {
            if (event.explicitLocalForfeitFlag) return true;
            if (!event.bForfeit) return false;
            if (event.forfeitTeam.has_value() &&
                (*event.forfeitTeam == 0 || *event.forfeitTeam == 1)) {
                return *event.forfeitTeam == localTeam;
            }
            if constexpr (std::is_same_v<T, TelemetryEvents::MatchEndedEvent>) {
                return (event.winnerTeam == 0 || event.winnerTeam == 1) &&
                       (localTeam == 0 || localTeam == 1) &&
                       event.winnerTeam != localTeam;
            } else {
                return event.winnerTeam.has_value() &&
                       (*event.winnerTeam == 0 || *event.winnerTeam == 1) &&
                       (localTeam == 0 || localTeam == 1) &&
                       *event.winnerTeam != localTeam;
            }
        }
    } // namespace
    void HandleMatchEnded(MatchState& state,
                          const TelemetryEvents::MatchEndedEvent& ev,
                          const ReducerContext& ctx,
                          SideEffects& effects) {
        const int winner = ev.winnerTeam;
        const std::string& eventMatchGuid = ev.matchGuid;
        const bool hasExplicitEventGuid = !eventMatchGuid.empty();
        const auto pendingIt = hasExplicitEventGuid
                                   ? state.pendingDestroyedMatches.find(eventMatchGuid)
                                   : state.pendingDestroyedMatches.end();
        if (hasExplicitEventGuid && pendingIt == state.pendingDestroyedMatches.end()) {
            AttachTerminalGuidToCurrent(state, eventMatchGuid);
        }

        if (pendingIt != state.pendingDestroyedMatches.end()) {
            if (winner != 0 && winner != 1) return;
            CapturedMatch match = std::move(pendingIt->second);
            state.pendingDestroyedMatches.erase(pendingIt);
            if (ev.hasTeamScores) match.score = ev.teamScores;
            const bool localForfeit = HasExplicitLocalForfeit(ev, match.myTeam);
            const bool won = winner == match.myTeam;
            const std::string resolvedGuid = match.matchGuid;
            FinalizeCapturedMatch(state,
                                  std::move(match),
                                  winner,
                                  localForfeit ? TelemetryReducer::MatchFinalizeSource::LocalForfeit
                                               : TelemetryReducer::MatchFinalizeSource::MatchEnded,
                                  false,
                                  effects,
                                  ctx);
            effects.resolvedDestroyedMatch = ResolvedDestroyedMatch{.matchGuid = resolvedGuid, .won = won};
            return;
        }

        bool targetsCurrent = hasExplicitEventGuid && eventMatchGuid == state.matchGuid;
        if (!hasExplicitEventGuid) {
            if (!state.inMatch || state.matchFinalized || !state.pendingDestroyedMatches.empty() ||
                !state.matchGuid.empty() || state.missingGuidAssociationBlockedByReconnect) {
                return;
            }
            targetsCurrent = true;
        }
        if (!targetsCurrent || !state.inMatch || state.matchFinalized ||
            (hasExplicitEventGuid && state.finalizedMatchGuids.count(eventMatchGuid) != 0)) {
            return;
        }

        const int64_t nowMs = ctx.nowMs > 0 ? ctx.nowMs : SteadyNowMs();
        state.inGoalReplay = false;
        state.lastMatchEndMs = nowMs;
        state.countdownSeenThisRound = false;
        state.roundActive = false;
        if (ev.hasTeamScores) state.score = ev.teamScores;
        const bool localForfeit = HasExplicitLocalForfeit(ev, state.myTeam);
        TelemetryReducer::FinalizeMatch(state,
                                        winner,
                                        localForfeit ? TelemetryReducer::MatchFinalizeSource::LocalForfeit
                                                     : TelemetryReducer::MatchFinalizeSource::MatchEnded,
                                        ctx,
                                        effects);
    }

    void HandleMatchDestroyed(MatchState& state,
                              const TelemetryEvents::MatchDestroyedEvent& ev,
                              const ReducerContext& ctx,
                              SideEffects& effects) {
        const std::string& eventMatchGuid = ev.matchGuid;
        const bool hasExplicitEventGuid = !eventMatchGuid.empty();
        if (hasExplicitEventGuid) AttachTerminalGuidToCurrent(state, eventMatchGuid);

        if (state.inMatch) {
            if ((hasExplicitEventGuid && eventMatchGuid != state.matchGuid) ||
                (!hasExplicitEventGuid && (!state.pendingDestroyedMatches.empty() ||
                                           !state.matchGuid.empty() ||
                                           state.missingGuidAssociationBlockedByReconnect))) {
                return;
            }
        }

        state.roundActive = false;
        if (HasExplicitLocalForfeit(ev, state.myTeam)) state.explicitLocalForfeit = true;
        if (ev.hasTeamScores) state.score = ev.teamScores;

        if (!state.inMatch) {
            return;
        }

        state.inGoalReplay = false;
        state.firstCountdownOfMatchMs = 0;
        state.lastCountdownMs = 0;
        state.lastGoalMs = 0;
        state.lastMatchStartMs = 0;
        state.uiMatchGuid.clear();
        state.countdownSeenThisRound = false;
        state.ResetTiming();

        const bool replayActiveAtDestruction = state.inReplay;
        const int authoritativeWinner = ev.winnerTeam.value_or(-1);
        if (!state.matchFinalized && (authoritativeWinner == 0 || authoritativeWinner == 1)) {
            TelemetryReducer::FinalizeMatch(
                state,
                authoritativeWinner,
                state.explicitLocalForfeit && authoritativeWinner != state.myTeam
                    ? TelemetryReducer::MatchFinalizeSource::LocalForfeit
                    : TelemetryReducer::MatchFinalizeSource::MatchDestroyed,
                ctx,
                effects);
        } else if (!state.matchFinalized) {
            std::string mode;
            std::string voidReason;
            const bool validEarlyCompetitiveExit =
                TelemetryReducer::IsValidEarlyCompetitiveExit(state, mode, voidReason);
            if (!validEarlyCompetitiveExit) {
                MarkDestroyedMatchVoid(state, voidReason, effects);
            } else if (state.explicitLocalForfeit) {
                TelemetryReducer::FinalizeMatch(
                    state,
                    1 - state.myTeam,
                    TelemetryReducer::MatchFinalizeSource::LocalForfeit,
                    ctx,
                    effects);
            } else {
                CapturedMatch pendingMatch = TelemetryReducer::CaptureMatch(state);
                pendingMatch.mode = mode;
                PostMatchMmrRefresh refresh;
                if (!BuildPostMatchMmrRefresh(state, pendingMatch, false, refresh)) {
                    MarkDestroyedMatchVoid(state, "missing_local_tracker_identity", effects);
                } else {
                    const int localTeam = pendingMatch.myTeam;
                    const std::array<int, 2> matchScore = pendingMatch.score;
                    const bool disappeared = pendingMatch.localPlayerDisappeared;
                    const int64_t destroyedAtMs = pendingMatch.endedAtUnixMs;
                    PendingDestroyedMatchMmrRefresh pending{
                        .primaryId = refresh.primaryId,
                        .name = refresh.name,
                        .matchGuid = refresh.matchGuid,
                        .playlist = refresh.playlist,
                        .localTeam = localTeam,
                        .score = matchScore,
                        .previousMmr = refresh.previousMmr,
                        .previousMatches = refresh.previousMatches,
                        .previousMmrIsPlaylistSpecific = refresh.previousMmrIsPlaylistSpecific,
                        .localPlayerDisappeared = disappeared,
                        .explicitLocalForfeit = pendingMatch.explicitLocalForfeit,
                        .destroyedAtUnixMs = destroyedAtMs,
                        .validCompetitiveMatch = true};

                    SessionMatchSummary pendingSummary;
                    pendingSummary.ranked = true;
                    pendingSummary.mode = FormatPendingMatchHistoryMode(pendingMatch.mode);
                    pendingSummary.matchGuid = pendingMatch.matchGuid;
                    pendingSummary.ourScore = localTeam == 1 ? matchScore[1] : matchScore[0];
                    pendingSummary.theirScore = localTeam == 1 ? matchScore[0] : matchScore[1];
                    pendingSummary.mmr = 0;
                    pendingSummary.win = pendingSummary.ourScore > pendingSummary.theirScore;
                    pendingSummary.pendingTrackerConfirmation = true;
                    pendingSummary.endedAtUnix = destroyedAtMs / 1000;
                    effects.pendingMatchHistory = std::move(pendingSummary);

                    if ((localTeam == 0 || localTeam == 1) &&
                        (disappeared || matchScore[0] != matchScore[1])) {
                        const bool provisionalWin =
                            !disappeared && matchScore[localTeam] > matchScore[1 - localTeam];
                        const int provisionalWinner = provisionalWin ? localTeam : 1 - localTeam;
                        const TelemetryReducer::MatchEndDecision decision =
                            TelemetryReducer::ClassifyMatchEnd(pendingMatch, provisionalWinner, ctx.config);
                        if (decision.shouldPersist) {
                            MatchSaveSnapshot provisional =
                                TelemetryReducer::BuildMatchSaveSnapshot(pendingMatch, provisionalWinner, decision);
                            provisional.roster = pendingMatch.roster;
                            provisional.localMmrNeedsReconciliation =
                                ctx.config.enable_mmr_tracking ||
                                (ctx.config.custom_api_enabled && !ctx.config.account_refresh_token.empty());
                            provisional.resultPending = true;
                            effects.provisionalSaveSnapshot = std::move(provisional);
                        }
                    }

                    const std::string pendingGuid = pendingMatch.matchGuid;
                    state.pendingDestroyedMatches.emplace(pendingGuid, std::move(pendingMatch));
                    state.lastMatchWasVoid = true;
                    state.lastMatchVoidReason = "destroyed_pending_tracker_confirmation";
                    state.matchSummaryScore = state.score;
                    state.matchSummaryMyTeam = state.myTeam;
                    state.matchSummaryWinnerTeam = -1;
                    effects.pendingDestroyedMatch = std::move(pending);
                }
            }
        }

        (void)replayActiveAtDestruction;
        state.inMatch = false;
        state.arenaName.clear();
        state.arenaAsset.clear();
        state.myTeam = -1;
        state.inReplay = false;
        state.nonLiveReplayActive = false;
        std::cout << "[Event] Match Destroyed (Back to Menu)\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
    }

} // namespace TelemetryReducerDetail

void TelemetryReducer::FinalizeMatch(MatchState& state,
                                     int winnerTeam,
                                     MatchFinalizeSource source,
                                     const ReducerContext& context,
                                     SideEffects& effects) {
    const std::string matchGuid = state.matchGuid;
    if (state.matchFinalized || (!matchGuid.empty() && state.finalizedMatchGuids.count(matchGuid) > 0)) {
        std::cout << "[TelemetryReducer] Skipping duplicate save: match already finalized.\n";
        return;
    }

    FinalizeCapturedMatch(state, CaptureMatch(state), winnerTeam, source, true, effects, context);
}

MatchSaveSnapshot TelemetryReducer::BuildMatchSaveSnapshot(
    const CapturedMatch& match,
    int winnerTeam,
    const MatchEndDecision& decision) {
    MatchSaveSnapshot snapshot;
    snapshot.arenaName = match.arenaName;
    snapshot.arenaAsset = match.arenaAsset;
    snapshot.matchGuid = match.matchGuid;
    snapshot.playlistId = match.playlistId;
    snapshot.gamemode = match.mode;
    snapshot.myTeam = match.myTeam;
    snapshot.winnerTeam = winnerTeam;
    snapshot.validResult = decision.shouldPersist;
    snapshot.voidReason = decision.voidReason;
    snapshot.score[0] = match.score[0];
    snapshot.score[1] = match.score[1];
    snapshot.legacyPlayerCount =
        PlaylistMetadata::HasAuthoritativeId(match.playlistId)
            ? 0
            : (match.legacyMaxPlayersSeen > 0
                   ? match.legacyMaxPlayersSeen
                   : match.legacyMaxTeamPlayersSeen[0] + match.legacyMaxTeamPlayersSeen[1]);
    snapshot.rosterMmrCategory = match.rosterMmrCategory;
    snapshot.myPrimaryId = match.myPrimaryId;
    snapshot.endedAtUnixMs = match.endedAtUnixMs;
    snapshot.events = match.events;
    snapshot.analytics = match.analytics;
    snapshot.localStats = match.stats;
    snapshot.durationSeconds = match.durationSeconds;
    snapshot.overtimeSeconds = match.overtimeSeconds;
    return snapshot;
}

SideEffects TelemetryReducer::ConfirmPendingDestroyedMatch(
    MatchState& state,
    const std::string& matchGuid,
    bool won,
    const ReducerContext& context) {
    SideEffects effects;
    const auto pendingIt = state.pendingDestroyedMatches.find(matchGuid);
    if (pendingIt == state.pendingDestroyedMatches.end() ||
        state.finalizedMatchGuids.count(matchGuid) > 0) {
        std::cout << "[TelemetryReducer] Skipping duplicate destroyed-match confirmation.\n";
        return effects;
    }

    CapturedMatch match = std::move(pendingIt->second);
    state.pendingDestroyedMatches.erase(pendingIt);
    const int winnerTeam = won ? match.myTeam : 1 - match.myTeam;
    FinalizeCapturedMatch(
        state,
        std::move(match),
        winnerTeam,
        MatchFinalizeSource::TrackerConfirmedDestroyed,
        false,
        effects,
        context);
    PublishUiTimestamps(state, effects);
    return effects;
}
