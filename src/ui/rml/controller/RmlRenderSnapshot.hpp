#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/state/SessionState.hpp"

struct RmlRenderSnapshot {
    std::string matchGuid;
    std::string arenaName;
    int score[2] = {0, 0};
    bool inMatch = false;
    bool inReplay = false;
    bool matchFinalized = false;
    std::string myPrimaryId;
    int myTeam = -1;
    MatchStats currentMatch;
    float lastGoalSpeed = 0.0f;
    float lastGoalSpeedSelf = 0.0f;
    SessionTotals sessionTotals;
    std::unordered_map<std::string, PlayerData> roster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    int matchSummaryScore[2] = {0, 0};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;
    float matchSummaryOvertimeSeconds = 0.0f;

    float initialMmr = -1.0f;
    std::map<std::string, int> playlistInitialMmr;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, std::vector<bool>> playlistHistoryEstimated;
    std::vector<int64_t> lifetimeMmrX;
    std::vector<float> lifetimeMmrY;
    std::vector<SessionMatchSummary> recentSavedMatches;
    bool recentSavedMatchesLoaded = false;
    bool showLifetimeGraph = false;

    // Identity key -> single-line preview, refreshed when SessionState::notes changes.
    std::unordered_map<std::string, std::string> notePreviews;
    std::unordered_map<std::string, std::vector<std::string>> noteTags;
    bool notesLoaded = false;
    uint64_t notesVersion = 0;
};
