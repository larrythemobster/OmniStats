#pragma once

#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace database::repository {

    // One player's totals in one playlist, from BakkesMod DejaVu's player_counter.json.
    struct DejaVuEncounter {
        std::string primaryId;
        std::string name;
        int playlistId = 0;
        int winsWith = 0;
        int lossesWith = 0;
        int winsAgainst = 0;
        int lossesAgainst = 0;
        int64_t lastSeenUnix = 0;
    };

    // DejaVu history is offered once per install; the answer is kept in Settings.
    class DejaVuImporter {
      public:
        // %APPDATA%\bakkesmod\bakkesmod\data\dejavu\player_counter.json, or empty when APPDATA is unset.
        static std::filesystem::path DefaultPlayerCounterPath();
        static std::optional<std::vector<DejaVuEncounter>> LoadFile(const std::filesystem::path& path, std::string& error);
        static int CountPlayers(const std::vector<DejaVuEncounter>& rows);
        // True once the user has imported or declined.
        static bool Answered(sqlite::Connection& db);
        static bool Import(sqlite::Connection& db, const std::string& account, const std::vector<DejaVuEncounter>& rows,
                           std::string& error);
        static bool Decline(sqlite::Connection& db, std::string& error);
    };

} // namespace database::repository
