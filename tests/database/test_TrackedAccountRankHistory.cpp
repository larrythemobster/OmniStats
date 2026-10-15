#include <gtest/gtest.h>
#include <sqlite3.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/accounts/TrackedAccounts.hpp"
#include "core/accounts/AccountManager.hpp"
#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"

namespace {
    constexpr const char* kIdA = "Steam|76561198000000001";
    constexpr const char* kIdB = "Steam|76561198000000002";
    constexpr const char* kIdC = "Epic|0123456789abcdef0123456789abcdef";

    TrackedAccounts::PlaylistRank Rank(const char* playlist, int mmr, int64_t when,
                                       const char* source = "Tracker") {
        TrackedAccounts::PlaylistRank r;
        r.playlistKey = playlist;
        r.mmr = mmr;
        r.rankTier = "Champion II";
        r.matchesPlayed = 20;
        r.source = source;
        r.fetchedAtUnix = when;
        return r;
    }

    bool Add(DatabaseManager& db, const char* id) {
        const auto identity = TrackedAccounts::FromPrimaryId(id);
        return identity && db.RegisterTrackedAccount(
                               TrackedAccounts::MakeRecord(*identity, "test", "test", TrackedAccounts::DiscoverySource::Manual)) == TrackedAccountInsertResult::Inserted;
    }
    int Count(sqlite3* db) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM TrackedAccountRankObservations;", -1, &stmt, nullptr) != SQLITE_OK) return -1;
        int result = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return result;
    }
    void Cleanup(const std::string& base) {
        std::error_code ec;
        std::filesystem::remove(base, ec);
        std::filesystem::remove(base + "-wal", ec);
        std::filesystem::remove(base + "-shm", ec);
        std::filesystem::remove(base + ".bak-v8", ec);
    }
}

TEST(TrackedAccountRankHistory, RecordsOnlyValidSuccessfulSnapshotsWithoutPollingDuplicates) {
    auto db = DatabaseManager(std::make_shared<SessionState>());
    ASSERT_TRUE(db.Initialize(":memory:"));
    EXPECT_EQ(db.GetSchemaVersion(), 9);
    ASSERT_TRUE(Add(db, kIdA));

    const auto base = Rank("2v2", 1210, 1710000000);
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {base}, 1710000000));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1210, 1710000005)}, 1710000005));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1217, 1710000020)}, 1710000020));
    // Clearing the latest snapshot or a failed refresh must not become a new observation.
    auto missing = Rank("1v1", 500, 1710000021);
    missing.mmr = std::nullopt;
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {missing}, 1710000021));
    ASSERT_TRUE(db.UpdateTrackedAccountRefreshAttempt(kIdA, 1710000022));
    EXPECT_EQ(Count(db.GetRawDb()), 2);
    const auto result = db.GetTrackedAccountRankHistory({kIdA}, "2v2");
    ASSERT_EQ(result.size(), 1u);
    ASSERT_EQ(result[0].observations.size(), 2u);
    EXPECT_EQ(result[0].observations[0].mmr, 1210);
    EXPECT_EQ(result[0].observations[1].mmr, 1217);
    EXPECT_EQ(result[0].observations[1].source, "Tracker");
    EXPECT_EQ(result[0].highestObservedMmr, std::optional<int>{1217});
    EXPECT_EQ(result[0].totalObservations, 2);
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1217, 1710030000)}, 1710030000));
    EXPECT_EQ(Count(db.GetRawDb()), 3); // unchanged rank re-confirmed after 6 hours
}

TEST(TrackedAccountRankHistory, FiltersPlaylistsAccountsAndDatesWithoutInventingHistory) {
    DatabaseManager db(std::make_shared<SessionState>());
    ASSERT_TRUE(db.Initialize(":memory:"));
    ASSERT_TRUE(Add(db, kIdA));
    ASSERT_TRUE(Add(db, kIdB));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("1v1", 720, 1710000100), Rank("2v2", 1320, 1710000100)}, 1710000100));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdB, {Rank("2v2", 1555, 1710000200)}, 1710000200));
    auto first = db.GetTrackedAccountRankHistory({kIdA, kIdB, kIdA}, "2v2", 1710000150);
    ASSERT_EQ(first.size(), 2u);
    EXPECT_TRUE(first[0].observations.empty());
    EXPECT_EQ(first[0].highestObservedMmr, std::optional<int>{1320});
    ASSERT_EQ(first[1].observations.size(), 1u);
    EXPECT_EQ(first[1].observations[0].mmr, 1555);
    EXPECT_EQ(db.GetTrackedAccountRankHistory({kIdA}, "1v1")[0].observations.size(), 1u);
    EXPECT_TRUE(db.GetTrackedAccountRankHistory({kIdA}, "not-a-playlist").empty());
    EXPECT_TRUE(db.GetTrackedAccountRankHistory({kIdC}, "2v2")[0].observations.empty());
}

TEST(TrackedAccountRankHistory, DeleteCascadeAndLateObservationDoesNotRegressCurrentRank) {
    DatabaseManager db(std::make_shared<SessionState>());
    ASSERT_TRUE(db.Initialize(":memory:"));
    ASSERT_TRUE(Add(db, kIdA));
    ASSERT_TRUE(Add(db, kIdB));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1500, 1710000500)}, 1710000500));
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1400, 1710000100)}, 1710000100));
    auto latest = db.GetTrackedAccountRanks(kIdA);
    ASSERT_TRUE(latest.at("2v2").mmr);
    EXPECT_EQ(*latest.at("2v2").mmr, 1500);
    auto history = db.GetTrackedAccountRankHistory({kIdA}, "2v2");
    ASSERT_EQ(history[0].observations.size(), 2u);
    EXPECT_EQ(history[0].observations[0].mmr, 1400);
    ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdB, {Rank("2v2", 1590, 1710000600)}, 1710000600));
    ASSERT_TRUE(db.DeleteTrackedAccount(kIdA));
    EXPECT_EQ(Count(db.GetRawDb()), 1);
    ASSERT_TRUE(db.DeleteTrackedAccount(kIdB));
    EXPECT_EQ(Count(db.GetRawDb()), 0);
}

TEST(TrackedAccountRankHistory, DownsamplingPreservesRealEndpointsAndAllTimePeak) {
    DatabaseManager db(std::make_shared<SessionState>());
    ASSERT_TRUE(db.Initialize(":memory:"));
    ASSERT_TRUE(Add(db, kIdA));
    for (int i = 0; i < 750; ++i) {
        // Each successful refresh records a changed, observed MMR value.
        ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("3v3", 800 + i, 1710000000 + i * 400)}, 1710000000 + i * 400));
    }
    const auto result = db.GetTrackedAccountRankHistory({kIdA}, "3v3");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].totalObservations, 750);
    EXPECT_LE(result[0].observations.size(), 242u);
    ASSERT_GE(result[0].observations.size(), 2u);
    EXPECT_EQ(result[0].observations.front().mmr, 800);
    EXPECT_EQ(result[0].observations.back().mmr, 1549);
    EXPECT_EQ(result[0].highestObservedMmr, std::optional<int>{1549});
}

TEST(TrackedAccountRankHistory, VersionEightUpgradeSeedsOnlyActualFetchedSnapshotsAndSurvivesRestart) {
    const std::string path = "test_rank_history_upgrade.db";
    Cleanup(path);
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        ASSERT_TRUE(Add(db, kIdA));
        ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1313, 1710001234)}, 1710001234));
        ASSERT_EQ(sqlite3_exec(db.GetRawDb(),
                               "DROP TABLE TrackedAccountRankObservations; PRAGMA user_version=8;",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        EXPECT_EQ(db.GetSchemaVersion(), 9);
        auto history = db.GetTrackedAccountRankHistory({kIdA}, "2v2");
        ASSERT_EQ(history.size(), 1u);
        ASSERT_EQ(history[0].observations.size(), 1u);
        EXPECT_EQ(history[0].observations[0].mmr, 1313);
        EXPECT_EQ(history[0].observations[0].observedAtUnix, 1710001234);
        EXPECT_EQ(db.GetTrackedAccountRanks(kIdA).at("2v2").mmr, std::optional<int>{1313});
        ASSERT_TRUE(db.SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1320, 1710004000)}, 1710004000));
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        const auto history = db.GetTrackedAccountRankHistory({kIdA}, "2v2");
        ASSERT_EQ(history.size(), 1u);
        ASSERT_EQ(history[0].observations.size(), 2u);
        EXPECT_EQ(history[0].observations[1].mmr, 1320);
    }
    Cleanup(path);
}

TEST(TrackedAccountRankHistory, AsyncAccountHistoryQueryKeepsDataOffTheRenderPath) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    ASSERT_TRUE(Add(*db, kIdA));
    ASSERT_TRUE(Add(*db, kIdB));
    ASSERT_TRUE(db->SaveTrackedAccountRanks(kIdA, {Rank("2v2", 1210, 1710000000)}, 1710000000));
    ASSERT_TRUE(db->SaveTrackedAccountRanks(kIdB, {Rank("2v2", 1490, 1710000100)}, 1710000100));
    auto service = std::make_shared<AccountManagerService>(db, nullptr);
    service->SetClockForTests([] { return 1710000200; });
    service->Start();
    service->FlushForTests();
    service->RequestHistory({kIdA, kIdB}, "2v2", "all");
    service->FlushForTests();
    auto snap = service->GetSnapshot();
    ASSERT_TRUE(snap.history.loaded);
    EXPECT_FALSE(snap.history.loading);
    ASSERT_EQ(snap.history.series.size(), 2u);
    EXPECT_EQ(snap.history.series[0].observations[0].mmr, 1210);
    EXPECT_EQ(snap.history.series[1].observations[0].mmr, 1490);

    service->RequestHistory({kIdA}, "1v1", "all");
    service->FlushForTests();
    snap = service->GetSnapshot();
    ASSERT_TRUE(snap.history.loaded);
    EXPECT_EQ(snap.history.playlistKey, "1v1");
    ASSERT_EQ(snap.history.series.size(), 1u);
    EXPECT_TRUE(snap.history.series[0].observations.empty());
    service->Stop();
}
