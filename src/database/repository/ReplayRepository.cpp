#include "ReplayRepository.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include <chrono>
#include <ctime>
#include <utility>

namespace database::repository {

    namespace {

        int64_t NowUnix() {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

    } // namespace

    bool ReplayRepository::RecordDiscoveredReplay(sqlite::Connection& db, const std::string& filePath, const std::string& fileName,
                                                  const std::string& matchGuid, const std::string& initialState,
                                                  const std::string& error) {
        if (!db.IsOpen() || filePath.empty() || !db.TableExists("ReplayUploads")) return false;

        const char* sql =
            "INSERT OR IGNORE INTO ReplayUploads (file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, created_at, updated_at) "
            "VALUES (?1, ?2, NULLIF(?3, ''), ?4, 0, 0, 0, NULLIF(?5, ''), ?6, ?6);";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, filePath);
            stmt.Bind(2, fileName);
            stmt.Bind(3, matchGuid);
            stmt.Bind(4, initialState);
            stmt.Bind(5, error);
            stmt.Bind(6, NowUnix());
            stmt.Execute();
            return db.Changes() > 0;
        } catch (...) {
            return false;
        }
    }

    bool ReplayRepository::UpdateReplayUploadState(sqlite::Connection& db, int64_t id, const std::string& state, int attempts,
                                                   int64_t nextAttemptAt, int lastHttpStatus, const std::string& lastError,
                                                   const std::string& ballchasingId, const std::string& ballchasingUrl) {
        if (!db.IsOpen() || id <= 0 || !db.TableExists("ReplayUploads")) return false;

        const char* sql =
            "UPDATE ReplayUploads SET state = ?1, attempts = ?2, next_attempt_at = ?3, last_http_status = ?4, "
            "last_error = NULLIF(?5, ''), ballchasing_id = COALESCE(NULLIF(?6, ''), ballchasing_id), "
            "ballchasing_url = COALESCE(NULLIF(?7, ''), ballchasing_url), updated_at = ?8 WHERE id = ?9;";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, state);
            stmt.Bind(2, attempts);
            stmt.Bind(3, nextAttemptAt);
            stmt.Bind(4, lastHttpStatus);
            stmt.Bind(5, lastError);
            stmt.Bind(6, ballchasingId);
            stmt.Bind(7, ballchasingUrl);
            stmt.Bind(8, NowUnix());
            stmt.Bind(9, id);
            stmt.Execute();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ReplayRepository::SetReplayUploading(sqlite::Connection& db, int64_t id) {
        if (!db.IsOpen() || id <= 0 || !db.TableExists("ReplayUploads")) return false;

        try {
            sqlite::Statement stmt(db, "UPDATE ReplayUploads SET state = 'uploading', updated_at = ?1 WHERE id = ?2;");
            stmt.Bind(1, NowUnix());
            stmt.Bind(2, id);
            stmt.Execute();
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ReplayRepository::RecoverStuckUploadingReplays(sqlite::Connection& db) {
        if (!db.IsOpen() || !db.TableExists("ReplayUploads")) return false;

        const char* sql =
            "UPDATE ReplayUploads SET state = 'pending', updated_at = ?1 "
            "WHERE state = 'uploading';";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, NowUnix());
            stmt.Execute();
            return true;
        } catch (...) {
            return false;
        }
    }

    std::vector<ReplayUploadRecord> ReplayRepository::GetPendingReplayUploads(sqlite::Connection& db, int limit) {
        std::vector<ReplayUploadRecord> records;
        if (!db.IsOpen() || !db.TableExists("ReplayUploads")) return records;

        const char* sql =
            "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, "
            "last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
            "FROM ReplayUploads WHERE state = 'pending' AND next_attempt_at <= ?1 "
            "ORDER BY id ASC LIMIT ?2;";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, NowUnix());
            stmt.Bind(2, limit > 0 ? limit : 10);

            while (stmt.Step()) {
                ReplayUploadRecord r;
                r.id = stmt.Get<int64_t>(0);
                r.filePath = stmt.Get<std::string>(1);
                r.fileName = stmt.Get<std::string>(2);
                r.matchGuid = stmt.Get<std::string>(3);
                r.state = stmt.Get<std::string>(4);
                r.attempts = stmt.Get<int>(5);
                r.nextAttemptAt = stmt.Get<int64_t>(6);
                r.lastHttpStatus = stmt.Get<int>(7);
                r.lastError = stmt.Get<std::string>(8);
                r.ballchasingId = stmt.Get<std::string>(9);
                r.ballchasingUrl = stmt.Get<std::string>(10);
                r.createdAt = stmt.Get<int64_t>(11);
                r.updatedAt = stmt.Get<int64_t>(12);
                records.push_back(std::move(r));
            }
        } catch (...) {
        }
        return records;
    }

    std::optional<ReplayUploadRecord> ReplayRepository::GetReplayUploadByPath(sqlite::Connection& db, const std::string& filePath) {
        if (!db.IsOpen() || filePath.empty() || !db.TableExists("ReplayUploads")) return std::nullopt;

        const char* sql =
            "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, "
            "last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
            "FROM ReplayUploads WHERE file_path = ?1 LIMIT 1;";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, filePath);
            if (stmt.Step()) {
                ReplayUploadRecord r;
                r.id = stmt.Get<int64_t>(0);
                r.filePath = stmt.Get<std::string>(1);
                r.fileName = stmt.Get<std::string>(2);
                r.matchGuid = stmt.Get<std::string>(3);
                r.state = stmt.Get<std::string>(4);
                r.attempts = stmt.Get<int>(5);
                r.nextAttemptAt = stmt.Get<int64_t>(6);
                r.lastHttpStatus = stmt.Get<int>(7);
                r.lastError = stmt.Get<std::string>(8);
                r.ballchasingId = stmt.Get<std::string>(9);
                r.ballchasingUrl = stmt.Get<std::string>(10);
                r.createdAt = stmt.Get<int64_t>(11);
                r.updatedAt = stmt.Get<int64_t>(12);
                return r;
            }
        } catch (...) {
        }
        return std::nullopt;
    }

    ReplayUploadStatus ReplayRepository::GetReplayUploadStatus(sqlite::Connection& db) {
        ReplayUploadStatus status;
        if (!db.IsOpen() || !db.TableExists("ReplayUploads")) return status;

        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm tmNow{};
        localtime_s(&tmNow, &nowTime);
        tmNow.tm_hour = 0;
        tmNow.tm_min = 0;
        tmNow.tm_sec = 0;
        int64_t startOfToday = static_cast<int64_t>(std::mktime(&tmNow));

        const char* sql =
            "SELECT "
            "SUM(CASE WHEN state IN ('done', 'duplicate') AND updated_at >= ?1 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN state = 'pending' AND attempts > 0 THEN 1 ELSE 0 END), "
            "SUM(CASE WHEN state = 'failed' THEN 1 ELSE 0 END), "
            "COUNT(*) "
            "FROM ReplayUploads;";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, startOfToday);
            if (stmt.Step()) {
                status.uploadedToday = stmt.IsNull(0) ? 0 : stmt.Get<int>(0);
                status.retrying = stmt.IsNull(1) ? 0 : stmt.Get<int>(1);
                status.failed = stmt.IsNull(2) ? 0 : stmt.Get<int>(2);
                status.total = stmt.IsNull(3) ? 0 : stmt.Get<int>(3);
            }
        } catch (...) {
        }
        return status;
    }

    bool ReplayRepository::RetryFailedReplayUploads(sqlite::Connection& db) {
        if (!db.IsOpen() || !db.TableExists("ReplayUploads")) return false;

        const char* sql = "UPDATE ReplayUploads SET state = 'pending', next_attempt_at = 0, last_error = NULL WHERE state = 'failed';";
        try {
            db.Execute(sql);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ReplayRepository::QueryMatchReplayInfo(sqlite::Connection& db, const std::string& matchGuid, bool& outFound,
                                                bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId) {
        outFound = false;
        outRanked = false;
        outWin = false;
        outResultPending = false;
        outPlaylistId = -1;
        if (matchGuid.empty() || !db.IsOpen()) return false;

        const char* sql = "SELECT playlist_id, win, COALESCE(result_pending, 0), gamemode FROM Matches WHERE match_guid = ?1 ORDER BY id DESC LIMIT 1;";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, matchGuid);
            if (stmt.Step()) {
                outFound = true;
                bool hasPlaylistId = !stmt.IsNull(0);
                outPlaylistId = hasPlaylistId ? stmt.Get<int>(0) : -1;
                outWin = stmt.Get<int>(1) != 0;
                outResultPending = stmt.Get<int>(2) != 0;
                std::string gamemode = stmt.Get<std::string>(3);
                if (hasPlaylistId) {
                    if (const auto* info = PlaylistMetadata::Find(outPlaylistId)) {
                        outRanked = (info->playlistClass == PlaylistMetadata::PlaylistClass::Ranked ||
                                     info->playlistClass == PlaylistMetadata::PlaylistClass::Tournament);
                    } else {
                        outRanked = false;
                    }
                } else {
                    outRanked = (gamemode != "casual");
                }
            }
        } catch (...) {
            return false;
        }
        return outFound;
    }

} // namespace database::repository
