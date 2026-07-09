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
    dbManager->Initialize(db_path);
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
    dbManager->Initialize(db_path);
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
    dbManager->Initialize(db_path);
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

BENCHMARK_MAIN();
