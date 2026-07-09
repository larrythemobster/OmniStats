#pragma once
#include <string>
#include "core/SessionState.hpp"

namespace GamemodeUtils {
    std::string InferFromPlayerCount(int playerCount);
    std::string InferFromArenaName(const std::string& arenaName);
    std::string InferFromSnapshot(int maxPlayersSeen, int rosterSize, MmrCategory rosterCat, MmrCategory graphCat,
                                  const std::string& arenaName = "");
    bool IsTrackedCompetitiveMode(const std::string& mode);
}
