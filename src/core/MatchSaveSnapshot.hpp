#pragma once
#include <string>
#include <unordered_map>
#include <array>
#include "SessionState.hpp"

struct MatchSaveSnapshot {
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int myTeam = -1;
    int winnerTeam = -1;
    bool validResult = false;
    std::string voidReason;
    std::array<int, 2> maxTeamPlayersSeen{0, 0};
    int score[2] = {0, 0};
    int maxPlayersSeen = 0;
    std::unordered_map<std::string, PlayerData> roster;
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    MmrCategory graphMmrCategory = MmrCategory::Best;
    std::string myPrimaryId;
};
