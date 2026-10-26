#pragma once

#include "core/state/SessionState.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace database::repository {

    class SessionRepository {
      public:
        explicit SessionRepository(sqlite::Connection& db) : m_db(db) {}

        static bool HasSessionsTable(sqlite::Connection& db);
        static bool HasUnlinkedMatches(sqlite::Connection& db, const std::string& account = "");

        static int64_t SaveSession(sqlite::Connection& db, const SessionRecap& recap,
                                   std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>>* sessionIdByGeneration = nullptr,
                                   const std::string& liveAccount = "");
        static bool UpdateSession(sqlite::Connection& db, const SessionRecap& recap,
                                  const std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>>* sessionIdByGeneration = nullptr,
                                  const std::string& liveAccount = "");

        static std::vector<SessionRecap> ListSessions(sqlite::Connection& db, const std::string& account, int offset = 0, int limit = 50);
        static SessionRecap GetSession(sqlite::Connection& db, int64_t id);

        static int BackfillSessionsChunk(sqlite::Connection& db, int chunkSize, bool& outMoreRemaining, int& outLinked,
                                         const std::string& preferredAccount = "",
                                         const std::vector<std::string>& activeSessionGuids = {});
        static bool RecomputeBackfillSession(sqlite::Connection& db, int64_t sessionId, const std::string& account);
        static void LinkSessionMatches(sqlite::Connection& db, int64_t sessionId,
                                       const std::string& account,
                                       int64_t startedAt,
                                       int64_t endedAt,
                                       const std::vector<std::string>& matchGuids,
                                       int expectedGames,
                                       int64_t& outEarliestMatchTs,
                                       int64_t& outLatestMatchTs);

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository
