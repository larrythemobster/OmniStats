#include "DatabaseManager.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/Storage.hpp"
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

// Append new migrations in ascending version order; each runs in its own BEGIN IMMEDIATE transaction.
static const std::vector<Migration> kMigrations = {
    {1, kMigrationV1Sql, ApplyMigrationV1},
    {2, kMigrationV2Sql},
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
    return hasV1Tables;
}

bool DatabaseManager::HasStatsTablesLocked() const {
    return m_hasStatsTables &&
           SqliteTableExists(m_db, "MatchPlayerStats") &&
           SqliteTableExists(m_db, "MatchLocalStats");
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
        sqlite3_bind_null(stmtPlayerStats, 3);
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

    for (const auto& [id, p] : snapshot.roster) {
        bool isOpponent = (p.team != snapshot.myTeam);

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
            ok = false;
            break;
        }
        sqlite3_reset(stmtPlayers);
        sqlite3_clear_bindings(stmtPlayers);
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
               COALESCE(MatchPlayers.mmr_estimated, 0)
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

    const char* matchesSql = "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count FROM Matches ORDER BY timestamp ASC;";
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
    sql += "DELETE FROM MatchPlayers; DELETE FROM Matches; "
           "DELETE FROM sqlite_sequence WHERE name IN ('Matches', 'MatchPlayers'); COMMIT;";
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        error = errMsg ? errMsg : "Failed to delete local history.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

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

void DatabaseManager::AsyncLoadInsights(const std::string& primaryId) {
    std::string pid = primaryId;
    (void)EnqueueDbJob([this, pid]() {
        std::vector<PersonRecord> people;
        std::vector<MatchOutcome> outcomes;
        std::vector<MatchMmrContext> mmrContext;
        GetPeopleRecords(pid, people);
        GetMatchOutcomes(pid, outcomes);
        GetMatchMmrContext(pid, mmrContext);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->insights.mutex);
        m_state->insights.primaryId = pid;
        m_state->insights.people = std::move(people);
        m_state->insights.outcomes = std::move(outcomes);
        m_state->insights.mmrContext = std::move(mmrContext);
        m_state->insights.loaded = true;
        m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Coalescable, "insights:" + pid);
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
