#pragma once
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif

enum class GamemodeBreakdownScope : int {
    CurrentSession = 0,
    AllTime = 1,
    Last7Days = 2,
    Last30Days = 3
};

inline const char* ToDisplayString(GamemodeBreakdownScope scope) {
    switch (scope) {
    case GamemodeBreakdownScope::CurrentSession:
        return "Current Session";
    case GamemodeBreakdownScope::AllTime:
        return "All-Time";
    case GamemodeBreakdownScope::Last7Days:
        return "Last 7 Days";
    case GamemodeBreakdownScope::Last30Days:
        return "Last 30 Days";
    default:
        return "Current Session";
    }
}

inline const char* ToConfigString(GamemodeBreakdownScope scope) {
    switch (scope) {
    case GamemodeBreakdownScope::CurrentSession:
        return "current_session";
    case GamemodeBreakdownScope::AllTime:
        return "all_time";
    case GamemodeBreakdownScope::Last7Days:
        return "last_7_days";
    case GamemodeBreakdownScope::Last30Days:
        return "last_30_days";
    default:
        return "current_session";
    }
}

inline GamemodeBreakdownScope ScopeFromConfigString(const std::string& value) {
    if (value == "all_time") return GamemodeBreakdownScope::AllTime;
    if (value == "last_7_days") return GamemodeBreakdownScope::Last7Days;
    if (value == "last_30_days") return GamemodeBreakdownScope::Last30Days;
    return GamemodeBreakdownScope::CurrentSession;
}
