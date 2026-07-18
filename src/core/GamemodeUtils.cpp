#include "core/GamemodeUtils.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace GamemodeUtils {

    static std::string NormalizeArenaKey(std::string value) {
        std::string normalized;
        normalized.reserve(value.size());
        for (unsigned char c : value) {
            if (std::isalnum(c)) {
                normalized.push_back(static_cast<char>(std::tolower(c)));
            }
        }
        return normalized;
    }

    std::string InferFromPlayerCount(int playerCount) {
        if (playerCount <= 0) return "Unknown";
        if (playerCount <= 2) return "1v1";
        if (playerCount <= 4) return "2v2";
        if (playerCount <= 6) return "3v3";
        return "Unknown";
    }

    std::string InferFromArenaName(const std::string& arenaName) {
        const std::string key = NormalizeArenaKey(arenaName);
        if (key.empty()) return "Unknown";

        static const std::unordered_map<std::string, std::string> arenaModes = {
            {"hoopsdunkhouse", "hoops"},
            {"dunkhouse", "hoops"},
            {"hoopstheblock", "hoops"},
            {"theblock", "hoops"},
            {"shattershot", "dropshot"},
            {"dropshot", "dropshot"},
            {"core707", "dropshot"},
            {"championsfieldsnowday", "snowday"},
            {"snowday", "snowday"}};

        auto exact = arenaModes.find(key);
        if (exact != arenaModes.end()) return exact->second;

        if (key.find("hoops") != std::string::npos || key.find("dunkhouse") != std::string::npos) return "hoops";
        if (key.find("dropshot") != std::string::npos || key.find("shattershot") != std::string::npos ||
            key.find("core707") != std::string::npos) return "dropshot";
        if (key.find("snow") != std::string::npos || key.find("hockey") != std::string::npos) return "snowday";

        return "Unknown";
    }

    std::string InferFromSnapshot(int maxPlayersSeen, int rosterSize, MmrCategory rosterCat, MmrCategory graphCat,
                                  const std::string& arenaName) {
        if (rosterCat == MmrCategory::Casual || graphCat == MmrCategory::Casual) {
            return "casual";
        }
        if (rosterCat == MmrCategory::Tourny || graphCat == MmrCategory::Tourny) {
            return "t";
        }

        const std::string arenaMode = InferFromArenaName(arenaName);
        if (arenaMode != "Unknown") return arenaMode;

        if (rosterCat == MmrCategory::Rumble || graphCat == MmrCategory::Rumble) {
            return "rumble";
        }
        if (rosterCat == MmrCategory::Heatseeker || graphCat == MmrCategory::Heatseeker) {
            return "heatseeker";
        }

        const int playerCount = maxPlayersSeen > 0 ? maxPlayersSeen : rosterSize;
        return InferFromPlayerCount(playerCount);
    }

    bool IsTrackedCompetitiveMode(const std::string& mode) {
        return mode == "1v1" || mode == "2v2" || mode == "3v3" || mode == "casual" || mode == "t" ||
               mode == "hoops" || mode == "rumble" || mode == "dropshot" || mode == "snowday" || mode == "heatseeker";
    }

} // namespace GamemodeUtils
