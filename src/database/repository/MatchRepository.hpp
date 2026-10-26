#pragma once

#include "core/state/MatchSaveSnapshot.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <string>

namespace database::repository {

    class MatchRepository {
      public:
        explicit MatchRepository(sqlite::Connection& db) : m_db(db) {}

        static void SaveMatch(sqlite::Connection& db, const MatchSaveSnapshot& snapshot, bool hasStatsTables);
        static bool UpdateMatchPlayerMmr(sqlite::Connection& db, const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated = false);
        static void ResolvePendingMatch(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot, bool hasStatsTables);
        static bool UpsertMatchStats(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);
        static bool InsertMatchEvents(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);
        static bool UpsertMatchAnalytics(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot);

        void SaveMatch(const MatchSaveSnapshot& snapshot, bool hasStatsTables) {
            SaveMatch(m_db, snapshot, hasStatsTables);
        }
        bool UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated = false) {
            return UpdateMatchPlayerMmr(m_db, matchGuid, primaryId, mmr, estimated);
        }

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository
