#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"

#include <chrono>
#include <set>
#include <unordered_map>
using namespace DatabaseDetail;

TrackedAccountInsertResult DatabaseManager::RegisterTrackedAccount(const TrackedAccounts::AccountRecord& record) {
    if (record.primaryId.empty()) {
        return TrackedAccountInsertResult::Failed;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return TrackedAccountInsertResult::Failed;
    }

    TrackedAccounts::Platform platform = record.platform;
    std::string platformAccountId = record.platformAccountId;
    if (platformAccountId.empty()) {
        if (auto id = TrackedAccounts::FromPrimaryId(record.primaryId)) {
            platform = id->platform;
            platformAccountId = id->accountId;
        }
    }
    if (platformAccountId.empty()) {
        return TrackedAccountInsertResult::Failed;
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return TrackedAccountInsertResult::Failed;
    }

    const char* existsSql =
        "SELECT 1 FROM TrackedAccounts WHERE primary_id = ?1 OR (platform = ?2 AND platform_account_id = ?3) LIMIT 1;";
    sqlite3_stmt* checkStmt = nullptr;
    bool alreadyExists = false;
    if (sqlite3_prepare_v2(m_db, existsSql, -1, &checkStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(checkStmt, 1, record.primaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(checkStmt, 2, TrackedAccounts::PlatformKey(platform), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(checkStmt, 3, platformAccountId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(checkStmt) == SQLITE_ROW) {
            alreadyExists = true;
        }
        sqlite3_finalize(checkStmt);
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return TrackedAccountInsertResult::Failed;
    }

    if (alreadyExists) {
        sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr);
        return TrackedAccountInsertResult::AlreadyExists;
    }

    int64_t sortOrder = record.sortOrder;
    if (sortOrder <= 0) {
        const char* maxSql = "SELECT COALESCE(MAX(sort_order), 0) + 1 FROM TrackedAccounts;";
        sqlite3_stmt* maxStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, maxSql, -1, &maxStmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(maxStmt) == SQLITE_ROW) {
                sortOrder = sqlite3_column_int64(maxStmt, 0);
            }
            sqlite3_finalize(maxStmt);
        }
        if (sortOrder <= 0) sortOrder = 1;
    }

    int64_t createdAt = record.createdAtUnix;
    if (createdAt <= 0) {
        createdAt = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    }

    const char* insertSql =
        "INSERT INTO TrackedAccounts (primary_id, platform, platform_account_id, nickname, last_known_name, "
        "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);";
    sqlite3_stmt* insStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &insStmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return TrackedAccountInsertResult::Failed;
    }

    sqlite3_bind_text(insStmt, 1, record.primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insStmt, 2, TrackedAccounts::PlatformKey(platform), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insStmt, 3, platformAccountId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insStmt, 4, record.nickname.c_str(), -1, SQLITE_TRANSIENT);
    if (record.lastKnownName.empty()) {
        sqlite3_bind_null(insStmt, 5);
    } else {
        sqlite3_bind_text(insStmt, 5, record.lastKnownName.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int64(insStmt, 6, createdAt);
    if (record.lastSeenAtUnix <= 0) {
        sqlite3_bind_null(insStmt, 7);
    } else {
        sqlite3_bind_int64(insStmt, 7, record.lastSeenAtUnix);
    }
    if (record.lastRefreshAttemptAtUnix <= 0) {
        sqlite3_bind_null(insStmt, 8);
    } else {
        sqlite3_bind_int64(insStmt, 8, record.lastRefreshAttemptAtUnix);
    }
    if (record.lastSuccessfulRefreshAtUnix <= 0) {
        sqlite3_bind_null(insStmt, 9);
    } else {
        sqlite3_bind_int64(insStmt, 9, record.lastSuccessfulRefreshAtUnix);
    }
    sqlite3_bind_int64(insStmt, 10, sortOrder);
    sqlite3_bind_text(insStmt, 11, TrackedAccounts::DiscoverySourceKey(record.discoverySource), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(insStmt) != SQLITE_DONE) {
        sqlite3_finalize(insStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return TrackedAccountInsertResult::Failed;
    }
    sqlite3_finalize(insStmt);

    if (!record.ranks.empty() && SqliteTableExists(m_db, "TrackedAccountRanks")) {
        const char* rankSql =
            "INSERT INTO TrackedAccountRanks (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(primary_id, playlist_key) DO UPDATE SET "
            "mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played, "
            "source = excluded.source, fetched_at = excluded.fetched_at;";
        sqlite3_stmt* rankStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, rankSql, -1, &rankStmt, nullptr) == SQLITE_OK) {
            for (const auto& [key, rank] : record.ranks) {
                sqlite3_reset(rankStmt);
                sqlite3_clear_bindings(rankStmt);
                sqlite3_bind_text(rankStmt, 1, record.primaryId.c_str(), -1, SQLITE_TRANSIENT);
                const std::string& pKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                sqlite3_bind_text(rankStmt, 2, pKey.c_str(), -1, SQLITE_TRANSIENT);
                if (rank.mmr.has_value()) {
                    sqlite3_bind_int(rankStmt, 3, *rank.mmr);
                } else {
                    sqlite3_bind_null(rankStmt, 3);
                }
                if (rank.rankTier.empty()) {
                    sqlite3_bind_null(rankStmt, 4);
                } else {
                    sqlite3_bind_text(rankStmt, 4, rank.rankTier.c_str(), -1, SQLITE_TRANSIENT);
                }
                if (rank.matchesPlayed.has_value()) {
                    sqlite3_bind_int(rankStmt, 5, *rank.matchesPlayed);
                } else {
                    sqlite3_bind_null(rankStmt, 5);
                }
                if (rank.source.empty()) {
                    sqlite3_bind_null(rankStmt, 6);
                } else {
                    sqlite3_bind_text(rankStmt, 6, rank.source.c_str(), -1, SQLITE_TRANSIENT);
                }
                int64_t rankFetched = rank.fetchedAtUnix <= 0 ? createdAt : rank.fetchedAtUnix;
                sqlite3_bind_int64(rankStmt, 7, rankFetched);
                if (sqlite3_step(rankStmt) != SQLITE_DONE) {
                    sqlite3_finalize(rankStmt);
                    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    return TrackedAccountInsertResult::Failed;
                }
            }
            sqlite3_finalize(rankStmt);
            // Preserve imported verified snapshots in historical tracking without
            // pretending earlier periods contain observations.
            const char* obsSql =
                "INSERT OR IGNORE INTO TrackedAccountRankObservations "
                "(primary_id, playlist_key, mmr, rank_tier, matches_played, source, observed_at) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";
            sqlite3_stmt* obsStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, obsSql, -1, &obsStmt, nullptr) != SQLITE_OK) {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return TrackedAccountInsertResult::Failed;
            }
            for (const auto& [key, rank] : record.ranks) {
                if (!rank.mmr || *rank.mmr <= 0 || rank.fetchedAtUnix <= 0) continue;
                const std::string& pKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                if (!TrackedAccounts::IsSupportedPlaylistKey(pKey)) continue;
                sqlite3_reset(obsStmt);
                sqlite3_clear_bindings(obsStmt);
                sqlite3_bind_text(obsStmt, 1, record.primaryId.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(obsStmt, 2, pKey.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(obsStmt, 3, *rank.mmr);
                if (!rank.rankTier.empty())
                    sqlite3_bind_text(obsStmt, 4, rank.rankTier.c_str(), -1, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(obsStmt, 4);
                if (rank.matchesPlayed)
                    sqlite3_bind_int(obsStmt, 5, *rank.matchesPlayed);
                else
                    sqlite3_bind_null(obsStmt, 5);
                if (!rank.source.empty())
                    sqlite3_bind_text(obsStmt, 6, rank.source.c_str(), -1, SQLITE_TRANSIENT);
                else
                    sqlite3_bind_null(obsStmt, 6);
                sqlite3_bind_int64(obsStmt, 7, rank.fetchedAtUnix);
                if (sqlite3_step(obsStmt) != SQLITE_DONE) {
                    sqlite3_finalize(obsStmt);
                    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    return TrackedAccountInsertResult::Failed;
                }
            }
            sqlite3_finalize(obsStmt);
        } else {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return TrackedAccountInsertResult::Failed;
        }
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return TrackedAccountInsertResult::Failed;
    }

    return TrackedAccountInsertResult::Inserted;
}

std::vector<TrackedAccounts::AccountRecord> DatabaseManager::ListTrackedAccounts() {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "TrackedAccounts")) {
        return {};
    }

    std::unordered_map<std::string, std::map<std::string, TrackedAccounts::PlaylistRank>> ranksByPrimary;
    if (SqliteTableExists(db, "TrackedAccountRanks")) {
        const char* rankSql =
            "SELECT primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at FROM TrackedAccountRanks;";
        sqlite3_stmt* rankStmt = nullptr;
        if (sqlite3_prepare_v2(db, rankSql, -1, &rankStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(rankStmt) == SQLITE_ROW) {
                std::string pid = SqlColumnText(rankStmt, 0);
                TrackedAccounts::PlaylistRank rank;
                rank.playlistKey = SqlColumnText(rankStmt, 1);
                if (sqlite3_column_type(rankStmt, 2) != SQLITE_NULL) {
                    rank.mmr = sqlite3_column_int(rankStmt, 2);
                }
                if (sqlite3_column_type(rankStmt, 3) != SQLITE_NULL) {
                    rank.rankTier = SqlColumnText(rankStmt, 3);
                }
                if (sqlite3_column_type(rankStmt, 4) != SQLITE_NULL) {
                    rank.matchesPlayed = sqlite3_column_int(rankStmt, 4);
                }
                if (sqlite3_column_type(rankStmt, 5) != SQLITE_NULL) {
                    rank.source = SqlColumnText(rankStmt, 5);
                }
                if (sqlite3_column_type(rankStmt, 6) != SQLITE_NULL) {
                    rank.fetchedAtUnix = sqlite3_column_int64(rankStmt, 6);
                }
                ranksByPrimary[pid][rank.playlistKey] = std::move(rank);
            }
            sqlite3_finalize(rankStmt);
        }
    }

    std::vector<TrackedAccounts::AccountRecord> records;
    const char* selSql =
        "SELECT primary_id, platform, platform_account_id, nickname, last_known_name, "
        "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source "
        "FROM TrackedAccounts ORDER BY sort_order ASC, created_at ASC, primary_id ASC;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, selSql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TrackedAccounts::AccountRecord rec;
            rec.primaryId = SqlColumnText(stmt, 0);
            rec.platform = TrackedAccounts::ParsePlatform(SqlColumnText(stmt, 1)).value_or(TrackedAccounts::Platform::Epic);
            rec.platformAccountId = SqlColumnText(stmt, 2);
            rec.nickname = SqlColumnText(stmt, 3);
            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                rec.lastKnownName = SqlColumnText(stmt, 4);
            }
            if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                rec.createdAtUnix = sqlite3_column_int64(stmt, 5);
            }
            if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
                rec.lastSeenAtUnix = sqlite3_column_int64(stmt, 6);
            }
            if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
                rec.lastRefreshAttemptAtUnix = sqlite3_column_int64(stmt, 7);
            }
            if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
                rec.lastSuccessfulRefreshAtUnix = sqlite3_column_int64(stmt, 8);
            }
            rec.sortOrder = sqlite3_column_int64(stmt, 9);
            rec.discoverySource = TrackedAccounts::ParseDiscoverySource(SqlColumnText(stmt, 10));

            auto it = ranksByPrimary.find(rec.primaryId);
            if (it != ranksByPrimary.end()) {
                rec.ranks = std::move(it->second);
            }
            records.push_back(std::move(rec));
        }
        sqlite3_finalize(stmt);
    }
    return records;
}

std::optional<TrackedAccounts::AccountRecord> DatabaseManager::GetTrackedAccount(const std::string& primaryId) {
    if (primaryId.empty()) {
        return std::nullopt;
    }
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "TrackedAccounts")) {
        return std::nullopt;
    }

    const char* selSql =
        "SELECT primary_id, platform, platform_account_id, nickname, last_known_name, "
        "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source "
        "FROM TrackedAccounts WHERE primary_id = ?1 LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, selSql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<TrackedAccounts::AccountRecord> record;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        TrackedAccounts::AccountRecord rec;
        rec.primaryId = SqlColumnText(stmt, 0);
        rec.platform = TrackedAccounts::ParsePlatform(SqlColumnText(stmt, 1)).value_or(TrackedAccounts::Platform::Epic);
        rec.platformAccountId = SqlColumnText(stmt, 2);
        rec.nickname = SqlColumnText(stmt, 3);
        if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
            rec.lastKnownName = SqlColumnText(stmt, 4);
        }
        if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
            rec.createdAtUnix = sqlite3_column_int64(stmt, 5);
        }
        if (sqlite3_column_type(stmt, 6) != SQLITE_NULL) {
            rec.lastSeenAtUnix = sqlite3_column_int64(stmt, 6);
        }
        if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
            rec.lastRefreshAttemptAtUnix = sqlite3_column_int64(stmt, 7);
        }
        if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
            rec.lastSuccessfulRefreshAtUnix = sqlite3_column_int64(stmt, 8);
        }
        rec.sortOrder = sqlite3_column_int64(stmt, 9);
        rec.discoverySource = TrackedAccounts::ParseDiscoverySource(SqlColumnText(stmt, 10));
        record = std::move(rec);
    }
    sqlite3_finalize(stmt);

    if (record.has_value() && SqliteTableExists(db, "TrackedAccountRanks")) {
        const char* rankSql =
            "SELECT playlist_key, mmr, rank_tier, matches_played, source, fetched_at "
            "FROM TrackedAccountRanks WHERE primary_id = ?1;";
        sqlite3_stmt* rankStmt = nullptr;
        if (sqlite3_prepare_v2(db, rankSql, -1, &rankStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(rankStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(rankStmt) == SQLITE_ROW) {
                TrackedAccounts::PlaylistRank rank;
                rank.playlistKey = SqlColumnText(rankStmt, 0);
                if (sqlite3_column_type(rankStmt, 1) != SQLITE_NULL) {
                    rank.mmr = sqlite3_column_int(rankStmt, 1);
                }
                if (sqlite3_column_type(rankStmt, 2) != SQLITE_NULL) {
                    rank.rankTier = SqlColumnText(rankStmt, 2);
                }
                if (sqlite3_column_type(rankStmt, 3) != SQLITE_NULL) {
                    rank.matchesPlayed = sqlite3_column_int(rankStmt, 3);
                }
                if (sqlite3_column_type(rankStmt, 4) != SQLITE_NULL) {
                    rank.source = SqlColumnText(rankStmt, 4);
                }
                if (sqlite3_column_type(rankStmt, 5) != SQLITE_NULL) {
                    rank.fetchedAtUnix = sqlite3_column_int64(rankStmt, 5);
                }
                record->ranks[rank.playlistKey] = std::move(rank);
            }
            sqlite3_finalize(rankStmt);
        }
    }

    return record;
}

bool DatabaseManager::UpdateTrackedAccountNickname(const std::string& primaryId, const std::string& nickname) {
    if (primaryId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return false;
    }

    const char* sql = "UPDATE TrackedAccounts SET nickname = ?1 WHERE primary_id = ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, nickname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(m_db) > 0;
}

bool DatabaseManager::UpdateTrackedAccountLastSeen(const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix) {
    if (primaryId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return false;
    }

    sqlite3_stmt* stmt = nullptr;
    if (!lastKnownName.empty()) {
        const char* sql = "UPDATE TrackedAccounts SET last_seen_at = ?1, last_known_name = ?2 WHERE primary_id = ?3;";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        if (seenAtUnix <= 0) {
            sqlite3_bind_null(stmt, 1);
        } else {
            sqlite3_bind_int64(stmt, 1, seenAtUnix);
        }
        sqlite3_bind_text(stmt, 2, lastKnownName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    } else {
        const char* sql = "UPDATE TrackedAccounts SET last_seen_at = ?1 WHERE primary_id = ?2;";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        if (seenAtUnix <= 0) {
            sqlite3_bind_null(stmt, 1);
        } else {
            sqlite3_bind_int64(stmt, 1, seenAtUnix);
        }
        sqlite3_bind_text(stmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    }

    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(m_db) > 0;
}

bool DatabaseManager::UpdateTrackedAccountRefreshAttempt(const std::string& primaryId, int64_t attemptedAtUnix) {
    if (primaryId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return false;
    }

    const char* sql = "UPDATE TrackedAccounts SET last_refresh_attempt_at = ?1 WHERE primary_id = ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    if (attemptedAtUnix <= 0) {
        sqlite3_bind_null(stmt, 1);
    } else {
        sqlite3_bind_int64(stmt, 1, attemptedAtUnix);
    }
    sqlite3_bind_text(stmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(m_db) > 0;
}

bool DatabaseManager::UpdateTrackedAccountLastKnownName(const std::string& primaryId, const std::string& lastKnownName) {
    if (primaryId.empty() || lastKnownName.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return false;
    }

    const char* sql = "UPDATE TrackedAccounts SET last_known_name = ?1 WHERE primary_id = ?2;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(stmt, 1, lastKnownName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return ok && sqlite3_changes(m_db) > 0;
}

std::optional<std::string> DatabaseManager::FindLatestPlayerName(const std::string& primaryId) {
    if (primaryId.empty()) {
        return std::nullopt;
    }
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) {
        return std::nullopt;
    }

    const char* sql =
        "SELECT mp.name FROM MatchPlayers mp JOIN Matches m ON m.id = mp.match_id "
        "WHERE (lower(mp.primary_id) = lower(?1) OR lower(mp.primary_id) LIKE lower(?2)) "
        "AND mp.name IS NOT NULL AND mp.name <> '' "
        "ORDER BY m.timestamp DESC, m.id DESC LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    const std::string splitScreenPattern = primaryId + "|%";
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, splitScreenPattern.c_str(), -1, SQLITE_TRANSIENT);
    std::optional<std::string> name;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        name = SqlColumnText(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return name;
}

bool DatabaseManager::SaveTrackedAccountRanks(const std::string& primaryId,
                                              const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                              int64_t fetchedAtUnix) {
    if (primaryId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts") || !SqliteTableExists(m_db, "TrackedAccountRanks")) {
        return false;
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* checkSql = "SELECT 1 FROM TrackedAccounts WHERE primary_id = ?1 LIMIT 1;";
    sqlite3_stmt* checkStmt = nullptr;
    bool exists = false;
    if (sqlite3_prepare_v2(m_db, checkSql, -1, &checkStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(checkStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(checkStmt) == SQLITE_ROW) {
            exists = true;
        }
        sqlite3_finalize(checkStmt);
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!exists) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    int64_t effectiveFetched = fetchedAtUnix;
    if (effectiveFetched <= 0) {
        effectiveFetched = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
    }

    const char* updAccountSql =
        "UPDATE TrackedAccounts SET "
        "last_refresh_attempt_at = MAX(COALESCE(last_refresh_attempt_at,0), ?1), "
        "last_successful_refresh_at = MAX(COALESCE(last_successful_refresh_at,0), ?1) "
        "WHERE primary_id = ?2;";
    sqlite3_stmt* updStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, updAccountSql, -1, &updStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(updStmt, 1, effectiveFetched);
        sqlite3_bind_text(updStmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(updStmt);
        sqlite3_finalize(updStmt);
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (!ranks.empty()) {
        const char* upsertRankSql =
            "INSERT INTO TrackedAccountRanks (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(primary_id, playlist_key) DO UPDATE SET "
            "mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played, "
            "source = excluded.source, fetched_at = excluded.fetched_at "
            "WHERE excluded.fetched_at >= TrackedAccountRanks.fetched_at;";
        sqlite3_stmt* rankStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, upsertRankSql, -1, &rankStmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }

        for (const auto& rank : ranks) {
            if (rank.playlistKey.empty()) continue;
            sqlite3_reset(rankStmt);
            sqlite3_clear_bindings(rankStmt);
            sqlite3_bind_text(rankStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(rankStmt, 2, rank.playlistKey.c_str(), -1, SQLITE_TRANSIENT);
            if (rank.mmr.has_value()) {
                sqlite3_bind_int(rankStmt, 3, *rank.mmr);
            } else {
                sqlite3_bind_null(rankStmt, 3);
            }
            if (rank.rankTier.empty()) {
                sqlite3_bind_null(rankStmt, 4);
            } else {
                sqlite3_bind_text(rankStmt, 4, rank.rankTier.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (rank.matchesPlayed.has_value()) {
                sqlite3_bind_int(rankStmt, 5, *rank.matchesPlayed);
            } else {
                sqlite3_bind_null(rankStmt, 5);
            }
            if (rank.source.empty()) {
                sqlite3_bind_null(rankStmt, 6);
            } else {
                sqlite3_bind_text(rankStmt, 6, rank.source.c_str(), -1, SQLITE_TRANSIENT);
            }
            int64_t rFetched = rank.fetchedAtUnix <= 0 ? effectiveFetched : rank.fetchedAtUnix;
            sqlite3_bind_int64(rankStmt, 7, rFetched);
            if (sqlite3_step(rankStmt) != SQLITE_DONE) {
                sqlite3_finalize(rankStmt);
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
        }
        sqlite3_finalize(rankStmt);

        // Record only valid, successfully fetched MMR values. Repeated identical
        // readings within six hours do not create high-frequency duplicates, but
        // every MMR or rank change remains a distinct observation.
        const char* observeSql =
            "INSERT OR IGNORE INTO TrackedAccountRankObservations "
            "(primary_id, playlist_key, mmr, rank_tier, matches_played, source, observed_at) "
            "SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7 "
            "WHERE NOT EXISTS (SELECT 1 FROM TrackedAccountRankObservations "
            "WHERE primary_id=?1 AND playlist_key=?2 AND mmr=?3 "
            "AND COALESCE(rank_tier,'')=COALESCE(?4,'') "
            "AND COALESCE(source,'')=COALESCE(?6,'') "
            "AND observed_at BETWEEN ?7 - 21600 AND ?7);";
        sqlite3_stmt* observeStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, observeSql, -1, &observeStmt, nullptr) != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
        for (const auto& rank : ranks) {
            if (!rank.mmr || *rank.mmr <= 0 || !TrackedAccounts::IsSupportedPlaylistKey(rank.playlistKey)) continue;
            const int64_t observedAt = rank.fetchedAtUnix > 0 ? rank.fetchedAtUnix : effectiveFetched;
            if (observedAt <= 0) continue;
            sqlite3_reset(observeStmt);
            sqlite3_clear_bindings(observeStmt);
            sqlite3_bind_text(observeStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(observeStmt, 2, rank.playlistKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(observeStmt, 3, *rank.mmr);
            if (rank.rankTier.empty())
                sqlite3_bind_null(observeStmt, 4);
            else
                sqlite3_bind_text(observeStmt, 4, rank.rankTier.c_str(), -1, SQLITE_TRANSIENT);
            if (rank.matchesPlayed)
                sqlite3_bind_int(observeStmt, 5, *rank.matchesPlayed);
            else
                sqlite3_bind_null(observeStmt, 5);
            if (rank.source.empty())
                sqlite3_bind_null(observeStmt, 6);
            else
                sqlite3_bind_text(observeStmt, 6, rank.source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(observeStmt, 7, observedAt);
            if (sqlite3_step(observeStmt) != SQLITE_DONE) {
                sqlite3_finalize(observeStmt);
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                return false;
            }
        }
        sqlite3_finalize(observeStmt);
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::map<std::string, TrackedAccounts::PlaylistRank> DatabaseManager::GetTrackedAccountRanks(const std::string& primaryId) {
    if (primaryId.empty()) {
        return {};
    }
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "TrackedAccountRanks")) {
        return {};
    }

    std::map<std::string, TrackedAccounts::PlaylistRank> ranks;
    const char* sql =
        "SELECT playlist_key, mmr, rank_tier, matches_played, source, fetched_at "
        "FROM TrackedAccountRanks WHERE primary_id = ?1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            TrackedAccounts::PlaylistRank rank;
            rank.playlistKey = SqlColumnText(stmt, 0);
            if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
                rank.mmr = sqlite3_column_int(stmt, 1);
            }
            if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) {
                rank.rankTier = SqlColumnText(stmt, 2);
            }
            if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
                rank.matchesPlayed = sqlite3_column_int(stmt, 3);
            }
            if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                rank.source = SqlColumnText(stmt, 4);
            }
            if (sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                rank.fetchedAtUnix = sqlite3_column_int64(stmt, 5);
            }
            ranks[rank.playlistKey] = std::move(rank);
        }
        sqlite3_finalize(stmt);
    }
    return ranks;
}

std::vector<TrackedAccounts::RankHistorySeries> DatabaseManager::GetTrackedAccountRankHistory(
    const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix) {
    std::vector<TrackedAccounts::RankHistorySeries> result;
    if (!TrackedAccounts::IsSupportedPlaylistKey(playlistKey)) return result;
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "TrackedAccountRankObservations")) return result;
    constexpr size_t kMaxAccounts = 5;
    const char* statsSql =
        "SELECT MAX(mmr), COUNT(*) FROM TrackedAccountRankObservations "
        "WHERE primary_id=?1 AND playlist_key=?2;";
    // SQLite evaluates the indexed series and returns at most 242 real sample
    // points per account, evenly spaced by ordinal, with actual timestamps.
    const char* samplesSql =
        "WITH ordered AS ("
        " SELECT mmr,rank_tier,matches_played,source,observed_at,id,"
        " ROW_NUMBER() OVER (ORDER BY observed_at,id) AS rn,"
        " COUNT(*) OVER () AS n"
        " FROM TrackedAccountRankObservations"
        " WHERE primary_id=?1 AND playlist_key=?2 AND observed_at>=?3"
        ") SELECT mmr,rank_tier,matches_played,source,observed_at"
        " FROM ordered WHERE rn=1 OR rn=n OR (rn-1) % MAX(1,(n+238)/239)=0"
        " ORDER BY observed_at,id LIMIT 242;";
    sqlite3_stmt* stats = nullptr;
    sqlite3_stmt* samples = nullptr;
    if (sqlite3_prepare_v2(db, statsSql, -1, &stats, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(db, samplesSql, -1, &samples, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stats);
        sqlite3_finalize(samples);
        return result;
    }
    std::set<std::string> unique;
    for (const auto& id : primaryIds) {
        if (result.size() >= kMaxAccounts) break;
        const auto canonical = TrackedAccounts::CanonicalPrimaryId(id);
        if (canonical.empty() || !unique.insert(canonical).second) continue;
        TrackedAccounts::RankHistorySeries series;
        series.primaryId = canonical;
        sqlite3_reset(stats);
        sqlite3_clear_bindings(stats);
        sqlite3_bind_text(stats, 1, canonical.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stats, 2, playlistKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stats) == SQLITE_ROW) {
            if (sqlite3_column_type(stats, 0) != SQLITE_NULL)
                series.highestObservedMmr = sqlite3_column_int(stats, 0);
            series.totalObservations = sqlite3_column_int64(stats, 1);
        }
        sqlite3_reset(samples);
        sqlite3_clear_bindings(samples);
        sqlite3_bind_text(samples, 1, canonical.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(samples, 2, playlistKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(samples, 3, (std::max)(int64_t{0}, sinceUnix));
        while (sqlite3_step(samples) == SQLITE_ROW) {
            TrackedAccounts::RankObservation item;
            item.primaryId = canonical;
            item.playlistKey = playlistKey;
            item.mmr = sqlite3_column_int(samples, 0);
            item.rankTier = SqlColumnText(samples, 1);
            if (sqlite3_column_type(samples, 2) != SQLITE_NULL) item.matchesPlayed = sqlite3_column_int(samples, 2);
            item.source = SqlColumnText(samples, 3);
            item.observedAtUnix = sqlite3_column_int64(samples, 4);
            series.observations.push_back(std::move(item));
        }
        result.push_back(std::move(series));
    }
    sqlite3_finalize(samples);
    sqlite3_finalize(stats);
    return result;
}

bool DatabaseManager::DeleteTrackedAccount(const std::string& primaryId) {
    if (primaryId.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return false;
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    if (SqliteTableExists(m_db, "TrackedAccountRanks")) {
        const char* delRanksSql = "DELETE FROM TrackedAccountRanks WHERE primary_id = ?1;";
        sqlite3_stmt* rStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, delRanksSql, -1, &rStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(rStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(rStmt);
            sqlite3_finalize(rStmt);
        } else {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return false;
        }
    }

    const char* delAccSql = "DELETE FROM TrackedAccounts WHERE primary_id = ?1;";
    sqlite3_stmt* accStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, delAccSql, -1, &accStmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_text(accStmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    bool stepOk = (sqlite3_step(accStmt) == SQLITE_DONE);
    int changes = sqlite3_changes(m_db);
    sqlite3_finalize(accStmt);

    if (!stepOk) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    return changes > 0;
}

int DatabaseManager::ImportTrackedAccounts(const std::vector<TrackedAccounts::AccountRecord>& records) {
    if (records.empty()) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "TrackedAccounts")) {
        return 0;
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::unordered_set<std::string> knownPrimaryIds;
    std::set<std::pair<std::string, std::string>> knownPlatformAccounts;
    const char* selExistingSql = "SELECT primary_id, platform, platform_account_id FROM TrackedAccounts;";
    sqlite3_stmt* existStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, selExistingSql, -1, &existStmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(existStmt) == SQLITE_ROW) {
            std::string pid = SqlColumnText(existStmt, 0);
            std::string plat = SqlColumnText(existStmt, 1);
            std::string paid = SqlColumnText(existStmt, 2);
            knownPrimaryIds.insert(std::move(pid));
            knownPlatformAccounts.insert({std::move(plat), std::move(paid)});
        }
        sqlite3_finalize(existStmt);
    } else {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    int64_t maxSortOrder = 0;
    const char* maxSql = "SELECT COALESCE(MAX(sort_order), 0) FROM TrackedAccounts;";
    sqlite3_stmt* maxStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, maxSql, -1, &maxStmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(maxStmt) == SQLITE_ROW) {
            maxSortOrder = sqlite3_column_int64(maxStmt, 0);
        }
        sqlite3_finalize(maxStmt);
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();

    const char* insAccSql =
        "INSERT INTO TrackedAccounts (primary_id, platform, platform_account_id, nickname, last_known_name, "
        "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);";
    sqlite3_stmt* insStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insAccSql, -1, &insStmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    const bool hasRanksTable = SqliteTableExists(m_db, "TrackedAccountRanks");
    sqlite3_stmt* rankStmt = nullptr;
    if (hasRanksTable) {
        const char* insRankSql =
            "INSERT INTO TrackedAccountRanks (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
            "ON CONFLICT(primary_id, playlist_key) DO UPDATE SET "
            "mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played, "
            "source = excluded.source, fetched_at = excluded.fetched_at;";
        if (sqlite3_prepare_v2(m_db, insRankSql, -1, &rankStmt, nullptr) != SQLITE_OK) {
            sqlite3_finalize(insStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return 0;
        }
    }

    int insertedCount = 0;
    for (const auto& rec : records) {
        if (rec.primaryId.empty()) {
            continue;
        }
        TrackedAccounts::Platform platform = rec.platform;
        std::string platformAccountId = rec.platformAccountId;
        if (platformAccountId.empty()) {
            if (auto id = TrackedAccounts::FromPrimaryId(rec.primaryId)) {
                platform = id->platform;
                platformAccountId = id->accountId;
            }
        }
        if (platformAccountId.empty()) {
            continue;
        }

        std::string platKey = TrackedAccounts::PlatformKey(platform);
        if (knownPrimaryIds.contains(rec.primaryId) || knownPlatformAccounts.contains({platKey, platformAccountId})) {
            continue;
        }

        int64_t sortOrder = rec.sortOrder > 0 ? rec.sortOrder : ++maxSortOrder;
        if (sortOrder > maxSortOrder) {
            maxSortOrder = sortOrder;
        }
        int64_t createdAt = rec.createdAtUnix > 0 ? rec.createdAtUnix : now;

        sqlite3_reset(insStmt);
        sqlite3_clear_bindings(insStmt);
        sqlite3_bind_text(insStmt, 1, rec.primaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insStmt, 2, platKey.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insStmt, 3, platformAccountId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(insStmt, 4, rec.nickname.c_str(), -1, SQLITE_TRANSIENT);
        if (rec.lastKnownName.empty()) {
            sqlite3_bind_null(insStmt, 5);
        } else {
            sqlite3_bind_text(insStmt, 5, rec.lastKnownName.c_str(), -1, SQLITE_TRANSIENT);
        }
        sqlite3_bind_int64(insStmt, 6, createdAt);
        if (rec.lastSeenAtUnix <= 0) {
            sqlite3_bind_null(insStmt, 7);
        } else {
            sqlite3_bind_int64(insStmt, 7, rec.lastSeenAtUnix);
        }
        if (rec.lastRefreshAttemptAtUnix <= 0) {
            sqlite3_bind_null(insStmt, 8);
        } else {
            sqlite3_bind_int64(insStmt, 8, rec.lastRefreshAttemptAtUnix);
        }
        if (rec.lastSuccessfulRefreshAtUnix <= 0) {
            sqlite3_bind_null(insStmt, 9);
        } else {
            sqlite3_bind_int64(insStmt, 9, rec.lastSuccessfulRefreshAtUnix);
        }
        sqlite3_bind_int64(insStmt, 10, sortOrder);
        sqlite3_bind_text(insStmt, 11, TrackedAccounts::DiscoverySourceKey(rec.discoverySource), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(insStmt) != SQLITE_DONE) {
            sqlite3_finalize(insStmt);
            if (rankStmt) sqlite3_finalize(rankStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            return 0;
        }

        if (rankStmt && !rec.ranks.empty()) {
            for (const auto& [key, rank] : rec.ranks) {
                sqlite3_reset(rankStmt);
                sqlite3_clear_bindings(rankStmt);
                sqlite3_bind_text(rankStmt, 1, rec.primaryId.c_str(), -1, SQLITE_TRANSIENT);
                const std::string& pKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                sqlite3_bind_text(rankStmt, 2, pKey.c_str(), -1, SQLITE_TRANSIENT);
                if (rank.mmr.has_value()) {
                    sqlite3_bind_int(rankStmt, 3, *rank.mmr);
                } else {
                    sqlite3_bind_null(rankStmt, 3);
                }
                if (rank.rankTier.empty()) {
                    sqlite3_bind_null(rankStmt, 4);
                } else {
                    sqlite3_bind_text(rankStmt, 4, rank.rankTier.c_str(), -1, SQLITE_TRANSIENT);
                }
                if (rank.matchesPlayed.has_value()) {
                    sqlite3_bind_int(rankStmt, 5, *rank.matchesPlayed);
                } else {
                    sqlite3_bind_null(rankStmt, 5);
                }
                if (rank.source.empty()) {
                    sqlite3_bind_null(rankStmt, 6);
                } else {
                    sqlite3_bind_text(rankStmt, 6, rank.source.c_str(), -1, SQLITE_TRANSIENT);
                }
                int64_t rFetched = rank.fetchedAtUnix <= 0 ? createdAt : rank.fetchedAtUnix;
                sqlite3_bind_int64(rankStmt, 7, rFetched);
                if (sqlite3_step(rankStmt) != SQLITE_DONE) {
                    sqlite3_finalize(insStmt);
                    sqlite3_finalize(rankStmt);
                    sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
                    return 0;
                }
            }
        }

        knownPrimaryIds.insert(rec.primaryId);
        knownPlatformAccounts.insert({platKey, platformAccountId});
        insertedCount++;
    }

    sqlite3_finalize(insStmt);
    if (rankStmt) sqlite3_finalize(rankStmt);

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    return insertedCount;
}
