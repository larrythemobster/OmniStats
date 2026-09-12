#pragma once
#include <string>
#include <unordered_map>
#include <array>
#include <cstdint>
#include "SessionState.hpp"

struct MatchSaveSnapshot {
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int playlistId = -1;
    std::string gamemode;
    int myTeam = -1;
    int winnerTeam = -1;
    bool validResult = false;
    std::string voidReason;
    int score[2] = {0, 0};
    // Used only for pre-PlaylistId compatibility. New authoritative rows keep
    // this at zero so player observations cannot become match truth.
    int legacyPlayerCount = 0;
    std::unordered_map<std::string, PlayerData> roster;
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    std::string myPrimaryId;
    // The local player's saved MMR is a pre-match placeholder until the
    // post-match Tracker reconciliation owns this row.
    bool localMmrNeedsReconciliation = false;
    int64_t endedAtUnixMs = 0;
};
