#include "core/reducer/TelemetryReducer.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

#include "core/config/Config.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

int TelemetryReducerDetail::LegacyExpectedTeamSizeForMode(const std::string& mode) {
    if (mode == "1v1") return 1;
    if (mode == "2v2" || mode == "hoops" || mode == "heatseeker") return 2;
    if (mode == "3v3" || mode == "rumble" || mode == "dropshot" || mode == "snowday") return 3;
    if (mode == "4v4") return 4;
    return 0;
}

int TelemetryReducer::LegacyExpectedTeamSizeForMode(const std::string& mode) {
    return TelemetryReducerDetail::LegacyExpectedTeamSizeForMode(mode);
}

CapturedMatch TelemetryReducer::CaptureMatch(const MatchState& state) {
    CapturedMatch match;
    match.arenaName = state.arenaName;
    match.arenaAsset = state.arenaAsset;
    match.matchGuid = state.matchGuid;
    match.playlistId = state.playlistId;
    match.matchGeneration = state.activeMatchGeneration;
    match.sessionGeneration = state.sessionGeneration;
    match.myPrimaryId = state.myPrimaryId;
    match.myTeam = state.myTeam;
    match.score = state.score;
    match.legacyMaxPlayersSeen = state.legacyMaxPlayersSeen;
    match.legacyMaxTeamPlayersSeen = state.legacyMaxTeamPlayersSeen;
    match.roundEverStarted = state.roundEverStarted;
    match.localPlayerWasActive = state.localPlayerWasActive;
    match.localPlayerWasSpectator = state.localPlayerWasSpectator;
    match.legacyLobbyWasEverFull = state.legacyLobbyWasEverFull;
    match.localPlayerDisappeared =
        state.localPlayerPresenceObserved && !state.localPlayerPresentInLatestUpdate;
    match.explicitLocalForfeit = state.explicitLocalForfeit;
    match.fallbackCasualContext = state.fallbackCasualContext;
    match.fallbackNonRecordableContext = state.fallbackNonRecordableContext;
    match.fallbackNonRecordableReason = state.fallbackNonRecordableReason;
    match.nonLiveReplay = state.nonLiveReplayActive;
    match.stats = state.currentMatch;
    match.events = state.matchEvents;

    if (MatchAnalytics analytics = state.analytics.Snapshot(); !analytics.Empty()) match.analytics = std::move(analytics);

    match.roster = state.matchRoster;
    for (const auto& [primaryId, player] : state.roster) {
        match.roster[primaryId] = player;
    }
    match.rosterMmrCategory = state.autoSwitchedPlaylistCategory;
    match.mode = InferModeFromMatchState(state, match.rosterMmrCategory);

    const auto snapshotIt = state.preMatchMmrByGuid.find(state.matchGuid);
    if (snapshotIt != state.preMatchMmrByGuid.end()) {
        match.preMatchMmr = snapshotIt->second;
        match.hasPreMatchMmr = true;
    }

    const int64_t nowMs = state.currentReducerNowMs > 0 ? state.currentReducerNowMs : SteadyNowMs();
    if (state.overtimeStartedAtMs.has_value()) {
        const float rawOt = static_cast<float>(nowMs - *state.overtimeStartedAtMs) / 1000.0f;
        const float pausedOt = state.PausedSecondsWithin(*state.overtimeStartedAtMs, nowMs);
        match.overtimeSeconds = (std::max)(0.0f, rawOt - pausedOt);
    } else {
        match.overtimeSeconds = 0.0f;
    }

    const float regulationSeconds =
        state.regulationClockStart.has_value()
            ? (std::max)(0.0f, *state.regulationClockStart - state.regulationClockLatest)
            : 0.0f;
    match.durationSeconds = regulationSeconds > 0.0f ? regulationSeconds + match.overtimeSeconds : 0.0f;
    match.endedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return match;
}

TelemetryReducer::MatchEndDecision TelemetryReducer::ClassifyMatchEnd(
    const CapturedMatch& match,
    int winnerTeam,
    const ConfigData& config) {
    (void)config;
    MatchEndDecision decision;

    if (match.nonLiveReplay) {
        decision.voidReason = "non_live_replay";
        return decision;
    }

    if (PlaylistMetadata::HasAuthoritativeId(match.playlistId) &&
        !PlaylistMetadata::IsKnown(match.playlistId)) {
        decision.voidReason = "unknown_playlist_id";
        return decision;
    }

    if (const char* reason = PlaylistMetadata::NonRecordableReason(match.playlistId)) {
        decision.voidReason = reason;
        return decision;
    }

    if (!PlaylistMetadata::HasAuthoritativeId(match.playlistId) &&
        match.fallbackNonRecordableContext) {
        decision.voidReason = match.fallbackNonRecordableReason.empty()
                                  ? "explicit_non_competitive_context"
                                  : match.fallbackNonRecordableReason;
        return decision;
    }

    if (winnerTeam != 0 && winnerTeam != 1) {
        decision.voidReason = "invalid_winner";
        return decision;
    }

    if (match.myTeam != 0 && match.myTeam != 1) {
        decision.voidReason = match.localPlayerWasSpectator
                                  ? "local_player_spectator_bug"
                                  : "local_player_team_unknown";
        return decision;
    }

    if (!match.roundEverStarted) {
        decision.voidReason = "round_never_started";
        return decision;
    }

    if (!match.localPlayerWasActive) {
        decision.voidReason = match.localPlayerWasSpectator
                                  ? "local_player_spectator_bug"
                                  : "local_player_never_active";
        return decision;
    }

    if (!PlaylistMetadata::HasAuthoritativeId(match.playlistId)) {
        const int expectedTeamSize = LegacyExpectedTeamSizeForMode(match.mode);
        if (expectedTeamSize > 0) {
            const bool teamsWereEverFull =
                match.legacyLobbyWasEverFull ||
                (match.legacyMaxTeamPlayersSeen[0] >= expectedTeamSize &&
                 match.legacyMaxTeamPlayersSeen[1] >= expectedTeamSize);
            if (!teamsWereEverFull) {
                decision.voidReason = "lobby_never_full";
                return decision;
            }
        }
    }

    decision.shouldCount = true;
    decision.shouldPersist = true;
    decision.iWon = (winnerTeam == match.myTeam);
    decision.resultText = decision.iWon ? "Win" : "Loss";
    return decision;
}

bool TelemetryReducer::IsValidEarlyCompetitiveExit(
    const MatchState& state,
    std::string& mode,
    std::string& voidReason) {
    if (!state.inMatch) {
        voidReason = "match_not_active";
        return false;
    }
    if (state.matchFinalized) {
        voidReason = "match_already_finalized";
        return false;
    }
    if (state.nonLiveReplayActive) {
        voidReason = "non_live_replay";
        return false;
    }
    if (state.localPlayerWasSpectator) {
        voidReason = "local_player_spectator_bug";
        return false;
    }
    if (!state.roundEverStarted) {
        voidReason = "round_never_started";
        return false;
    }
    if (!state.localPlayerWasActive) {
        voidReason = "local_player_never_active";
        return false;
    }
    if (state.myTeam != 0 && state.myTeam != 1) {
        voidReason = "local_player_team_unknown";
        return false;
    }
    if (state.matchGuid.empty()) {
        voidReason = "missing_match_guid";
        return false;
    }
    if (state.excludedEarlyExitContext) {
        voidReason = state.earlyExitExclusionReason.empty()
                         ? "explicit_non_competitive_context"
                         : state.earlyExitExclusionReason;
        return false;
    }

    mode = InferModeFromMatchState(state, state.autoSwitchedPlaylistCategory);
    if (!IsTrackedRankedEarlyExitMode(mode)) {
        voidReason = "untracked_or_non_competitive_playlist";
        return false;
    }

    if (!PlaylistMetadata::HasAuthoritativeId(state.playlistId)) {
        const int expectedTeamSize = LegacyExpectedTeamSizeForMode(mode);
        if (expectedTeamSize <= 0) {
            voidReason = "unknown_competitive_team_size";
            return false;
        }
        const bool lobbyWasFull =
            state.legacyLobbyWasEverFull ||
            (state.legacyMaxTeamPlayersSeen[0] >= expectedTeamSize &&
             state.legacyMaxTeamPlayersSeen[1] >= expectedTeamSize);
        if (!lobbyWasFull) {
            voidReason = "lobby_never_full";
            return false;
        }
    }

    return true;
}

namespace TelemetryReducerDetail {

    bool BuildPostMatchMmrRefresh(const MatchState& state,
                                  const CapturedMatch& match,
                                  bool won,
                                  PostMatchMmrRefresh& refresh) {
        (void)state;
        const bool hasPlaylistId = PlaylistMetadata::HasAuthoritativeId(match.playlistId);
        const bool authoritativeCasual = hasPlaylistId && PlaylistMetadata::IsCasual(match.playlistId);
        if ((!hasPlaylistId && (match.fallbackCasualContext || match.fallbackNonRecordableContext)) ||
            (hasPlaylistId && !PlaylistMetadata::IsKnown(match.playlistId)) ||
            match.matchGuid.empty() || match.myPrimaryId.empty() ||
            (!authoritativeCasual && !GamemodeUtils::IsTrackedCompetitiveMode(match.mode))) {
            return false;
        }

        const std::string mmrKey = hasPlaylistId ? PlaylistMetadata::MmrKey(match.playlistId) : match.mode;
        if (mmrKey.empty()) return false;

        const auto playerIt = match.roster.find(match.myPrimaryId);
        if (playerIt == match.roster.end()) return false;
        const auto& player = playerIt->second;

        int previousMmr = player.mmr;
        int previousMatches = -1;
        bool previousMmrIsPlaylistSpecific = false;
        bool usedCapturedSnapshot = false;
        if (match.hasPreMatchMmr) {
            const auto mmrIt = match.preMatchMmr.playlistMmrs.find(mmrKey);
            if (mmrIt != match.preMatchMmr.playlistMmrs.end() && mmrIt->second > 0) {
                previousMmr = mmrIt->second;
                previousMmrIsPlaylistSpecific = true;
                usedCapturedSnapshot = true;
                const auto countIt = match.preMatchMmr.playlistMatches.find(mmrKey);
                if (countIt != match.preMatchMmr.playlistMatches.end()) {
                    previousMatches = countIt->second;
                }
            }
        }

        if (!usedCapturedSnapshot) {
            const auto plIt = player.playlists.find(mmrKey);
            if (plIt != player.playlists.end() && plIt->second > 0) {
                previousMmr = plIt->second;
                previousMmrIsPlaylistSpecific = true;
            }
            const auto countIt = player.playlistMatches.find(mmrKey);
            if (countIt != player.playlistMatches.end()) {
                previousMatches = countIt->second;
            }
        }

        refresh = PostMatchMmrRefresh{
            .primaryId = match.myPrimaryId,
            .name = player.name,
            .matchGuid = match.matchGuid,
            .playlist = mmrKey,
            .previousMmr = previousMmr,
            .previousMatches = previousMatches,
            .previousMmrIsPlaylistSpecific = previousMmrIsPlaylistSpecific,
            .won = won};
        return true;
    }

} // namespace TelemetryReducerDetail
