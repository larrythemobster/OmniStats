#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include <chrono>
#include <ctime>
#include <utility>

using namespace DatabaseDetail;

bool DatabaseManager::RecordDiscoveredReplay(const std::string& filePath, const std::string& fileName, const std::string& matchGuid, const std::string& initialState, const std::string& error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || filePath.empty() || !SqliteTableExists(m_db, "ReplayUploads")) return false;
    const char* sql =
        "INSERT OR IGNORE INTO ReplayUploads (file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, created_at, updated_at) "
        "VALUES (?1, ?2, NULLIF(?3, ''), ?4, 0, 0, 0, NULLIF(?5, ''), ?6, ?6);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, fileName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, matchGuid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, initialState.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, error.c_str(), -1, SQLITE_TRANSIENT);
    int64_t nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 6, nowUnix);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(stmt);
    return ok && changes > 0;
}

bool DatabaseManager::UpdateReplayUploadState(int64_t id, const std::string& state, int attempts, int64_t nextAttemptAt, int lastHttpStatus, const std::string& lastError, const std::string& ballchasingId, const std::string& ballchasingUrl) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || id <= 0 || !SqliteTableExists(m_db, "ReplayUploads")) return false;
    const char* sql =
        "UPDATE ReplayUploads SET state = ?1, attempts = ?2, next_attempt_at = ?3, last_http_status = ?4, "
        "last_error = NULLIF(?5, ''), ballchasing_id = COALESCE(NULLIF(?6, ''), ballchasing_id), "
        "ballchasing_url = COALESCE(NULLIF(?7, ''), ballchasing_url), updated_at = ?8 WHERE id = ?9;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, state.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, attempts);
    sqlite3_bind_int64(stmt, 3, nextAttemptAt);
    sqlite3_bind_int(stmt, 4, lastHttpStatus);
    sqlite3_bind_text(stmt, 5, lastError.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, ballchasingId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, ballchasingUrl.c_str(), -1, SQLITE_TRANSIENT);
    int64_t nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 8, nowUnix);
    sqlite3_bind_int64(stmt, 9, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::SetReplayUploading(int64_t id) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || id <= 0 || !SqliteTableExists(m_db, "ReplayUploads")) return false;
    const char* sql = "UPDATE ReplayUploads SET state = 'uploading', updated_at = ?1 WHERE id = ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    int64_t nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 1, nowUnix);
    sqlite3_bind_int64(stmt, 2, id);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::RecoverStuckUploadingReplays() {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "ReplayUploads")) return false;
    const char* sql = "UPDATE ReplayUploads SET state = 'pending', next_attempt_at = 0 WHERE state = 'uploading';";
    return sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::vector<ReplayUploadRecord> DatabaseManager::GetPendingReplayUploads(int limit) {
    std::vector<ReplayUploadRecord> records;
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "ReplayUploads")) return records;
    if (limit <= 0) limit = 10;
    const char* sql =
        "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, "
        "last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
        "FROM ReplayUploads WHERE state = 'pending' AND next_attempt_at <= ?1 ORDER BY id ASC LIMIT ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return records;
    int64_t nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    sqlite3_bind_int64(stmt, 1, nowUnix);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ReplayUploadRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.filePath = SqlColumnText(stmt, 1);
        r.fileName = SqlColumnText(stmt, 2);
        r.matchGuid = SqlColumnText(stmt, 3);
        r.state = SqlColumnText(stmt, 4);
        r.attempts = sqlite3_column_int(stmt, 5);
        r.nextAttemptAt = sqlite3_column_int64(stmt, 6);
        r.lastHttpStatus = sqlite3_column_int(stmt, 7);
        r.lastError = SqlColumnText(stmt, 8);
        r.ballchasingId = SqlColumnText(stmt, 9);
        r.ballchasingUrl = SqlColumnText(stmt, 10);
        r.createdAt = sqlite3_column_int64(stmt, 11);
        r.updatedAt = sqlite3_column_int64(stmt, 12);
        records.push_back(std::move(r));
    }
    sqlite3_finalize(stmt);
    return records;
}

std::optional<ReplayUploadRecord> DatabaseManager::GetReplayUploadByPath(const std::string& filePath) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || filePath.empty() || !SqliteTableExists(db, "ReplayUploads")) return std::nullopt;
    const char* sql =
        "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, "
        "last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
        "FROM ReplayUploads WHERE file_path = ?1 LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, filePath.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<ReplayUploadRecord> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ReplayUploadRecord r;
        r.id = sqlite3_column_int64(stmt, 0);
        r.filePath = SqlColumnText(stmt, 1);
        r.fileName = SqlColumnText(stmt, 2);
        r.matchGuid = SqlColumnText(stmt, 3);
        r.state = SqlColumnText(stmt, 4);
        r.attempts = sqlite3_column_int(stmt, 5);
        r.nextAttemptAt = sqlite3_column_int64(stmt, 6);
        r.lastHttpStatus = sqlite3_column_int(stmt, 7);
        r.lastError = SqlColumnText(stmt, 8);
        r.ballchasingId = SqlColumnText(stmt, 9);
        r.ballchasingUrl = SqlColumnText(stmt, 10);
        r.createdAt = sqlite3_column_int64(stmt, 11);
        r.updatedAt = sqlite3_column_int64(stmt, 12);
        result = std::move(r);
    }
    sqlite3_finalize(stmt);
    return result;
}

ReplayUploadStatus DatabaseManager::GetReplayUploadStatus() {
    ReplayUploadStatus status;
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "ReplayUploads")) return status;

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
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, startOfToday);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            status.uploadedToday = sqlite3_column_int(stmt, 0);
            status.retrying = sqlite3_column_int(stmt, 1);
            status.failed = sqlite3_column_int(stmt, 2);
            status.total = sqlite3_column_int(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }
    return status;
}

bool DatabaseManager::RetryFailedReplayUploads() {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "ReplayUploads")) return false;
    const char* sql = "UPDATE ReplayUploads SET state = 'pending', next_attempt_at = 0, last_error = NULL WHERE state = 'failed';";
    return sqlite3_exec(m_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool DatabaseManager::QueryMatchReplayInfo(const std::string& matchGuid, bool& outFound, bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId) {
    outFound = false;
    outRanked = false;
    outWin = false;
    outResultPending = false;
    outPlaylistId = -1;
    if (matchGuid.empty()) return false;

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return false;

    const char* sql = "SELECT playlist_id, win, COALESCE(result_pending, 0), gamemode FROM Matches WHERE match_guid = ?1 ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, matchGuid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        outFound = true;
        bool hasPlaylistId = (sqlite3_column_type(stmt, 0) != SQLITE_NULL);
        outPlaylistId = hasPlaylistId ? sqlite3_column_int(stmt, 0) : -1;
        outWin = sqlite3_column_int(stmt, 1) != 0;
        outResultPending = sqlite3_column_int(stmt, 2) != 0;
        std::string gamemode = SqlColumnText(stmt, 3);
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
    sqlite3_finalize(stmt);
    return outFound;
}
