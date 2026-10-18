#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include "core/game/ArenaNames.hpp"
#include <filesystem>
#include <iostream>
#include <utility>

namespace fs = std::filesystem;
using namespace DatabaseDetail;

static bool SqliteHasAnyUserTable(sqlite3* db) {
    if (!db) return false;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static void BackupDatabaseBeforeMigration(sqlite3* db, const std::string& dbPath, int oldVersion) {
    if (IsInMemoryDbPath(dbPath)) return;
    std::error_code ec;
    const fs::path srcPath(dbPath);
    if (!fs::exists(srcPath, ec) || !fs::is_regular_file(srcPath, ec) || fs::file_size(srcPath, ec) == 0) {
        return;
    }

    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);

    const fs::path parentDir = srcPath.parent_path().empty() ? fs::path(".") : srcPath.parent_path();
    const std::string prefix = srcPath.filename().string() + ".bak-v";
    for (const auto& entry : fs::directory_iterator(parentDir, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0) {
            std::error_code rmEc;
            fs::remove(entry.path(), rmEc);
        }
    }

    const fs::path backupPath(dbPath + ".bak-v" + std::to_string(oldVersion));
    fs::copy_file(srcPath, backupPath, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[Database] Warning: Failed to create pre-migration backup " << backupPath.string()
                  << ": " << ec.message() << "\n";
    }
}

struct Migration {
    int version = 0;
    const char* sql = nullptr;
    bool (*apply)(sqlite3* db, std::string& error) = nullptr;
};

static constexpr const char* kMigrationV1Sql = R"(
    CREATE TABLE IF NOT EXISTS Matches (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
        arena TEXT,
        our_score INTEGER,
        their_score INTEGER,
        win BOOLEAN,
        match_guid TEXT,
        playlist_id INTEGER,
        gamemode TEXT,
        player_count INTEGER,
        result_pending BOOLEAN DEFAULT 0
    );
    CREATE TABLE IF NOT EXISTS MatchPlayers (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        match_id INTEGER,
        primary_id TEXT,
        name TEXT,
        team INTEGER,
        mmr INTEGER,
        mmr_estimated BOOLEAN DEFAULT 0,
        is_opponent BOOLEAN DEFAULT 0,
        FOREIGN KEY(match_id) REFERENCES Matches(id)
    );
    CREATE TABLE IF NOT EXISTS Settings (
        key TEXT PRIMARY KEY,
        value TEXT
    );
)";

static bool ApplyMigrationV1(sqlite3* db, std::string& error) {
    auto execSql = [&](const char* sql, const char* context) {
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            error = std::string(context) + ": " + (errMsg ? errMsg : "unknown");
            if (errMsg) sqlite3_free(errMsg);
            return false;
        }
        return true;
    };

    if (!SqliteTableHasColumn(db, "Matches", "playlist_id")) {
        if (!execSql("ALTER TABLE Matches ADD COLUMN playlist_id INTEGER;", "Matches.playlist_id")) return false;
    }
    if (!SqliteTableHasColumn(db, "MatchPlayers", "mmr_estimated")) {
        if (!execSql("ALTER TABLE MatchPlayers ADD COLUMN mmr_estimated BOOLEAN DEFAULT 0;", "MatchPlayers.mmr_estimated")) return false;
    }
    if (!SqliteTableHasColumn(db, "Matches", "result_pending")) {
        if (!execSql("ALTER TABLE Matches ADD COLUMN result_pending BOOLEAN DEFAULT 0;", "Matches.result_pending")) return false;
    }

    const char* createIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_matchplayers_primary_id ON MatchPlayers(primary_id);
        CREATE INDEX IF NOT EXISTS idx_matchplayers_match_id ON MatchPlayers(match_id);
        CREATE INDEX IF NOT EXISTS idx_mp_primary_match ON MatchPlayers(primary_id, match_id);
        CREATE INDEX IF NOT EXISTS idx_matches_timestamp ON Matches(timestamp);
        CREATE INDEX IF NOT EXISTS idx_matches_gamemode ON Matches(gamemode);
        CREATE INDEX IF NOT EXISTS idx_matches_playlist_id ON Matches(playlist_id);
    )";
    return execSql(createIndexes, "v1 indexes");
}

static constexpr const char* kMigrationV2Sql = R"(
    CREATE INDEX IF NOT EXISTS idx_mp_primary_match ON MatchPlayers(primary_id, match_id);
    CREATE TABLE MatchPlayerStats (
        match_id INTEGER NOT NULL REFERENCES Matches(id) ON DELETE CASCADE,
        primary_id TEXT NOT NULL,
        score INTEGER, goals INTEGER, assists INTEGER, saves INTEGER, shots INTEGER, demos INTEGER,
        touches INTEGER, car_touches INTEGER,
        max_goal_speed REAL, fastest_goal_time REAL,
        PRIMARY KEY (match_id, primary_id)
    );
    CREATE TABLE MatchLocalStats (
        match_id INTEGER PRIMARY KEY REFERENCES Matches(id) ON DELETE CASCADE,
        boost_collected INTEGER, demoed INTEGER, crossbars INTEGER, hardest_crossbar REAL,
        max_ball_speed REAL, own_goals INTEGER, duration_seconds REAL, overtime_seconds REAL,
        stats_version INTEGER NOT NULL DEFAULT 1
    );
    CREATE INDEX IF NOT EXISTS idx_mps_primary_match ON MatchPlayerStats(primary_id, match_id);
)";

static constexpr const char* kMigrationV3Sql = R"(
    CREATE TABLE Sessions (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        account TEXT NOT NULL,
        started_at INTEGER,
        ended_at INTEGER,
        wins INTEGER,
        losses INTEGER,
        mmr_change_json TEXT,
        totals_json TEXT,
        source TEXT NOT NULL DEFAULT 'live'
    );
)";

static bool ApplyMigrationV3(sqlite3* db, std::string& error) {
    auto execSql = [&](const char* sql, const char* context) {
        char* errMsg = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
            error = std::string(context) + ": " + (errMsg ? errMsg : "unknown");
            if (errMsg) sqlite3_free(errMsg);
            return false;
        }
        return true;
    };

    if (!SqliteTableHasColumn(db, "Matches", "session_id")) {
        if (!execSql("ALTER TABLE Matches ADD COLUMN session_id INTEGER REFERENCES Sessions(id);", "Matches.session_id")) {
            return false;
        }
    }
    if (!execSql("CREATE INDEX IF NOT EXISTS idx_matches_session ON Matches(session_id);", "idx_matches_session")) {
        return false;
    }
    if (!execSql("CREATE INDEX IF NOT EXISTS idx_sessions_account_ended ON Sessions(account, ended_at DESC, id DESC);",
                 "idx_sessions_account_ended")) {
        return false;
    }
    return true;
}

static constexpr const char* kMigrationV4Sql = R"(
    CREATE TABLE IF NOT EXISTS ReplayUploads (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        file_path TEXT NOT NULL UNIQUE,
        file_name TEXT NOT NULL,
        match_guid TEXT,
        state TEXT NOT NULL DEFAULT 'pending',
        attempts INTEGER NOT NULL DEFAULT 0,
        next_attempt_at INTEGER NOT NULL DEFAULT 0,
        last_http_status INTEGER NOT NULL DEFAULT 0,
        last_error TEXT,
        ballchasing_id TEXT,
        ballchasing_url TEXT,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS idx_replay_uploads_match_guid ON ReplayUploads(match_guid);
    CREATE INDEX IF NOT EXISTS idx_replay_uploads_state ON ReplayUploads(state);
)";
static constexpr const char* kMigrationV5Sql = R"(
    CREATE TABLE IF NOT EXISTS TrackedAccounts (
        primary_id TEXT PRIMARY KEY NOT NULL,
        platform TEXT NOT NULL,
        platform_account_id TEXT NOT NULL,
        nickname TEXT NOT NULL DEFAULT '',
        last_known_name TEXT,
        created_at INTEGER NOT NULL,
        last_seen_at INTEGER,
        last_refresh_attempt_at INTEGER,
        last_successful_refresh_at INTEGER,
        sort_order INTEGER NOT NULL DEFAULT 0,
        discovery_source TEXT NOT NULL DEFAULT 'manual',
        UNIQUE(platform, platform_account_id)
    );
    CREATE TABLE IF NOT EXISTS TrackedAccountRanks (
        primary_id TEXT NOT NULL REFERENCES TrackedAccounts(primary_id) ON DELETE CASCADE,
        playlist_key TEXT NOT NULL,
        mmr INTEGER,
        rank_tier TEXT,
        matches_played INTEGER,
        source TEXT,
        fetched_at INTEGER NOT NULL,
        PRIMARY KEY(primary_id, playlist_key)
    );
    CREATE INDEX IF NOT EXISTS idx_tracked_accounts_sort_order_created_at ON TrackedAccounts(sort_order, created_at);
)";
static constexpr const char* kMigrationV6Sql = R"(
    CREATE TABLE IF NOT EXISTS PlayerNotes (
        platform TEXT NOT NULL,
        account_id TEXT NOT NULL,
        splitscreen INTEGER NOT NULL DEFAULT 0,
        note TEXT NOT NULL,
        last_known_name TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (platform, account_id, splitscreen)
    );
)";
// Non-unique: existing databases may already hold duplicate GUIDs, and the app-level duplicate check stays authoritative.
static constexpr const char* kMigrationV7Sql = R"(
    CREATE INDEX IF NOT EXISTS idx_matches_match_guid ON Matches(match_guid);
)";

static constexpr const char* kMigrationV8Sql = R"(
    CREATE TABLE IF NOT EXISTS MatchEvents (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        match_id INTEGER NOT NULL REFERENCES Matches(id) ON DELETE CASCADE,
        match_guid TEXT NOT NULL DEFAULT '',
        event_type TEXT NOT NULL,
        observed_at_ms INTEGER NOT NULL,
        match_clock_seconds REAL,
        actor_id TEXT,
        actor_name TEXT,
        secondary_id TEXT,
        secondary_name TEXT,
        team INTEGER,
        value REAL,
        ball_speed REAL,
        source TEXT NOT NULL,
        event_key TEXT,
        format_version INTEGER NOT NULL DEFAULT 1,
        UNIQUE(match_id, event_key)
    );
    CREATE INDEX IF NOT EXISTS idx_matchevents_match_time ON MatchEvents(match_id, observed_at_ms, id);
)";

// Existing valid rank snapshots become the first *actual* observations on upgrade.
// No interpolated or backfilled synthetic MMR points are created.
static constexpr const char* kMigrationV9Sql = R"(
    CREATE TABLE IF NOT EXISTS TrackedAccountRankObservations (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        primary_id TEXT NOT NULL REFERENCES TrackedAccounts(primary_id) ON DELETE CASCADE,
        playlist_key TEXT NOT NULL,
        mmr INTEGER NOT NULL CHECK(mmr > 0),
        rank_tier TEXT,
        matches_played INTEGER,
        source TEXT,
        observed_at INTEGER NOT NULL CHECK(observed_at > 0),
        UNIQUE(primary_id, playlist_key, observed_at, mmr)
    );
    CREATE INDEX IF NOT EXISTS idx_account_rank_obs_series
        ON TrackedAccountRankObservations(primary_id, playlist_key, observed_at, id);
    INSERT OR IGNORE INTO TrackedAccountRankObservations
        (primary_id, playlist_key, mmr, rank_tier, matches_played, source, observed_at)
    SELECT primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at
    FROM TrackedAccountRanks WHERE mmr > 0 AND fetched_at > 0;
)";

// Private tags share PlayerNotes identity, but do not require a note row.
static constexpr const char* kMigrationV10Sql = R"(
    CREATE TABLE IF NOT EXISTS PlayerTags (
        platform TEXT NOT NULL,
        account_id TEXT NOT NULL,
        splitscreen INTEGER NOT NULL DEFAULT 0,
        tag TEXT NOT NULL COLLATE NOCASE,
        created_at INTEGER NOT NULL,
        PRIMARY KEY(platform, account_id, splitscreen, tag)
    );
    CREATE INDEX IF NOT EXISTS idx_player_tags_identity ON PlayerTags(platform, account_id, splitscreen);
)";

static constexpr const char* kMigrationV11Sql = R"(
    CREATE TABLE IF NOT EXISTS MatchAnalytics (
        match_id INTEGER PRIMARY KEY REFERENCES Matches(id) ON DELETE CASCADE,
        format_version INTEGER NOT NULL,
        data TEXT NOT NULL
    );
)";

// Runs once per database, tracked in Settings rather than user_version: other local builds already use
// schema versions past 10. Rows are rewritten by id so chained legacy renames cannot cascade.
static void RepairArenaNamesOnce(sqlite3* db) {
    static constexpr const char* kFlag = "arena_names_repaired_v1";
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) return;
    bool ok = true;
    bool done = false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM Settings WHERE key = ?1;", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, kFlag, -1, SQLITE_STATIC);
        done = sqlite3_step(stmt) == SQLITE_ROW;
    } else {
        ok = false;
    }
    sqlite3_finalize(stmt);

    std::vector<std::pair<sqlite3_int64, std::string>> updates;
    if (ok && !done) {
        if (sqlite3_prepare_v2(db, "SELECT id, arena FROM Matches WHERE arena IS NOT NULL;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const std::string stored = SqlColumnText(stmt, 1);
                std::string repaired = ArenaNames::RepairStoredName(stored);
                if (repaired != stored) updates.emplace_back(sqlite3_column_int64(stmt, 0), std::move(repaired));
            }
        } else {
            ok = false;
        }
        sqlite3_finalize(stmt);
    }
    if (ok && !done && sqlite3_prepare_v2(db, "UPDATE Matches SET arena = ?1 WHERE id = ?2;", -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto& [id, arena] : updates) {
            sqlite3_bind_text(stmt, 1, arena.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, id);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                ok = false;
                break;
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    } else if (!done) {
        ok = false;
    }
    if (ok && !done && sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO Settings (key, value) VALUES (?1, '1');", -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, kFlag, -1, SQLITE_STATIC);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    if (ok) {
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
        if (!updates.empty()) std::cout << "[Database] Corrected map names on " << updates.size() << " saved matches.\n";
    } else {
        std::cerr << "[Database] Map name repair failed: " << sqlite3_errmsg(db) << "\n";
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
}

// Append new migrations in ascending version order; each runs in its own BEGIN IMMEDIATE transaction.
static const std::vector<Migration> kMigrations = {
    {1, kMigrationV1Sql, ApplyMigrationV1},
    {2, kMigrationV2Sql},
    {3, kMigrationV3Sql, ApplyMigrationV3},
    {4, kMigrationV4Sql},
    {5, kMigrationV5Sql},
    {6, kMigrationV6Sql},
    {7, kMigrationV7Sql},
    {8, kMigrationV8Sql},
    {9, kMigrationV9Sql},
    {10, kMigrationV10Sql},
    {11, kMigrationV11Sql},
};

bool DatabaseManager::CreateTables() {
    const bool hadExistingTables = SqliteHasAnyUserTable(m_db);
    int currentVersion = ReadUserVersion(m_db);
    const int latestVersion = kMigrations.empty() ? 0 : kMigrations.back().version;

    if (hadExistingTables && currentVersion < latestVersion) {
        BackupDatabaseBeforeMigration(m_db, m_dbPath, currentVersion);
    }

    for (const auto& migration : kMigrations) {
        if (migration.version <= currentVersion) continue;

        char* errMsg = nullptr;
        if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
            std::cerr << "[Database] Failed to begin migration v" << migration.version << ": "
                      << (errMsg ? errMsg : "unknown") << "\n";
            if (errMsg) sqlite3_free(errMsg);
            break;
        }

        bool stepOk = true;
        std::string stepError;
        if (migration.sql && migration.sql[0] != '\0') {
            if (sqlite3_exec(m_db, migration.sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
                stepError = errMsg ? errMsg : "SQL execution failed";
                if (errMsg) sqlite3_free(errMsg);
                errMsg = nullptr;
                stepOk = false;
            }
        }

        if (stepOk && migration.apply) {
            stepOk = migration.apply(m_db, stepError);
        }

        if (stepOk) {
            const std::string setVersionSql =
                "PRAGMA user_version = " + std::to_string(migration.version) + ";";
            if (sqlite3_exec(m_db, setVersionSql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
                stepError = errMsg ? errMsg : "Failed to set user_version";
                if (errMsg) sqlite3_free(errMsg);
                errMsg = nullptr;
                stepOk = false;
            }
        }

        if (!stepOk || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
            if (errMsg) {
                if (stepError.empty()) stepError = errMsg;
                sqlite3_free(errMsg);
            }
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            std::cerr << "[Database] Migration to v" << migration.version << " failed (rolling back): "
                      << stepError << "\n";
            break;
        }

        currentVersion = migration.version;
    }

    // Ensure MatchAnalytics exists even if user_version is already >= 11 from another build.
    char* ensureErrMsg = nullptr;
    if (sqlite3_exec(m_db, kMigrationV11Sql, nullptr, nullptr, &ensureErrMsg) != SQLITE_OK) {
        std::cerr << "[Database] Failed to ensure MatchAnalytics table: "
                  << (ensureErrMsg ? ensureErrMsg : "unknown") << "\n";
        if (ensureErrMsg) sqlite3_free(ensureErrMsg);
    }

    const bool hasV1Tables = SqliteTableExists(m_db, "Matches") &&
                             SqliteTableExists(m_db, "MatchPlayers") &&
                             SqliteTableExists(m_db, "Settings");
    if (hasV1Tables) RepairArenaNamesOnce(m_db);
    m_hasStatsTables = SqliteTableExists(m_db, "MatchPlayerStats") &&
                       SqliteTableExists(m_db, "MatchLocalStats");
    m_hasSessionsTable = SqliteTableExists(m_db, "Sessions") &&
                         SqliteTableHasColumn(m_db, "Matches", "session_id");
    if (m_hasSessionsTable) {
        sqlite3_stmt* stmt = nullptr;
        bool hasUnlinked = false;
        if (sqlite3_prepare_v2(m_db,
                               "SELECT 1 FROM Matches WHERE session_id IS NULL AND COALESCE(result_pending, 0) = 0 LIMIT 1;",
                               -1, &stmt, nullptr) == SQLITE_OK) {
            hasUnlinked = (sqlite3_step(stmt) == SQLITE_ROW);
            sqlite3_finalize(stmt);
        }
        if (hasUnlinked) {
            m_backfillPending.store(true, std::memory_order_release);
            (void)EnqueueDbJob([this]() {
                BackfillSessions(500);
            },
                               DbJobPriority::Coalescable, "sessions_backfill");
        }
    }
    return hasV1Tables;
}
