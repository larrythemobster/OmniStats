#include "DatabaseManager.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
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
    const char* createMatches = R"(
        CREATE TABLE IF NOT EXISTS Matches (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
            arena TEXT,
            our_score INTEGER,
            their_score INTEGER,
            win BOOLEAN,
            match_guid TEXT,
            gamemode TEXT,
            player_count INTEGER
        );
    )";

    const char* createPlayers = R"(
        CREATE TABLE IF NOT EXISTS MatchPlayers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id INTEGER,
            primary_id TEXT,
            name TEXT,
            team INTEGER,
            mmr INTEGER,
            is_opponent BOOLEAN DEFAULT 0,
            FOREIGN KEY(match_id) REFERENCES Matches(id)
        );
    )";

    const char* createSettings = R"(
        CREATE TABLE IF NOT EXISTS Settings (
            key TEXT PRIMARY KEY,
            value TEXT
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(m_db, createMatches, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating Matches table: " << errMsg << "\n";
        sqlite3_free(errMsg);
        return false;
    }

    rc = sqlite3_exec(m_db, createPlayers, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating MatchPlayers table: " << errMsg << "\n";
        sqlite3_free(errMsg);
        return false;
    }

    rc = sqlite3_exec(m_db, createSettings, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Error creating Settings table: " << errMsg << "\n";
        sqlite3_free(errMsg);
        return false;
    }

    const char* createIndexes = R"(
        CREATE INDEX IF NOT EXISTS idx_matchplayers_primary_id ON MatchPlayers(primary_id);
        CREATE INDEX IF NOT EXISTS idx_matchplayers_match_id ON MatchPlayers(match_id);
        CREATE INDEX IF NOT EXISTS idx_matches_timestamp ON Matches(timestamp);
        CREATE INDEX IF NOT EXISTS idx_matches_gamemode ON Matches(gamemode);
    )";
    sqlite3_exec(m_db, createIndexes, nullptr, nullptr, nullptr);

    return true;
}

void DatabaseManager::SaveMatch(const MatchSaveSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    if (snapshot.arenaName.empty()) return;

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

    int playerCount = snapshot.maxPlayersSeen > 0 ? snapshot.maxPlayersSeen : static_cast<int>(snapshot.roster.size());
    const std::string arenaKey = !snapshot.arenaAsset.empty() ? snapshot.arenaAsset : snapshot.arenaName;
    std::string gamemode = GamemodeUtils::InferFromSnapshot(
        playerCount,
        static_cast<int>(snapshot.roster.size()),
        snapshot.rosterMmrCategory,
        snapshot.graphMmrCategory,
        arenaKey);

    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        return;
    }

    std::string sqlMatches = "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, gamemode, player_count) VALUES (?, ?, ?, ?, ?, ?, ?);";
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
    sqlite3_bind_text(stmtMatches, 6, gamemode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMatches, 7, playerCount);

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
    std::string sqlPlayers = "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent) VALUES (?, ?, ?, ?, ?, ?);";
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
        if (p.playlists.count(gamemode)) {
            mmrVal = p.playlists.at(gamemode);
        }
        sqlite3_bind_int(stmtPlayers, 5, mmrVal);
        sqlite3_bind_int(stmtPlayers, 6, isOpponent ? 1 : 0);

        if (sqlite3_step(stmtPlayers) != SQLITE_DONE) {
            std::cerr << "Failed to insert player record\n";
            ok = false;
            break;
        }
        sqlite3_reset(stmtPlayers);
        sqlite3_clear_bindings(stmtPlayers);
    }
    sqlite3_finalize(stmtPlayers);

    if (!ok) {
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

void DatabaseManager::GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<float>& outX, std::vector<float>& outY) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    outX.clear();
    outY.clear();

    const char* sql;
    bool filtered = (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" || playlist == "casual" || playlist == "t" ||
                     playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" || playlist == "snowday");

    if (filtered) {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr 
            FROM MatchPlayers 
            JOIN Matches ON MatchPlayers.match_id = Matches.id 
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0 AND Matches.gamemode = ?
            ORDER BY Matches.timestamp ASC;
        )";
    } else {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr 
            FROM MatchPlayers 
            JOIN Matches ON MatchPlayers.match_id = Matches.id 
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0
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
               strftime('%s', Matches.timestamp), COALESCE(MatchPlayers.mmr, 0)
        FROM Matches
        LEFT JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?
        ORDER BY Matches.timestamp DESC, Matches.id DESC
        LIMIT ?;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const std::string gamemode = SqlColumnText(stmt, 3);
        const int playerCount = sqlite3_column_int(stmt, 4);

        SessionMatchSummary summary;
        summary.ranked = gamemode != "casual";
        summary.mode = FormatMatchHistoryMode(gamemode, playerCount);
        summary.ourScore = sqlite3_column_int(stmt, 0);
        summary.theirScore = sqlite3_column_int(stmt, 1);
        summary.mmr = sqlite3_column_int(stmt, 6);
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

    matchesCsv << "id,timestamp,arena,our_score,their_score,win,match_guid,gamemode,player_count\n";
    playersCsv << "match_id,primary_id,name,team,mmr,is_opponent\n";

    const char* matchesSql = "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, gamemode, player_count FROM Matches ORDER BY timestamp ASC;";
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
        match["gamemode"] = SqlColumnText(matchStmt, 7);
        match["player_count"] = sqlite3_column_int(matchStmt, 8);
        match["players"] = nlohmann::json::array();

        matchesCsv << matchId << ","
                   << CsvEscape(match["timestamp"].get<std::string>()) << ","
                   << CsvEscape(match["arena"].get<std::string>()) << ","
                   << match["our_score"].get<int>() << ","
                   << match["their_score"].get<int>() << ","
                   << (match["win"].get<bool>() ? 1 : 0) << ","
                   << CsvEscape(match["match_guid"].get<std::string>()) << ","
                   << CsvEscape(match["gamemode"].get<std::string>()) << ","
                   << match["player_count"].get<int>() << "\n";

        const char* playersSql = "SELECT primary_id, name, team, mmr, is_opponent FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";
        sqlite3_stmt* playerStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, playersSql, -1, &playerStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(playerStmt, 1, matchId);
            while (sqlite3_step(playerStmt) == SQLITE_ROW) {
                nlohmann::json player;
                player["primary_id"] = SqlColumnText(playerStmt, 0);
                player["name"] = SqlColumnText(playerStmt, 1);
                player["team"] = sqlite3_column_int(playerStmt, 2);
                player["mmr"] = sqlite3_column_int(playerStmt, 3);
                player["is_opponent"] = sqlite3_column_int(playerStmt, 4) != 0;
                match["players"].push_back(player);

                playersCsv << matchId << ","
                           << CsvEscape(player["primary_id"].get<std::string>()) << ","
                           << CsvEscape(player["name"].get<std::string>()) << ","
                           << player["team"].get<int>() << ","
                           << player["mmr"].get<int>() << ","
                           << (player["is_opponent"].get<bool>() ? 1 : 0) << "\n";
            }
        }
        if (playerStmt) sqlite3_finalize(playerStmt);

        matches.push_back(match);
    }
    sqlite3_finalize(matchStmt);

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
    const char* sql = R"(
        BEGIN IMMEDIATE;
        DELETE FROM MatchPlayers;
        DELETE FROM Matches;
        DELETE FROM sqlite_sequence WHERE name IN ('Matches', 'MatchPlayers');
        COMMIT;
    )";
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg) != SQLITE_OK) {
        error = errMsg ? errMsg : "Failed to delete local history.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    error.clear();
    return true;
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
        ORDER BY Matches.timestamp DESC
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

std::string DatabaseManager::InferGamemode(int playerCount) {
    return GamemodeUtils::InferFromPlayerCount(playerCount);
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
    for (const auto& gm : {"1v1", "2v2", "3v3", "hoops", "rumble", "dropshot", "snowday", "casual", "t"}) {
        GetGamemodeStats(primaryId, gm, newStats.gamemodes[gm].wins, newStats.gamemodes[gm].losses, newStats.gamemodes[gm].total);
    }
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
        m_state->ui.cachedDbStats = newStats;
        m_state->ui.dbStatsDirty.store(false);
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

void DatabaseManager::AsyncSaveMatch(MatchSaveSnapshot snapshot) {
    if (!EnqueueDbJob([this, snap = std::move(snapshot)]() {
            SaveMatch(snap);
            RefreshDbStatsSync(snap.myPrimaryId);
            ConfigData conf = Config::Read();
            std::string primaryId = conf.last_primary_id.empty() ? snap.myPrimaryId : conf.last_primary_id;
            std::vector<SessionMatchSummary> matches;
            GetRecentMatchHistory(primaryId, matches, conf.previous_games_limit);
            if (m_state) {
                std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
                m_state->history.recentSavedMatches = std::move(matches);
                m_state->history.recentSavedMatchesLoaded = true;
                m_state->history.version++;
            }
            if (m_state) m_state->ui.dbStatsDirty.store(true);
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue match save during shutdown.\n";
    }
}
