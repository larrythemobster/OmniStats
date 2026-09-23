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

int TelemetryReducer::LegacyExpectedTeamSizeForMode(const std::string& mode) {
    if (mode == "1v1") return 1;
    if (mode == "2v2" || mode == "hoops" || mode == "heatseeker") return 2;
    if (mode == "3v3" || mode == "rumble" || mode == "dropshot" || mode == "snowday") return 3;
    if (mode == "4v4") return 4;
    return 0;
}

TelemetryReducer::CapturedMatch TelemetryReducer::CaptureMatchLocked() const {
    const auto& game = m_state->game;
    CapturedMatch match;
    match.arenaName = game.arenaName;
    match.arenaAsset = game.arenaAsset;
    match.matchGuid = game.matchGuid;
    match.playlistId = game.playlistId;
    match.matchGeneration = game.activeMatchGeneration;
    match.myPrimaryId = game.myPrimaryId;
    match.myTeam = game.myTeam;
    match.score = game.score;
    match.legacyMaxPlayersSeen = game.legacyMaxPlayersSeen;
    match.legacyMaxTeamPlayersSeen = game.legacyMaxTeamPlayersSeen;
    match.roundEverStarted = game.roundEverStarted;
    match.localPlayerWasActive = game.localPlayerWasActive;
    match.localPlayerWasSpectator = game.localPlayerWasSpectator;
    match.legacyLobbyWasEverFull = game.legacyLobbyWasEverFull;
    match.localPlayerDisappeared =
        game.localPlayerPresenceObserved &&
        !game.localPlayerPresentInLatestUpdate;
    match.explicitLocalForfeit = game.explicitLocalForfeit;
    match.fallbackCasualContext = game.fallbackCasualContext;
    match.fallbackNonRecordableContext = game.fallbackNonRecordableContext;
    match.fallbackNonRecordableReason = game.fallbackNonRecordableReason;
    match.nonLiveReplay = m_nonLiveReplayActive;
    match.stats = game.currentMatch;
    match.roster = game.matchRoster;
    for (const auto& [primaryId, player] : game.roster) {
        match.roster[primaryId] = player;
    }
    match.rosterMmrCategory = m_state->ui.rosterMmrCategory.load();
    match.mode = InferModeFromMatchState(
        game, match.rosterMmrCategory);
    const auto snapshotIt = game.preMatchMmrByGuid.find(game.matchGuid);
    if (snapshotIt != game.preMatchMmrByGuid.end()) {
        match.preMatchMmr = snapshotIt->second;
        match.hasPreMatchMmr = true;
    }
    match.endedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return match;
}

bool TelemetryReducer::BuildPostMatchMmrRefreshLocked(
    const CapturedMatch& match,
    bool won,
    PostMatchMmrRefresh& refresh) const {
    const bool hasPlaylistId =
        PlaylistMetadata::HasAuthoritativeId(match.playlistId);
    const bool authoritativeCasual =
        hasPlaylistId && PlaylistMetadata::IsCasual(match.playlistId);
    if ((!hasPlaylistId &&
         (match.fallbackCasualContext || match.fallbackNonRecordableContext)) ||
        (hasPlaylistId && !PlaylistMetadata::IsKnown(match.playlistId)) ||
        match.matchGuid.empty() || match.myPrimaryId.empty() ||
        (!authoritativeCasual &&
         !GamemodeUtils::IsTrackedCompetitiveMode(match.mode))) {
        return false;
    }

    // Every authoritative casual playlist owns the same Tracker/MMR bucket.
    // PlaylistId identifies the match; it never needs roster/player-count
    // inference to select which casual rating to refresh.
    const std::string mmrKey =
        hasPlaylistId ? PlaylistMetadata::MmrKey(match.playlistId)
                      : match.mode;
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
        if (mmrIt != match.preMatchMmr.playlistMmrs.end() &&
            mmrIt->second > 0) {
            previousMmr = mmrIt->second;
            previousMmrIsPlaylistSpecific = true;
            usedCapturedSnapshot = true;
            const auto matchesIt =
                match.preMatchMmr.playlistMatches.find(mmrKey);
            if (matchesIt != match.preMatchMmr.playlistMatches.end()) {
                previousMatches = matchesIt->second;
            }
        }
    }
    if (!usedCapturedSnapshot) {
        const auto playlistIt = player.playlists.find(mmrKey);
        if (playlistIt != player.playlists.end() && playlistIt->second > 0) {
            previousMmr = playlistIt->second;
            previousMmrIsPlaylistSpecific = true;
        }
        const auto matchesIt = player.playlistMatches.find(mmrKey);
        if (matchesIt != player.playlistMatches.end()) {
            previousMatches = matchesIt->second;
        }
    }

    refresh = PostMatchMmrRefresh{
        .primaryId = match.myPrimaryId,
        .name = player.name,
        .matchGuid = match.matchGuid,
        .playlist = mmrKey,
        .previousMmr = previousMmr,
        .previousMatches = previousMatches,
        .previousMmrIsPlaylistSpecific =
            previousMmrIsPlaylistSpecific,
        .won = won};
    return true;
}

TelemetryReducer::MatchEndDecision TelemetryReducer::ClassifyMatchEndLocked(
    const CapturedMatch& match,
    int winnerTeam) const {
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

    // These sessions can emit normal-looking MatchEnded events but they are
    // not real tracked matches and must not invalidate/refresh rank state.
    if (const char* reason = PlaylistMetadata::NonRecordableReason(match.playlistId)) {
        decision.voidReason = reason;
        return decision;
    }

    if (!PlaylistMetadata::HasAuthoritativeId(match.playlistId) &&
        match.fallbackNonRecordableContext) {
        decision.voidReason =
            match.fallbackNonRecordableReason.empty()
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

    // A real PlaylistId already identifies the match format. Player-count
    // heuristics are retained only for old telemetry that never supplied one.
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
    decision.iWon = winnerTeam == match.myTeam;
    decision.resultText = decision.iWon ? "Win" : "Loss";
    return decision;
}

bool TelemetryReducer::IsValidEarlyCompetitiveExitLocked(
    std::string& mode,
    std::string& voidReason) const {
    const auto& game = m_state->game;
    if (!game.inMatch) {
        voidReason = "match_not_active";
        return false;
    }
    if (game.matchFinalized) {
        voidReason = "match_already_finalized";
        return false;
    }
    if (m_nonLiveReplayActive) {
        voidReason = "non_live_replay";
        return false;
    }
    if (game.localPlayerWasSpectator) {
        voidReason = "local_player_spectator_bug";
        return false;
    }
    if (!game.roundEverStarted) {
        voidReason = "round_never_started";
        return false;
    }
    if (!game.localPlayerWasActive) {
        voidReason = "local_player_never_active";
        return false;
    }
    if (game.myTeam != 0 && game.myTeam != 1) {
        voidReason = "local_player_team_unknown";
        return false;
    }
    if (game.matchGuid.empty()) {
        voidReason = "missing_match_guid";
        return false;
    }
    if (game.excludedEarlyExitContext) {
        voidReason = game.earlyExitExclusionReason.empty()
                         ? "explicit_non_competitive_context"
                         : game.earlyExitExclusionReason;
        return false;
    }

    const MmrCategory rosterCategory =
        m_state->ui.rosterMmrCategory.load();
    mode = InferModeFromMatchState(game, rosterCategory);
    if (!IsTrackedRankedEarlyExitMode(mode)) {
        voidReason = "untracked_or_non_competitive_playlist";
        return false;
    }

    if (!PlaylistMetadata::HasAuthoritativeId(game.playlistId)) {
        const int expectedTeamSize = LegacyExpectedTeamSizeForMode(mode);
        if (expectedTeamSize <= 0) {
            voidReason = "unknown_competitive_team_size";
            return false;
        }
        const bool lobbyWasFull =
            game.legacyLobbyWasEverFull ||
            (game.legacyMaxTeamPlayersSeen[0] >= expectedTeamSize &&
             game.legacyMaxTeamPlayersSeen[1] >= expectedTeamSize);
        if (!lobbyWasFull) {
            voidReason = "lobby_never_full";
            return false;
        }
    }

    return true;
}
