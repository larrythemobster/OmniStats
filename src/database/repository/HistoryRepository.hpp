#pragma once

#include "core/state/SessionState.hpp"
#include "database/sqlite/Connection.hpp"
#include "database/DatabaseTypes.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace database::repository {

    class HistoryRepository {
      public:
        explicit HistoryRepository(sqlite::Connection& db) : m_db(db) {}

        static int GetSchemaVersion(sqlite::Connection& db);
        static void GetLifetimeMmrHistory(sqlite::Connection& db, const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY);
        static void GetRecentMatchHistory(sqlite::Connection& db, const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit = kPreviousGamesDefaultLimit);
        static std::optional<PersonRecord> GetPlayerEncounterRecord(sqlite::Connection& db, const std::string& account, const std::string& primaryId);
        static void GetPeopleRecords(sqlite::Connection& db, const std::string& primaryId, std::vector<PersonRecord>& outPeople);
        static void GetMatchOutcomes(sqlite::Connection& db, const std::string& primaryId, std::vector<MatchOutcome>& outMatches);
        static void GetMatchMmrContext(sqlite::Connection& db, const std::string& primaryId, std::vector<MatchMmrContext>& out);
        static void GetGamemodeStats(sqlite::Connection& db, const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed);
        static void GetStreakStats(sqlite::Connection& db, const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss);
        static std::vector<PlayerStatRow> GetPlayerStatSeries(sqlite::Connection& db, const std::string& primaryId, const std::string& playlistFilter = "", int limit = 100);
        static std::vector<AnalyticsMatchRow> GetAnalyticsMatches(sqlite::Connection& db, const std::string& primaryId, int limit = 100);
        static void GetDetailedStatsSummary(sqlite::Connection& db, int& outMatchCount, std::string& outSinceDate);
        static void QueryMatches(sqlite::Connection& db, const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount);
        static bool GetMatchDetail(sqlite::Connection& db, int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId = "");
        static bool GetMatchDetailByGuid(sqlite::Connection& db, const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId = "");
        static std::optional<std::vector<PersonRecord>> QueryEncounterRecords(sqlite::Connection& db, const std::string& account, const std::string& onlyPlayer = {}, int64_t excludeMatchId = 0);

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository
