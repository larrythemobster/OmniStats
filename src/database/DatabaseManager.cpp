#include "DatabaseManager.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/Storage.hpp"
#include "network/MMRFetcher.hpp"
#include <cctype>
#include <iostream>
#include <chrono>
#include <ctime>
#include <thread>
#include <utility>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

static std::string SqlColumnText(sqlite3_stmt* stmt, int column) {
    const unsigned char* value = sqlite3_column_text(stmt, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}

static bool SqliteTableHasColumn(sqlite3* db,
                                 const char* tableName,
                                 const char* columnName) {
    if (!db || !tableName || !columnName) return false;
    const std::string sql =
        "PRAGMA table_info(" + std::string(tableName) + ");";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return false;
    }

    bool found = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string name = SqlColumnText(stmt, 1);
        if (name == columnName) {
            found = true;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static bool SqliteTableExists(sqlite3* db, const char* tableName) {
    if (!db || !tableName) return false;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_STATIC);
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static bool SqliteHasAnyUserTable(sqlite3* db) {
    if (!db) return false;
    const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static int ReadUserVersion(sqlite3* db) {
    if (!db) return 0;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) return 0;
    int version = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

static bool IsInMemoryDbPath(const std::string& dbPath) {
    if (dbPath.empty() || dbPath == ":memory:") return true;
    return dbPath.rfind("file:", 0) == 0 && dbPath.find("mode=memory") != std::string::npos;
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

// Append new migrations in ascending version order; each runs in its own BEGIN IMMEDIATE transaction.
static const std::vector<Migration> kMigrations = {
    {1, kMigrationV1Sql, ApplyMigrationV1},
    {2, kMigrationV2Sql},
    {3, kMigrationV3Sql, ApplyMigrationV3},
};

static std::string CsvEscape(const std::string& value) {
    bool needsQuotes = value.find_first_of(",\"\r\n") != std::string::npos;
    if (!needsQuotes) return value;

    std::string escaped = "\"";
    for (char c : value) {
        if (c == '\"')
            escaped += "\"\"";
        else
            escaped += c;
    }
    escaped += "\"";
    return escaped;
}

static std::string TimestampForFilename() {
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);

    std::ostringstream ss;
    ss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return ss.str();
}

static std::string FormatPlaylistName(int playerCount) {
    if (playerCount <= 0) return "Unknown";
    if (playerCount <= 2) return "Duel";
    if (playerCount <= 4) return "Doubles";
    if (playerCount <= 6) return "Standard";
    if (playerCount <= 8) return "Chaos";
    return "Unknown";
}

static std::string FormatMatchHistoryMode(const std::string& gamemode, int playerCount) {
    if (gamemode == "1v1") return "Duel";
    if (gamemode == "2v2") return "Doubles";
    if (gamemode == "3v3") return "Standard";
    if (gamemode == "hoops") return "Hoops";
    if (gamemode == "rumble") return "Rumble";
    if (gamemode == "dropshot") return "Dropshot";
    if (gamemode == "snowday") return "Snow Day";
    if (gamemode == "heatseeker") return "Heatseeker";
    if (gamemode == "casual") return FormatPlaylistName(playerCount);
    if (gamemode == "t") return "Tournament Match";
    return FormatPlaylistName(playerCount);
}

// Human-readable playlist for a Matches row. Rows with Game.PlaylistId use the
// authoritative metadata; legacy rows fall back to gamemode/player count.
static std::string DescribeMatchPlaylist(sqlite3_stmt* stmt, int gamemodeColumn, int playerCountColumn, int playlistIdColumn, bool& ranked) {
    if (sqlite3_column_type(stmt, playlistIdColumn) != SQLITE_NULL) {
        const int playlistId = sqlite3_column_int(stmt, playlistIdColumn);
        if (const auto* info = PlaylistMetadata::Find(playlistId)) {
            ranked = info->playlistClass == PlaylistMetadata::PlaylistClass::Ranked ||
                     info->playlistClass == PlaylistMetadata::PlaylistClass::Tournament;
            return info->playlistClass == PlaylistMetadata::PlaylistClass::Casual ? "Casual" : info->displayName;
        }
        ranked = false;
        return "Unknown Playlist (" + std::to_string(playlistId) + ")";
    }
    const std::string gamemode = SqlColumnText(stmt, gamemodeColumn);
    ranked = gamemode != "casual";
    return FormatMatchHistoryMode(gamemode, sqlite3_column_int(stmt, playerCountColumn));
}

DatabaseManager::DatabaseManager(std::shared_ptr<SessionState> state)
    : m_state(state) {
    s_test_async_get_lifetime_calls.store(0);
    s_test_async_refresh_calls.store(0);
    m_worker = std::jthread([this]() {
        while (true) {
            DbJob job;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait(lock, [this] {
                    return m_stopWorker || !m_jobs.empty();
                });

                if (m_stopWorker && m_jobs.empty()) {
                    break;
                }

                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }

            try {
                job.run();
            } catch (const std::exception& e) {
                std::cerr << "[DatabaseManager] Job Exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[DatabaseManager] Job Exception: Unknown\n";
            }

            if (job.priority == DbJobPriority::Coalescable && !job.coalesceKey.empty()) {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_pendingCoalescedJobs.erase(job.coalesceKey);
            }
        }
    });
}

DatabaseManager::~DatabaseManager() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopWorker = true;
    }

    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool DatabaseManager::Initialize(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    m_dbPath = dbPath;
    int rc = sqlite3_open(dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open SQLite database: " << sqlite3_errmsg(m_db) << "\n";
        return false;
    }

    sqlite3_busy_timeout(m_db, 3000);

    // Enable WAL (Write-Ahead Logging) and synchronous NORMAL mode
    char* errMsg = nullptr;
    rc = sqlite3_exec(m_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Warning: Failed to enable WAL mode: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
    }

    return CreateTables();
}

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

    const bool hasV1Tables = SqliteTableExists(m_db, "Matches") &&
                             SqliteTableExists(m_db, "MatchPlayers") &&
                             SqliteTableExists(m_db, "Settings");
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

bool DatabaseManager::HasStatsTablesLocked() const {
    return m_hasStatsTables &&
           SqliteTableExists(m_db, "MatchPlayerStats") &&
           SqliteTableExists(m_db, "MatchLocalStats");
}

bool DatabaseManager::HasSessionsTableLocked() const {
    return m_hasSessionsTable &&
           SqliteTableExists(m_db, "Sessions") &&
           SqliteTableHasColumn(m_db, "Matches", "session_id");
}

int DatabaseManager::GetSchemaVersion() {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return ReadUserVersion(m_db);
}

bool DatabaseManager::UpsertMatchStatsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    if (!HasStatsTablesLocked()) {
        return true;
    }

    const char* sqlPlayerStats = R"(
        INSERT INTO MatchPlayerStats (
            match_id, primary_id, score, goals, assists, saves, shots, demos,
            touches, car_touches, max_goal_speed, fastest_goal_time
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(match_id, primary_id) DO UPDATE SET
            score = excluded.score,
            goals = excluded.goals,
            assists = excluded.assists,
            saves = excluded.saves,
            shots = excluded.shots,
            demos = excluded.demos,
            touches = excluded.touches,
            car_touches = excluded.car_touches,
            max_goal_speed = excluded.max_goal_speed,
            fastest_goal_time = excluded.fastest_goal_time;
    )";

    sqlite3_stmt* stmtPlayerStats = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlPlayerStats, -1, &stmtPlayerStats, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare MatchPlayerStats upsert.\n";
        return false;
    }

    bool ok = true;
    for (const auto& [id, p] : snapshot.roster) {
        const std::string& primaryId = p.primaryId.empty() ? id : p.primaryId;
        if (primaryId.empty()) continue;

        sqlite3_bind_int64(stmtPlayerStats, 1, matchId);
        sqlite3_bind_text(stmtPlayerStats, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
        if (p.score >= 0) {
            sqlite3_bind_int(stmtPlayerStats, 3, p.score);
        } else {
            sqlite3_bind_null(stmtPlayerStats, 3);
        }
        sqlite3_bind_int(stmtPlayerStats, 4, p.goals);
        sqlite3_bind_int(stmtPlayerStats, 5, p.assists);
        sqlite3_bind_int(stmtPlayerStats, 6, p.saves);
        sqlite3_bind_int(stmtPlayerStats, 7, p.shots);
        sqlite3_bind_int(stmtPlayerStats, 8, p.demos);
        sqlite3_bind_null(stmtPlayerStats, 9);
        sqlite3_bind_null(stmtPlayerStats, 10);
        if (p.maxGoalSpeed > 0.0f) {
            sqlite3_bind_double(stmtPlayerStats, 11, static_cast<double>(p.maxGoalSpeed));
        } else {
            sqlite3_bind_null(stmtPlayerStats, 11);
        }
        if (p.fastestGoalTime > 0.0f) {
            sqlite3_bind_double(stmtPlayerStats, 12, static_cast<double>(p.fastestGoalTime));
        } else {
            sqlite3_bind_null(stmtPlayerStats, 12);
        }

        if (sqlite3_step(stmtPlayerStats) != SQLITE_DONE) {
            std::cerr << "[Database] Failed to upsert MatchPlayerStats row.\n";
            ok = false;
            break;
        }
        sqlite3_reset(stmtPlayerStats);
        sqlite3_clear_bindings(stmtPlayerStats);
    }
    sqlite3_finalize(stmtPlayerStats);
    if (!ok) return false;

    const char* sqlLocalStats = R"(
        INSERT INTO MatchLocalStats (
            match_id, boost_collected, demoed, crossbars, hardest_crossbar,
            max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
        ON CONFLICT(match_id) DO UPDATE SET
            boost_collected = excluded.boost_collected,
            demoed = excluded.demoed,
            crossbars = excluded.crossbars,
            hardest_crossbar = excluded.hardest_crossbar,
            max_ball_speed = excluded.max_ball_speed,
            own_goals = excluded.own_goals,
            duration_seconds = excluded.duration_seconds,
            overtime_seconds = excluded.overtime_seconds,
            stats_version = excluded.stats_version;
    )";

    sqlite3_stmt* stmtLocalStats = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlLocalStats, -1, &stmtLocalStats, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare MatchLocalStats upsert.\n";
        return false;
    }

    const auto& local = snapshot.localStats;
    sqlite3_bind_int64(stmtLocalStats, 1, matchId);
    sqlite3_bind_int(stmtLocalStats, 2, local.boostPickedUpSelf);
    sqlite3_bind_int(stmtLocalStats, 3, local.demoedSelf);
    sqlite3_bind_int(stmtLocalStats, 4, local.crossbarsSelf);
    if (local.maxImpactForceSelf > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 5, static_cast<double>(local.maxImpactForceSelf));
    } else {
        sqlite3_bind_null(stmtLocalStats, 5);
    }
    if (local.maxBallSpeedSelf > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 6, static_cast<double>(local.maxBallSpeedSelf));
    } else {
        sqlite3_bind_null(stmtLocalStats, 6);
    }
    sqlite3_bind_int(stmtLocalStats, 7, local.ownGoalsSelf);
    if (snapshot.durationSeconds > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 8, static_cast<double>(snapshot.durationSeconds));
    } else {
        sqlite3_bind_null(stmtLocalStats, 8);
    }
    if (snapshot.overtimeSeconds > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 9, static_cast<double>(snapshot.overtimeSeconds));
    } else {
        sqlite3_bind_null(stmtLocalStats, 9);
    }

    ok = (sqlite3_step(stmtLocalStats) == SQLITE_DONE);
    sqlite3_finalize(stmtLocalStats);
    if (!ok) {
        std::cerr << "[Database] Failed to upsert MatchLocalStats row.\n";
    }
    return ok;
}

void DatabaseManager::SaveMatch(const MatchSaveSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    if (snapshot.arenaName.empty()) return;

    sqlite3_int64 pendingMatchId = 0;
    if (!snapshot.matchGuid.empty()) {
        sqlite3_stmt* duplicateCheck = nullptr;
        const char* duplicateSql =
            "SELECT id, COALESCE(result_pending, 0) FROM Matches "
            "WHERE match_guid = ? ORDER BY id DESC LIMIT 1;";
        bool alreadySaved = false;
        if (sqlite3_prepare_v2(
                m_db,
                duplicateSql,
                -1,
                &duplicateCheck,
                nullptr) == SQLITE_OK) {
            sqlite3_bind_text(
                duplicateCheck,
                1,
                snapshot.matchGuid.c_str(),
                -1,
                SQLITE_TRANSIENT);
            if (sqlite3_step(duplicateCheck) == SQLITE_ROW) {
                alreadySaved = true;
                if (sqlite3_column_int(duplicateCheck, 1) != 0) {
                    pendingMatchId = sqlite3_column_int64(duplicateCheck, 0);
                }
            }
            sqlite3_finalize(duplicateCheck);
        } else if (duplicateCheck) {
            sqlite3_finalize(duplicateCheck);
        }
        if (alreadySaved &&
            (pendingMatchId == 0 || snapshot.resultPending)) {
            std::cout
                << "[Database] Skipping duplicate match GUID.\n";
            return;
        }
    }

    if (pendingMatchId != 0) {
        ResolvePendingMatchLocked(pendingMatchId, snapshot);
        return;
    }

    if (!snapshot.validResult) {
        std::cout << "[Database] Skipping void match"
                  << (snapshot.voidReason.empty() ? "" : ": " + snapshot.voidReason)
                  << "\n";
        return;
    }

    if (snapshot.myTeam != 0 && snapshot.myTeam != 1) return;
    if (snapshot.winnerTeam != 0 && snapshot.winnerTeam != 1) return;

    bool win = (snapshot.winnerTeam == snapshot.myTeam);

    int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
    int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

    const bool hasPlaylistId =
        PlaylistMetadata::HasAuthoritativeId(snapshot.playlistId);
    const int playerCount = hasPlaylistId
                                ? 0
                                : (snapshot.legacyPlayerCount > 0
                                       ? snapshot.legacyPlayerCount
                                       : static_cast<int>(snapshot.roster.size()));
    const std::string arenaKey = !snapshot.arenaAsset.empty()
                                     ? snapshot.arenaAsset
                                     : snapshot.arenaName;

    std::string gamemode;
    std::string mmrKey;
    if (hasPlaylistId) {
        if (PlaylistMetadata::IsKnown(snapshot.playlistId)) {
            gamemode = PlaylistMetadata::StorageMode(snapshot.playlistId);
            mmrKey = PlaylistMetadata::MmrKey(snapshot.playlistId);
        } else {
            // Preserve the fact that the ID was unknown rather than inventing
            // a ranked mode from UI state, arena, or roster size.
            gamemode = "unknown";
        }
    } else {
        gamemode = snapshot.gamemode;
        if (gamemode.empty()) {
            gamemode = GamemodeUtils::InferFromSnapshot(
                playerCount,
                static_cast<int>(snapshot.roster.size()),
                snapshot.rosterMmrCategory,
                arenaKey);
        }
        mmrKey = gamemode;
    }

    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        return;
    }

    std::string sqlMatches =
        "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, timestamp, result_pending) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, COALESCE(strftime('%Y-%m-%d %H:%M:%f', NULLIF(?, 0) / 1000.0, 'unixepoch'), CURRENT_TIMESTAMP), ?);";
    sqlite3_stmt* stmtMatches;

    if (sqlite3_prepare_v2(m_db, sqlMatches.c_str(), -1, &stmtMatches, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare insert statement\n";
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    sqlite3_bind_text(stmtMatches, 1, snapshot.arenaName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMatches, 2, ourScore);
    sqlite3_bind_int(stmtMatches, 3, theirScore);
    sqlite3_bind_int(stmtMatches, 4, win ? 1 : 0);
    sqlite3_bind_text(stmtMatches, 5, snapshot.matchGuid.c_str(), -1, SQLITE_TRANSIENT);
    if (hasPlaylistId) {
        sqlite3_bind_int(stmtMatches, 6, snapshot.playlistId);
    } else {
        sqlite3_bind_null(stmtMatches, 6);
    }
    sqlite3_bind_text(stmtMatches, 7, gamemode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMatches, 8, playerCount);
    sqlite3_bind_int64(
        stmtMatches, 9, snapshot.endedAtUnixMs);
    sqlite3_bind_int(stmtMatches, 10, snapshot.resultPending ? 1 : 0);

    bool ok = true;
    if (sqlite3_step(stmtMatches) != SQLITE_DONE) {
        std::cerr << "Failed to insert match record\n";
        ok = false;
    }
    sqlite3_finalize(stmtMatches);

    if (!ok) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    // Get the ID of the match we just inserted
    sqlite3_int64 matchId = sqlite3_last_insert_rowid(m_db);

    // Save players
    std::string sqlPlayers = "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmtPlayers;

    if (sqlite3_prepare_v2(m_db, sqlPlayers.c_str(), -1, &stmtPlayers, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare player insert statement\n";
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    auto insertPlayerRow = [&](const PlayerData& p) -> bool {
        const bool isOpponent = (p.team != snapshot.myTeam);
        sqlite3_bind_int64(stmtPlayers, 1, matchId);
        sqlite3_bind_text(stmtPlayers, 2, p.primaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmtPlayers, 3, p.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmtPlayers, 4, p.team);
        int mmrVal = p.mmr;
        if (!mmrKey.empty() && p.playlists.count(mmrKey)) {
            mmrVal = p.playlists.at(mmrKey);
        }
        sqlite3_bind_int(stmtPlayers, 5, mmrVal);
        const bool mmrEstimated =
            snapshot.localMmrNeedsReconciliation &&
            p.primaryId == snapshot.myPrimaryId;
        sqlite3_bind_int(stmtPlayers, 6, mmrEstimated ? 1 : 0);
        sqlite3_bind_int(stmtPlayers, 7, isOpponent ? 1 : 0);

        if (sqlite3_step(stmtPlayers) != SQLITE_DONE) {
            std::cerr << "Failed to insert player record\n";
            return false;
        }
        sqlite3_reset(stmtPlayers);
        sqlite3_clear_bindings(stmtPlayers);
        return true;
    };

    if (!snapshot.myPrimaryId.empty()) {
        const auto selfIt = snapshot.roster.find(snapshot.myPrimaryId);
        if (selfIt != snapshot.roster.end()) {
            ok = insertPlayerRow(selfIt->second);
        }
    }
    if (ok) {
        for (const auto& [id, p] : snapshot.roster) {
            if (!snapshot.myPrimaryId.empty() && id == snapshot.myPrimaryId) continue;
            if (!insertPlayerRow(p)) {
                ok = false;
                break;
            }
        }
    }
    sqlite3_finalize(stmtPlayers);

    if (!ok || !UpsertMatchStatsLocked(matchId, snapshot)) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to commit transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    std::cout << "Match saved to database. ID: " << matchId << " Gamemode: " << gamemode << "\n";
}

void DatabaseManager::ResolvePendingMatchLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    const bool validResult =
        snapshot.validResult &&
        (snapshot.myTeam == 0 || snapshot.myTeam == 1) &&
        (snapshot.winnerTeam == 0 || snapshot.winnerTeam == 1);

    if (!validResult) {
        sqlite3_stmt* stmt = nullptr;
        bool ok = sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
        if (ok && HasStatsTablesLocked()) {
            for (const char* sql : {"DELETE FROM MatchPlayerStats WHERE match_id = ?;",
                                    "DELETE FROM MatchLocalStats WHERE match_id = ?;"}) {
                if (!ok) break;
                ok = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_int64(stmt, 1, matchId);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                }
                sqlite3_finalize(stmt);
                stmt = nullptr;
            }
        }
        for (const char* sql : {"DELETE FROM MatchPlayers WHERE match_id = ?;",
                                "DELETE FROM Matches WHERE id = ?;"}) {
            if (!ok) break;
            ok = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_int64(stmt, 1, matchId);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        if (!ok || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            std::cerr << "[Database] Failed to remove voided pending match.\n";
            return;
        }
        std::cout << "[Database] Removed pending match voided on confirmation. ID: " << matchId
                  << (snapshot.voidReason.empty() ? "" : ", reason=" + snapshot.voidReason) << "\n";
        return;
    }

    const bool win = snapshot.winnerTeam == snapshot.myTeam;
    const int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
    const int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to begin pending match confirmation.\n";
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE Matches SET win = ?, our_score = ?, their_score = ?, result_pending = 0 WHERE id = ?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        std::cerr << "[Database] Failed to prepare pending match confirmation.\n";
        return;
    }
    sqlite3_bind_int(stmt, 1, win ? 1 : 0);
    sqlite3_bind_int(stmt, 2, ourScore);
    sqlite3_bind_int(stmt, 3, theirScore);
    sqlite3_bind_int64(stmt, 4, matchId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && UpsertMatchStatsLocked(matchId, snapshot);
    sqlite3_finalize(stmt);
    if (!ok || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        std::cerr << "[Database] Failed to confirm pending match.\n";
        return;
    }
    std::cout << "Pending match confirmed in database. ID: " << matchId
              << " Result: " << (win ? "win" : "loss") << "\n";
}

bool DatabaseManager::UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated) {
    if (matchGuid.empty() || primaryId.empty() || mmr <= 0) return false;

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return false;

    const char* sql = R"(
        UPDATE MatchPlayers
        SET mmr = ?, mmr_estimated = ?
        WHERE primary_id = ?
          AND match_id = (
              SELECT id
              FROM Matches
              WHERE match_guid = ?
              ORDER BY id DESC
              LIMIT 1
          );
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare post-match MMR update.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, mmr);
    sqlite3_bind_int(stmt, 2, estimated ? 1 : 0);
    sqlite3_bind_text(stmt, 3, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, matchGuid.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

void DatabaseManager::AsyncUpdateMatchPlayerMmr(std::string matchGuid, std::string primaryId, int mmr, bool estimated) {
    if (matchGuid.empty() || primaryId.empty() || mmr <= 0) return;

    (void)EnqueueDbJob([this,
                        matchGuid = std::move(matchGuid),
                        primaryId = std::move(primaryId),
                        mmr,
                        estimated]() {
        if (!UpdateMatchPlayerMmr(matchGuid, primaryId, mmr, estimated)) {
            std::cout << "[Database] Post-match MMR update found no saved row for match GUID.\n";
            return;
        }

        ConfigData conf = Config::Read();
        std::vector<SessionMatchSummary> matches;
        GetRecentMatchHistory(
            primaryId, matches, conf.previous_games_limit);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(
                m_state->history.mutex);
            m_state->history.recentSavedMatches = std::move(matches);
            std::erase_if(
                m_state->history.pendingRecentMatches,
                [&](const SessionMatchSummary& summary) {
                    return summary.matchGuid == matchGuid;
                });
            m_state->history.recentSavedMatchesLoaded = true;
            m_state->history.version++;
        }
    },
                       DbJobPriority::Critical);
}

void DatabaseManager::GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<float>& outX, std::vector<float>& outY) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    outX.clear();
    outY.clear();

    const char* sql;
    bool filtered = (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" || playlist == "casual" || playlist == "t" ||
                     playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" || playlist == "snowday" ||
                     playlist == "heatseeker");

    if (filtered) {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
            FROM MatchPlayers
            JOIN Matches ON MatchPlayers.match_id = Matches.id
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0
              AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
              AND Matches.gamemode = ?
            ORDER BY Matches.timestamp ASC;
        )";
    } else {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
            FROM MatchPlayers
            JOIN Matches ON MatchPlayers.match_id = Matches.id
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0
              AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
            ORDER BY Matches.timestamp ASC;
        )";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    if (filtered) {
        sqlite3_bind_text(stmt, 2, playlist.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        float epoch = (float)sqlite3_column_double(stmt, 0);
        float mmr = (float)sqlite3_column_int(stmt, 1);
        outX.push_back(epoch);
        outY.push_back(mmr);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    outMatches.clear();
    if (!m_db) return;

    if (limit <= 0) limit = kPreviousGamesDefaultLimit;
    limit = std::clamp(limit, 10, kPreviousGamesMaxLimit);

    const char* sql = R"(
        SELECT Matches.our_score, Matches.their_score, Matches.win, Matches.gamemode, Matches.player_count,
               strftime('%s', Matches.timestamp), COALESCE(MatchPlayers.mmr, 0), Matches.match_guid, Matches.playlist_id,
               COALESCE(MatchPlayers.mmr_estimated, 0), Matches.id
        FROM Matches
        LEFT JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?1
        WHERE EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id = Matches.id AND p.primary_id = ?1)
        ORDER BY Matches.timestamp DESC, Matches.id DESC
        LIMIT ?2;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionMatchSummary summary;
        summary.matchId = sqlite3_column_int64(stmt, 10);
        summary.mode = DescribeMatchPlaylist(stmt, 3, 4, 8, summary.ranked);
        summary.matchGuid = SqlColumnText(stmt, 7);
        summary.ourScore = sqlite3_column_int(stmt, 0);
        summary.theirScore = sqlite3_column_int(stmt, 1);
        summary.mmr = sqlite3_column_int(stmt, 6);
        summary.mmrEstimated = sqlite3_column_int(stmt, 9) != 0;
        summary.win = sqlite3_column_int(stmt, 2) != 0;
        summary.endedAtUnix = sqlite3_column_int64(stmt, 5);
        outMatches.push_back(std::move(summary));
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetPlayerEncounterRecord(const std::string& primaryId, int& winsWith, int& lossesWith, int& winsAgainst, int& lossesAgainst) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    winsWith = 0;
    lossesWith = 0;
    winsAgainst = 0;
    lossesAgainst = 0;
    if (!m_db) return;

    const char* sql = R"(
        SELECT
            SUM(CASE WHEN Matches.win = 1 AND MatchPlayers.is_opponent = 0 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 AND MatchPlayers.is_opponent = 0 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 1 AND MatchPlayers.is_opponent = 1 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 AND MatchPlayers.is_opponent = 1 THEN 1 ELSE 0 END)
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        winsWith = sqlite3_column_int(stmt, 0);
        lossesWith = sqlite3_column_int(stmt, 1);
        winsAgainst = sqlite3_column_int(stmt, 2);
        lossesAgainst = sqlite3_column_int(stmt, 3);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetPeopleRecords(const std::string& primaryId, std::vector<PersonRecord>& outPeople) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    outPeople.clear();
    if (!m_db || primaryId.empty()) return;

    // Everyone who shared a saved match with the local account. The bare
    // MatchPlayers.name column takes its value from the row that supplied
    // MAX(timestamp), so each person shows the most recent name they used.
    const char* sql = R"(
        SELECT MatchPlayers.primary_id,
               MatchPlayers.name,
               SUM(CASE WHEN Matches.win = 1 AND MatchPlayers.is_opponent = 0 THEN 1 ELSE 0 END),
               SUM(CASE WHEN Matches.win = 0 AND MatchPlayers.is_opponent = 0 THEN 1 ELSE 0 END),
               SUM(CASE WHEN Matches.win = 1 AND MatchPlayers.is_opponent = 1 THEN 1 ELSE 0 END),
               SUM(CASE WHEN Matches.win = 0 AND MatchPlayers.is_opponent = 1 THEN 1 ELSE 0 END),
               MAX(strftime('%s', Matches.timestamp))
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id != ?1
          AND MatchPlayers.primary_id NOT LIKE 'Unknown|%'
          AND Matches.id IN (SELECT match_id FROM MatchPlayers WHERE primary_id = ?1)
        GROUP BY MatchPlayers.primary_id
        ORDER BY COUNT(*) DESC
        LIMIT 500;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PersonRecord person;
        person.primaryId = SqlColumnText(stmt, 0);
        person.name = SqlColumnText(stmt, 1);
        person.winsWith = sqlite3_column_int(stmt, 2);
        person.lossesWith = sqlite3_column_int(stmt, 3);
        person.winsAgainst = sqlite3_column_int(stmt, 4);
        person.lossesAgainst = sqlite3_column_int(stmt, 5);
        person.lastSeenUnix = sqlite3_column_int64(stmt, 6);
        outPeople.push_back(std::move(person));
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    outMatches.clear();
    if (!m_db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT strftime('%s', Matches.timestamp),
               CAST(strftime('%H', Matches.timestamp, 'localtime') AS INTEGER),
               Matches.win, Matches.gamemode, Matches.player_count, Matches.playlist_id
        FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?
        ORDER BY Matches.timestamp ASC, Matches.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchOutcome outcome;
        outcome.endedAtUnix = sqlite3_column_int64(stmt, 0);
        outcome.localHour = sqlite3_column_int(stmt, 1);
        outcome.win = sqlite3_column_int(stmt, 2) != 0;
        bool ranked = false;
        outcome.playlist = DescribeMatchPlaylist(stmt, 3, 4, 5, ranked);
        outMatches.push_back(std::move(outcome));
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    out.clear();
    if (!m_db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT m.id, m.playlist_id, m.gamemode, m.player_count, m.win, strftime('%s', m.timestamp),
               COALESCE(me.mmr, 0) AS my_mmr, COALESCE(me.mmr_estimated, 0),
               COALESCE(AVG(CASE WHEN p.is_opponent = 0 AND p.mmr > 0 THEN p.mmr END), 0.0) AS team_avg,
               COALESCE(AVG(CASE WHEN p.is_opponent = 1 AND p.mmr > 0 THEN p.mmr END), 0.0) AS opp_avg,
               SUM(CASE WHEN p.is_opponent = 0 AND p.mmr > 0 THEN 1 ELSE 0 END) AS team_n,
               SUM(CASE WHEN p.is_opponent = 1 AND p.mmr > 0 THEN 1 ELSE 0 END) AS opp_n
        FROM Matches m
        JOIN MatchPlayers me ON me.match_id = m.id AND me.primary_id = ?1
        JOIN MatchPlayers p  ON p.match_id = m.id
        GROUP BY m.id
        ORDER BY m.timestamp ASC, m.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchMmrContext ctx;
        bool ranked = false;
        std::string playlist = DescribeMatchPlaylist(stmt, 2, 3, 1, ranked);
        ctx.playlist = ranked ? std::move(playlist) : "Casual";
        ctx.win = sqlite3_column_int(stmt, 4) != 0;
        ctx.endedAtUnix = sqlite3_column_int64(stmt, 5);
        ctx.myMmr = sqlite3_column_int(stmt, 6);
        ctx.mmrEstimated = sqlite3_column_int(stmt, 7) != 0;
        ctx.teamAvg = sqlite3_column_double(stmt, 8);
        ctx.oppAvg = sqlite3_column_double(stmt, 9);
        ctx.teamCount = sqlite3_column_int(stmt, 10);
        ctx.oppCount = sqlite3_column_int(stmt, 11);
        out.push_back(std::move(ctx));
    }
    sqlite3_finalize(stmt);
}

bool DatabaseManager::ExportLocalData(std::string& exportPath, std::string& error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
        error = "Database is not open.";
        return false;
    }

    exportPath = Storage::GetDataDirectory() + "exports\\omnistats_export_" + TimestampForFilename() + "\\";
    std::error_code ec;
    fs::create_directories(exportPath, ec);
    if (ec) {
        error = "Failed to create export folder.";
        return false;
    }

    nlohmann::json matches = nlohmann::json::array();
    std::ofstream matchesCsv(exportPath + "matches.csv", std::ios::trunc);
    std::ofstream playersCsv(exportPath + "match_players.csv", std::ios::trunc);
    if (!matchesCsv.is_open() || !playersCsv.is_open()) {
        error = "Failed to open export files.";
        return false;
    }

    matchesCsv << "id,timestamp,arena,our_score,their_score,win,match_guid,playlist_id,gamemode,player_count\n";
    playersCsv << "match_id,primary_id,name,team,mmr,mmr_estimated,is_opponent\n";

    const bool hasStatsTables = HasStatsTablesLocked();
    sqlite3_stmt* localStatsStmt = nullptr;
    sqlite3_stmt* playerStatsStmt = nullptr;
    if (hasStatsTables) {
        const char* localStatsSql =
            "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
            "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;";
        sqlite3_prepare_v2(m_db, localStatsSql, -1, &localStatsStmt, nullptr);
        const char* playerStatsSql =
            "SELECT score, goals, assists, saves, shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time "
            "FROM MatchPlayerStats WHERE match_id = ? AND primary_id = ?;";
        sqlite3_prepare_v2(m_db, playerStatsSql, -1, &playerStatsStmt, nullptr);
    }

    const bool hasSessionsTable = HasSessionsTableLocked();
    const char* matchesSql = hasSessionsTable
                                 ? "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, session_id FROM Matches ORDER BY timestamp ASC;"
                                 : "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, NULL FROM Matches ORDER BY timestamp ASC;";
    sqlite3_stmt* matchStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, matchesSql, -1, &matchStmt, nullptr) != SQLITE_OK) {
        error = "Failed to read matches.";
        return false;
    }

    while (sqlite3_step(matchStmt) == SQLITE_ROW) {
        nlohmann::json match;
        int matchId = sqlite3_column_int(matchStmt, 0);
        match["id"] = matchId;
        match["timestamp"] = SqlColumnText(matchStmt, 1);
        match["arena"] = SqlColumnText(matchStmt, 2);
        match["our_score"] = sqlite3_column_int(matchStmt, 3);
        match["their_score"] = sqlite3_column_int(matchStmt, 4);
        match["win"] = sqlite3_column_int(matchStmt, 5) != 0;
        match["match_guid"] = SqlColumnText(matchStmt, 6);
        if (sqlite3_column_type(matchStmt, 7) == SQLITE_NULL)
            match["playlist_id"] = nullptr;
        else
            match["playlist_id"] = sqlite3_column_int(matchStmt, 7);
        match["gamemode"] = SqlColumnText(matchStmt, 8);
        match["player_count"] = sqlite3_column_int(matchStmt, 9);
        if (sqlite3_column_type(matchStmt, 10) == SQLITE_NULL)
            match["session_id"] = nullptr;
        else
            match["session_id"] = sqlite3_column_int64(matchStmt, 10);
        match["players"] = nlohmann::json::array();
        match["local_stats"] = nullptr;
        if (localStatsStmt) {
            sqlite3_reset(localStatsStmt);
            sqlite3_clear_bindings(localStatsStmt);
            sqlite3_bind_int(localStatsStmt, 1, matchId);
            if (sqlite3_step(localStatsStmt) == SQLITE_ROW) {
                auto optInt = [&](int col) -> nlohmann::json {
                    return sqlite3_column_type(localStatsStmt, col) == SQLITE_NULL
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(sqlite3_column_int(localStatsStmt, col));
                };
                auto optDouble = [&](int col) -> nlohmann::json {
                    return sqlite3_column_type(localStatsStmt, col) == SQLITE_NULL
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(sqlite3_column_double(localStatsStmt, col));
                };
                match["local_stats"] = {
                    {"boost_collected", optInt(0)},
                    {"demoed", optInt(1)},
                    {"crossbars", optInt(2)},
                    {"hardest_crossbar", optDouble(3)},
                    {"max_ball_speed", optDouble(4)},
                    {"own_goals", optInt(5)},
                    {"duration_seconds", optDouble(6)},
                    {"overtime_seconds", optDouble(7)},
                    {"stats_version", sqlite3_column_int(localStatsStmt, 8)},
                };
            }
        }

        matchesCsv << matchId << ","
                   << CsvEscape(match["timestamp"].get<std::string>()) << ","
                   << CsvEscape(match["arena"].get<std::string>()) << ","
                   << match["our_score"].get<int>() << ","
                   << match["their_score"].get<int>() << ","
                   << (match["win"].get<bool>() ? 1 : 0) << ","
                   << CsvEscape(match["match_guid"].get<std::string>()) << ","
                   << (match["playlist_id"].is_null()
                           ? ""
                           : std::to_string(match["playlist_id"].get<int>()))
                   << ","
                   << CsvEscape(match["gamemode"].get<std::string>()) << ","
                   << match["player_count"].get<int>() << "\n";

        const char* playersSql = "SELECT primary_id, name, team, mmr, mmr_estimated, is_opponent FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";
        sqlite3_stmt* playerStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, playersSql, -1, &playerStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(playerStmt, 1, matchId);
            while (sqlite3_step(playerStmt) == SQLITE_ROW) {
                nlohmann::json player;
                player["primary_id"] = SqlColumnText(playerStmt, 0);
                player["name"] = SqlColumnText(playerStmt, 1);
                player["team"] = sqlite3_column_int(playerStmt, 2);
                player["mmr"] = sqlite3_column_int(playerStmt, 3);
                player["mmr_estimated"] = sqlite3_column_int(playerStmt, 4) != 0;
                player["is_opponent"] = sqlite3_column_int(playerStmt, 5) != 0;
                player["stats"] = nullptr;
                if (playerStatsStmt) {
                    sqlite3_reset(playerStatsStmt);
                    sqlite3_clear_bindings(playerStatsStmt);
                    sqlite3_bind_int(playerStatsStmt, 1, matchId);
                    const std::string pid = player["primary_id"].get<std::string>();
                    sqlite3_bind_text(playerStatsStmt, 2, pid.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(playerStatsStmt) == SQLITE_ROW) {
                        auto optInt = [&](int col) -> nlohmann::json {
                            return sqlite3_column_type(playerStatsStmt, col) == SQLITE_NULL
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(sqlite3_column_int(playerStatsStmt, col));
                        };
                        auto optDouble = [&](int col) -> nlohmann::json {
                            return sqlite3_column_type(playerStatsStmt, col) == SQLITE_NULL
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(sqlite3_column_double(playerStatsStmt, col));
                        };
                        player["stats"] = {
                            {"score", optInt(0)},
                            {"goals", optInt(1)},
                            {"assists", optInt(2)},
                            {"saves", optInt(3)},
                            {"shots", optInt(4)},
                            {"demos", optInt(5)},
                            {"touches", optInt(6)},
                            {"car_touches", optInt(7)},
                            {"max_goal_speed", optDouble(8)},
                            {"fastest_goal_time", optDouble(9)},
                        };
                    }
                }
                match["players"].push_back(player);

                playersCsv << matchId << ","
                           << CsvEscape(player["primary_id"].get<std::string>()) << ","
                           << CsvEscape(player["name"].get<std::string>()) << ","
                           << player["team"].get<int>() << ","
                           << player["mmr"].get<int>() << ","
                           << (player["mmr_estimated"].get<bool>() ? 1 : 0) << ","
                           << (player["is_opponent"].get<bool>() ? 1 : 0) << "\n";
            }
        }
        if (playerStmt) sqlite3_finalize(playerStmt);

        matches.push_back(match);
    }
    sqlite3_finalize(matchStmt);
    if (playerStatsStmt) sqlite3_finalize(playerStatsStmt);
    if (localStatsStmt) sqlite3_finalize(localStatsStmt);

    std::ofstream jsonFile(exportPath + "matches.json", std::ios::trunc);
    if (!jsonFile.is_open()) {
        error = "Failed to write JSON export.";
        return false;
    }
    jsonFile << matches.dump(2);

    if (hasSessionsTable) {
        nlohmann::json sessionsJson = nlohmann::json::array();
        std::ofstream sessionsCsv(exportPath + "sessions.csv", std::ios::trunc);
        if (sessionsCsv.is_open()) {
            sessionsCsv << "id,account,started_at,ended_at,wins,losses,source,mmr_change_json,totals_json\n";
        }
        sqlite3_stmt* sessStmt = nullptr;
        const char* sessSql =
            "SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0), COALESCE(wins, 0), COALESCE(losses, 0), "
            "COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'), COALESCE(source, 'live') "
            "FROM Sessions ORDER BY started_at ASC, id ASC;";
        if (sqlite3_prepare_v2(m_db, sessSql, -1, &sessStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(sessStmt) == SQLITE_ROW) {
                const int64_t sid = sqlite3_column_int64(sessStmt, 0);
                const std::string account = SqlColumnText(sessStmt, 1);
                const int64_t startedAt = sqlite3_column_int64(sessStmt, 2);
                const int64_t endedAt = sqlite3_column_int64(sessStmt, 3);
                const int wins = sqlite3_column_int(sessStmt, 4);
                const int losses = sqlite3_column_int(sessStmt, 5);
                const std::string mmrJsonStr = SqlColumnText(sessStmt, 6);
                const std::string totalsJsonStr = SqlColumnText(sessStmt, 7);
                const std::string source = SqlColumnText(sessStmt, 8);

                nlohmann::json sObj;
                sObj["id"] = sid;
                sObj["account"] = account;
                sObj["started_at"] = startedAt;
                sObj["ended_at"] = endedAt;
                sObj["wins"] = wins;
                sObj["losses"] = losses;
                sObj["source"] = source;
                sObj["mmr_change"] = nlohmann::json::parse(mmrJsonStr, nullptr, false);
                if (sObj["mmr_change"].is_discarded()) sObj["mmr_change"] = nlohmann::json::object();
                sObj["totals"] = nlohmann::json::parse(totalsJsonStr, nullptr, false);
                if (sObj["totals"].is_discarded()) sObj["totals"] = nlohmann::json::object();
                sessionsJson.push_back(std::move(sObj));

                if (sessionsCsv.is_open()) {
                    sessionsCsv << sid << ","
                                << CsvEscape(account) << ","
                                << startedAt << ","
                                << endedAt << ","
                                << wins << ","
                                << losses << ","
                                << CsvEscape(source) << ","
                                << CsvEscape(mmrJsonStr) << ","
                                << CsvEscape(totalsJsonStr) << "\n";
                }
            }
            sqlite3_finalize(sessStmt);
        }
        std::ofstream sessJsonFile(exportPath + "sessions.json", std::ios::trunc);
        if (sessJsonFile.is_open()) {
            sessJsonFile << sessionsJson.dump(2);
        }
    }
    error.clear();
    return true;
}

bool DatabaseManager::DeleteLocalMatchHistory(std::string& error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
        error = "Database is not open.";
        return false;
    }

    char* errMsg = nullptr;
    std::string sql = "BEGIN IMMEDIATE;";
    if (SqliteTableExists(m_db, "MatchPlayerStats")) sql += "DELETE FROM MatchPlayerStats;";
    if (SqliteTableExists(m_db, "MatchLocalStats")) sql += "DELETE FROM MatchLocalStats;";
    sql += "DELETE FROM MatchPlayers; DELETE FROM Matches; ";
    if (SqliteTableExists(m_db, "Sessions")) sql += "DELETE FROM Sessions; ";
    sql += "DELETE FROM sqlite_sequence WHERE name IN ('Matches', 'MatchPlayers', 'Sessions'); COMMIT;";
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        error = errMsg ? errMsg : "Failed to delete local history.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    m_sessionIdByGeneration.clear();
    m_backfillPending.store(false, std::memory_order_release);
    error.clear();
    return true;
}

DbMergeResult DatabaseManager::MergeDatabase(const std::string& sourceDbPath) {
    DbMergeResult result;
    if (sourceDbPath.empty()) {
        result.error = "No database file selected.";
        return result;
    }

    std::error_code ec;
    fs::path srcPath(sourceDbPath);
    if (!fs::exists(srcPath, ec) || !fs::is_regular_file(srcPath, ec)) {
        result.error = "Selected file does not exist or is not a regular file.";
        return result;
    }

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
        result.error = "Target database is not open.";
        return result;
    }

    if (!m_dbPath.empty()) {
        fs::path targetPath(m_dbPath);
        if (fs::equivalent(srcPath, targetPath, ec)) {
            result.error = "Cannot merge the active database into itself.";
            return result;
        }
    }

    sqlite3* srcDb = nullptr;
    int rc = sqlite3_open_v2(srcPath.string().c_str(), &srcDb, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK || !srcDb) {
        result.error = "Failed to open source database: " + std::string(srcDb ? sqlite3_errmsg(srcDb) : "unknown error");
        if (srcDb) sqlite3_close(srcDb);
        return result;
    }

    auto tableExists = [](sqlite3* db, const char* name) -> bool {
        const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    };

    if (!tableExists(srcDb, "Matches")) {
        result.error = "Selected file is not an OmniStats database (missing Matches table).";
        sqlite3_close(srcDb);
        return result;
    }

    if (!tableExists(srcDb, "MatchPlayers")) {
        result.error = "Selected file is not an OmniStats database (missing MatchPlayers table).";
        sqlite3_close(srcDb);
        return result;
    }

    const bool srcHasPlaylistId = SqliteTableHasColumn(srcDb, "Matches", "playlist_id");
    const bool srcHasMmrEstimated = SqliteTableHasColumn(srcDb, "MatchPlayers", "mmr_estimated");
    const bool srcHasIsOpponent = SqliteTableHasColumn(srcDb, "MatchPlayers", "is_opponent");

    const bool copyPlayerStats =
        tableExists(srcDb, "MatchPlayerStats") && SqliteTableExists(m_db, "MatchPlayerStats");
    const bool copyLocalStats =
        tableExists(srcDb, "MatchLocalStats") && SqliteTableExists(m_db, "MatchLocalStats");
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        result.error = errMsg ? errMsg : "Failed to begin transaction on target database.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_close(srcDb);
        return result;
    }

    const char* checkGuidSql = "SELECT id FROM Matches WHERE match_guid = ? AND match_guid != '' LIMIT 1;";
    sqlite3_stmt* checkGuidStmt = nullptr;
    const char* checkFallbackSql = "SELECT id FROM Matches WHERE timestamp = ? AND arena = ? AND our_score = ? AND their_score = ? AND win = ? LIMIT 1;";
    sqlite3_stmt* checkFallbackStmt = nullptr;

    const char* insertMatchSql =
        "INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertMatchStmt = nullptr;

    const char* insertPlayerSql =
        "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertPlayerStmt = nullptr;

    if (sqlite3_prepare_v2(m_db, checkGuidSql, -1, &checkGuidStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, checkFallbackSql, -1, &checkFallbackStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, insertMatchSql, -1, &insertMatchStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, insertPlayerSql, -1, &insertPlayerStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to prepare target database statements.";
        if (checkGuidStmt) sqlite3_finalize(checkGuidStmt);
        if (checkFallbackStmt) sqlite3_finalize(checkFallbackStmt);
        if (insertMatchStmt) sqlite3_finalize(insertMatchStmt);
        if (insertPlayerStmt) sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    std::string selectMatchesSql = "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, ";
    selectMatchesSql += srcHasPlaylistId ? "playlist_id, " : "NULL, ";
    selectMatchesSql += "gamemode, player_count FROM Matches ORDER BY id ASC;";

    sqlite3_stmt* selectMatchesStmt = nullptr;
    if (sqlite3_prepare_v2(srcDb, selectMatchesSql.c_str(), -1, &selectMatchesStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to read matches from source database: " + std::string(sqlite3_errmsg(srcDb));
        sqlite3_finalize(checkGuidStmt);
        sqlite3_finalize(checkFallbackStmt);
        sqlite3_finalize(insertMatchStmt);
        sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    std::string selectPlayersSql = "SELECT primary_id, name, team, mmr, ";
    selectPlayersSql += srcHasMmrEstimated ? "mmr_estimated, " : "0, ";
    selectPlayersSql += srcHasIsOpponent ? "is_opponent " : "0 ";
    selectPlayersSql += "FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";

    sqlite3_stmt* selectPlayersStmt = nullptr;
    if (sqlite3_prepare_v2(srcDb, selectPlayersSql.c_str(), -1, &selectPlayersStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to prepare player read statement from source database: " + std::string(sqlite3_errmsg(srcDb));
        sqlite3_finalize(selectMatchesStmt);
        sqlite3_finalize(checkGuidStmt);
        sqlite3_finalize(checkFallbackStmt);
        sqlite3_finalize(insertMatchStmt);
        sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    sqlite3_stmt* selectPlayerStatsStmt = nullptr;
    sqlite3_stmt* insertPlayerStatsStmt = nullptr;
    if (copyPlayerStats) {
        const char* selectPlayerStatsSql =
            "SELECT primary_id, score, goals, assists, saves, shots, demos, touches, car_touches, "
            "max_goal_speed, fastest_goal_time FROM MatchPlayerStats WHERE match_id = ?;";
        const char* insertPlayerStatsSql =
            "INSERT OR REPLACE INTO MatchPlayerStats (match_id, primary_id, score, goals, assists, saves, "
            "shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(srcDb, selectPlayerStatsSql, -1, &selectPlayerStatsStmt, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(m_db, insertPlayerStatsSql, -1, &insertPlayerStatsStmt, nullptr) != SQLITE_OK) {
            result.error = "Failed to prepare MatchPlayerStats merge statements.";
            if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
            if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
            sqlite3_finalize(selectPlayersStmt);
            sqlite3_finalize(selectMatchesStmt);
            sqlite3_finalize(checkGuidStmt);
            sqlite3_finalize(checkFallbackStmt);
            sqlite3_finalize(insertMatchStmt);
            sqlite3_finalize(insertPlayerStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(srcDb);
            return result;
        }
    }

    sqlite3_stmt* selectLocalStatsStmt = nullptr;
    sqlite3_stmt* insertLocalStatsStmt = nullptr;
    if (copyLocalStats) {
        const char* selectLocalStatsSql =
            "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
            "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;";
        const char* insertLocalStatsSql =
            "INSERT OR REPLACE INTO MatchLocalStats (match_id, boost_collected, demoed, crossbars, "
            "hardest_crossbar, max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(srcDb, selectLocalStatsSql, -1, &selectLocalStatsStmt, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(m_db, insertLocalStatsSql, -1, &insertLocalStatsStmt, nullptr) != SQLITE_OK) {
            result.error = "Failed to prepare MatchLocalStats merge statements.";
            if (selectLocalStatsStmt) sqlite3_finalize(selectLocalStatsStmt);
            if (insertLocalStatsStmt) sqlite3_finalize(insertLocalStatsStmt);
            if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
            if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
            sqlite3_finalize(selectPlayersStmt);
            sqlite3_finalize(selectMatchesStmt);
            sqlite3_finalize(checkGuidStmt);
            sqlite3_finalize(checkFallbackStmt);
            sqlite3_finalize(insertMatchStmt);
            sqlite3_finalize(insertPlayerStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(srcDb);
            return result;
        }
    }
    bool loopOk = true;
    while (sqlite3_step(selectMatchesStmt) == SQLITE_ROW) {
        int64_t oldMatchId = sqlite3_column_int64(selectMatchesStmt, 0);
        std::string timestamp = SqlColumnText(selectMatchesStmt, 1);
        std::string arena = SqlColumnText(selectMatchesStmt, 2);
        int ourScore = sqlite3_column_int(selectMatchesStmt, 3);
        int theirScore = sqlite3_column_int(selectMatchesStmt, 4);
        int win = sqlite3_column_int(selectMatchesStmt, 5);
        std::string matchGuid = SqlColumnText(selectMatchesStmt, 6);
        bool playlistNull = (sqlite3_column_type(selectMatchesStmt, 7) == SQLITE_NULL);
        int playlistId = playlistNull ? 0 : sqlite3_column_int(selectMatchesStmt, 7);
        std::string gamemode = SqlColumnText(selectMatchesStmt, 8);
        int playerCount = sqlite3_column_int(selectMatchesStmt, 9);

        if (arena.empty()) {
            result.matchesSkipped++;
            continue;
        }

        bool isDuplicate = false;
        if (!matchGuid.empty()) {
            sqlite3_reset(checkGuidStmt);
            sqlite3_clear_bindings(checkGuidStmt);
            sqlite3_bind_text(checkGuidStmt, 1, matchGuid.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(checkGuidStmt) == SQLITE_ROW) {
                isDuplicate = true;
            }
        }

        if (!isDuplicate && !timestamp.empty()) {
            sqlite3_reset(checkFallbackStmt);
            sqlite3_clear_bindings(checkFallbackStmt);
            sqlite3_bind_text(checkFallbackStmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(checkFallbackStmt, 2, arena.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(checkFallbackStmt, 3, ourScore);
            sqlite3_bind_int(checkFallbackStmt, 4, theirScore);
            sqlite3_bind_int(checkFallbackStmt, 5, win);
            if (sqlite3_step(checkFallbackStmt) == SQLITE_ROW) {
                isDuplicate = true;
            }
        }

        if (isDuplicate) {
            result.matchesSkipped++;
            continue;
        }

        sqlite3_reset(insertMatchStmt);
        sqlite3_clear_bindings(insertMatchStmt);
        sqlite3_bind_text(insertMatchStmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(insertMatchStmt, 2, arena.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(insertMatchStmt, 3, ourScore);
        sqlite3_bind_int(insertMatchStmt, 4, theirScore);
        sqlite3_bind_int(insertMatchStmt, 5, win);
        sqlite3_bind_text(insertMatchStmt, 6, matchGuid.c_str(), -1, SQLITE_STATIC);
        if (playlistNull) {
            sqlite3_bind_null(insertMatchStmt, 7);
        } else {
            sqlite3_bind_int(insertMatchStmt, 7, playlistId);
        }
        sqlite3_bind_text(insertMatchStmt, 8, gamemode.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(insertMatchStmt, 9, playerCount);

        if (sqlite3_step(insertMatchStmt) != SQLITE_DONE) {
            result.error = "Failed to insert match into target database: " + std::string(sqlite3_errmsg(m_db));
            loopOk = false;
            break;
        }

        sqlite3_int64 newMatchId = sqlite3_last_insert_rowid(m_db);
        result.matchesImported++;

        sqlite3_reset(selectPlayersStmt);
        sqlite3_clear_bindings(selectPlayersStmt);
        sqlite3_bind_int64(selectPlayersStmt, 1, oldMatchId);

        while (sqlite3_step(selectPlayersStmt) == SQLITE_ROW) {
            std::string primaryId = SqlColumnText(selectPlayersStmt, 0);
            std::string name = SqlColumnText(selectPlayersStmt, 1);
            int team = sqlite3_column_int(selectPlayersStmt, 2);
            int mmr = sqlite3_column_int(selectPlayersStmt, 3);
            int mmrEstimated = sqlite3_column_int(selectPlayersStmt, 4);
            int isOpponent = sqlite3_column_int(selectPlayersStmt, 5);

            sqlite3_reset(insertPlayerStmt);
            sqlite3_clear_bindings(insertPlayerStmt);
            sqlite3_bind_int64(insertPlayerStmt, 1, newMatchId);
            sqlite3_bind_text(insertPlayerStmt, 2, primaryId.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(insertPlayerStmt, 3, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(insertPlayerStmt, 4, team);
            sqlite3_bind_int(insertPlayerStmt, 5, mmr);
            sqlite3_bind_int(insertPlayerStmt, 6, mmrEstimated);
            sqlite3_bind_int(insertPlayerStmt, 7, isOpponent);

            if (sqlite3_step(insertPlayerStmt) != SQLITE_DONE) {
                result.error = "Failed to insert player into target database: " + std::string(sqlite3_errmsg(m_db));
                loopOk = false;
                break;
            }
            result.playersImported++;
        }

        if (!loopOk) break;
        if (copyPlayerStats) {
            sqlite3_reset(selectPlayerStatsStmt);
            sqlite3_clear_bindings(selectPlayerStatsStmt);
            sqlite3_bind_int64(selectPlayerStatsStmt, 1, oldMatchId);
            while (sqlite3_step(selectPlayerStatsStmt) == SQLITE_ROW) {
                std::string primaryId = SqlColumnText(selectPlayerStatsStmt, 0);
                sqlite3_reset(insertPlayerStatsStmt);
                sqlite3_clear_bindings(insertPlayerStatsStmt);
                sqlite3_bind_int64(insertPlayerStatsStmt, 1, newMatchId);
                sqlite3_bind_text(insertPlayerStatsStmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
                for (int col = 1; col <= 8; ++col) {
                    if (sqlite3_column_type(selectPlayerStatsStmt, col) == SQLITE_NULL) {
                        sqlite3_bind_null(insertPlayerStatsStmt, col + 2);
                    } else {
                        sqlite3_bind_int(insertPlayerStatsStmt, col + 2, sqlite3_column_int(selectPlayerStatsStmt, col));
                    }
                }
                for (int col = 9; col <= 10; ++col) {
                    if (sqlite3_column_type(selectPlayerStatsStmt, col) == SQLITE_NULL) {
                        sqlite3_bind_null(insertPlayerStatsStmt, col + 2);
                    } else {
                        sqlite3_bind_double(insertPlayerStatsStmt, col + 2, sqlite3_column_double(selectPlayerStatsStmt, col));
                    }
                }
                if (sqlite3_step(insertPlayerStatsStmt) != SQLITE_DONE) {
                    result.error = "Failed to insert MatchPlayerStats into target database: " + std::string(sqlite3_errmsg(m_db));
                    loopOk = false;
                    break;
                }
            }
            if (!loopOk) break;
        }

        if (copyLocalStats) {
            sqlite3_reset(selectLocalStatsStmt);
            sqlite3_clear_bindings(selectLocalStatsStmt);
            sqlite3_bind_int64(selectLocalStatsStmt, 1, oldMatchId);
            if (sqlite3_step(selectLocalStatsStmt) == SQLITE_ROW) {
                sqlite3_reset(insertLocalStatsStmt);
                sqlite3_clear_bindings(insertLocalStatsStmt);
                sqlite3_bind_int64(insertLocalStatsStmt, 1, newMatchId);
                auto bindOptInt = [&](int srcCol, int dstParam) {
                    if (sqlite3_column_type(selectLocalStatsStmt, srcCol) == SQLITE_NULL) {
                        sqlite3_bind_null(insertLocalStatsStmt, dstParam);
                    } else {
                        sqlite3_bind_int(insertLocalStatsStmt, dstParam, sqlite3_column_int(selectLocalStatsStmt, srcCol));
                    }
                };
                auto bindOptDouble = [&](int srcCol, int dstParam) {
                    if (sqlite3_column_type(selectLocalStatsStmt, srcCol) == SQLITE_NULL) {
                        sqlite3_bind_null(insertLocalStatsStmt, dstParam);
                    } else {
                        sqlite3_bind_double(insertLocalStatsStmt, dstParam, sqlite3_column_double(selectLocalStatsStmt, srcCol));
                    }
                };
                bindOptInt(0, 2);
                bindOptInt(1, 3);
                bindOptInt(2, 4);
                bindOptDouble(3, 5);
                bindOptDouble(4, 6);
                bindOptInt(5, 7);
                bindOptDouble(6, 8);
                bindOptDouble(7, 9);
                sqlite3_bind_int(insertLocalStatsStmt, 10, sqlite3_column_int(selectLocalStatsStmt, 8));
                if (sqlite3_step(insertLocalStatsStmt) != SQLITE_DONE) {
                    result.error = "Failed to insert MatchLocalStats into target database: " + std::string(sqlite3_errmsg(m_db));
                    loopOk = false;
                    break;
                }
            }
            if (!loopOk) break;
        }
    }

    sqlite3_finalize(selectPlayersStmt);
    if (selectLocalStatsStmt) sqlite3_finalize(selectLocalStatsStmt);
    if (insertLocalStatsStmt) sqlite3_finalize(insertLocalStatsStmt);
    if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
    if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
    sqlite3_finalize(selectMatchesStmt);
    sqlite3_finalize(checkGuidStmt);
    sqlite3_finalize(checkFallbackStmt);
    sqlite3_finalize(insertMatchStmt);
    sqlite3_finalize(insertPlayerStmt);
    sqlite3_close(srcDb);

    if (!loopOk) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return result;
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        result.error = errMsg ? errMsg : "Failed to commit merge transaction.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return result;
    }
    while (result.matchesImported > 0) {
        bool moreRemaining = false;
        BackfillSessionsChunkLocked(500, moreRemaining);
        if (!moreRemaining) break;
    }
    result.success = true;
    return result;
}

static std::string NormalizeStatSeriesPlaylistFilter(const std::string& filter) {
    if (filter.empty() || filter == "all" || filter == "All" || filter == "best" || filter == "Best") {
        return "";
    }
    if (filter == "Duel") return "1v1";
    if (filter == "Doubles") return "2v2";
    if (filter == "Standard") return "3v3";
    if (filter == "Casual") return "casual";
    if (filter == "Tournament" || filter == "Tournament Match") return "t";
    if (filter == "Hoops") return "hoops";
    if (filter == "Rumble") return "rumble";
    if (filter == "Dropshot") return "dropshot";
    if (filter == "Snow Day") return "snowday";
    if (filter == "Heatseeker") return "heatseeker";
    return filter;
}

std::vector<PlayerStatRow> DatabaseManager::GetPlayerStatSeries(const std::string& primaryId,
                                                                const std::string& playlistFilter,
                                                                int limit) {
    std::vector<PlayerStatRow> rows;
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || primaryId.empty() || !HasStatsTablesLocked()) return rows;

    if (limit <= 0) limit = 100;
    const std::string normalizedFilter = NormalizeStatSeriesPlaylistFilter(playlistFilter);
    const bool filtered = !normalizedFilter.empty();

    std::string sql = R"(
        SELECT Matches.id, Matches.match_guid, strftime('%s', Matches.timestamp),
               Matches.gamemode, COALESCE(Matches.playlist_id, -1), Matches.win,
               Matches.our_score, Matches.their_score, COALESCE(MatchPlayers.mmr, 0),
               MatchPlayerStats.score, COALESCE(MatchPlayerStats.goals, 0),
               COALESCE(MatchPlayerStats.assists, 0), COALESCE(MatchPlayerStats.saves, 0),
               COALESCE(MatchPlayerStats.shots, 0), COALESCE(MatchPlayerStats.demos, 0),
               MatchPlayerStats.touches, MatchPlayerStats.car_touches,
               MatchPlayerStats.max_goal_speed, MatchPlayerStats.fastest_goal_time,
               MatchLocalStats.match_id, COALESCE(MatchLocalStats.boost_collected, 0),
               COALESCE(MatchLocalStats.demoed, 0), COALESCE(MatchLocalStats.crossbars, 0),
               MatchLocalStats.hardest_crossbar, MatchLocalStats.max_ball_speed,
               COALESCE(MatchLocalStats.own_goals, 0), MatchLocalStats.duration_seconds,
               MatchLocalStats.overtime_seconds, COALESCE(MatchLocalStats.stats_version, 1)
        FROM MatchPlayerStats
        JOIN Matches ON Matches.id = MatchPlayerStats.match_id
        LEFT JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?1
        LEFT JOIN MatchLocalStats ON MatchLocalStats.match_id = Matches.id
        WHERE MatchPlayerStats.primary_id = ?1
    )";
    if (filtered) {
        sql += " AND Matches.gamemode = ?2 ORDER BY Matches.timestamp DESC, Matches.id DESC LIMIT ?3;";
    } else {
        sql += " ORDER BY Matches.timestamp DESC, Matches.id DESC LIMIT ?2;";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    if (filtered) {
        sqlite3_bind_text(stmt, 2, normalizedFilter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
    } else {
        sqlite3_bind_int(stmt, 2, limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PlayerStatRow row;
        row.matchId = sqlite3_column_int64(stmt, 0);
        row.matchGuid = SqlColumnText(stmt, 1);
        row.timestampUnix = sqlite3_column_int64(stmt, 2);
        row.playlist = SqlColumnText(stmt, 3);
        row.playlistId = sqlite3_column_int(stmt, 4);
        row.win = sqlite3_column_int(stmt, 5) != 0;
        row.ourScore = sqlite3_column_int(stmt, 6);
        row.theirScore = sqlite3_column_int(stmt, 7);
        row.mmr = sqlite3_column_int(stmt, 8);
        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) row.score = sqlite3_column_int(stmt, 9);
        row.goals = sqlite3_column_int(stmt, 10);
        row.assists = sqlite3_column_int(stmt, 11);
        row.saves = sqlite3_column_int(stmt, 12);
        row.shots = sqlite3_column_int(stmt, 13);
        row.demos = sqlite3_column_int(stmt, 14);
        if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) row.touches = sqlite3_column_int(stmt, 15);
        if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) row.carTouches = sqlite3_column_int(stmt, 16);
        if (sqlite3_column_type(stmt, 17) != SQLITE_NULL)
            row.maxGoalSpeed = static_cast<float>(sqlite3_column_double(stmt, 17));
        if (sqlite3_column_type(stmt, 18) != SQLITE_NULL)
            row.fastestGoalTime = static_cast<float>(sqlite3_column_double(stmt, 18));
        row.hasLocalStats = sqlite3_column_type(stmt, 19) != SQLITE_NULL;
        row.boostCollected = sqlite3_column_int(stmt, 20);
        row.demoed = sqlite3_column_int(stmt, 21);
        row.crossbars = sqlite3_column_int(stmt, 22);
        if (sqlite3_column_type(stmt, 23) != SQLITE_NULL)
            row.hardestCrossbar = static_cast<float>(sqlite3_column_double(stmt, 23));
        if (sqlite3_column_type(stmt, 24) != SQLITE_NULL)
            row.maxBallSpeed = static_cast<float>(sqlite3_column_double(stmt, 24));
        row.ownGoals = sqlite3_column_int(stmt, 25);
        if (sqlite3_column_type(stmt, 26) != SQLITE_NULL)
            row.durationSeconds = static_cast<float>(sqlite3_column_double(stmt, 26));
        if (sqlite3_column_type(stmt, 27) != SQLITE_NULL)
            row.overtimeSeconds = static_cast<float>(sqlite3_column_double(stmt, 27));
        row.statsVersion = sqlite3_column_int(stmt, 28);
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return rows;
}

void DatabaseManager::GetDetailedStatsSummary(int& outMatchCount, std::string& outSinceDate) {
    outMatchCount = 0;
    outSinceDate.clear();
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !HasStatsTablesLocked()) return;

    const char* sql = R"(
        SELECT COUNT(*), COALESCE(MIN(substr(Matches.timestamp, 1, 10)), '')
        FROM Matches
        WHERE EXISTS (SELECT 1 FROM MatchLocalStats s WHERE s.match_id = Matches.id)
           OR EXISTS (SELECT 1 FROM MatchPlayerStats p WHERE p.match_id = Matches.id);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        outMatchCount = sqlite3_column_int(stmt, 0);
        if (outMatchCount > 0) {
            outSinceDate = SqlColumnText(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::AsyncRefreshDetailedStatsSummary() {
    (void)EnqueueDbJob([this]() {
        int count = 0;
        std::string sinceDate;
        GetDetailedStatsSummary(count, sinceDate);
        if (m_state) {
            std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
            m_state->ui.cachedDbStats.detailedStatsMatchCount = count;
            m_state->ui.cachedDbStats.detailedStatsSinceDate = std::move(sinceDate);
            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
        }
    },
                       DbJobPriority::Coalescable, "detailed_stats_summary");
}

void DatabaseManager::GetOpponentRecord(const std::string& primaryId, const std::string& opponentId, int& wins, int& losses) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    wins = 0;
    losses = 0;
    if (!m_db) return;

    const char* sql = R"(
        SELECT
            SUM(CASE WHEN Matches.win = 1 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 THEN 1 ELSE 0 END)
        FROM Matches
        WHERE Matches.id IN (
            SELECT DISTINCT match_id FROM MatchPlayers WHERE primary_id = ?
        )
        AND Matches.id IN (
            SELECT DISTINCT match_id FROM MatchPlayers WHERE primary_id = ?
        );
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, opponentId.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        wins = sqlite3_column_int(stmt, 0);
        losses = sqlite3_column_int(stmt, 1);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    wins = 0;
    losses = 0;
    gamesPlayed = 0;
    if (!m_db) return;

    const char* sql = R"(
        SELECT
            SUM(CASE WHEN Matches.win = 1 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 THEN 1 ELSE 0 END),
            COUNT(*)
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ? AND Matches.gamemode = ?;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, gamemode.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        wins = sqlite3_column_int(stmt, 0);
        losses = sqlite3_column_int(stmt, 1);
        gamesPlayed = sqlite3_column_int(stmt, 2);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss) {
    outCurWin = 0;
    outCurLoss = 0;
    outLongestWin = 0;
    outLongestLoss = 0;

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT Matches.win FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?
        ORDER BY Matches.timestamp DESC, Matches.id DESC
        LIMIT 500;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);

    std::vector<int> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.push_back(sqlite3_column_int(stmt, 0));
    }
    sqlite3_finalize(stmt);

    if (results.empty()) return;

    // 1. Calculate Current Win Streak (from latest match backward)
    for (int win : results) {
        if (win == 1) {
            outCurWin++;
        } else {
            break;
        }
    }

    // 2. Calculate Current Loss Streak (from latest match backward)
    for (int win : results) {
        if (win == 0) {
            outCurLoss++;
        } else {
            break;
        }
    }

    // 3. Calculate Longest Win Streak
    int currentWins = 0;
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        int win = *it;
        if (win == 1) {
            currentWins++;
            if (currentWins > outLongestWin) {
                outLongestWin = currentWins;
            }
        } else {
            currentWins = 0;
        }
    }

    // 4. Calculate Longest Loss Streak
    int currentLosses = 0;
    for (auto it = results.rbegin(); it != results.rend(); ++it) {
        int win = *it;
        if (win == 0) {
            currentLosses++;
            if (currentLosses > outLongestLoss) {
                outLongestLoss = currentLosses;
            }
        } else {
            currentLosses = 0;
        }
    }
}

bool DatabaseManager::SetSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return false;

    const char* sql = "INSERT OR REPLACE INTO Settings (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

std::string DatabaseManager::GetSetting(const std::string& key, const std::string& defaultValue) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return defaultValue;

    const char* sql = "SELECT value FROM Settings WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return defaultValue;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    std::string result = defaultValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) {
            result = reinterpret_cast<const char*>(val);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

void DatabaseManager::AsyncSetSetting(std::string key, std::string value) {
    if (!EnqueueDbJob([this, key = std::move(key), value = std::move(value)]() {
            (void)SetSetting(key, value);
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue setting write for " << key << ".\n";
    }
}

bool DatabaseManager::EnqueueDbJob(std::function<void()> job, DbJobPriority priority, std::string coalesceKey) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stopWorker) {
            std::cerr << "[Database] Rejecting job during shutdown.\n";
            return false;
        }

        if (priority == DbJobPriority::Coalescable && !coalesceKey.empty()) {
            if (m_pendingCoalescedJobs.count(coalesceKey)) {
                return true;
            }
            m_pendingCoalescedJobs.insert(coalesceKey);
        }

        if (m_jobs.size() > 100) {
            if (priority != DbJobPriority::Critical) {
                if (priority == DbJobPriority::Coalescable && !coalesceKey.empty()) {
                    m_pendingCoalescedJobs.erase(coalesceKey);
                }
                std::cerr << "[Database] Queue full! Dropping non-critical job.\n";
                return false;
            }
            std::cerr << "[Database] Queue full, preserving critical job.\n";
        }

        m_jobs.push_back({std::move(job), priority, std::move(coalesceKey)});
    }

    m_cv.notify_one();
    return true;
}

void DatabaseManager::AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    std::string pl = playlist;
    {
        std::lock_guard<std::mutex> lk(s_test_mutex);
        s_test_last_get_lifetime_primary = pid;
        s_test_last_get_lifetime_playlist = pl;
    }
    s_test_async_get_lifetime_calls.fetch_add(1);
    (void)EnqueueDbJob([this, pid, pl]() {
        std::vector<float> tempX, tempY;
        GetLifetimeMmrHistory(pid, pl, tempX, tempY);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
            m_state->history.lifetimeMmrX = std::move(tempX);
            m_state->history.lifetimeMmrY = std::move(tempY);
            m_state->history.version++;
        }
    },
                       DbJobPriority::Coalescable, "lifetime:" + pid + "|" + pl);
}

void DatabaseManager::AsyncGetRecentMatchHistory(const std::string& primaryId, int limit) {
    std::string pid = primaryId;
    int rowLimit = std::clamp(limit, 10, kPreviousGamesMaxLimit);
    (void)EnqueueDbJob([this, pid, rowLimit]() {
        std::vector<SessionMatchSummary> matches;
        GetRecentMatchHistory(pid, matches, rowLimit);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
            m_state->history.recentSavedMatches = std::move(matches);
            m_state->history.recentSavedMatchesLoaded = true;
            m_state->history.version++;
        }
    },
                       DbJobPriority::Coalescable, "recent_matches:" + pid + "|" + std::to_string(rowLimit));
}

// Testing instrumentation defaults
std::atomic<int> DatabaseManager::s_test_async_get_lifetime_calls{0};
std::atomic<int> DatabaseManager::s_test_async_refresh_calls{0};
std::mutex DatabaseManager::s_test_mutex;
std::string DatabaseManager::s_test_last_get_lifetime_primary = "";
std::string DatabaseManager::s_test_last_get_lifetime_playlist = "";
std::string DatabaseManager::s_test_last_refresh_primary = "";

void DatabaseManager::RefreshDbStatsSync(const std::string& primaryId) {
    if (primaryId.empty()) return;
    CachedDbStats newStats;
    GetStreakStats(primaryId, newStats.currentWins, newStats.currentLosses, newStats.longestWins, newStats.longestLosses);
    for (const auto& gm : {"1v1", "2v2", "3v3", "hoops", "rumble", "dropshot", "snowday", "heatseeker", "casual", "t"}) {
        GetGamemodeStats(primaryId, gm, newStats.gamemodes[gm].wins, newStats.gamemodes[gm].losses, newStats.gamemodes[gm].total);
    }
    GetDetailedStatsSummary(newStats.detailedStatsMatchCount, newStats.detailedStatsSinceDate);
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
        m_state->ui.cachedDbStats = newStats;
        m_state->ui.dbStatsDirty.store(false);
        m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatabaseManager::AsyncRefreshDbStats(const std::string& primaryId) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    {
        std::lock_guard<std::mutex> lk(s_test_mutex);
        s_test_last_refresh_primary = pid;
    }
    s_test_async_refresh_calls.fetch_add(1);
    (void)EnqueueDbJob([this, pid]() {
        RefreshDbStatsSync(pid);
    },
                       DbJobPriority::Coalescable, "refresh:" + pid);
}

void DatabaseManager::AsyncGetPlayerEncounterRecord(const std::string& primaryId) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    (void)EnqueueDbJob([this, pid]() {
        int winsWith = 0, lossesWith = 0, winsAgainst = 0, lossesAgainst = 0;
        GetPlayerEncounterRecord(pid, winsWith, lossesWith, winsAgainst, lossesAgainst);
        if (winsWith > 0 || lossesWith > 0 || winsAgainst > 0 || lossesAgainst > 0) {
            if (m_state) {
                std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
                if (m_state->game.roster.count(pid)) {
                    m_state->game.roster[pid].lifetimeWinsWith = winsWith;
                    m_state->game.roster[pid].lifetimeLossesWith = lossesWith;
                    m_state->game.roster[pid].lifetimeWinsAgainst = winsAgainst;
                    m_state->game.roster[pid].lifetimeLossesAgainst = lossesAgainst;
                    m_state->game.roster[pid].hasLifetimeData = true;
                    m_state->game.version++;
                }
            }
        }
    },
                       DbJobPriority::Coalescable, "encounter:" + pid);
}

static std::string SerializeMmrChangeJson(const std::map<std::string, int>& changes) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [playlist, delta] : changes) {
        if (!playlist.empty()) obj[playlist] = delta;
    }
    return obj.dump();
}

static std::string SerializeTotalsJson(const SessionRecap& recap) {
    const auto& t = recap.totals;
    nlohmann::json gamemodesObj = nlohmann::json::object();
    for (const auto& [mode, gm] : recap.gamemodes) {
        if (!mode.empty()) {
            gamemodesObj[mode] = {{"wins", gm.wins}, {"losses", gm.losses}, {"total", gm.total}};
        }
    }
    nlohmann::json obj = {
        {"wins", t.wins},
        {"losses", t.losses},
        {"goals", t.goals},
        {"saves", t.saves},
        {"savesTotal", t.savesTotal},
        {"shots", t.shots},
        {"shotsTotal", t.shotsTotal},
        {"demos", t.demos},
        {"demosTotal", t.demosTotal},
        {"demoed", t.demoed},
        {"assists", t.assists},
        {"assistsTotal", t.assistsTotal},
        {"crossbars", t.crossbars},
        {"crossbarsTotal", t.crossbarsTotal},
        {"maxGoalSpeed", t.maxGoalSpeed},
        {"maxGoalSpeedSelf", t.maxGoalSpeedSelf},
        {"maxBallSpeed", t.maxBallSpeed},
        {"maxBallSpeedSelf", t.maxBallSpeedSelf},
        {"maxImpactForce", t.maxImpactForce},
        {"maxImpactForceSelf", t.maxImpactForceSelf},
        {"fastestGoalTime", t.fastestGoalTime},
        {"fastestGoalTimeSelf", t.fastestGoalTimeSelf},
        {"ownGoals", t.ownGoals},
        {"ownGoalsSelf", t.ownGoalsSelf},
        {"boostPickedUp", t.boostPickedUp},
        {"totalMmrChange", t.totalMmrChange},
        {"teamGoals", t.teamGoals},
        {"goalParticipations", t.goalParticipations},
        {"sessionGeneration", recap.sessionGeneration},
        {"gamemodes", std::move(gamemodesObj)},
    };
    return obj.dump();
}

static void ParseSessionRecapJson(const std::string& mmrJsonStr,
                                  const std::string& totalsJsonStr,
                                  SessionRecap& recap) {
    if (!totalsJsonStr.empty()) {
        const nlohmann::json obj = nlohmann::json::parse(totalsJsonStr, nullptr, false);
        if (obj.is_object()) {
            auto& t = recap.totals;
            t.wins = obj.value("wins", t.wins);
            t.losses = obj.value("losses", t.losses);
            t.goals = obj.value("goals", 0);
            t.saves = obj.value("saves", 0);
            t.savesTotal = obj.value("savesTotal", 0);
            t.shots = obj.value("shots", 0);
            t.shotsTotal = obj.value("shotsTotal", 0);
            t.demos = obj.value("demos", 0);
            t.demosTotal = obj.value("demosTotal", 0);
            t.demoed = obj.value("demoed", 0);
            t.assists = obj.value("assists", 0);
            t.assistsTotal = obj.value("assistsTotal", 0);
            t.crossbars = obj.value("crossbars", 0);
            t.crossbarsTotal = obj.value("crossbarsTotal", 0);
            t.maxGoalSpeed = obj.value("maxGoalSpeed", 0.0f);
            t.maxGoalSpeedSelf = obj.value("maxGoalSpeedSelf", 0.0f);
            t.maxBallSpeed = obj.value("maxBallSpeed", 0.0f);
            t.maxBallSpeedSelf = obj.value("maxBallSpeedSelf", 0.0f);
            t.maxImpactForce = obj.value("maxImpactForce", 0.0f);
            t.maxImpactForceSelf = obj.value("maxImpactForceSelf", 0.0f);
            t.fastestGoalTime = obj.value("fastestGoalTime", 0.0f);
            t.fastestGoalTimeSelf = obj.value("fastestGoalTimeSelf", 0.0f);
            t.ownGoals = obj.value("ownGoals", 0);
            t.ownGoalsSelf = obj.value("ownGoalsSelf", 0);
            t.boostPickedUp = obj.value("boostPickedUp", 0);
            t.teamGoals = obj.value("teamGoals", 0);
            t.goalParticipations = obj.value("goalParticipations", 0);
            recap.sessionGeneration = obj.value("sessionGeneration", uint64_t{0});
            if (obj.contains("gamemodes") && obj["gamemodes"].is_object()) {
                for (auto it = obj["gamemodes"].begin(); it != obj["gamemodes"].end(); ++it) {
                    if (it.value().is_object()) {
                        GamemodeStat gm;
                        gm.wins = it.value().value("wins", 0);
                        gm.losses = it.value().value("losses", 0);
                        gm.total = it.value().value("total", gm.wins + gm.losses);
                        recap.gamemodes[it.key()] = gm;
                    }
                }
            }
        }
    }
    if (!mmrJsonStr.empty()) {
        const nlohmann::json mmrObj = nlohmann::json::parse(mmrJsonStr, nullptr, false);
        if (mmrObj.is_object()) {
            for (auto it = mmrObj.begin(); it != mmrObj.end(); ++it) {
                if (it.value().is_number_integer()) {
                    recap.totals.mmrChangeByPlaylist[it.key()] = it.value().get<int>();
                }
            }
        }
    }
    recap.totals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(recap.totals.mmrChangeByPlaylist));
}

void DatabaseManager::LinkSessionMatchesLocked(sqlite3_int64 sessionId,
                                               const std::string& account,
                                               int64_t startedAt,
                                               int64_t endedAt,
                                               const std::vector<std::string>& matchGuids,
                                               int expectedGames,
                                               int64_t& outEarliestMatchTs,
                                               int64_t& outLatestMatchTs) {
    outEarliestMatchTs = 0;
    outLatestMatchTs = 0;
    if (!m_db || !HasSessionsTableLocked() || sessionId <= 0) return;

    if (!matchGuids.empty()) {
        sqlite3_stmt* guidStmt = nullptr;
        const char* guidSql =
            "UPDATE Matches SET session_id = ?1 WHERE match_guid = ?2 AND (session_id IS NULL OR session_id = ?1);";
        if (sqlite3_prepare_v2(m_db, guidSql, -1, &guidStmt, nullptr) == SQLITE_OK) {
            for (const auto& guid : matchGuids) {
                if (guid.empty()) continue;
                sqlite3_bind_int64(guidStmt, 1, sessionId);
                sqlite3_bind_text(guidStmt, 2, guid.c_str(), -1, SQLITE_TRANSIENT);
                (void)sqlite3_step(guidStmt);
                sqlite3_reset(guidStmt);
                sqlite3_clear_bindings(guidStmt);
            }
            sqlite3_finalize(guidStmt);
        }
    }

    if (!account.empty() && endedAt > 0) {
        const int64_t rangeStart = startedAt > 0 ? std::min(startedAt, endedAt)
                                                 : (endedAt - Insights::kSessionGapSeconds);
        const int64_t rangeEnd = std::max(startedAt, endedAt);
        sqlite3_stmt* rangeStmt = nullptr;
        const char* rangeSql = R"(
            UPDATE Matches
            SET session_id = ?1
            WHERE session_id IS NULL
              AND CAST(strftime('%s', timestamp) AS INTEGER) BETWEEN ?2 AND ?3
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers p
                  WHERE p.match_id = Matches.id AND p.primary_id = ?4
              );
        )";
        if (sqlite3_prepare_v2(m_db, rangeSql, -1, &rangeStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(rangeStmt, 1, sessionId);
            sqlite3_bind_int64(rangeStmt, 2, rangeStart);
            sqlite3_bind_int64(rangeStmt, 3, rangeEnd);
            sqlite3_bind_text(rangeStmt, 4, account.c_str(), -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(rangeStmt);
            sqlite3_finalize(rangeStmt);
        }
    }

    int linkedCount = 0;
    {
        sqlite3_stmt* cntStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM Matches WHERE session_id = ?1;", -1, &cntStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cntStmt, 1, sessionId);
            if (sqlite3_step(cntStmt) == SQLITE_ROW) linkedCount = sqlite3_column_int(cntStmt, 0);
            sqlite3_finalize(cntStmt);
        }
    }

    if (linkedCount == 0 && expectedGames > 0 && !account.empty()) {
        std::vector<std::pair<sqlite3_int64, int64_t>> candidates;
        sqlite3_stmt* candStmt = nullptr;
        const char* candSql = R"(
            SELECT Matches.id, CAST(strftime('%s', Matches.timestamp) AS INTEGER)
            FROM Matches
            WHERE Matches.session_id IS NULL
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers p
                  WHERE p.match_id = Matches.id AND p.primary_id = ?1
              )
            ORDER BY Matches.timestamp DESC, Matches.id DESC
            LIMIT ?2;
        )";
        if (sqlite3_prepare_v2(m_db, candSql, -1, &candStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(candStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(candStmt, 2, expectedGames);
            while (sqlite3_step(candStmt) == SQLITE_ROW) {
                candidates.emplace_back(sqlite3_column_int64(candStmt, 0), sqlite3_column_int64(candStmt, 1));
            }
            sqlite3_finalize(candStmt);
        }
        if (!candidates.empty()) {
            sqlite3_stmt* linkStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;", -1, &linkStmt, nullptr) == SQLITE_OK) {
                int64_t prevTs = candidates.front().second;
                for (const auto& [mid, ts] : candidates) {
                    if (prevTs - ts > Insights::kSessionGapSeconds) break;
                    prevTs = ts;
                    sqlite3_bind_int64(linkStmt, 1, sessionId);
                    sqlite3_bind_int64(linkStmt, 2, mid);
                    (void)sqlite3_step(linkStmt);
                    sqlite3_reset(linkStmt);
                    sqlite3_clear_bindings(linkStmt);
                }
                sqlite3_finalize(linkStmt);
            }
        }
    }

    sqlite3_stmt* minMaxStmt = nullptr;
    const char* minMaxSql =
        "SELECT MIN(CAST(strftime('%s', timestamp) AS INTEGER)), MAX(CAST(strftime('%s', timestamp) AS INTEGER)) "
        "FROM Matches WHERE session_id = ?1;";
    if (sqlite3_prepare_v2(m_db, minMaxSql, -1, &minMaxStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(minMaxStmt, 1, sessionId);
        if (sqlite3_step(minMaxStmt) == SQLITE_ROW && sqlite3_column_type(minMaxStmt, 0) != SQLITE_NULL) {
            outEarliestMatchTs = sqlite3_column_int64(minMaxStmt, 0);
            outLatestMatchTs = sqlite3_column_int64(minMaxStmt, 1);
        }
        sqlite3_finalize(minMaxStmt);
    }
}

int64_t DatabaseManager::SaveSessionLocked(const SessionRecap& recap) {
    if (!m_db || !HasSessionsTableLocked()) return 0;
    if (recap.totals.wins + recap.totals.losses <= 0 && !recap.valid) return 0;

    std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
    if (account.empty() && m_state) {
        std::shared_lock<std::shared_mutex> lk(m_state->game.mutex);
        account = m_state->game.myPrimaryId;
    }
    if (account.empty()) {
        account = Config::Read().last_primary_id;
    }

    const std::string source = recap.source.empty() ? "live" : recap.source;
    const auto genKey = std::make_pair(account, recap.sessionGeneration);
    if (source == "live" && recap.id == 0) {
        const auto existingIt = m_sessionIdByGeneration.find(genKey);
        if (existingIt != m_sessionIdByGeneration.end() &&
            existingIt->second.sessionId > 0 &&
            existingIt->second.endedAtUnix == recap.endedAtUnix) {
            SessionRecap withId = recap;
            withId.id = existingIt->second.sessionId;
            withId.account = account;
            UpdateSessionLocked(withId);
            return existingIt->second.sessionId;
        }
    }

    const int64_t endedAt = recap.endedAtUnix > 0 ? recap.endedAtUnix : static_cast<int64_t>(std::time(nullptr));
    int64_t startedAt = recap.startedAtUnix > 0 ? std::min(recap.startedAtUnix, endedAt) : endedAt;
    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return 0;
    }

    const char* insertSql = R"(
        INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, startedAt);
    sqlite3_bind_int64(stmt, 3, endedAt);
    sqlite3_bind_int(stmt, 4, recap.totals.wins);
    sqlite3_bind_int(stmt, 5, recap.totals.losses);
    sqlite3_bind_text(stmt, 6, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, source.c_str(), -1, SQLITE_TRANSIENT);

    const bool inserted = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!inserted) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    const sqlite3_int64 sessionId = sqlite3_last_insert_rowid(m_db);
    int64_t earliestTs = 0;
    int64_t latestTs = 0;
    LinkSessionMatchesLocked(sessionId,
                             account,
                             recap.startedAtUnix,
                             endedAt,
                             recap.matchGuids,
                             recap.totals.wins + recap.totals.losses,
                             earliestTs,
                             latestTs);
    if (recap.startedAtUnix <= 0 && earliestTs > 0) {
        startedAt = std::min(earliestTs, endedAt);
        sqlite3_stmt* updStart = nullptr;
        if (sqlite3_prepare_v2(m_db, "UPDATE Sessions SET started_at = ?1 WHERE id = ?2;", -1, &updStart, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(updStart, 1, startedAt);
            sqlite3_bind_int64(updStart, 2, sessionId);
            (void)sqlite3_step(updStart);
            sqlite3_finalize(updStart);
        }
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    if (source == "live") {
        m_sessionIdByGeneration[genKey] = {sessionId, endedAt};
    }
    return sessionId;
}

bool DatabaseManager::UpdateSessionLocked(const SessionRecap& recap) {
    if (!m_db || !HasSessionsTableLocked()) return false;

    std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
    if (account.empty() && m_state) {
        std::shared_lock<std::shared_mutex> lk(m_state->game.mutex);
        account = m_state->game.myPrimaryId;
    }
    if (account.empty()) {
        account = Config::Read().last_primary_id;
    }

    sqlite3_int64 sessionId = recap.id;
    if (sessionId <= 0) {
        const auto it = m_sessionIdByGeneration.find(std::make_pair(account, recap.sessionGeneration));
        if (it != m_sessionIdByGeneration.end()) {
            sessionId = it->second.sessionId;
        }
    }
    if (sessionId <= 0 && !account.empty()) {
        sqlite3_stmt* findStmt = nullptr;
        const char* findSql =
            "SELECT id FROM Sessions WHERE account = ?1 AND source = 'live' ORDER BY id DESC LIMIT 1;";
        if (sqlite3_prepare_v2(m_db, findSql, -1, &findStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(findStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(findStmt) == SQLITE_ROW) {
                sessionId = sqlite3_column_int64(findStmt, 0);
            }
            sqlite3_finalize(findStmt);
        }
    }
    if (sessionId <= 0) {
        return SaveSessionLocked(recap) > 0;
    }

    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* updateSql = R"(
        UPDATE Sessions
        SET wins = ?1,
            losses = ?2,
            mmr_change_json = ?3,
            totals_json = ?4,
            ended_at = CASE WHEN ?5 > COALESCE(ended_at, 0) THEN ?5 ELSE ended_at END
        WHERE id = ?6;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int(stmt, 1, recap.totals.wins);
    sqlite3_bind_int(stmt, 2, recap.totals.losses);
    sqlite3_bind_text(stmt, 3, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, recap.endedAtUnix);
    sqlite3_bind_int64(stmt, 6, sessionId);

    const bool updated = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!updated) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    int64_t earliestTs = 0;
    int64_t latestTs = 0;
    LinkSessionMatchesLocked(sessionId,
                             account,
                             recap.startedAtUnix,
                             recap.endedAtUnix,
                             recap.matchGuids,
                             recap.totals.wins + recap.totals.losses,
                             earliestTs,
                             latestTs);

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

int64_t DatabaseManager::SaveSession(const SessionRecap& recap) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return SaveSessionLocked(recap);
}

bool DatabaseManager::UpdateSession(const SessionRecap& recap) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return UpdateSessionLocked(recap);
}

void DatabaseManager::AsyncSaveSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        const int64_t id = SaveSession(r);
        if (id > 0 && m_state) {
            std::string pid;
            {
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.loaded) pid = m_state->insights.primaryId;
            }
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.primaryId == pid) {
                    m_state->insights.sessions = std::move(sessions);
                    m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    },
                       DbJobPriority::Critical);
}

void DatabaseManager::AsyncUpdateSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        if (UpdateSession(r) && m_state) {
            std::string pid;
            {
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.loaded) pid = m_state->insights.primaryId;
            }
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.primaryId == pid) {
                    m_state->insights.sessions = std::move(sessions);
                    m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    },
                       DbJobPriority::Critical);
}

bool DatabaseManager::RecomputeBackfillSessionLocked(sqlite3_int64 sessionId, const std::string& account) {
    if (!m_db || !HasSessionsTableLocked() || sessionId <= 0) return false;
    const bool hasStats = HasStatsTablesLocked();
    const char* sql = hasStats ? R"(
        SELECT m.id,
               CAST(strftime('%s', m.timestamp) AS INTEGER),
               COALESCE(m.win, 0),
               COALESCE(m.our_score, 0),
               COALESCE(m.their_score, 0),
               m.gamemode,
               COALESCE(m.player_count, 0),
               m.playlist_id,
               COALESCE(mp.mmr, 0),
               COALESCE(mp.mmr_estimated, 0),
               COALESCE(mps.goals, 0),
               COALESCE(mps.assists, 0),
               COALESCE(mps.saves, 0),
               COALESCE(mps.shots, 0),
               COALESCE(mps.demos, 0),
               COALESCE(mps.max_goal_speed, 0.0),
               COALESCE(mps.fastest_goal_time, 0.0),
               COALESCE(mls.boost_collected, 0),
               COALESCE(mls.demoed, 0),
               COALESCE(mls.crossbars, 0),
               COALESCE(mls.hardest_crossbar, 0.0),
               COALESCE(mls.max_ball_speed, 0.0),
               COALESCE(mls.own_goals, 0)
        FROM Matches m
        LEFT JOIN MatchPlayers mp ON mp.match_id = m.id AND mp.primary_id = ?2
        LEFT JOIN MatchPlayerStats mps ON mps.match_id = m.id AND mps.primary_id = ?2
        LEFT JOIN MatchLocalStats mls ON mls.match_id = m.id
        WHERE m.session_id = ?1
        ORDER BY m.timestamp ASC, m.id ASC;
    )"
                               : R"(
        SELECT m.id,
               CAST(strftime('%s', m.timestamp) AS INTEGER),
               COALESCE(m.win, 0),
               COALESCE(m.our_score, 0),
               COALESCE(m.their_score, 0),
               m.gamemode,
               COALESCE(m.player_count, 0),
               m.playlist_id,
               COALESCE(mp.mmr, 0),
               COALESCE(mp.mmr_estimated, 0),
               0, 0, 0, 0, 0, 0.0, 0.0,
               0, 0, 0, 0.0, 0.0, 0
        FROM Matches m
        LEFT JOIN MatchPlayers mp ON mp.match_id = m.id AND mp.primary_id = ?2
        WHERE m.session_id = ?1
        ORDER BY m.timestamp ASC, m.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, sessionId);
    sqlite3_bind_text(stmt, 2, account.c_str(), -1, SQLITE_TRANSIENT);

    SessionRecap recap;
    recap.valid = true;
    recap.id = sessionId;
    recap.account = account;
    recap.source = "backfill";

    struct PlaylistMmrTrack {
        int firstMmr = 0;
        int lastMmr = 0;
        int count = 0;
        bool moved = false;
    };
    std::map<std::string, PlaylistMmrTrack> mmrTracks;
    int rowCount = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t ts = sqlite3_column_int64(stmt, 1);
        const bool win = sqlite3_column_int(stmt, 2) != 0;
        const int ourScore = sqlite3_column_int(stmt, 3);
        const std::string rawGamemode = SqlColumnText(stmt, 5);
        const int playerCount = sqlite3_column_int(stmt, 6);
        const bool hasPlaylistId = sqlite3_column_type(stmt, 7) != SQLITE_NULL;
        const int playlistId = hasPlaylistId ? sqlite3_column_int(stmt, 7) : -1;
        const int mmr = sqlite3_column_int(stmt, 8);
        const bool mmrEstimated = sqlite3_column_int(stmt, 9) != 0;
        const int goals = sqlite3_column_int(stmt, 10);
        const int assists = sqlite3_column_int(stmt, 11);
        const int saves = sqlite3_column_int(stmt, 12);
        const int shots = sqlite3_column_int(stmt, 13);
        const int demos = sqlite3_column_int(stmt, 14);
        const float maxGoalSpeed = static_cast<float>(sqlite3_column_double(stmt, 15));
        const float fastestGoalTime = static_cast<float>(sqlite3_column_double(stmt, 16));
        const int boostCollected = sqlite3_column_int(stmt, 17);
        const int demoed = sqlite3_column_int(stmt, 18);
        const int crossbars = sqlite3_column_int(stmt, 19);
        const float hardestCrossbar = static_cast<float>(sqlite3_column_double(stmt, 20));
        const float maxBallSpeed = static_cast<float>(sqlite3_column_double(stmt, 21));
        const int ownGoals = sqlite3_column_int(stmt, 22);

        if (rowCount == 0) {
            recap.startedAtUnix = ts;
            recap.endedAtUnix = ts;
        } else {
            recap.startedAtUnix = std::min(recap.startedAtUnix, ts);
            recap.endedAtUnix = std::max(recap.endedAtUnix, ts);
        }
        ++rowCount;

        auto& t = recap.totals;
        if (win)
            t.wins++;
        else
            t.losses++;
        t.goals += goals;
        t.assists += assists;
        t.saves += saves;
        t.shots += shots;
        t.demos += demos;
        t.boostPickedUp += boostCollected;
        t.demoed += demoed;
        t.crossbars += crossbars;
        t.ownGoalsSelf += ownGoals;
        t.maxGoalSpeedSelf = std::max(t.maxGoalSpeedSelf, maxGoalSpeed);
        t.maxGoalSpeed = std::max(t.maxGoalSpeed, maxGoalSpeed);
        t.maxImpactForceSelf = std::max(t.maxImpactForceSelf, hardestCrossbar);
        t.maxImpactForce = std::max(t.maxImpactForce, hardestCrossbar);
        t.maxBallSpeedSelf = std::max(t.maxBallSpeedSelf, maxBallSpeed);
        t.maxBallSpeed = std::max(t.maxBallSpeed, maxBallSpeed);
        if (fastestGoalTime > 0.0f && (t.fastestGoalTimeSelf == 0.0f || fastestGoalTime < t.fastestGoalTimeSelf)) {
            t.fastestGoalTimeSelf = fastestGoalTime;
            t.fastestGoalTime = fastestGoalTime;
        }

        std::string modeKey = rawGamemode;
        std::string mmrKey = rawGamemode;
        if (hasPlaylistId && PlaylistMetadata::IsKnown(playlistId)) {
            modeKey = PlaylistMetadata::StorageMode(playlistId);
            mmrKey = PlaylistMetadata::MmrKey(playlistId);
        } else if (modeKey.empty()) {
            modeKey = GamemodeUtils::InferFromSnapshot(playerCount, playerCount, MmrCategory::Best, "");
            mmrKey = modeKey;
        }

        if (modeKey != "1v1") {
            const int teamGoalsThisMatch = std::max(0, ourScore);
            t.teamGoals += teamGoalsThisMatch;
            t.goalParticipations += std::clamp(goals + assists, 0, teamGoalsThisMatch);
        }

        const bool isCasual = (hasPlaylistId && PlaylistMetadata::IsCasual(playlistId)) || modeKey == "casual";
        if (!isCasual && GamemodeUtils::IsTrackedCompetitiveMode(modeKey)) {
            auto& gm = recap.gamemodes[modeKey];
            if (win)
                gm.wins++;
            else
                gm.losses++;
            gm.total++;

            if (!mmrKey.empty() && mmr > 0 && !mmrEstimated) {
                auto& track = mmrTracks[mmrKey];
                if (track.count == 0) {
                    track.firstMmr = mmr;
                    track.lastMmr = mmr;
                } else {
                    if (mmr != track.firstMmr) track.moved = true;
                    track.lastMmr = mmr;
                }
                track.count++;
            }
        }
    }
    sqlite3_finalize(stmt);
    if (rowCount == 0) return false;

    for (const auto& [playlist, track] : mmrTracks) {
        if (track.count >= 2 && track.moved) {
            recap.totals.mmrChangeByPlaylist[playlist] = track.lastMmr - track.firstMmr;
        }
    }
    recap.totals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(recap.totals.mmrChangeByPlaylist));

    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    sqlite3_stmt* updStmt = nullptr;
    const char* updSql = R"(
        UPDATE Sessions
        SET started_at = ?1,
            ended_at = ?2,
            wins = ?3,
            losses = ?4,
            mmr_change_json = ?5,
            totals_json = ?6
        WHERE id = ?7;
    )";
    if (sqlite3_prepare_v2(m_db, updSql, -1, &updStmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(updStmt, 1, recap.startedAtUnix);
    sqlite3_bind_int64(updStmt, 2, recap.endedAtUnix);
    sqlite3_bind_int(updStmt, 3, recap.totals.wins);
    sqlite3_bind_int(updStmt, 4, recap.totals.losses);
    sqlite3_bind_text(updStmt, 5, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updStmt, 6, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(updStmt, 7, sessionId);
    const bool ok = (sqlite3_step(updStmt) == SQLITE_DONE);
    sqlite3_finalize(updStmt);
    return ok;
}

int DatabaseManager::BackfillSessionsChunkLocked(int chunkSize, bool& outMoreRemaining) {
    outMoreRemaining = false;
    if (!m_db || !HasSessionsTableLocked()) return 0;
    if (chunkSize <= 0) chunkSize = 500;

    std::unordered_set<std::string> activeSessionGuids;
    std::string preferredAccount;
    if (m_state) {
        std::shared_lock<std::shared_mutex> lk(m_state->game.mutex);
        preferredAccount = m_state->game.myPrimaryId;
        if (m_state->game.sessionTotals.wins + m_state->game.sessionTotals.losses > 0) {
            for (const auto& g : m_state->game.sessionMatchGuids) {
                if (!g.empty()) activeSessionGuids.insert(g);
            }
        }
    }
    if (preferredAccount.empty()) {
        preferredAccount = Config::Read().last_primary_id;
    }

    std::unordered_map<std::string, int> playerNonOpponentCounts;
    {
        sqlite3_stmt* freqStmt = nullptr;
        const char* freqSql = R"(
            SELECT primary_id, COUNT(DISTINCT match_id)
            FROM MatchPlayers
            WHERE COALESCE(is_opponent, 0) = 0
              AND primary_id IS NOT NULL
              AND primary_id != ''
              AND primary_id NOT LIKE 'Unknown|%'
            GROUP BY primary_id;
        )";
        if (sqlite3_prepare_v2(m_db, freqSql, -1, &freqStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(freqStmt) == SQLITE_ROW) {
                playerNonOpponentCounts[SqlColumnText(freqStmt, 0)] = sqlite3_column_int(freqStmt, 1);
            }
            sqlite3_finalize(freqStmt);
        }
    }

    struct UnlinkedMatch {
        sqlite3_int64 id = 0;
        int64_t endedAtUnix = 0;
        std::string matchGuid;
        std::string account;
    };
    std::vector<UnlinkedMatch> chunk;
    {
        sqlite3_stmt* mStmt = nullptr;
        const char* mSql = R"(
            SELECT Matches.id, CAST(strftime('%s', Matches.timestamp) AS INTEGER), COALESCE(Matches.match_guid, '')
            FROM Matches
            WHERE Matches.session_id IS NULL
              AND COALESCE(Matches.result_pending, 0) = 0
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers mp
                  WHERE mp.match_id = Matches.id
                    AND COALESCE(mp.is_opponent, 0) = 0
                    AND mp.primary_id IS NOT NULL
                    AND mp.primary_id != ''
                    AND mp.primary_id NOT LIKE 'Unknown|%'
              )
            ORDER BY Matches.timestamp ASC, Matches.id ASC
            LIMIT ?1;
        )";
        if (sqlite3_prepare_v2(m_db, mSql, -1, &mStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(mStmt, 1, chunkSize + 1);
            while (sqlite3_step(mStmt) == SQLITE_ROW) {
                UnlinkedMatch um;
                um.id = sqlite3_column_int64(mStmt, 0);
                um.endedAtUnix = sqlite3_column_int64(mStmt, 1);
                um.matchGuid = SqlColumnText(mStmt, 2);
                if (!um.matchGuid.empty() && activeSessionGuids.count(um.matchGuid) > 0) {
                    continue;
                }
                chunk.push_back(std::move(um));
            }
            sqlite3_finalize(mStmt);
        }
    }

    if (chunk.size() > static_cast<size_t>(chunkSize)) {
        outMoreRemaining = true;
        chunk.resize(static_cast<size_t>(chunkSize));
    }
    if (chunk.empty()) return 0;

    {
        sqlite3_stmt* pStmt = nullptr;
        const char* pSql = R"(
            SELECT primary_id, id
            FROM MatchPlayers
            WHERE match_id = ?1
              AND COALESCE(is_opponent, 0) = 0
              AND primary_id IS NOT NULL
              AND primary_id != ''
              AND primary_id NOT LIKE 'Unknown|%'
            ORDER BY id ASC;
        )";
        if (sqlite3_prepare_v2(m_db, pSql, -1, &pStmt, nullptr) == SQLITE_OK) {
            for (auto& um : chunk) {
                sqlite3_bind_int64(pStmt, 1, um.id);
                std::string bestPid;
                int bestScore = -1;
                while (sqlite3_step(pStmt) == SQLITE_ROW) {
                    const std::string pid = SqlColumnText(pStmt, 0);
                    int score = playerNonOpponentCounts[pid];
                    if (!preferredAccount.empty() && pid == preferredAccount) score += 1'000'000;
                    if (score > bestScore) {
                        bestScore = score;
                        bestPid = pid;
                    }
                }
                sqlite3_reset(pStmt);
                sqlite3_clear_bindings(pStmt);
                um.account = std::move(bestPid);
            }
            sqlite3_finalize(pStmt);
        }
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::map<std::string, std::vector<UnlinkedMatch>> byAccount;
    sqlite3_stmt* liveOverlapStmt = nullptr;
    const char* liveOverlapSql = R"(
        SELECT id FROM Sessions
        WHERE account = ?1 AND source = 'live' AND ?2 BETWEEN started_at AND ended_at
        ORDER BY id DESC LIMIT 1;
    )";
    sqlite3_prepare_v2(m_db, liveOverlapSql, -1, &liveOverlapStmt, nullptr);

    sqlite3_stmt* setMatchSessionStmt = nullptr;
    sqlite3_prepare_v2(m_db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;", -1, &setMatchSessionStmt, nullptr);

    for (auto& um : chunk) {
        if (um.account.empty()) continue;
        sqlite3_int64 overlappingLiveId = 0;
        if (liveOverlapStmt) {
            sqlite3_bind_text(liveOverlapStmt, 1, um.account.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(liveOverlapStmt, 2, um.endedAtUnix);
            if (sqlite3_step(liveOverlapStmt) == SQLITE_ROW) {
                overlappingLiveId = sqlite3_column_int64(liveOverlapStmt, 0);
            }
            sqlite3_reset(liveOverlapStmt);
            sqlite3_clear_bindings(liveOverlapStmt);
        }
        if (overlappingLiveId > 0 && setMatchSessionStmt) {
            sqlite3_bind_int64(setMatchSessionStmt, 1, overlappingLiveId);
            sqlite3_bind_int64(setMatchSessionStmt, 2, um.id);
            (void)sqlite3_step(setMatchSessionStmt);
            sqlite3_reset(setMatchSessionStmt);
            sqlite3_clear_bindings(setMatchSessionStmt);
            continue;
        }
        byAccount[um.account].push_back(std::move(um));
    }
    if (liveOverlapStmt) sqlite3_finalize(liveOverlapStmt);

    int sessionsCreated = 0;
    sqlite3_stmt* prevBackfillStmt = nullptr;
    const char* prevBackfillSql = R"(
        SELECT id, COALESCE(ended_at, 0)
        FROM Sessions
        WHERE account = ?1 AND source = 'backfill' AND COALESCE(ended_at, 0) <= ?2
        ORDER BY ended_at DESC, id DESC
        LIMIT 1;
    )";
    sqlite3_prepare_v2(m_db, prevBackfillSql, -1, &prevBackfillStmt, nullptr);

    sqlite3_stmt* insertSessStmt = nullptr;
    const char* insertSessSql = R"(
        INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
        VALUES (?1, ?2, ?3, 0, 0, '{}', '{}', 'backfill');
    )";
    sqlite3_prepare_v2(m_db, insertSessSql, -1, &insertSessStmt, nullptr);

    for (const auto& [account, matches] : byAccount) {
        std::vector<int64_t> timestamps;
        timestamps.reserve(matches.size());
        for (const auto& m : matches)
            timestamps.push_back(m.endedAtUnix);

        const std::vector<Insights::SessionSlice> slices = Insights::GroupSessionTimestamps(timestamps);
        for (size_t sliceIdx = 0; sliceIdx < slices.size(); ++sliceIdx) {
            const auto& slice = slices[sliceIdx];
            sqlite3_int64 targetSessionId = 0;

            if (sliceIdx == 0 && prevBackfillStmt) {
                sqlite3_bind_text(prevBackfillStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(prevBackfillStmt, 2, slice.startedAtUnix);
                if (sqlite3_step(prevBackfillStmt) == SQLITE_ROW) {
                    const sqlite3_int64 prevId = sqlite3_column_int64(prevBackfillStmt, 0);
                    const int64_t prevEnded = sqlite3_column_int64(prevBackfillStmt, 1);
                    if (prevEnded > 0 && !Insights::StartsNewSession(prevEnded, slice.startedAtUnix, true)) {
                        targetSessionId = prevId;
                    }
                }
                sqlite3_reset(prevBackfillStmt);
                sqlite3_clear_bindings(prevBackfillStmt);
            }

            if (targetSessionId <= 0 && insertSessStmt) {
                sqlite3_bind_text(insertSessStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(insertSessStmt, 2, slice.startedAtUnix);
                sqlite3_bind_int64(insertSessStmt, 3, slice.endedAtUnix);
                if (sqlite3_step(insertSessStmt) == SQLITE_DONE) {
                    targetSessionId = sqlite3_last_insert_rowid(m_db);
                    ++sessionsCreated;
                }
                sqlite3_reset(insertSessStmt);
                sqlite3_clear_bindings(insertSessStmt);
            }

            if (targetSessionId <= 0 || !setMatchSessionStmt) continue;
            for (size_t idx = slice.beginIndex; idx < slice.endIndex; ++idx) {
                sqlite3_bind_int64(setMatchSessionStmt, 1, targetSessionId);
                sqlite3_bind_int64(setMatchSessionStmt, 2, matches[idx].id);
                (void)sqlite3_step(setMatchSessionStmt);
                sqlite3_reset(setMatchSessionStmt);
                sqlite3_clear_bindings(setMatchSessionStmt);
            }
            (void)RecomputeBackfillSessionLocked(targetSessionId, account);
        }
    }

    if (insertSessStmt) sqlite3_finalize(insertSessStmt);
    if (prevBackfillStmt) sqlite3_finalize(prevBackfillStmt);
    if (setMatchSessionStmt) sqlite3_finalize(setMatchSessionStmt);

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    return sessionsCreated;
}

int DatabaseManager::BackfillSessions(int chunkSize) {
    int totalCreated = 0;
    while (true) {
        bool moreRemaining = false;
        {
            std::lock_guard<std::mutex> lock(m_dbMutex);
            totalCreated += BackfillSessionsChunkLocked(chunkSize, moreRemaining);
            if (!moreRemaining) {
                m_backfillPending.store(false, std::memory_order_release);
                break;
            }
        }
    }
    return totalCreated;
}

std::vector<SessionRecap> DatabaseManager::ListSessions(const std::string& account, int offset, int limit) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    std::vector<SessionRecap> out;
    if (account.empty()) return out;

    bool shouldBackfillAccount = false;
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        if (!m_db || !HasSessionsTableLocked()) return out;
        bool liveOrEndedInMemory = false;
        if (m_state) {
            std::shared_lock<std::shared_mutex> lk(m_state->game.mutex);
            liveOrEndedInMemory =
                (m_state->game.sessionTotals.wins + m_state->game.sessionTotals.losses > 0) ||
                m_state->game.lastSessionRecap.valid ||
                m_state->game.inMatch.load();
        }
        if (!liveOrEndedInMemory) {
            sqlite3_stmt* chkStmt = nullptr;
            const char* chkSql = R"(
                SELECT 1 FROM Matches m
                WHERE m.session_id IS NULL
                  AND COALESCE(m.result_pending, 0) = 0
                  AND EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id = m.id AND p.primary_id = ?1)
                LIMIT 1;
            )";
            if (sqlite3_prepare_v2(m_db, chkSql, -1, &chkStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(chkStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                shouldBackfillAccount = (sqlite3_step(chkStmt) == SQLITE_ROW);
                sqlite3_finalize(chkStmt);
            }
        }
    }
    if (shouldBackfillAccount) {
        BackfillSessions(500);
    }

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !HasSessionsTableLocked()) return out;

    const int clampedOffset = std::max(0, offset);
    const int clampedLimit = limit <= 0 ? 50 : limit;

    const char* sql = R"(
        SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0),
               COALESCE(wins, 0), COALESCE(losses, 0),
               COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'),
               COALESCE(source, 'live')
        FROM Sessions
        WHERE account = ?1
        ORDER BY ended_at DESC, id DESC
        LIMIT ?2 OFFSET ?3;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, clampedLimit);
    sqlite3_bind_int(stmt, 3, clampedOffset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionRecap recap;
        recap.valid = true;
        recap.id = sqlite3_column_int64(stmt, 0);
        recap.account = SqlColumnText(stmt, 1);
        recap.mmrOwnerPrimaryId = recap.account;
        recap.startedAtUnix = sqlite3_column_int64(stmt, 2);
        recap.endedAtUnix = sqlite3_column_int64(stmt, 3);
        const int wins = sqlite3_column_int(stmt, 4);
        const int losses = sqlite3_column_int(stmt, 5);
        const std::string mmrJsonStr = SqlColumnText(stmt, 6);
        const std::string totalsJsonStr = SqlColumnText(stmt, 7);
        recap.source = SqlColumnText(stmt, 8);
        ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
        recap.totals.wins = wins;
        recap.totals.losses = losses;
        out.push_back(std::move(recap));
    }
    sqlite3_finalize(stmt);
    return out;
}

SessionRecap DatabaseManager::GetSession(int64_t id) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    SessionRecap recap;
    if (id <= 0) return recap;
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !HasSessionsTableLocked()) return recap;

    const char* sql = R"(
        SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0),
               COALESCE(wins, 0), COALESCE(losses, 0),
               COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'),
               COALESCE(source, 'live')
        FROM Sessions
        WHERE id = ?1
        LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return recap;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        recap.valid = true;
        recap.id = sqlite3_column_int64(stmt, 0);
        recap.account = SqlColumnText(stmt, 1);
        recap.mmrOwnerPrimaryId = recap.account;
        recap.startedAtUnix = sqlite3_column_int64(stmt, 2);
        recap.endedAtUnix = sqlite3_column_int64(stmt, 3);
        const int wins = sqlite3_column_int(stmt, 4);
        const int losses = sqlite3_column_int(stmt, 5);
        const std::string mmrJsonStr = SqlColumnText(stmt, 6);
        const std::string totalsJsonStr = SqlColumnText(stmt, 7);
        recap.source = SqlColumnText(stmt, 8);
        ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
        recap.totals.wins = wins;
        recap.totals.losses = losses;
    }
    sqlite3_finalize(stmt);
    return recap;
}

void DatabaseManager::AsyncLoadInsights(const std::string& primaryId) {
    std::string pid = primaryId;
    (void)EnqueueDbJob([this, pid]() {
        std::vector<PersonRecord> people;
        std::vector<MatchOutcome> outcomes;
        std::vector<MatchMmrContext> mmrContext;
        std::vector<SessionRecap> sessions;
        GetPeopleRecords(pid, people);
        GetMatchOutcomes(pid, outcomes);
        GetMatchMmrContext(pid, mmrContext);
        sessions = ListSessions(pid, 0, 200);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->insights.mutex);
        m_state->insights.primaryId = pid;
        m_state->insights.people = std::move(people);
        m_state->insights.outcomes = std::move(outcomes);
        m_state->insights.mmrContext = std::move(mmrContext);
        m_state->insights.sessions = std::move(sessions);
        m_state->insights.loaded = true;
        m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Coalescable, "insights:" + pid);
}

namespace {
    std::string ToLowerAscii(std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    std::string EscapeLikePattern(std::string_view input) {
        std::string out;
        out.reserve(input.size() + 4);
        for (char c : input) {
            if (c == '%' || c == '_' || c == '\\') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return "%" + out + "%";
    }

    std::string PlatformFromPrimaryId(const std::string& primaryId) {
        const size_t sep = primaryId.find('|');
        if (sep == std::string::npos) return {};
        const std::string raw = primaryId.substr(0, sep);
        const std::string lower = ToLowerAscii(raw);
        if (lower.empty()) return {};
        if (lower == "epic" || lower == "epicgames") return "Epic";
        if (lower == "steam") return "Steam";
        if (lower.rfind("ps", 0) == 0 || lower == "playstation") return "PlayStation";
        if (lower.rfind("xb", 0) == 0) return "Xbox";
        if (lower == "switch" || lower == "nintendo") return "Nintendo";
        if (lower == "unknown" || lower == "bot") return "Bot";
        return raw;
    }

    std::string TierPlaylistKey(int playlistId, const std::string& gamemode) {
        if (PlaylistMetadata::HasAuthoritativeId(playlistId) && PlaylistMetadata::IsKnown(playlistId)) {
            const std::string key = PlaylistMetadata::MmrKey(playlistId);
            if (!key.empty()) return key;
        }
        return gamemode;
    }

    struct BoundSqlParam {
        enum class Kind { Text,
                          Int,
                          Int64 };
        Kind kind = Kind::Text;
        std::string text;
        int intVal = 0;
        int64_t int64Val = 0;
    };

    void BindParams(sqlite3_stmt* stmt, const std::vector<BoundSqlParam>& params) {
        for (size_t i = 0; i < params.size(); ++i) {
            const int idx = static_cast<int>(i + 1);
            const auto& p = params[i];
            switch (p.kind) {
            case BoundSqlParam::Kind::Text:
                sqlite3_bind_text(stmt, idx, p.text.c_str(), -1, SQLITE_TRANSIENT);
                break;
            case BoundSqlParam::Kind::Int:
                sqlite3_bind_int(stmt, idx, p.intVal);
                break;
            case BoundSqlParam::Kind::Int64:
                sqlite3_bind_int64(stmt, idx, p.int64Val);
                break;
            }
        }
    }
}

void DatabaseManager::QueryMatches(const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount) {
    outRows.clear();
    outTotalCount = 0;
    if (query.account.empty()) return;

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    std::vector<BoundSqlParam> params;
    auto addText = [&](std::string val) -> int {
        params.push_back({BoundSqlParam::Kind::Text, std::move(val), 0, 0});
        return static_cast<int>(params.size());
    };
    auto addInt = [&](int val) -> int {
        params.push_back({BoundSqlParam::Kind::Int, {}, val, 0});
        return static_cast<int>(params.size());
    };
    auto addInt64 = [&](int64_t val) -> int {
        params.push_back({BoundSqlParam::Kind::Int64, {}, 0, val});
        return static_cast<int>(params.size());
    };

    (void)addText(query.account); // ?1

    std::string whereSql;
    if (query.playlistLabel.has_value() && !query.playlistLabel->empty()) {
        const std::string lower = ToLowerAscii(*query.playlistLabel);
        if (lower != "all") {
            if (lower == "extra" || lower == "extras") {
                whereSql += " AND m.gamemode IN ('hoops', 'rumble', 'dropshot', 'snowday', 'heatseeker')";
            } else if (lower == "ranked") {
                whereSql += " AND COALESCE(m.gamemode, '') != 'casual'";
            } else if (lower == "casual") {
                whereSql += " AND m.gamemode = 'casual'";
            } else {
                std::string mapped = *query.playlistLabel;
                if (lower == "1v1" || lower == "duel")
                    mapped = "1v1";
                else if (lower == "2v2" || lower == "doubles")
                    mapped = "2v2";
                else if (lower == "3v3" || lower == "standard")
                    mapped = "3v3";
                else if (lower == "hoops")
                    mapped = "hoops";
                else if (lower == "rumble")
                    mapped = "rumble";
                else if (lower == "dropshot")
                    mapped = "dropshot";
                else if (lower == "snow day" || lower == "snowday" || lower == "snow_day")
                    mapped = "snowday";
                else if (lower == "heatseeker")
                    mapped = "heatseeker";
                else if (lower == "tournament" || lower == "tournament match" || lower == "t" || lower == "tourny")
                    mapped = "t";
                const int pMode = addText(std::move(mapped));
                whereSql += " AND m.gamemode = ?" + std::to_string(pMode);
            }
        }
    }

    if (query.ranked.has_value()) {
        if (*query.ranked) {
            whereSql += " AND COALESCE(m.gamemode, '') != 'casual'";
        } else {
            whereSql += " AND m.gamemode = 'casual'";
        }
    }

    if (query.win.has_value()) {
        const int pWin = addInt(*query.win ? 1 : 0);
        whereSql += " AND m.win = ?" + std::to_string(pWin);
    }

    if (query.fromUnix > 0) {
        const int pFrom = addInt64(query.fromUnix);
        whereSql += " AND CAST(strftime('%s', m.timestamp) AS INTEGER) >= ?" + std::to_string(pFrom);
    }

    if (query.toUnix > 0) {
        const int pTo = addInt64(query.toUnix);
        whereSql += " AND CAST(strftime('%s', m.timestamp) AS INTEGER) <= ?" + std::to_string(pTo);
    }

    if (!query.arena.empty() && ToLowerAscii(query.arena) != "all") {
        const int pExact = addText(query.arena);
        const int pLike = addText(EscapeLikePattern(query.arena));
        whereSql += " AND (LOWER(m.arena) = LOWER(?" + std::to_string(pExact) +
                    ") OR m.arena LIKE ?" + std::to_string(pLike) + " ESCAPE '\\')";
    }

    if (!query.withPlayer.empty()) {
        const int pId = addText(query.withPlayer);
        const int pLike = addText(EscapeLikePattern(query.withPlayer));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers wp WHERE wp.match_id = m.id"
                    " AND COALESCE(wp.is_opponent, 0) = 0 AND wp.primary_id != ?1"
                    " AND (wp.primary_id = ?" +
                    std::to_string(pId) +
                    " OR wp.name LIKE ?" + std::to_string(pLike) + " ESCAPE '\\'))";
    }

    if (!query.againstPlayer.empty()) {
        const int pId = addText(query.againstPlayer);
        const int pLike = addText(EscapeLikePattern(query.againstPlayer));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers ap WHERE ap.match_id = m.id"
                    " AND COALESCE(ap.is_opponent, 0) = 1"
                    " AND (ap.primary_id = ?" +
                    std::to_string(pId) +
                    " OR ap.name LIKE ?" + std::to_string(pLike) + " ESCAPE '\\'))";
    }

    if (!query.nameSearch.empty()) {
        const int pLike = addText(EscapeLikePattern(query.nameSearch));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers np WHERE np.match_id = m.id"
                    " AND np.name LIKE ?" +
                    std::to_string(pLike) + " ESCAPE '\\')";
    }

    std::string orderSql;
    switch (query.sort) {
    case MatchQuery::Sort::Oldest:
        orderSql = "ORDER BY timestamp ASC, id ASC";
        break;
    case MatchQuery::Sort::BiggestWin:
        orderSql = "ORDER BY (mmr_delta IS NULL) ASC, mmr_delta DESC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::BiggestLoss:
        orderSql = "ORDER BY (mmr_delta IS NULL) ASC, mmr_delta ASC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::ScoreMargin:
        orderSql = "ORDER BY (our_score - their_score) DESC, ABS(our_score - their_score) DESC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::Newest:
    default:
        orderSql = "ORDER BY timestamp DESC, id DESC";
        break;
    }

    const std::vector<BoundSqlParam> filterParams = params;
    const int limit = query.limit > 0 ? query.limit : 100;
    const int offset = std::max(0, query.offset);
    const int pLimit = addInt(limit);
    const int pOffset = addInt(offset);

    const std::string sql =
        "WITH AccountMatches AS ("
        "    SELECT m.id, m.timestamp, m.arena, m.our_score, m.their_score, m.win,"
        "           m.match_guid, m.playlist_id, m.gamemode, m.player_count,"
        "           COALESCE(me.mmr, 0) AS my_mmr, COALESCE(me.mmr_estimated, 0) AS mmr_estimated,"
        "           LAG(CASE WHEN me.mmr > 0 THEN me.mmr END) OVER ("
        "               PARTITION BY m.gamemode"
        "               ORDER BY m.timestamp ASC, m.id ASC"
        "           ) AS prev_mmr"
        "    FROM MatchPlayers me"
        "    JOIN Matches m ON m.id = me.match_id"
        "    WHERE me.primary_id = ?1 AND COALESCE(m.result_pending, 0) = 0"
        "),"
        "Filtered AS ("
        "    SELECT id, timestamp, arena, our_score, their_score, win, match_guid,"
        "           playlist_id, gamemode, player_count, my_mmr, mmr_estimated,"
        "           CASE WHEN my_mmr > 0 AND prev_mmr > 0 THEN my_mmr - prev_mmr ELSE NULL END AS mmr_delta,"
        "           COUNT(*) OVER () AS total_count"
        "    FROM AccountMatches m"
        "    WHERE 1=1" +
        whereSql +
        "),"
        "PageRows AS ("
        "    SELECT id, timestamp, arena, our_score, their_score, win, match_guid,"
        "           playlist_id, gamemode, player_count, my_mmr, mmr_estimated, mmr_delta, total_count"
        "    FROM Filtered " +
        orderSql +
        "    LIMIT ?" + std::to_string(pLimit) + " OFFSET ?" + std::to_string(pOffset) +
        ") "
        "SELECT id, CAST(strftime('%s', timestamp) AS INTEGER), arena, our_score, their_score, win,"
        "       match_guid, playlist_id, gamemode, player_count, my_mmr, mmr_estimated, mmr_delta, total_count "
        "FROM PageRows;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    BindParams(stmt, params);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchRow row;
        row.matchId = sqlite3_column_int64(stmt, 0);
        row.timestampUnix = sqlite3_column_int64(stmt, 1);
        row.arena = SqlColumnText(stmt, 2);
        row.ourScore = sqlite3_column_int(stmt, 3);
        row.theirScore = sqlite3_column_int(stmt, 4);
        row.win = sqlite3_column_int(stmt, 5) != 0;
        row.matchGuid = SqlColumnText(stmt, 6);
        row.playlistId = sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 7);
        row.gamemode = SqlColumnText(stmt, 8);
        row.playlist = DescribeMatchPlaylist(stmt, 8, 9, 7, row.ranked);
        row.myMmr = sqlite3_column_int(stmt, 10);
        row.mmrEstimated = sqlite3_column_int(stmt, 11) != 0;
        if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
            row.mmrDelta = sqlite3_column_int(stmt, 12);
        }
        outTotalCount = sqlite3_column_int(stmt, 13);
        outRows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (outRows.empty() && offset > 0) {
        const std::string countSql =
            "SELECT COUNT(*) FROM MatchPlayers me "
            "JOIN Matches m ON m.id = me.match_id "
            "WHERE me.primary_id = ?1 AND COALESCE(m.result_pending, 0) = 0" +
            whereSql + ";";
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
            BindParams(countStmt, filterParams);
            if (sqlite3_step(countStmt) == SQLITE_ROW) {
                outTotalCount = sqlite3_column_int(countStmt, 0);
            }
            sqlite3_finalize(countStmt);
        }
    }

    if (!outRows.empty()) {
        const char* tmSql =
            "SELECT name FROM MatchPlayers "
            "WHERE match_id = ?1 AND COALESCE(is_opponent, 0) = 0 AND primary_id != ?2 "
            "ORDER BY id ASC;";
        sqlite3_stmt* tmStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, tmSql, -1, &tmStmt, nullptr) == SQLITE_OK) {
            for (auto& row : outRows) {
                sqlite3_reset(tmStmt);
                sqlite3_clear_bindings(tmStmt);
                sqlite3_bind_int64(tmStmt, 1, row.matchId);
                sqlite3_bind_text(tmStmt, 2, query.account.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(tmStmt) == SQLITE_ROW) {
                    std::string tmName = SqlColumnText(tmStmt, 0);
                    if (!tmName.empty()) row.teammates.push_back(std::move(tmName));
                }
            }
            sqlite3_finalize(tmStmt);
        }
    }
}

void DatabaseManager::AsyncQueryMatches(MatchQuery query, uint64_t requestId, bool append) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId >= m_state->historyView.requestId) {
            m_state->historyView.requestId = requestId;
            m_state->historyView.loading = true;
            m_state->historyView.appendMode = append;
            m_state->historyView.query = query;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, q = std::move(query), requestId, append]() {
        std::vector<MatchRow> rows;
        int total = 0;
        QueryMatches(q, rows, total);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId < m_state->historyView.requestId) {
            return;
        }
        m_state->historyView.requestId = requestId;
        m_state->historyView.query = q;
        m_state->historyView.total = total;
        m_state->historyView.loading = false;
        m_state->historyView.appendMode = append;
        if (append && q.offset > 0) {
            for (auto& row : rows) {
                m_state->historyView.rows.push_back(std::move(row));
            }
        } else {
            m_state->historyView.rows = std::move(rows);
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

bool DatabaseManager::GetMatchDetail(int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    outDetail = {};
    if (matchId <= 0) return false;

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return false;

    const char* matchSql = R"(
        SELECT id, timestamp, CAST(strftime('%s', timestamp) AS INTEGER), arena,
               our_score, their_score, win, match_guid, playlist_id, gamemode, player_count
        FROM Matches
        WHERE id = ?1;
    )";
    sqlite3_stmt* matchStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, matchSql, -1, &matchStmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(matchStmt, 1, matchId);

    std::string rawTimestamp;
    if (sqlite3_step(matchStmt) != SQLITE_ROW) {
        sqlite3_finalize(matchStmt);
        return false;
    }

    outDetail.found = true;
    outDetail.matchId = sqlite3_column_int64(matchStmt, 0);
    rawTimestamp = SqlColumnText(matchStmt, 1);
    outDetail.timestampUnix = sqlite3_column_int64(matchStmt, 2);
    outDetail.arena = SqlColumnText(matchStmt, 3);
    outDetail.ourScore = sqlite3_column_int(matchStmt, 4);
    outDetail.theirScore = sqlite3_column_int(matchStmt, 5);
    outDetail.win = sqlite3_column_int(matchStmt, 6) != 0;
    outDetail.matchGuid = SqlColumnText(matchStmt, 7);
    outDetail.playlistId = sqlite3_column_type(matchStmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int(matchStmt, 8);
    outDetail.gamemode = SqlColumnText(matchStmt, 9);
    outDetail.playlist = DescribeMatchPlaylist(matchStmt, 9, 10, 8, outDetail.ranked);
    sqlite3_finalize(matchStmt);

    const bool hasStatsTables = HasStatsTablesLocked();
    if (hasStatsTables) {
        const char* localSql =
            "SELECT duration_seconds, overtime_seconds FROM MatchLocalStats WHERE match_id = ?1;";
        sqlite3_stmt* localStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, localSql, -1, &localStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(localStmt, 1, matchId);
            if (sqlite3_step(localStmt) == SQLITE_ROW) {
                outDetail.hasLocalStats = true;
                if (sqlite3_column_type(localStmt, 0) != SQLITE_NULL) {
                    outDetail.durationSeconds = static_cast<float>(sqlite3_column_double(localStmt, 0));
                }
                if (sqlite3_column_type(localStmt, 1) != SQLITE_NULL) {
                    outDetail.overtimeSeconds = static_cast<float>(sqlite3_column_double(localStmt, 1));
                }
            }
            sqlite3_finalize(localStmt);
        }
    }

    const std::string tierKey = TierPlaylistKey(outDetail.playlistId, outDetail.gamemode);
    std::string playersSql;
    if (hasStatsTables) {
        playersSql = R"(
            SELECT mp.primary_id, mp.name, COALESCE(mp.team, 0), COALESCE(mp.mmr, 0),
                   COALESCE(mp.mmr_estimated, 0), COALESCE(mp.is_opponent, 0),
                   mps.match_id, mps.score, mps.goals, mps.assists, mps.saves, mps.shots, mps.demos,
                   mps.touches, mps.car_touches, mps.max_goal_speed, mps.fastest_goal_time
            FROM MatchPlayers mp
            LEFT JOIN MatchPlayerStats mps ON mps.match_id = mp.match_id AND mps.primary_id = mp.primary_id
            WHERE mp.match_id = ?1
            ORDER BY COALESCE(mp.is_opponent, 0) ASC, mp.id ASC;
        )";
    } else {
        playersSql = R"(
            SELECT mp.primary_id, mp.name, COALESCE(mp.team, 0), COALESCE(mp.mmr, 0),
                   COALESCE(mp.mmr_estimated, 0), COALESCE(mp.is_opponent, 0)
            FROM MatchPlayers mp
            WHERE mp.match_id = ?1
            ORDER BY COALESCE(mp.is_opponent, 0) ASC, mp.id ASC;
        )";
    }

    sqlite3_stmt* playerStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, playersSql.c_str(), -1, &playerStmt, nullptr) != SQLITE_OK) {
        return true;
    }
    sqlite3_bind_int64(playerStmt, 1, matchId);

    std::vector<MatchDetailPlayer> allPlayers;
    while (sqlite3_step(playerStmt) == SQLITE_ROW) {
        MatchDetailPlayer p;
        p.primaryId = SqlColumnText(playerStmt, 0);
        p.name = SqlColumnText(playerStmt, 1);
        p.platform = PlatformFromPrimaryId(p.primaryId);
        p.team = sqlite3_column_int(playerStmt, 2);
        p.mmr = sqlite3_column_int(playerStmt, 3);
        p.mmrEstimated = sqlite3_column_int(playerStmt, 4) != 0;
        p.isOpponent = sqlite3_column_int(playerStmt, 5) != 0;
        p.tier = MMRFetcher::GetRankTierForPlaylistMmr(tierKey, p.mmr);
        if (hasStatsTables && sqlite3_column_type(playerStmt, 6) != SQLITE_NULL) {
            p.hasStats = true;
            outDetail.hasPlayerStats = true;
            if (sqlite3_column_type(playerStmt, 7) != SQLITE_NULL) p.score = sqlite3_column_int(playerStmt, 7);
            if (sqlite3_column_type(playerStmt, 8) != SQLITE_NULL) p.goals = sqlite3_column_int(playerStmt, 8);
            if (sqlite3_column_type(playerStmt, 9) != SQLITE_NULL) p.assists = sqlite3_column_int(playerStmt, 9);
            if (sqlite3_column_type(playerStmt, 10) != SQLITE_NULL) p.saves = sqlite3_column_int(playerStmt, 10);
            if (sqlite3_column_type(playerStmt, 11) != SQLITE_NULL) p.shots = sqlite3_column_int(playerStmt, 11);
            if (sqlite3_column_type(playerStmt, 12) != SQLITE_NULL) p.demos = sqlite3_column_int(playerStmt, 12);
            if (sqlite3_column_type(playerStmt, 13) != SQLITE_NULL) p.touches = sqlite3_column_int(playerStmt, 13);
            if (sqlite3_column_type(playerStmt, 14) != SQLITE_NULL) p.carTouches = sqlite3_column_int(playerStmt, 14);
            if (sqlite3_column_type(playerStmt, 15) != SQLITE_NULL)
                p.maxGoalSpeed = static_cast<float>(sqlite3_column_double(playerStmt, 15));
            if (sqlite3_column_type(playerStmt, 16) != SQLITE_NULL)
                p.fastestGoalTime = static_cast<float>(sqlite3_column_double(playerStmt, 16));
        }
        allPlayers.push_back(std::move(p));
    }
    sqlite3_finalize(playerStmt);

    auto hasPlayerInMatch = [&](const std::string& pid) {
        if (pid.empty()) return false;
        return std::any_of(allPlayers.begin(), allPlayers.end(), [&](const MatchDetailPlayer& p) {
            return p.primaryId == pid;
        });
    };

    std::string resolvedAccount;
    if (hasPlayerInMatch(accountPrimaryId)) {
        resolvedAccount = accountPrimaryId;
    } else if (m_state) {
        std::string statePid;
        {
            std::shared_lock<std::shared_mutex> gLock(m_state->game.mutex);
            statePid = m_state->game.myPrimaryId;
        }
        if (hasPlayerInMatch(statePid)) {
            resolvedAccount = statePid;
        }
    }
    if (resolvedAccount.empty()) {
        const std::string cfgPid = Config::Read().last_primary_id;
        if (hasPlayerInMatch(cfgPid)) {
            resolvedAccount = cfgPid;
        }
    }
    if (resolvedAccount.empty()) {
        const char* inferSql = R"(
            SELECT mp.primary_id
            FROM MatchPlayers mp
            WHERE mp.match_id = ?1 AND COALESCE(mp.is_opponent, 0) = 0 AND mp.primary_id != ''
            ORDER BY (SELECT COUNT(*) FROM MatchPlayers all_mp WHERE all_mp.primary_id = mp.primary_id) DESC,
                     mp.id ASC
            LIMIT 1;
        )";
        sqlite3_stmt* inferStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, inferSql, -1, &inferStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(inferStmt, 1, matchId);
            if (sqlite3_step(inferStmt) == SQLITE_ROW) {
                resolvedAccount = SqlColumnText(inferStmt, 0);
            }
            sqlite3_finalize(inferStmt);
        }
    }
    outDetail.accountPrimaryId = resolvedAccount;

    sqlite3_stmt* metStmt = nullptr;
    if (!resolvedAccount.empty()) {
        const char* metSql = R"(
            SELECT COUNT(DISTINCT mp1.match_id)
            FROM MatchPlayers mp1
            JOIN MatchPlayers mp2 ON mp2.match_id = mp1.match_id
            JOIN Matches m2 ON m2.id = mp1.match_id
            WHERE mp1.primary_id = ?1
              AND mp2.primary_id = ?2
              AND mp1.match_id != ?3
              AND COALESCE(m2.result_pending, 0) = 0;
        )";
        sqlite3_prepare_v2(m_db, metSql, -1, &metStmt, nullptr);
    }

    for (auto& p : allPlayers) {
        p.isMe = (!resolvedAccount.empty() && p.primaryId == resolvedAccount);
        if (p.isMe) {
            outDetail.mmrEstimated = p.mmrEstimated;
            if (p.mmr > 0) {
                outDetail.mmrAfter = p.mmr;
            }
        } else if (metStmt && !p.primaryId.empty() && p.primaryId.rfind("Unknown|", 0) != 0) {
            sqlite3_reset(metStmt);
            sqlite3_clear_bindings(metStmt);
            sqlite3_bind_text(metStmt, 1, resolvedAccount.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(metStmt, 2, p.primaryId.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(metStmt, 3, matchId);
            if (sqlite3_step(metStmt) == SQLITE_ROW) {
                p.metBeforeCount = sqlite3_column_int(metStmt, 0);
            }
        }

        if (p.isOpponent) {
            outDetail.theirTeam.push_back(std::move(p));
        } else {
            outDetail.ourTeam.push_back(std::move(p));
        }
    }
    if (metStmt) sqlite3_finalize(metStmt);

    if (!resolvedAccount.empty()) {
        const std::string partitionKey = !outDetail.gamemode.empty()
                                             ? outDetail.gamemode
                                             : (outDetail.playlistId >= 0 ? std::to_string(outDetail.playlistId) : "");
        const char* prevSql = R"(
            SELECT me.mmr
            FROM MatchPlayers me
            JOIN Matches m ON m.id = me.match_id
            WHERE me.primary_id = ?1
              AND COALESCE(m.result_pending, 0) = 0
              AND COALESCE(NULLIF(m.gamemode, ''), CAST(m.playlist_id AS TEXT), '') = ?2
              AND me.mmr > 0
              AND (m.timestamp < ?3 OR (m.timestamp = ?3 AND m.id < ?4))
            ORDER BY m.timestamp DESC, m.id DESC
            LIMIT 1;
        )";
        sqlite3_stmt* prevStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, prevSql, -1, &prevStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(prevStmt, 1, resolvedAccount.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(prevStmt, 2, partitionKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(prevStmt, 3, rawTimestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(prevStmt, 4, matchId);
            if (sqlite3_step(prevStmt) == SQLITE_ROW) {
                const int prevMmr = sqlite3_column_int(prevStmt, 0);
                if (prevMmr > 0) {
                    outDetail.mmrBefore = prevMmr;
                    if (outDetail.mmrAfter.has_value()) {
                        outDetail.mmrDelta = *outDetail.mmrAfter - prevMmr;
                    }
                }
            }
            sqlite3_finalize(prevStmt);
        }
    }

    return true;
}

bool DatabaseManager::GetMatchDetailByGuid(const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    outDetail = {};
    if (matchGuid.empty()) return false;

    int64_t matchId = 0;
    {
        std::lock_guard<std::mutex> lock(m_dbMutex);
        if (!m_db) return false;
        const char* sql = "SELECT id FROM Matches WHERE match_guid = ?1 ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, matchGuid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                matchId = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    if (matchId <= 0) return false;
    return GetMatchDetail(matchId, outDetail, accountPrimaryId);
}

void DatabaseManager::AsyncGetMatchDetail(int64_t matchId, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId == 0 || requestId >= m_state->historyView.detailRequestId) {
            if (requestId != 0) m_state->historyView.detailRequestId = requestId;
            m_state->historyView.detailLoading = true;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, matchId, requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetail(matchId, detail, account);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId != 0 && requestId < m_state->historyView.detailRequestId) {
            return;
        }
        if (requestId != 0) m_state->historyView.detailRequestId = requestId;
        m_state->historyView.detailLoading = false;
        if (ok && detail.found) {
            m_state->historyView.detail = std::move(detail);
        } else {
            m_state->historyView.detail.reset();
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncGetMatchDetailByGuid(std::string matchGuid, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId == 0 || requestId >= m_state->historyView.detailRequestId) {
            if (requestId != 0) m_state->historyView.detailRequestId = requestId;
            m_state->historyView.detailLoading = true;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, guid = std::move(matchGuid), requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetailByGuid(guid, detail, account);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId != 0 && requestId < m_state->historyView.detailRequestId) {
            return;
        }
        if (requestId != 0) m_state->historyView.detailRequestId = requestId;
        m_state->historyView.detailLoading = false;
        if (ok && detail.found) {
            m_state->historyView.detail = std::move(detail);
        } else {
            m_state->historyView.detail.reset();
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncSaveMatch(MatchSaveSnapshot snapshot) {
    if (!EnqueueDbJob([this, snap = std::move(snapshot)]() {
            SaveMatch(snap);
            RefreshDbStatsSync(snap.myPrimaryId);
            if (snap.myPrimaryId.empty()) {
                int detailedCount = 0;
                std::string detailedSince;
                GetDetailedStatsSummary(detailedCount, detailedSince);
                if (m_state) {
                    std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
                    m_state->ui.cachedDbStats.detailedStatsMatchCount = detailedCount;
                    m_state->ui.cachedDbStats.detailedStatsSinceDate = std::move(detailedSince);
                    m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ConfigData conf = Config::Read();
            std::string primaryId = snap.myPrimaryId.empty() ? conf.last_primary_id : snap.myPrimaryId;
            std::vector<SessionMatchSummary> matches;
            GetRecentMatchHistory(primaryId, matches, conf.previous_games_limit);
            if (m_state) {
                std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
                m_state->history.recentSavedMatches = std::move(matches);
                m_state->history.recentSavedMatchesLoaded = true;
                m_state->history.version++;
            }
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue match save during shutdown.\n";
    }
}
