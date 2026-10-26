#pragma once

#include "core/state/SessionState.hpp"
#include "database/DatabaseTypes.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace database::repository {

    class ReplayRepository {
      public:
        explicit ReplayRepository(sqlite::Connection& db) : m_db(db) {}

        static bool RecordDiscoveredReplay(sqlite::Connection& db, const std::string& filePath, const std::string& fileName,
                                           const std::string& matchGuid = "", const std::string& initialState = "pending",
                                           const std::string& error = "");
        static bool UpdateReplayUploadState(sqlite::Connection& db, int64_t id, const std::string& state, int attempts,
                                            int64_t nextAttemptAt, int lastHttpStatus, const std::string& lastError,
                                            const std::string& ballchasingId = "", const std::string& ballchasingUrl = "");
        static bool SetReplayUploading(sqlite::Connection& db, int64_t id);
        static bool RecoverStuckUploadingReplays(sqlite::Connection& db);
        static std::vector<ReplayUploadRecord> GetPendingReplayUploads(sqlite::Connection& db, int limit = 10);
        static std::optional<ReplayUploadRecord> GetReplayUploadByPath(sqlite::Connection& db, const std::string& filePath);
        static ReplayUploadStatus GetReplayUploadStatus(sqlite::Connection& db);
        static bool RetryFailedReplayUploads(sqlite::Connection& db);
        static bool QueryMatchReplayInfo(sqlite::Connection& db, const std::string& matchGuid, bool& outFound,
                                         bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId);

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository
