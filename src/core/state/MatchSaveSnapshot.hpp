#pragma once
#include <string>
#include <unordered_map>
#include <array>
#include <cstdint>
#include "core/state/SessionState.hpp"
#include "core/state/MatchEvent.hpp"
#include "core/game/MatchAnalytics.hpp"
#include <optional>
#include <vector>

struct MatchSaveData {
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int playlistId = -1;
    int myTeam = -1;
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    std::string myPrimaryId;
    int64_t endedAtUnixMs = 0;
    float durationSeconds = 0.0f;
    float overtimeSeconds = 0.0f;
    std::vector<MatchJournalEvent> events;
    std::optional<MatchAnalytics> analytics;
};

struct MatchSaveSnapshot : MatchSaveData {
    std::string gamemode;
    int winnerTeam = -1;
    bool validResult = false;
    std::string voidReason;
    int score[2] = {0, 0};
    // Used only for pre-PlaylistId compatibility. New authoritative rows keep
    // this at zero so player observations cannot become match truth.
    int legacyPlayerCount = 0;
    std::unordered_map<std::string, PlayerData> roster;
    // The local player's saved MMR is a pre-match placeholder until the
    // post-match Tracker reconciliation owns this row.
    bool localMmrNeedsReconciliation = false;
    bool resultPending = false;
    MatchStats localStats;
};
