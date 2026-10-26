#pragma once

#include "core/accounts/TrackedAccounts.hpp"
#include "database/DatabaseTypes.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace database::repository {

    class TrackedAccountsRepository {
      public:
        explicit TrackedAccountsRepository(sqlite::Connection& db) : m_db(db) {}

        static TrackedAccountInsertResult RegisterTrackedAccount(sqlite::Connection& db, const TrackedAccounts::AccountRecord& record);
        static std::vector<TrackedAccounts::AccountRecord> ListTrackedAccounts(sqlite::Connection& db);
        static std::optional<TrackedAccounts::AccountRecord> GetTrackedAccount(sqlite::Connection& db, const std::string& primaryId);
        static bool UpdateTrackedAccountNickname(sqlite::Connection& db, const std::string& primaryId, const std::string& nickname);
        static bool UpdateTrackedAccountLastSeen(sqlite::Connection& db, const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix);
        static bool UpdateTrackedAccountRefreshAttempt(sqlite::Connection& db, const std::string& primaryId, int64_t attemptedAtUnix);
        static bool UpdateTrackedAccountLastKnownName(sqlite::Connection& db, const std::string& primaryId, const std::string& lastKnownName);
        static std::optional<std::string> FindLatestPlayerName(sqlite::Connection& db, const std::string& primaryId);
        static bool SaveTrackedAccountRanks(sqlite::Connection& db, const std::string& primaryId,
                                            const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                            int64_t fetchedAtUnix);
        static std::map<std::string, TrackedAccounts::PlaylistRank> GetTrackedAccountRanks(sqlite::Connection& db, const std::string& primaryId);
        static std::vector<TrackedAccounts::RankHistorySeries> GetTrackedAccountRankHistory(
            sqlite::Connection& db, const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix = 0);
        static bool DeleteTrackedAccount(sqlite::Connection& db, const std::string& primaryId);
        static int ImportTrackedAccounts(sqlite::Connection& db, const std::vector<TrackedAccounts::AccountRecord>& records);

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository
