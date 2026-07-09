#pragma once
#include <string_view>

namespace Constants {
    constexpr std::string_view EVT_MATCH_CREATED = "MatchCreated";
    constexpr std::string_view EVT_MATCH_INITIALIZED = "MatchInitialized";
    constexpr std::string_view EVT_ROUND_STARTED = "RoundStarted";
    constexpr std::string_view EVT_UPDATE_STATE = "UpdateState";
    constexpr std::string_view EVT_MATCH_ENDED = "MatchEnded";
    constexpr std::string_view EVT_MATCH_DESTROYED = "MatchDestroyed";
    constexpr std::string_view EVT_REPLAY_CREATED = "ReplayCreated";
    constexpr std::string_view EVT_GOAL_SCORED = "GoalScored";
    constexpr std::string_view EVT_BALL_HIT = "BallHit";
    constexpr std::string_view EVT_CROSSBAR_HIT = "CrossbarHit";
    constexpr std::string_view EVT_STATFEED = "StatfeedEvent";
}