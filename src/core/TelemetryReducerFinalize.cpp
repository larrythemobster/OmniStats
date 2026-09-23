#include "TelemetryReducer.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>
#include "core/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

void TelemetryReducer::RecordTerminalMatchGuidLocked(
    const std::string& matchGuid) {
    if (!matchGuid.empty()) {
        m_finalizedMatchGuids.insert(matchGuid);
    }
}

void TelemetryReducer::MarkDestroyedMatchVoidLocked(
    const std::string& reason,
    SideEffects& effects) {
    RecordTerminalMatchGuidLocked(m_state->game.matchGuid);
    m_state->game.lastMatchWasVoid = true;
    m_state->game.lastMatchVoidReason = reason;
    m_state->game.matchSummaryScore = m_state->game.score;
    m_state->game.matchSummaryMyTeam = m_state->game.myTeam;
    m_state->game.matchSummaryWinnerTeam = -1;
    m_state->game.matchFinalized = true;
    std::cout << "[Event] MATCH VOIDED: " << reason << "\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

void TelemetryReducer::FinalizeMatchLocked(
    int winnerTeam,
    MatchFinalizeSource source,
    SideEffects& effects) {
    const std::string matchGuid = m_state->game.matchGuid;
    if (m_state->game.matchFinalized ||
        (!matchGuid.empty() &&
         m_finalizedMatchGuids.count(matchGuid) > 0)) {
        std::cout
            << "[TelemetryReducer] Skipping duplicate save: match already finalized.\n";
        return;
    }

    FinalizeCapturedMatchLocked(
        CaptureMatchLocked(), winnerTeam, source, true, effects);
}

void TelemetryReducer::FinalizeCapturedMatchLocked(
    CapturedMatch match,
    int winnerTeam,
    MatchFinalizeSource source,
    bool enqueueMmrRefresh,
    SideEffects& effects) {
    if (!match.matchGuid.empty() &&
        m_finalizedMatchGuids.count(match.matchGuid) > 0) {
        std::cout << "[TelemetryReducer] Skipping duplicate match save for guid: "
                  << PrivacyLog::Sensitive(match.matchGuid, "match GUID")
                  << "\n";
        return;
    }

    const MatchEndDecision decision =
        ClassifyMatchEndLocked(match, winnerTeam);
    RecordTerminalMatchGuidLocked(match.matchGuid);
    const bool isCurrentMatch =
        match.matchGeneration ==
            m_state->game.activeMatchGeneration &&
        match.matchGuid == m_state->game.matchGuid;

    if (isCurrentMatch) {
        m_state->game.matchFinalized = true;
        m_state->game.lastMatchWasVoid = !decision.shouldCount;
        m_state->game.lastMatchVoidReason = decision.voidReason;
        m_state->game.matchSummaryScore = match.score;
        m_state->game.matchSummaryMyTeam = match.myTeam;
        m_state->game.matchSummaryWinnerTeam = winnerTeam;
        m_state->ui.showMatchSummary = true;
        m_state->ui.matchSummaryStartMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    if (!decision.shouldCount) {
        std::cout << "[Event] MATCH VOIDED: " << decision.voidReason
                  << "\n";
        if (isCurrentMatch) {
            effects.pushDiscord = true;
            effects.discordSnapshot = BuildDiscordSnapshotLocked();
        }
        return;
    }

    if (!match.matchGuid.empty()) {
        m_lastSavedMatchGuid = match.matchGuid;
    }

    const bool iWon = decision.iWon;
    if (iWon) {
        m_state->game.sessionTotals.wins++;
    } else {
        m_state->game.sessionTotals.losses++;
    }

    const auto& currentMatch = match.stats;
    auto& sessionTotals = m_state->game.sessionTotals;
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
    sessionTotals.maxGoalSpeed =
        std::max(sessionTotals.maxGoalSpeed, currentMatch.maxGoalSpeed);
    sessionTotals.maxGoalSpeedSelf = std::max(
        sessionTotals.maxGoalSpeedSelf, currentMatch.maxGoalSpeedSelf);
    sessionTotals.maxBallSpeed =
        std::max(sessionTotals.maxBallSpeed, currentMatch.maxBallSpeed);
    sessionTotals.maxBallSpeedSelf = std::max(
        sessionTotals.maxBallSpeedSelf, currentMatch.maxBallSpeedSelf);
    sessionTotals.maxImpactForce = std::max(
        sessionTotals.maxImpactForce, currentMatch.maxImpactForce);
    sessionTotals.maxImpactForceSelf = std::max(
        sessionTotals.maxImpactForceSelf, currentMatch.maxImpactForceSelf);
    if (currentMatch.fastestGoalTime > 0.0f &&
        (sessionTotals.fastestGoalTime == 0.0f ||
         currentMatch.fastestGoalTime <
             sessionTotals.fastestGoalTime)) {
        sessionTotals.fastestGoalTime =
            currentMatch.fastestGoalTime;
    }
    if (currentMatch.fastestGoalTimeSelf > 0.0f &&
        (sessionTotals.fastestGoalTimeSelf == 0.0f ||
         currentMatch.fastestGoalTimeSelf <
             sessionTotals.fastestGoalTimeSelf)) {
        sessionTotals.fastestGoalTimeSelf =
            currentMatch.fastestGoalTimeSelf;
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
            std::max(0, teamGoalsThisMatch));
        sessionTotals.teamGoals += std::max(0, teamGoalsThisMatch);
        sessionTotals.goalParticipations += participationThisMatch;
    }

    if (!PlaylistMetadata::IsCasual(match.playlistId) &&
        !(!PlaylistMetadata::HasAuthoritativeId(match.playlistId) &&
          (match.fallbackCasualContext || match.fallbackNonRecordableContext)) &&
        GamemodeUtils::IsTrackedCompetitiveMode(match.mode)) {
        auto& gamemode = m_state->game.sessionGamemodes[match.mode];
        if (iWon) {
            gamemode.wins++;
        } else {
            gamemode.losses++;
        }
        gamemode.total++;
    }

    for (auto& [primaryId, player] : match.roster) {
        const bool isTeammate = player.team == match.myTeam;
        if (isTeammate) {
            if (iWon) {
                player.lifetimeWinsWith++;
            } else {
                player.lifetimeLossesWith++;
            }
        } else {
            if (iWon) {
                player.lifetimeWinsAgainst++;
            } else {
                player.lifetimeLossesAgainst++;
            }
        }
        player.hasLifetimeData = true;

        const auto livePlayer = m_state->game.roster.find(primaryId);
        if (livePlayer != m_state->game.roster.end()) {
            livePlayer->second.lifetimeWinsWith =
                player.lifetimeWinsWith;
            livePlayer->second.lifetimeLossesWith =
                player.lifetimeLossesWith;
            livePlayer->second.lifetimeWinsAgainst =
                player.lifetimeWinsAgainst;
            livePlayer->second.lifetimeLossesAgainst =
                player.lifetimeLossesAgainst;
            livePlayer->second.hasLifetimeData = true;
        }
    }

    const char* sourceName = "match-ended";
    if (source == MatchFinalizeSource::MatchDestroyed) {
        sourceName = "destroyed-authoritative-winner";
    } else if (source == MatchFinalizeSource::LocalForfeit) {
        sourceName = "local-forfeit";
    } else if (
        source == MatchFinalizeSource::TrackerConfirmedDestroyed) {
        sourceName = "tracker-confirmed-destruction";
    }
    std::cout << "========================================\n";
    std::cout << "[Event] MATCH FINALIZED! Final Score: "
              << match.score[0] << "-" << match.score[1]
              << ", source=" << sourceName << "\n";
    std::cout << "========================================\n";

    bool hasLocalRefresh = false;
    if (enqueueMmrRefresh) {
        PostMatchMmrRefresh refresh;
        hasLocalRefresh =
            BuildPostMatchMmrRefreshLocked(match, iWon, refresh);
        if (hasLocalRefresh) {
            refresh.provisionalImmediately =
                source == MatchFinalizeSource::LocalForfeit;
            effects.postMatchMmrRefresh = std::move(refresh);
        }
        for (const auto& [primaryId, player] : match.roster) {
            if (player.team != match.myTeam) continue;
            if (hasLocalRefresh && primaryId == match.myPrimaryId) {
                continue;
            }
            effects.fetchMmrQueue.emplace_back(primaryId, player.name);
        }
    }

    nlohmann::json matchRecord = {
        {"match_guid", match.matchGuid},
        {"arena", match.arenaName},
        {"playlist_id", match.playlistId},
        {"result", iWon ? "Win" : "Loss"},
        {"score", {match.score[0], match.score[1]}},
        {"stats",
         {{"goals", currentMatch.goalsSelf},
          {"saves", currentMatch.savesSelf},
          {"demos", currentMatch.demosSelf},
          {"fastest_goal", currentMatch.fastestGoalTimeSelf},
          {"max_ball_speed", currentMatch.maxGoalSpeedSelf}}},
        {"timestamp", match.endedAtUnixMs / 1000}};

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
                   : match.legacyMaxTeamPlayersSeen[0] +
                         match.legacyMaxTeamPlayersSeen[1]);
    snapshot.roster = std::move(match.roster);
    snapshot.rosterMmrCategory = match.rosterMmrCategory;
    snapshot.myPrimaryId = match.myPrimaryId;
    snapshot.localMmrNeedsReconciliation =
        hasLocalRefresh && m_cachedConf.enable_mmr_tracking;
    snapshot.endedAtUnixMs = match.endedAtUnixMs;

    effects.saveMatch = true;
    effects.matchRecord = std::move(matchRecord);
    effects.saveSnapshot = std::move(snapshot);

    if (isCurrentMatch) {
        effects.pushDiscord = true;
        effects.discordSnapshot = BuildDiscordSnapshotLocked();
    }
}

void TelemetryReducer::HandleMatchDestroyed(
    const nlohmann::json& data,
    SideEffects& effects) {
    std::string eventMatchGuid;
    for (const char* key : {"MatchGuid", "match_guid"}) {
        if (data.contains(key) && data[key].is_string()) {
            eventMatchGuid = data[key].get<std::string>();
            if (!eventMatchGuid.empty()) break;
        }
    }
    const bool hasExplicitEventGuid = !eventMatchGuid.empty();
    if (hasExplicitEventGuid) {
        AttachTerminalGuidToCurrentLocked(eventMatchGuid);
    }

    if (m_state->game.inMatch) {
        const char* ownershipFailure = nullptr;
        if (hasExplicitEventGuid &&
            eventMatchGuid != m_state->game.matchGuid) {
            ownershipFailure = "explicit-guid-not-current";
        } else if (
            !hasExplicitEventGuid &&
            !m_pendingDestroyedMatches.empty()) {
            ownershipFailure = "older-pending-match";
        } else if (
            !hasExplicitEventGuid &&
            !m_state->game.matchGuid.empty()) {
            ownershipFailure =
                "current-match-requires-explicit-guid";
        } else if (
            !hasExplicitEventGuid &&
            m_missingGuidAssociationBlockedByReconnect) {
            ownershipFailure = "telemetry-reconnect";
        }

        if (ownershipFailure) {
            std::cout
                << "[TelemetryReducer] MatchDestroyed: "
                << "eventGuid="
                << (hasExplicitEventGuid ? "present" : "missing")
                << ", currentGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", currentGeneration="
                << m_state->game.activeMatchGeneration
                << ", pendingMatches="
                << m_pendingDestroyedMatches.size()
                << ", action=ignore, reason="
                << ownershipFailure << ".\n";
            return;
        }
    }

    m_roundActive = false;
    UpdateLifecycleSignalsLocked(data);

    if (!m_state->game.inMatch) {
        if (m_pendingDestroyedMatches.count(
                m_state->game.matchGuid) > 0) {
            std::cout
                << "[TelemetryReducer] Ignoring duplicate MatchDestroyed for pending match.\n";
        }
        return;
    }

    if (data.contains("Teams") && data["Teams"].is_array()) {
        for (const auto& team : data["Teams"]) {
            if (!team.contains("TeamNum") ||
                !team["TeamNum"].is_number_integer() ||
                !team.contains("Score") ||
                !team["Score"].is_number_integer()) {
                continue;
            }
            const int teamNumber = team["TeamNum"].get<int>();
            if (teamNumber == 0 || teamNumber == 1) {
                m_state->game.score[teamNumber] =
                    team["Score"].get<int>();
            }
        }
    }

    const bool replayActiveAtDestruction = m_state->game.inReplay;

    const auto authoritativeWinner =
        ReadInteger(data, {"WinnerTeamNum", "winner_team_num"});
    if (!m_state->game.matchFinalized &&
        authoritativeWinner &&
        (*authoritativeWinner == 0 || *authoritativeWinner == 1)) {
        const MatchFinalizeSource source =
            m_state->game.explicitLocalForfeit &&
                    *authoritativeWinner != m_state->game.myTeam
                ? MatchFinalizeSource::LocalForfeit
                : MatchFinalizeSource::MatchDestroyed;
        FinalizeMatchLocked(*authoritativeWinner, source, effects);
    } else if (!m_state->game.matchFinalized) {
        std::string mode;
        std::string voidReason;
        const bool validEarlyCompetitiveExit =
            IsValidEarlyCompetitiveExitLocked(mode, voidReason);
        if (replayActiveAtDestruction) {
            const bool hasPlaylistId =
                PlaylistMetadata::HasAuthoritativeId(m_state->game.playlistId);
            const int expectedTeamSize =
                hasPlaylistId ? 0 : LegacyExpectedTeamSizeForMode(mode);
            const bool lobbyWasFull =
                hasPlaylistId || m_state->game.legacyLobbyWasEverFull ||
                (expectedTeamSize > 0 &&
                 m_state->game.legacyMaxTeamPlayersSeen[0] >= expectedTeamSize &&
                 m_state->game.legacyMaxTeamPlayersSeen[1] >= expectedTeamSize);
            const char* replayContext =
                m_nonLiveReplayActive
                    ? "saved-replay"
                    : (m_state->game.localPlayerWasSpectator
                           ? "spectator"
                           : "goal-replay");
            std::cout
                << "[TelemetryReducer] MatchDestroyed during replay: "
                << "matchGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", replayContext=" << replayContext
                << ", roundStarted="
                << (m_state->game.roundEverStarted ? "true"
                                                   : "false")
                << ", lobbyWasFull="
                << (lobbyWasFull ? "true" : "false")
                << ", playlist="
                << (mode.empty() ? "unknown" : mode)
                << ", action="
                << (!validEarlyCompetitiveExit
                        ? "void"
                        : (m_state->game.explicitLocalForfeit
                               ? "finalize-local-loss"
                               : "pending-tracker-confirmation"));
            if (!validEarlyCompetitiveExit) {
                std::cout << ", reason=" << voidReason;
            }
            std::cout << ".\n";
        }
        if (!validEarlyCompetitiveExit) {
            MarkDestroyedMatchVoidLocked(voidReason, effects);
        } else if (m_state->game.explicitLocalForfeit) {
            const int winnerTeam = 1 - m_state->game.myTeam;
            std::cout
                << "[TelemetryReducer] Competitive match destroyed before MatchEnded: "
                << "matchGuid="
                << PrivacyLog::Sensitive(
                       m_state->game.matchGuid, "match GUID")
                << ", playlist=" << mode
                << ", localTeam=" << m_state->game.myTeam
                << ", score=" << m_state->game.score[0] << "-"
                << m_state->game.score[1]
                << ", explicitLocalForfeit=true"
                << ", action=finalize-local-loss.\n";
            FinalizeMatchLocked(
                winnerTeam, MatchFinalizeSource::LocalForfeit, effects);
        } else {
            CapturedMatch match = CaptureMatchLocked();
            match.mode = mode;
            PostMatchMmrRefresh refresh;
            if (!BuildPostMatchMmrRefreshLocked(
                    match, false, refresh)) {
                MarkDestroyedMatchVoidLocked(
                    "missing_local_tracker_identity", effects);
            } else {
                PendingDestroyedMatchMmrRefresh pending;
                pending.primaryId = refresh.primaryId;
                pending.name = refresh.name;
                pending.matchGuid = refresh.matchGuid;
                pending.playlist = refresh.playlist;
                pending.localTeam = match.myTeam;
                pending.score = match.score;
                pending.previousMmr = refresh.previousMmr;
                pending.previousMatches = refresh.previousMatches;
                pending.previousMmrIsPlaylistSpecific =
                    refresh.previousMmrIsPlaylistSpecific;
                pending.localPlayerDisappeared =
                    match.localPlayerDisappeared;
                pending.explicitLocalForfeit =
                    match.explicitLocalForfeit;
                pending.destroyedAtUnixMs = match.endedAtUnixMs;
                pending.validCompetitiveMatch = true;

                SessionMatchSummary pendingSummary;
                pendingSummary.ranked = true;
                pendingSummary.mode = FormatPendingMatchHistoryMode(match.mode);
                pendingSummary.matchGuid = match.matchGuid;
                pendingSummary.ourScore =
                    match.myTeam == 1 ? match.score[1] : match.score[0];
                pendingSummary.theirScore =
                    match.myTeam == 1 ? match.score[0] : match.score[1];
                pendingSummary.mmr = 0;
                pendingSummary.win =
                    pendingSummary.ourScore > pendingSummary.theirScore;
                pendingSummary.pendingTrackerConfirmation = true;
                pendingSummary.endedAtUnix = match.endedAtUnixMs / 1000;
                {
                    std::unique_lock<std::shared_mutex> historyLock(
                        m_state->history.mutex);
                    const auto duplicate = std::find_if(
                        m_state->history.pendingRecentMatches.begin(),
                        m_state->history.pendingRecentMatches.end(),
                        [&](const SessionMatchSummary& summary) {
                            return summary.matchGuid == pendingSummary.matchGuid;
                        });
                    if (duplicate ==
                        m_state->history.pendingRecentMatches.end()) {
                        m_state->history.pendingRecentMatches.insert(
                            m_state->history.pendingRecentMatches.begin(),
                            std::move(pendingSummary));
                        if (m_state->history.pendingRecentMatches.size() >
                            static_cast<size_t>(kPreviousGamesMaxLimit)) {
                            m_state->history.pendingRecentMatches.resize(
                                kPreviousGamesMaxLimit);
                        }
                        m_state->history.version++;
                    }
                }

                m_pendingDestroyedMatches.emplace(
                    match.matchGuid, std::move(match));
                m_state->game.lastMatchWasVoid = true;
                m_state->game.lastMatchVoidReason =
                    "destroyed_pending_tracker_confirmation";
                m_state->game.matchSummaryScore =
                    m_state->game.score;
                m_state->game.matchSummaryMyTeam =
                    m_state->game.myTeam;
                m_state->game.matchSummaryWinnerTeam = -1;
                effects.pendingDestroyedMatch = std::move(pending);

                std::cout
                    << "[TelemetryReducer] Competitive match destroyed before MatchEnded: "
                    << "matchGuid="
                    << PrivacyLog::Sensitive(
                           m_state->game.matchGuid, "match GUID")
                    << ", playlist=" << mode
                    << ", localTeam=" << m_state->game.myTeam
                    << ", score=" << m_state->game.score[0] << "-"
                    << m_state->game.score[1]
                    << ", explicitLocalForfeit="
                    << (m_state->game.explicitLocalForfeit
                            ? "true"
                            : "false")
                    << ", localPlayerDisappeared="
                    << (m_state->game.localPlayerPresenceObserved &&
                                !m_state->game
                                     .localPlayerPresentInLatestUpdate
                            ? "true"
                            : "false")
                    << ", action=pending-tracker-confirmation.\n";
            }
        }
    }

    m_state->game.inMatch = false;
    m_state->game.arenaName.clear();
    m_state->game.arenaAsset.clear();
    m_state->game.myTeam = -1;
    m_state->game.inReplay = false;
    m_nonLiveReplayActive = false;
    std::cout << "[Event] Match Destroyed (Back to Menu)\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

SideEffects TelemetryReducer::ConfirmPendingDestroyedMatch(
    const std::string& matchGuid,
    bool won) {
    SideEffects effects;
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    const auto pendingIt =
        m_pendingDestroyedMatches.find(matchGuid);
    if (pendingIt == m_pendingDestroyedMatches.end() ||
        m_finalizedMatchGuids.count(matchGuid) > 0) {
        std::cout
            << "[TelemetryReducer] Skipping duplicate destroyed-match confirmation.\n";
        return effects;
    }

    CapturedMatch match = std::move(pendingIt->second);
    m_pendingDestroyedMatches.erase(pendingIt);
    const int winnerTeam = won ? match.myTeam : 1 - match.myTeam;
    FinalizeCapturedMatchLocked(
        std::move(match),
        winnerTeam,
        MatchFinalizeSource::TrackerConfirmedDestroyed,
        false,
        effects);
    m_state->game.version++;
    return effects;
}

void TelemetryReducer::HandleMatchEnded(
    const nlohmann::json& data,
    SideEffects& effects) {
    const auto winnerValue =
        ReadInteger(data, {"WinnerTeamNum", "winner_team_num"});
    const int winner = winnerValue ? *winnerValue : -1;

    std::string eventMatchGuid;
    for (const char* key : {"MatchGuid", "match_guid"}) {
        if (data.contains(key) && data[key].is_string()) {
            eventMatchGuid = data[key].get<std::string>();
            if (!eventMatchGuid.empty()) break;
        }
    }
    const bool hasExplicitEventGuid = !eventMatchGuid.empty();
    const auto pendingIt = hasExplicitEventGuid
                               ? m_pendingDestroyedMatches.find(
                                     eventMatchGuid)
                               : m_pendingDestroyedMatches.end();
    if (hasExplicitEventGuid &&
        pendingIt == m_pendingDestroyedMatches.end()) {
        AttachTerminalGuidToCurrentLocked(eventMatchGuid);
    }
    const bool targetsCurrentByGuid =
        hasExplicitEventGuid &&
        eventMatchGuid == m_state->game.matchGuid;

    const auto logDecision =
        [&](const char* action, const char* reason) {
            std::cout
                << "[TelemetryReducer] MatchEnded: "
                << "eventGuid="
                << (hasExplicitEventGuid ? "present" : "missing")
                << ", currentGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", currentGeneration="
                << m_state->game.activeMatchGeneration
                << ", pendingMatches="
                << m_pendingDestroyedMatches.size()
                << ", winnerTeam=" << winner
                << ", forfeit="
                << (ReadTrueBoolean(
                        data, {"bForfeit", "Forfeit"})
                        ? "true"
                        : "false")
                << ", action=" << action;
            if (reason && *reason) {
                std::cout << ", reason=" << reason;
            }
            std::cout << ".\n";
        };

    if (pendingIt != m_pendingDestroyedMatches.end()) {
        if (winner != 0 && winner != 1) {
            logDecision(
                "ignore",
                "invalid-winner-pending-match");
            return;
        }

        logDecision("finalize-pending", "");
        CapturedMatch match = std::move(pendingIt->second);
        m_pendingDestroyedMatches.erase(pendingIt);
        if (data.contains("Teams") && data["Teams"].is_array()) {
            for (const auto& team : data["Teams"]) {
                if (team.contains("TeamNum") &&
                    team["TeamNum"].is_number_integer() &&
                    team.contains("Score") &&
                    team["Score"].is_number_integer()) {
                    const int teamNumber =
                        team["TeamNum"].get<int>();
                    if (teamNumber == 0 || teamNumber == 1) {
                        match.score[teamNumber] =
                            team["Score"].get<int>();
                    }
                }
            }
        }
        const bool localForfeit =
            HasExplicitLocalForfeitSignal(data, match.myTeam) &&
            winner != match.myTeam;
        const bool won = winner == match.myTeam;
        const std::string resolvedGuid = match.matchGuid;
        FinalizeCapturedMatchLocked(
            std::move(match),
            winner,
            localForfeit ? MatchFinalizeSource::LocalForfeit
                         : MatchFinalizeSource::MatchEnded,
            false,
            effects);
        effects.resolvedDestroyedMatch =
            ResolvedDestroyedMatch{
                .matchGuid = resolvedGuid,
                .won = won};
        return;
    }

    bool targetsCurrent = targetsCurrentByGuid;
    if (!hasExplicitEventGuid) {
        const char* ambiguityReason = nullptr;
        if (!m_state->game.inMatch) {
            ambiguityReason = "no-active-match";
        } else if (m_state->game.matchFinalized) {
            ambiguityReason = "current-match-finalized";
        } else if (!m_pendingDestroyedMatches.empty()) {
            ambiguityReason = "older-pending-match";
        } else if (!m_state->game.matchGuid.empty()) {
            ambiguityReason =
                "current-match-requires-explicit-guid";
        } else if (
            m_missingGuidAssociationBlockedByReconnect) {
            ambiguityReason = "telemetry-reconnect";
        }

        if (ambiguityReason) {
            logDecision("ignore", ambiguityReason);
            return;
        }
        targetsCurrent = true;
    }

    if (!targetsCurrent) {
        logDecision("ignore", "explicit-guid-not-current");
        return;
    }

    if (!m_state->game.inMatch) {
        logDecision("ignore", "no-active-match");
        return;
    }
    if (m_state->game.matchFinalized) {
        logDecision("ignore", "current-match-finalized");
        return;
    }

    m_roundActive = false;
    if (data.contains("Teams") && data["Teams"].is_array()) {
        for (const auto& team : data["Teams"]) {
            if (team.contains("TeamNum") &&
                team["TeamNum"].is_number_integer() &&
                team.contains("Score") &&
                team["Score"].is_number_integer()) {
                const int teamNumber =
                    team["TeamNum"].get<int>();
                if (teamNumber == 0 || teamNumber == 1) {
                    m_state->game.score[teamNumber] =
                        team["Score"].get<int>();
                }
            }
        }
    }

    const bool localForfeit =
        HasExplicitLocalForfeitSignal(
            data, m_state->game.myTeam) &&
        winner != m_state->game.myTeam;
    logDecision("finalize-current", "");
    FinalizeMatchLocked(
        winner,
        localForfeit ? MatchFinalizeSource::LocalForfeit
                     : MatchFinalizeSource::MatchEnded,
        effects);
}
