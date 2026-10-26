#pragma once

#include "core/state/MatchSaveSnapshot.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <string>

class TelemetryManager;

namespace database::repository {

    class MatchRepository {
      public:
        static void SaveMatch(sqlite::Connection& db, const MatchSaveSnapshot& snapshot, bool hasStatsTables, TelemetryManager* telemetry);
        static bool UpdateMatchPlayerMmr(sqlite::Connection& db, const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated = false);
        static void ResolvePendingMatch(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot, bool hasStatsTables);
        static bool UpsertMatchStats(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);
        static bool InsertMatchEvents(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);
        static bool UpsertMatchAnalytics(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);
    };

} // namespace database::repository
