#pragma once

// Stateless helpers shared by the TelemetryReducer translation units.

#include <array>
#include <initializer_list>
#include <chrono>
#include <cstdint>
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

    inline int64_t SteadyNowMs() {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        return ms > 0 ? ms : 1;
    }

    inline std::string ExtractEventMatchGuid(const nlohmann::json& data) {
        if (!data.is_object()) return {};
        for (const char* key : {"MatchGuid", "match_guid"}) {
            if (data.contains(key) && data[key].is_string()) {
                std::string guid = data[key].get<std::string>();
                if (!guid.empty()) return guid;
            }
        }
        if (data.contains("Game") && data["Game"].is_object()) {
            const auto& game = data["Game"];
            for (const char* key : {"MatchGuid", "match_guid"}) {
                if (game.contains(key) && game[key].is_string()) {
                    std::string guid = game[key].get<std::string>();
                    if (!guid.empty()) return guid;
                }
            }
        }
        return {};
    }
}
