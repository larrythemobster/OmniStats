#include <benchmark/benchmark.h>
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include <memory>
#include <sqlite3.h>
#include <filesystem>

static void BM_SingleInserts(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto dbManager = std::make_shared<DatabaseManager>(session);
    std::string db_path = "bench_single.db";
    std::filesystem::remove(db_path);
    (void)dbManager->Initialize(db_path);
    sqlite3* db = dbManager->GetRawDb();

    for (auto _ : state) {
        for (int i = 0; i < 100; ++i) { // 100 for speed in bench runs
            const char* sql = "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, gamemode, player_count) VALUES ('DFH Stadium', 3, 2, 1, 'guid-xyz', '2v2', 4);";
            sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        }
    }
    dbManager.reset();
    std::filesystem::remove(db_path);
}
BENCHMARK(BM_SingleInserts)->Unit(benchmark::kMillisecond);

static void BM_BatchInserts(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto dbManager = std::make_shared<DatabaseManager>(session);
    std::string db_path = "bench_batch.db";
    std::filesystem::remove(db_path);
    (void)dbManager->Initialize(db_path);
    sqlite3* db = dbManager->GetRawDb();

    for (auto _ : state) {
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
        for (int i = 0; i < 1000; ++i) {
            const char* sql = "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, gamemode, player_count) VALUES ('DFH Stadium', 3, 2, 1, 'guid-xyz', '2v2', 4);";
            sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        }
        sqlite3_exec(db, "COMMIT TRANSACTION;", nullptr, nullptr, nullptr);
    }
    dbManager.reset();
    std::filesystem::remove(db_path);
}
BENCHMARK(BM_BatchInserts)->Unit(benchmark::kMillisecond);

static void BM_QueryMmrHistory(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto dbManager = std::make_shared<DatabaseManager>(session);
    std::string db_path = "bench_query.db";
    std::filesystem::remove(db_path);
    (void)dbManager->Initialize(db_path);
    sqlite3* db = dbManager->GetRawDb();

    // Seed the benchmark with representative match rows
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    for (int i = 0; i < 1000; ++i) {
        const char* sqlMatch = "INSERT INTO Matches (id, arena, our_score, their_score, win, match_guid, gamemode, player_count) VALUES (?, 'DFH Stadium', 3, 2, 1, 'guid', '2v2', 4);";
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, sqlMatch, -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, i + 1);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        const char* sqlPlayer = "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent) VALUES (?, 'user123', 'Player', 0, ?, 0);";
        sqlite3_prepare_v2(db, sqlPlayer, -1, &stmt, nullptr);
        sqlite3_bind_int(stmt, 1, i + 1);
        sqlite3_bind_int(stmt, 2, 1000 + i);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, "COMMIT TRANSACTION;", nullptr, nullptr, nullptr);

    std::vector<float> outX, outY;
    for (auto _ : state) {
        dbManager->GetLifetimeMmrHistory("user123", "2v2", outX, outY);
    }
    dbManager.reset();
    std::filesystem::remove(db_path);
}
BENCHMARK(BM_QueryMmrHistory)->Unit(benchmark::kMicrosecond);

static void BM_SaveMatch6PlayersOn10kDb(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto dbManager = std::make_shared<DatabaseManager>(session);
    std::string dbPath = "bench_save_10k.db";
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
    (void)dbManager->Initialize(dbPath);
    sqlite3* db = dbManager->GetRawDb();

    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* matchStmt = nullptr;
    sqlite3_prepare_v2(
        db,
        "INSERT INTO Matches (id, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count) "
        "VALUES (?, 'DFH Stadium', 3, 2, 1, ?, 13, '3v3', 0);",
        -1, &matchStmt, nullptr);
    for (int i = 0; i < 10000; ++i) {
        const std::string guid = "seed-guid-" + std::to_string(i);
        sqlite3_bind_int(matchStmt, 1, i + 1);
        sqlite3_bind_text(matchStmt, 2, guid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(matchStmt);
        sqlite3_reset(matchStmt);
        sqlite3_clear_bindings(matchStmt);
    }
    sqlite3_finalize(matchStmt);
    sqlite3_exec(db, "COMMIT TRANSACTION;", nullptr, nullptr, nullptr);

    int seq = 0;
    for (auto _ : state) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "bench-live-guid-" + std::to_string(seq++);
        snap.playlistId = 13;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.myPrimaryId = "Steam|p0";
        snap.localStats.boostPickedUpSelf = 310;
        snap.localStats.crossbarsSelf = 1;
        snap.localStats.maxImpactForceSelf = 1100.0f;
        snap.localStats.maxBallSpeedSelf = 114.0f;
        for (int p = 0; p < 6; ++p) {
            const std::string pid = "Steam|p" + std::to_string(p);
            snap.roster[pid] = PlayerData{
                .primaryId = pid,
                .name = "Player" + std::to_string(p),
                .team = p < 3 ? 0 : 1,
                .mmr = 1200 + p,
                .goals = p % 2,
                .saves = 1,
                .shots = 2,
                .demos = p == 0 ? 1 : 0,
                .assists = p == 1 ? 1 : 0,
                .maxGoalSpeed = p % 2 ? 102.0f : 0.0f,
                .fastestGoalTime = p % 2 ? 25.0f : 0.0f,
            };
        }
        dbManager->SaveMatch(snap);
    }

    dbManager.reset();
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}
BENCHMARK(BM_SaveMatch6PlayersOn10kDb)->Unit(benchmark::kMillisecond);

static void BM_QueryMatchesFirstPageOn10kDb(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto dbManager = std::make_shared<DatabaseManager>(session);
    std::string dbPath = "bench_query_matches_10k.db";
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
    (void)dbManager->Initialize(dbPath);
    sqlite3* db = dbManager->GetRawDb();

    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    sqlite3_stmt* matchStmt = nullptr;
    sqlite3_stmt* playerStmt = nullptr;
    sqlite3_prepare_v2(
        db,
        "INSERT INTO Matches (id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count) "
        "VALUES (?, datetime(1700000000 + ? * 300, 'unixepoch'), ?, 3, 1, ?, ?, 11, '2v2', 0);",
        -1, &matchStmt, nullptr);
    sqlite3_prepare_v2(
        db,
        "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) "
        "VALUES (?, ?, ?, ?, ?, 0, ?);",
        -1, &playerStmt, nullptr);

    for (int i = 0; i < 10000; ++i) {
        const int matchId = i + 1;
        const std::string guid = "bench-hist-guid-" + std::to_string(i);
        const char* arena = (i % 2 == 0) ? "DFH Stadium" : "Mannfield";
        sqlite3_bind_int(matchStmt, 1, matchId);
        sqlite3_bind_int(matchStmt, 2, i);
        sqlite3_bind_text(matchStmt, 3, arena, -1, SQLITE_STATIC);
        sqlite3_bind_int(matchStmt, 4, (i % 3 != 0) ? 1 : 0);
        sqlite3_bind_text(matchStmt, 5, guid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(matchStmt);
        sqlite3_reset(matchStmt);
        sqlite3_clear_bindings(matchStmt);

        const char* pids[4] = {"Steam|p0", "Steam|tm1", "Steam|opp1", "Steam|opp2"};
        const char* names[4] = {"Player0", "Zylo", "RivalA", "RivalB"};
        for (int p = 0; p < 4; ++p) {
            sqlite3_bind_int(playerStmt, 1, matchId);
            sqlite3_bind_text(playerStmt, 2, pids[p], -1, SQLITE_STATIC);
            sqlite3_bind_text(playerStmt, 3, names[p], -1, SQLITE_STATIC);
            sqlite3_bind_int(playerStmt, 4, p < 2 ? 0 : 1);
            sqlite3_bind_int(playerStmt, 5, 1200 + (i % 200) + p * 5);
            sqlite3_bind_int(playerStmt, 6, p < 2 ? 0 : 1);
            sqlite3_step(playerStmt);
            sqlite3_reset(playerStmt);
            sqlite3_clear_bindings(playerStmt);
        }
    }
    sqlite3_finalize(matchStmt);
    sqlite3_finalize(playerStmt);
    sqlite3_exec(db, "COMMIT TRANSACTION;", nullptr, nullptr, nullptr);

    MatchQuery query;
    query.account = "Steam|p0";
    query.limit = 100;
    query.offset = 0;
    std::vector<MatchRow> rows;
    int totalCount = 0;
    for (auto _ : state) {
        dbManager->QueryMatches(query, rows, totalCount);
        benchmark::DoNotOptimize(rows);
        benchmark::DoNotOptimize(totalCount);
    }

    dbManager.reset();
    std::filesystem::remove(dbPath, ec);
    std::filesystem::remove(dbPath + "-wal", ec);
    std::filesystem::remove(dbPath + "-shm", ec);
}
BENCHMARK(BM_QueryMatchesFirstPageOn10kDb)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
