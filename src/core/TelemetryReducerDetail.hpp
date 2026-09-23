#pragma once

// Stateless helpers shared by the TelemetryReducer translation units.

#include <array>
#include <initializer_list>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/SessionState.hpp"

namespace TelemetryReducerDetail {
    std::string FormatArenaName(const std::string& asset);
    MmrCategory CategoryFromTeamCounts(const std::array<int, 2>& teamCounts, bool roundStarted);
    MmrCategory CategoryFromMode(const std::string& mode);
    std::string InferModeFromMatchState(const GameState& game, MmrCategory rosterCategory);
    MmrCategory CategoryFromMatchContext(const GameState& game);
    bool IsSupportedMmrCategory(MmrCategory category);
    bool IsTrackedRankedEarlyExitMode(const std::string& mode);
    std::string FormatPendingMatchHistoryMode(const std::string& mode);
    std::string Lowercase(std::string value);
    bool ReadTrueBoolean(const nlohmann::json& object, std::initializer_list<const char*> keys);
    std::optional<int> ReadInteger(const nlohmann::json& object, std::initializer_list<const char*> keys);
    bool MatchTypeContains(const nlohmann::json& game, std::initializer_list<const char*> keywords);
}
