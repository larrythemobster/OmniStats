#include "SessionState.hpp"
#include <cmath>

std::string MmrCategoryToString(MmrCategory cat) {
    switch (cat) {
    case MmrCategory::OneVOne:
        return "1v1";
    case MmrCategory::TwoVTwo:
        return "2v2";
    case MmrCategory::ThreeVThree:
        return "3v3";
    case MmrCategory::Casual:
        return "casual";
    case MmrCategory::Tourny:
        return "t";
    case MmrCategory::Hoops:
        return "hoops";
    case MmrCategory::Rumble:
        return "rumble";
    case MmrCategory::Dropshot:
        return "dropshot";
    case MmrCategory::SnowDay:
        return "snowday";
    case MmrCategory::Heatseeker:
        return "heatseeker";
    default:
        return "best";
    }
}

MmrCategory StringToMmrCategory(const std::string& str) {
    if (str == "1v1") return MmrCategory::OneVOne;
    if (str == "2v2") return MmrCategory::TwoVTwo;
    if (str == "3v3") return MmrCategory::ThreeVThree;
    if (str == "casual") return MmrCategory::Casual;
    if (str == "t") return MmrCategory::Tourny;
    if (str == "hoops") return MmrCategory::Hoops;
    if (str == "rumble") return MmrCategory::Rumble;
    if (str == "dropshot") return MmrCategory::Dropshot;
    if (str == "snowday") return MmrCategory::SnowDay;
    if (str == "heatseeker") return MmrCategory::Heatseeker;
    return MmrCategory::Best;
}

bool IsExtraMmrCategory(MmrCategory cat) {
    return cat == MmrCategory::Hoops ||
           cat == MmrCategory::Rumble ||
           cat == MmrCategory::Dropshot ||
           cat == MmrCategory::SnowDay ||
           cat == MmrCategory::Heatseeker;
}

void HistoryState::SelectMmrOwner(const std::string& primaryId) {
    if (mmrOwnerPrimaryId == primaryId) return;

    if (!mmrOwnerPrimaryId.empty()) {
        auto& saved = inactiveMmrHistories[mmrOwnerPrimaryId];
        saved.mmrHistoryY = std::move(mmrHistoryY);
        saved.mmrHistoryX = std::move(mmrHistoryX);
        saved.playlistHistoryY = std::move(playlistHistoryY);
        saved.playlistMatchPoints = std::move(playlistMatchPoints);
        saved.playlistInitialMmr = std::move(playlistInitialMmr);
        saved.initialMmr = initialMmr;
    }

    mmrOwnerPrimaryId = primaryId;
    const auto previous = inactiveMmrHistories.find(primaryId);
    if (previous != inactiveMmrHistories.end()) {
        mmrHistoryY = std::move(previous->second.mmrHistoryY);
        mmrHistoryX = std::move(previous->second.mmrHistoryX);
        playlistHistoryY = std::move(previous->second.playlistHistoryY);
        playlistMatchPoints = std::move(previous->second.playlistMatchPoints);
        playlistInitialMmr = std::move(previous->second.playlistInitialMmr);
        initialMmr = previous->second.initialMmr;
        inactiveMmrHistories.erase(previous);
    } else {
        mmrHistoryY.clear();
        mmrHistoryX.clear();
        playlistHistoryY.clear();
        playlistMatchPoints.clear();
        playlistInitialMmr.clear();
        initialMmr = -1;
    }
    version++;
}

void HistoryState::ResetSessionMmr() {
    mmrHistoryY.clear();
    mmrHistoryX.clear();
    playlistHistoryY.clear();
    playlistMatchPoints.clear();
    playlistInitialMmr.clear();
    initialMmr = -1;
    inactiveMmrHistories.clear();
    version++;
}

void SessionState::resetMatch(const std::string& newArena, const std::string& newArenaAsset) {
    game.inMatch = true;
    game.inReplay = false;
    game.myTeam = -1;
    game.arenaName = newArena;
    game.arenaAsset = newArenaAsset;
    game.playlistId = -1;
    game.score[0] = 0;
    game.score[1] = 0;
    game.legacyMaxPlayersSeen = 0;
    game.roundEverStarted = false;

    game.localPlayerWasActive = false;
    game.localPlayerWasSpectator = false;
    game.localPlayerPresenceObserved = false;
    game.localPlayerPresentInLatestUpdate = false;
    game.explicitLocalForfeit = false;
    game.excludedEarlyExitContext = false;
    game.earlyExitExclusionReason.clear();
    game.fallbackCasualContext = false;
    game.fallbackNonRecordableContext = false;
    game.fallbackNonRecordableReason.clear();
    game.legacyLobbyWasEverFull = false;
    game.legacyCurrentTeamPlayersSeen = {0, 0};
    game.legacyMaxTeamPlayersSeen = {0, 0};
    game.lastMatchWasVoid = false;
    game.lastMatchVoidReason.clear();
    game.matchSummaryScore = {0, 0};
    game.matchSummaryMyTeam = -1;
    game.matchSummaryWinnerTeam = -1;

    game.currentMatch = MatchStats{};
    game.roster.clear();
    game.matchRoster.clear();
    game.preMatchMmrByGuid.clear();
    game.matchFinalized = false;
    ui.showOverlay = false;
    ui.showMatchSummary = false;
    ui.ResetMatchTimestamps(0);
}

void SessionState::clearActiveMatchOnDisconnect() {
    game.inMatch = false;
    game.inReplay = false;
    game.myTeam = -1;
    game.arenaName.clear();
    game.arenaAsset.clear();
    game.matchGuid.clear();
    game.playlistId = -1;
    game.score = {0, 0};
    game.legacyMaxPlayersSeen = 0;
    game.roundEverStarted = false;
    game.localPlayerWasActive = false;
    game.localPlayerWasSpectator = false;
    game.localPlayerPresenceObserved = false;
    game.localPlayerPresentInLatestUpdate = false;
    game.explicitLocalForfeit = false;
    game.excludedEarlyExitContext = false;
    game.earlyExitExclusionReason.clear();
    game.fallbackCasualContext = false;
    game.fallbackNonRecordableContext = false;
    game.fallbackNonRecordableReason.clear();
    game.legacyLobbyWasEverFull = false;
    game.legacyCurrentTeamPlayersSeen = {0, 0};
    game.legacyMaxTeamPlayersSeen = {0, 0};
    game.currentMatch = MatchStats{};
    game.roster.clear();
    game.matchRoster.clear();
    game.preMatchMmrByGuid.clear();
    game.matchFinalized = false;
    ui.inGoalReplay.store(false, std::memory_order_relaxed);
    ui.firstCountdownOfMatchMs.store(0, std::memory_order_relaxed);
    ui.lastCountdownMs.store(0, std::memory_order_relaxed);
    ui.lastGoalMs.store(0, std::memory_order_relaxed);
    ui.lastMatchStartMs.store(0, std::memory_order_relaxed);
}

bool SessionState::startNewSessionLocked(int64_t endedAtUnix) {
    const bool captureRecap = game.sessionTotals.wins + game.sessionTotals.losses > 0;
    if (captureRecap) {
        game.lastSessionRecap = {
            .valid = true,
            .endedAtUnix = endedAtUnix,
            .totals = std::move(game.sessionTotals),
            .gamemodes = std::move(game.sessionGamemodes),
            .sessionGeneration = game.sessionGeneration.load(),
            .mmrOwnerPrimaryId = history.mmrOwnerPrimaryId,
            .playlistInitialMmr = history.playlistInitialMmr};
    }
    game.sessionTotals = SessionTotals();
    game.sessionGamemodes.clear();
    game.sessionGeneration.fetch_add(1);
    history.ResetSessionMmr();
    ui.graphOffset.store(0);
    game.version++;
    return captureRecap;
}

void SessionState::syncSessionMmrChangeLocked() {
    auto& changes = game.sessionTotals.mmrChangeByPlaylist;
    changes.clear();
    for (const auto& [playlist, projection] : history.playlistHistoryY) {
        const auto initialIt = history.playlistInitialMmr.find(playlist);
        if (projection.empty() || initialIt == history.playlistInitialMmr.end() || initialIt->second <= 0) continue;
        changes[playlist] = static_cast<int>(std::lround(projection.back())) - initialIt->second;
    }
    game.sessionTotals.totalMmrChange = static_cast<float>(CalculateTrackedSessionMmrChange(changes));
}

void SessionState::selectMmrOwnerLocked(const std::string& primaryId) {
    history.SelectMmrOwner(primaryId);
    syncSessionMmrChangeLocked();
    game.version++;
}
