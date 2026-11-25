#pragma once

#include "database/DatabaseTypes.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace database::repository {

    class SeasonRepository {
      public:
        static bool UpsertSeasons(sqlite::Connection& db, const std::vector<SeasonRecord>& seasons);
        static std::vector<SeasonRecord> GetSeasons(sqlite::Connection& db);
        static bool ObserveRankedSeason(sqlite::Connection& db, const std::string& primaryId, const std::string& playlistKey, int matchesPlayed, int64_t observedAt);
        static bool SaveTrackedAccountRewards(sqlite::Connection& db, const std::string& primaryId, int season, int level, int wins, int64_t observedAt);
        static std::optional<TrackedAccountReward> GetTrackedAccountRewards(sqlite::Connection& db, const std::string& primaryId, int season);
        static std::vector<SeasonSummary> GetSeasonSummaries(sqlite::Connection& db, const std::string& primaryId);
    };

} // namespace database::repository
