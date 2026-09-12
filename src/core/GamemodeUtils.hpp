#pragma once
#include <string>
#include "core/SessionState.hpp"

namespace GamemodeUtils {
    std::string InferFromPlayerCount(int playerCount);
    std::string InferFromArenaName(const std::string& arenaName);
    // Legacy fallback used only when Game.PlaylistId is genuinely unavailable.
    std::string InferFromSnapshot(int legacyMaxPlayersSeen, int rosterSize, MmrCategory rosterCat,
                                  const std::string& arenaName = "");
    bool IsTrackedCompetitiveMode(const std::string& mode);
}
