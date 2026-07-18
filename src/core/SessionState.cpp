#include "SessionState.hpp"

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

void SessionState::resetMatch(const std::string& newArena, const std::string& newArenaAsset) {
    game.inMatch = true;
    game.inReplay = false;
    game.myTeam = -1;
    game.arenaName = newArena;
    game.arenaAsset = newArenaAsset;
    game.score[0] = 0;
    game.score[1] = 0;
    game.maxPlayersSeen = 0;
    game.roundEverStarted = false;

    game.localPlayerWasActive = false;
    game.localPlayerWasSpectator = false;
    game.lobbyWasEverFull = false;
    game.currentTeamPlayersSeen = {0, 0};
    game.maxTeamPlayersSeen = {0, 0};
    game.lastMatchWasVoid = false;
    game.lastMatchVoidReason.clear();
    game.matchSummaryScore = {0, 0};
    game.matchSummaryMyTeam = -1;
    game.matchSummaryWinnerTeam = -1;

    game.currentMatch = MatchStats{};
    game.roster.clear();
    game.matchRoster.clear();
    game.matchFinalized = false;
    ui.showOverlay = false;
    ui.showMatchSummary = false;
}
