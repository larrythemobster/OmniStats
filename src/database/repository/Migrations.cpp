#include "Migrations.hpp"
#include "core/game/ArenaNames.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include <filesystem>
#include <iostream>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace database::repository {

    namespace {

        bool IsInMemoryPath(const std::string& dbPath) {
            if (dbPath.empty() || dbPath == ":memory:") return true;
            return dbPath.rfind("file:", 0) == 0 && dbPath.find("mode=memory") != std::string::npos;
        }

        void BackupDatabaseBeforeMigration(sqlite::Connection& db, const std::string& dbPath, int oldVersion) {
            if (IsInMemoryPath(dbPath)) return;
            std::error_code ec;
            const fs::path srcPath(dbPath);
            if (!fs::exists(srcPath, ec) || !fs::is_regular_file(srcPath, ec) || fs::file_size(srcPath, ec) == 0) {
                return;
            }

            try {
                db.Execute("PRAGMA wal_checkpoint(TRUNCATE);");
            } catch (...) {
            }

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

        struct MigrationStep {
            int version = 0;
            const char* sql = nullptr;
            bool (*apply)(sqlite::Connection& db, std::string& error) = nullptr;
        };

        constexpr const char* kMigrationV1Sql = R"(
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

        bool ApplyMigrationV1(sqlite::Connection& db, std::string& error) {
            try {
                if (!db.TableHasColumn("Matches", "playlist_id")) {
                    db.Execute("ALTER TABLE Matches ADD COLUMN playlist_id INTEGER;");
                }
                if (!db.TableHasColumn("MatchPlayers", "mmr_estimated")) {
                    db.Execute("ALTER TABLE MatchPlayers ADD COLUMN mmr_estimated BOOLEAN DEFAULT 0;");
                }
                if (!db.TableHasColumn("Matches", "result_pending")) {
                    db.Execute("ALTER TABLE Matches ADD COLUMN result_pending BOOLEAN DEFAULT 0;");
                }
                const char* createIndexes = R"(
            CREATE INDEX IF NOT EXISTS idx_matchplayers_primary_id ON MatchPlayers(primary_id);
            CREATE INDEX IF NOT EXISTS idx_matchplayers_match_id ON MatchPlayers(match_id);
            CREATE INDEX IF NOT EXISTS idx_mp_primary_match ON MatchPlayers(primary_id, match_id);
            CREATE INDEX IF NOT EXISTS idx_matches_timestamp ON Matches(timestamp);
            CREATE INDEX IF NOT EXISTS idx_matches_gamemode ON Matches(gamemode);
            CREATE INDEX IF NOT EXISTS idx_matches_playlist_id ON Matches(playlist_id);
        )";
                db.Execute(createIndexes);
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

        constexpr const char* kMigrationV2Sql = R"(
    CREATE INDEX IF NOT EXISTS idx_mp_primary_match ON MatchPlayers(primary_id, match_id);
    CREATE TABLE MatchPlayerStats (
        match_id INTEGER NOT NULL REFERENCES Matches(id) ON DELETE CASCADE,
        primary_id TEXT NOT NULL,
        score INTEGER, goals INTEGER, assists INTEGER, saves INTEGER, shots INTEGER, demos INTEGER,
        touches INTEGER, car_touches INTEGER, max_goal_speed REAL, fastest_goal_time REAL,
        PRIMARY KEY(match_id, primary_id)
    );
    CREATE TABLE MatchLocalStats (
        match_id INTEGER PRIMARY KEY REFERENCES Matches(id) ON DELETE CASCADE,
        boost_collected INTEGER, demoed INTEGER, crossbars INTEGER, hardest_crossbar REAL,
        max_ball_speed REAL, own_goals INTEGER, duration_seconds REAL, overtime_seconds REAL,
        stats_version INTEGER NOT NULL DEFAULT 1
    );
    CREATE INDEX IF NOT EXISTS idx_mps_primary_match ON MatchPlayerStats(primary_id, match_id);
)";

        constexpr const char* kMigrationV3Sql = R"(
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

        bool ApplyMigrationV3(sqlite::Connection& db, std::string& error) {
            try {
                if (!db.TableHasColumn("Matches", "session_id")) {
                    db.Execute("ALTER TABLE Matches ADD COLUMN session_id INTEGER REFERENCES Sessions(id);");
                }
                db.Execute("CREATE INDEX IF NOT EXISTS idx_matches_session ON Matches(session_id);");
                db.Execute("CREATE INDEX IF NOT EXISTS idx_sessions_account_ended ON Sessions(account, ended_at DESC, id DESC);");
                return true;
            } catch (const std::exception& e) {
                error = e.what();
                return false;
            }
        }

        constexpr const char* kMigrationV4Sql = R"(
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

        constexpr const char* kMigrationV5Sql = R"(
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

        constexpr const char* kMigrationV6Sql = R"(
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

        constexpr const char* kMigrationV7Sql = R"(
    CREATE INDEX IF NOT EXISTS idx_matches_match_guid ON Matches(match_guid);
)";

        constexpr const char* kMigrationV8Sql = R"(
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

        constexpr const char* kMigrationV9Sql = R"(
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

        constexpr const char* kMigrationV10Sql = R"(
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

        constexpr const char* kMigrationV11Sql = R"(
    CREATE TABLE IF NOT EXISTS MatchAnalytics (
        match_id INTEGER PRIMARY KEY REFERENCES Matches(id) ON DELETE CASCADE,
        format_version INTEGER NOT NULL,
        data TEXT NOT NULL
    );
)";

        const std::vector<MigrationStep> kMigrations = {
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

    } // namespace

    bool Migrations::HasAnyUserTable(sqlite::Connection& db) {
        if (!db.IsOpen()) return false;
        sqlite::Statement stmt(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1;");
        return stmt.Step();
    }

    bool Migrations::HasStatsTables(sqlite::Connection& db) {
        return db.TableExists("MatchPlayerStats") && db.TableExists("MatchLocalStats");
    }

    bool Migrations::HasSessionsTable(sqlite::Connection& db) {
        return db.TableExists("Sessions") && db.TableHasColumn("Matches", "session_id");
    }

    void Migrations::RepairArenaNames(sqlite::Connection& db) {
        static constexpr const char* kFlag = "arena_names_repaired_v1";
        sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
        bool ok = true;
        bool done = false;

        try {
            sqlite::Statement checkStmt(db, "SELECT 1 FROM Settings WHERE key = ?1;");
            checkStmt.Bind(1, kFlag);
            done = checkStmt.Step();
        } catch (...) {
            ok = false;
        }

        std::vector<std::pair<int64_t, std::string>> updates;
        if (ok && !done) {
            try {
                sqlite::Statement stmt(db, "SELECT id, arena FROM Matches WHERE arena IS NOT NULL;");
                while (stmt.Step()) {
                    const std::string stored = stmt.Get<std::string>(1);
                    std::string repaired = ArenaNames::RepairStoredName(stored);
                    if (repaired != stored) {
                        updates.emplace_back(stmt.Get<int64_t>(0), std::move(repaired));
                    }
                }
            } catch (...) {
                ok = false;
            }
        }

        if (ok && !done) {
            try {
                sqlite::Statement updateStmt(db, "UPDATE Matches SET arena = ?1 WHERE id = ?2;");
                for (const auto& [id, arena] : updates) {
                    updateStmt.Bind(1, arena);
                    updateStmt.Bind(2, id);
                    updateStmt.Execute();
                }
                sqlite::Statement flagStmt(db, "INSERT OR REPLACE INTO Settings (key, value) VALUES (?1, '1');");
                flagStmt.Bind(1, kFlag);
                flagStmt.Execute();
            } catch (...) {
                ok = false;
            }
        }

        if (ok) {
            tx.Commit();
            if (!updates.empty()) {
                std::cout << "[Database] Corrected map names on " << updates.size() << " saved matches.\n";
            }
        } else {
            tx.Rollback();
            std::cerr << "[Database] Map name repair failed.\n";
        }
    }

    bool Migrations::Migrate(sqlite::Connection& db, const std::string& dbPath) {
        const bool hadExistingTables = HasAnyUserTable(db);
        int currentVersion = db.UserVersion();
        const int latestVersion = kMigrations.empty() ? 0 : kMigrations.back().version;

        if (hadExistingTables && currentVersion < latestVersion) {
            BackupDatabaseBeforeMigration(db, dbPath, currentVersion);
        }

        for (const auto& migration : kMigrations) {
            if (migration.version <= currentVersion) continue;

            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            bool stepOk = true;
            std::string stepError;

            if (migration.sql && migration.sql[0] != '\0') {
                try {
                    db.Execute(migration.sql);
                } catch (const std::exception& e) {
                    stepError = e.what();
                    stepOk = false;
                }
            }

            if (stepOk && migration.apply) {
                stepOk = migration.apply(db, stepError);
            }

            if (stepOk) {
                try {
                    db.SetUserVersion(migration.version);
                    tx.Commit();
                    currentVersion = migration.version;
                } catch (const std::exception& e) {
                    tx.Rollback();
                    std::cerr << "[Database] Migration to v" << migration.version << " failed: " << e.what() << "\n";
                    break;
                }
            } else {
                tx.Rollback();
                std::cerr << "[Database] Migration to v" << migration.version << " failed (rolling back): "
                          << stepError << "\n";
                break;
            }
        }

        // Ensure MatchAnalytics exists even if user_version is already >= 11 from another build.
        try {
            db.Execute(kMigrationV11Sql);
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to ensure MatchAnalytics table: " << e.what() << "\n";
        }

        const bool hasV1Tables = db.TableExists("Matches") &&
                                 db.TableExists("MatchPlayers") &&
                                 db.TableExists("Settings");
        if (hasV1Tables) {
            RepairArenaNames(db);
        }

        return hasV1Tables;
    }

} // namespace database::repository
