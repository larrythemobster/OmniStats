#include <gtest/gtest.h>
#include "database/DatabaseManager.hpp"
#include "core/state/SessionState.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include <sqlite3.h>
#include <memory>
#include <filesystem>
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <nlohmann/json.hpp>

static int QueryIntScalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
    int val = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        val = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return val;
}

static void RemoveTestDbFiles(const std::string& base) {
    std::error_code ec;
    std::filesystem::remove(base, ec);
    std::filesystem::remove(base + "-wal", ec);
    std::filesystem::remove(base + "-shm", ec);
}

class TrackedAccountsDbTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sessionState = std::make_shared<SessionState>();
        dbManager = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(dbManager->Initialize(":memory:"));
    }

    std::shared_ptr<SessionState> sessionState;
    std::shared_ptr<DatabaseManager> dbManager;
};

TEST_F(TrackedAccountsDbTest, FreshDbMigratesToLatestWithTrackedAccountTables) {
    EXPECT_EQ(dbManager->GetSchemaVersion(), 7);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='TrackedAccounts';"), 1);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='TrackedAccountRanks';"), 1);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_tracked_accounts_sort_order_created_at';"), 1);
}

TEST_F(TrackedAccountsDbTest, MigrationFromV4PreservesExistingDataAndCreatesTables) {
    const std::string path = "test_tracked_migration_v4.db";
    const std::string backupPath = path + ".bak-v4";
    RemoveTestDbFiles(path);
    std::error_code ec;
    std::filesystem::remove(backupPath, ec);

    {
        auto db = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db->Initialize(path));

        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "guid_mig_v4";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.myPrimaryId = "Steam|76561198000000001";
        snap.roster["Steam|76561198000000001"] = PlayerData{.primaryId = "Steam|76561198000000001", .name = "Player1", .team = 0, .mmr = 1000};
        db->SaveMatch(snap);

        SessionRecap recap;
        recap.account = "Steam|76561198000000001";
        recap.startedAtUnix = 1700000000;
        recap.endedAtUnix = 1700003600;
        recap.totals.wins = 1;
        recap.totals.losses = 0;
        recap.source = "live";
        const int64_t sessId = db->SaveSession(recap);
        ASSERT_GT(sessId, 0);

        ASSERT_EQ(sqlite3_exec(db->GetRawDb(), "DROP TABLE IF EXISTS TrackedAccountRanks; DROP TABLE IF EXISTS TrackedAccounts; PRAGMA user_version = 4;", nullptr, nullptr, nullptr), SQLITE_OK);
    }

    {
        auto db2 = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db2->Initialize(path));
        EXPECT_EQ(db2->GetSchemaVersion(), 7);
        EXPECT_EQ(QueryIntScalar(db2->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='TrackedAccounts';"), 1);
        EXPECT_EQ(QueryIntScalar(db2->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='TrackedAccountRanks';"), 1);
        EXPECT_EQ(QueryIntScalar(db2->GetRawDb(), "SELECT COUNT(*) FROM Matches WHERE match_guid = 'guid_mig_v4';"), 1);
        EXPECT_EQ(QueryIntScalar(db2->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers WHERE primary_id = 'Steam|76561198000000001';"), 1);
        EXPECT_EQ(QueryIntScalar(db2->GetRawDb(), "SELECT COUNT(*) FROM Sessions WHERE account = 'Steam|76561198000000001';"), 1);
    }

    RemoveTestDbFiles(path);
    std::filesystem::remove(backupPath, ec);
}

TEST_F(TrackedAccountsDbTest, RegisterListGetRenameRemoveAndDuplicateHandling) {
    const auto id1Opt = TrackedAccounts::FromPrimaryId("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(id1Opt.has_value());
    const auto rec1 = TrackedAccounts::MakeRecord(*id1Opt, "MainAcc", "OriginalNick", TrackedAccounts::DiscoverySource::Manual);

    EXPECT_EQ(dbManager->RegisterTrackedAccount(rec1), TrackedAccountInsertResult::Inserted);

    auto dupSamePrimary = rec1;
    dupSamePrimary.nickname = "AttemptedChange";
    EXPECT_EQ(dbManager->RegisterTrackedAccount(dupSamePrimary), TrackedAccountInsertResult::AlreadyExists);

    TrackedAccounts::AccountRecord dupSamePlatformAndAccount;
    dupSamePlatformAndAccount.primaryId = "Epic|DIFFERENT_PRIMARY_ID";
    dupSamePlatformAndAccount.platform = TrackedAccounts::Platform::Epic;
    dupSamePlatformAndAccount.platformAccountId = "0123456789abcdef0123456789abcdef";
    dupSamePlatformAndAccount.nickname = "Attempt2";
    EXPECT_EQ(dbManager->RegisterTrackedAccount(dupSamePlatformAndAccount), TrackedAccountInsertResult::AlreadyExists);

    const auto fetched = dbManager->GetTrackedAccount("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->nickname, "MainAcc");
    EXPECT_EQ(fetched->lastKnownName, "OriginalNick");

    const auto list = dbManager->ListTrackedAccounts();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].primaryId, "Epic|0123456789abcdef0123456789abcdef");

    EXPECT_TRUE(dbManager->UpdateTrackedAccountNickname("Epic|0123456789abcdef0123456789abcdef", "RenamedMain"));
    const auto fetchedAfterRename = dbManager->GetTrackedAccount("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(fetchedAfterRename.has_value());
    EXPECT_EQ(fetchedAfterRename->nickname, "RenamedMain");

    EXPECT_TRUE(dbManager->DeleteTrackedAccount("Epic|0123456789abcdef0123456789abcdef"));
    EXPECT_FALSE(dbManager->GetTrackedAccount("Epic|0123456789abcdef0123456789abcdef").has_value());
    EXPECT_EQ(dbManager->ListTrackedAccounts().size(), 0u);
    EXPECT_FALSE(dbManager->DeleteTrackedAccount("Epic|0123456789abcdef0123456789abcdef"));
}

TEST_F(TrackedAccountsDbTest, RestartPersistenceRoundTripsAllFieldsAndRanks) {
    const std::string path = "test_tracked_restart.db";
    RemoveTestDbFiles(path);

    {
        auto db = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db->Initialize(path));

        const auto idOpt = TrackedAccounts::FromPrimaryId("Steam|76561198000000001");
        ASSERT_TRUE(idOpt.has_value());
        auto rec = TrackedAccounts::MakeRecord(*idOpt, "PersistenceTest", "KnownHero", TrackedAccounts::DiscoverySource::Manual);
        rec.createdAtUnix = 1710000000;
        rec.lastSeenAtUnix = 1710000100;
        rec.sortOrder = 7;

        TrackedAccounts::PlaylistRank r2v2;
        r2v2.playlistKey = "2v2";
        r2v2.mmr = 1350;
        r2v2.rankTier = "Grand Champion I";
        r2v2.matchesPlayed = 120;
        r2v2.source = "live";
        r2v2.fetchedAtUnix = 1710000200;
        rec.ranks["2v2"] = r2v2;

        ASSERT_EQ(db->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);
    }

    {
        auto db2 = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db2->Initialize(path));

        const auto loaded = db2->GetTrackedAccount("Steam|76561198000000001");
        ASSERT_TRUE(loaded.has_value());
        EXPECT_EQ(loaded->primaryId, "Steam|76561198000000001");
        EXPECT_EQ(loaded->platform, TrackedAccounts::Platform::Steam);
        EXPECT_EQ(loaded->platformAccountId, "76561198000000001");
        EXPECT_EQ(loaded->nickname, "PersistenceTest");
        EXPECT_EQ(loaded->lastKnownName, "KnownHero");
        EXPECT_EQ(loaded->createdAtUnix, 1710000000);
        EXPECT_EQ(loaded->lastSeenAtUnix, 1710000100);
        EXPECT_EQ(loaded->sortOrder, 7);
        EXPECT_EQ(loaded->discoverySource, TrackedAccounts::DiscoverySource::Manual);

        ASSERT_EQ(loaded->ranks.size(), 1u);
        ASSERT_TRUE(loaded->ranks.contains("2v2"));
        const auto& r = loaded->ranks.at("2v2");
        EXPECT_EQ(r.playlistKey, "2v2");
        EXPECT_EQ(r.mmr, std::optional<int>(1350));
        EXPECT_EQ(r.rankTier, "Grand Champion I");
        EXPECT_EQ(r.matchesPlayed, std::optional<int>(120));
        EXPECT_EQ(r.source, "live");
        EXPECT_EQ(r.fetchedAtUnix, 1710000200);
    }

    RemoveTestDbFiles(path);
}

TEST_F(TrackedAccountsDbTest, RemovingTrackedAccountPreservesMatchDataAndRemovesRanks) {
    MatchSaveSnapshot snap;
    snap.arenaName = "Mannfield";
    snap.matchGuid = "guid_del_match_test";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 2;
    snap.score[1] = 0;
    snap.myPrimaryId = "Steam|76561198000000001";
    snap.roster["Steam|76561198000000001"] = PlayerData{.primaryId = "Steam|76561198000000001", .name = "Hero", .team = 0, .mmr = 1000};
    dbManager->SaveMatch(snap);

    const auto idOpt = TrackedAccounts::FromPrimaryId("Steam|76561198000000001");
    ASSERT_TRUE(idOpt.has_value());
    auto rec = TrackedAccounts::MakeRecord(*idOpt, "Hero", "Hero", TrackedAccounts::DiscoverySource::Manual);
    TrackedAccounts::PlaylistRank r;
    r.playlistKey = "3v3";
    r.mmr = 1100;
    r.rankTier = "Diamond I";
    rec.ranks["3v3"] = r;

    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM TrackedAccountRanks WHERE primary_id = 'Steam|76561198000000001';"), 1);

    EXPECT_TRUE(dbManager->DeleteTrackedAccount("Steam|76561198000000001"));

    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM TrackedAccountRanks WHERE primary_id = 'Steam|76561198000000001';"), 0);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM Matches WHERE match_guid = 'guid_del_match_test';"), 1);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers WHERE primary_id = 'Steam|76561198000000001';"), 1);
}

TEST_F(TrackedAccountsDbTest, RanksPersistPerPlaylistPartialSaveKeepsOthersAndRoundTripsNullValues) {
    const std::string pid = "Epic|0123456789abcdef0123456789abcdef";
    const auto idOpt = TrackedAccounts::FromPrimaryId(pid);
    ASSERT_TRUE(idOpt.has_value());
    const auto rec = TrackedAccounts::MakeRecord(*idOpt, "RankTester", "RankTester", TrackedAccounts::DiscoverySource::Manual);
    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);

    TrackedAccounts::PlaylistRank r1v1;
    r1v1.playlistKey = "1v1";
    r1v1.mmr = std::nullopt;
    r1v1.rankTier = "";
    r1v1.matchesPlayed = std::nullopt;
    r1v1.source = "";
    r1v1.fetchedAtUnix = 1710000010;

    TrackedAccounts::PlaylistRank r3v3;
    r3v3.playlistKey = "3v3";
    r3v3.mmr = 1200;
    r3v3.rankTier = "Champion I";
    r3v3.matchesPlayed = 50;
    r3v3.source = "live";
    r3v3.fetchedAtUnix = 1710000020;

    EXPECT_TRUE(dbManager->SaveTrackedAccountRanks(pid, {r1v1, r3v3}, 1710000030));

    const auto fetched1 = dbManager->GetTrackedAccountRanks(pid);
    ASSERT_EQ(fetched1.size(), 2u);
    ASSERT_TRUE(fetched1.contains("1v1"));
    ASSERT_TRUE(fetched1.contains("3v3"));

    EXPECT_EQ(fetched1.at("1v1").mmr, std::nullopt);
    EXPECT_EQ(fetched1.at("1v1").rankTier, "");
    EXPECT_EQ(fetched1.at("1v1").matchesPlayed, std::nullopt);
    EXPECT_EQ(fetched1.at("1v1").source, "");
    EXPECT_EQ(fetched1.at("1v1").fetchedAtUnix, 1710000010);

    EXPECT_EQ(fetched1.at("3v3").mmr, std::optional<int>(1200));
    EXPECT_EQ(fetched1.at("3v3").rankTier, "Champion I");
    EXPECT_EQ(fetched1.at("3v3").matchesPlayed, std::optional<int>(50));
    EXPECT_EQ(fetched1.at("3v3").source, "live");
    EXPECT_EQ(fetched1.at("3v3").fetchedAtUnix, 1710000020);

    TrackedAccounts::PlaylistRank r2v2;
    r2v2.playlistKey = "2v2";
    r2v2.mmr = 1300;
    r2v2.rankTier = "Champion II";
    r2v2.matchesPlayed = 75;
    r2v2.source = "tracker";
    r2v2.fetchedAtUnix = 1710000100;

    EXPECT_TRUE(dbManager->SaveTrackedAccountRanks(pid, {r2v2}, 1710000100));

    const auto fetched2 = dbManager->GetTrackedAccountRanks(pid);
    ASSERT_EQ(fetched2.size(), 3u);
    EXPECT_TRUE(fetched2.contains("1v1"));
    EXPECT_TRUE(fetched2.contains("2v2"));
    EXPECT_TRUE(fetched2.contains("3v3"));
    EXPECT_EQ(fetched2.at("2v2").mmr, std::optional<int>(1300));
    EXPECT_EQ(fetched2.at("1v1").mmr, std::nullopt);
    EXPECT_EQ(fetched2.at("3v3").mmr, std::optional<int>(1200));
}

TEST_F(TrackedAccountsDbTest, SaveTrackedAccountRanksAfterDeleteReturnsFalseAndDoesNotRecreateRows) {
    const std::string pid = "Epic|0123456789abcdef0123456789abcdef";
    const auto idOpt = TrackedAccounts::FromPrimaryId(pid);
    ASSERT_TRUE(idOpt.has_value());
    const auto rec = TrackedAccounts::MakeRecord(*idOpt, "Temp", "Temp", TrackedAccounts::DiscoverySource::Manual);
    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);

    EXPECT_TRUE(dbManager->DeleteTrackedAccount(pid));

    TrackedAccounts::PlaylistRank r;
    r.playlistKey = "2v2";
    r.mmr = 1000;
    EXPECT_FALSE(dbManager->SaveTrackedAccountRanks(pid, {r}, 1710000000));

    EXPECT_FALSE(dbManager->GetTrackedAccount(pid).has_value());
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM TrackedAccounts WHERE primary_id = 'Epic|0123456789abcdef0123456789abcdef';"), 0);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM TrackedAccountRanks WHERE primary_id = 'Epic|0123456789abcdef0123456789abcdef';"), 0);
}

TEST_F(TrackedAccountsDbTest, FailedRefreshRetainsPreviousRanksAndLastSuccessfulRefresh) {
    const std::string pid = "Epic|0123456789abcdef0123456789abcdef";
    const auto idOpt = TrackedAccounts::FromPrimaryId(pid);
    ASSERT_TRUE(idOpt.has_value());
    const auto rec = TrackedAccounts::MakeRecord(*idOpt, "RefreshUser", "RefreshUser", TrackedAccounts::DiscoverySource::Manual);
    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);

    TrackedAccounts::PlaylistRank r;
    r.playlistKey = "2v2";
    r.mmr = 1100;
    ASSERT_TRUE(dbManager->SaveTrackedAccountRanks(pid, {r}, 1000));

    const auto initial = dbManager->GetTrackedAccount(pid);
    ASSERT_TRUE(initial.has_value());
    EXPECT_EQ(initial->lastRefreshAttemptAtUnix, 1000);
    EXPECT_EQ(initial->lastSuccessfulRefreshAtUnix, 1000);

    EXPECT_TRUE(dbManager->UpdateTrackedAccountRefreshAttempt(pid, 2000));

    const auto afterFail = dbManager->GetTrackedAccount(pid);
    ASSERT_TRUE(afterFail.has_value());
    EXPECT_EQ(afterFail->lastRefreshAttemptAtUnix, 2000);
    EXPECT_EQ(afterFail->lastSuccessfulRefreshAtUnix, 1000);
    ASSERT_EQ(afterFail->ranks.size(), 1u);
    EXPECT_EQ(afterFail->ranks.at("2v2").mmr, std::optional<int>(1100));
}

TEST_F(TrackedAccountsDbTest, SameNicknameAllowedOnDifferentAccountsAndSameAccountIdOnDifferentPlatforms) {
    const auto epicIdOpt = TrackedAccounts::FromPrimaryId("XboxOne|1234567890");
    ASSERT_TRUE(epicIdOpt.has_value());
    auto epicRec = TrackedAccounts::MakeRecord(*epicIdOpt, "SharedNick", "SharedKnown", TrackedAccounts::DiscoverySource::Manual);

    const auto steamIdOpt = TrackedAccounts::FromPrimaryId("PS4|1234567890");
    ASSERT_TRUE(steamIdOpt.has_value());
    auto steamRec = TrackedAccounts::MakeRecord(*steamIdOpt, "SharedNick", "SharedKnown", TrackedAccounts::DiscoverySource::Manual);

    EXPECT_EQ(dbManager->RegisterTrackedAccount(epicRec), TrackedAccountInsertResult::Inserted);
    EXPECT_EQ(dbManager->RegisterTrackedAccount(steamRec), TrackedAccountInsertResult::Inserted);

    const auto list = dbManager->ListTrackedAccounts();
    EXPECT_EQ(list.size(), 2u);
}

TEST_F(TrackedAccountsDbTest, ImportTrackedAccountsSkipsExistingAndDuplicates) {
    const auto idAOpt = TrackedAccounts::FromPrimaryId("Epic|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    ASSERT_TRUE(idAOpt.has_value());
    const auto recA = TrackedAccounts::MakeRecord(*idAOpt, "AccA", "KnownA", TrackedAccounts::DiscoverySource::Manual);
    ASSERT_EQ(dbManager->RegisterTrackedAccount(recA), TrackedAccountInsertResult::Inserted);

    const auto idBOpt = TrackedAccounts::FromPrimaryId("Epic|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    ASSERT_TRUE(idBOpt.has_value());
    const auto recB = TrackedAccounts::MakeRecord(*idBOpt, "AccB", "KnownB", TrackedAccounts::DiscoverySource::Import);

    auto recBDup = recB;
    recBDup.nickname = "AccB_Duplicate";

    const auto idCOpt = TrackedAccounts::FromPrimaryId("Steam|76561198000000003");
    ASSERT_TRUE(idCOpt.has_value());
    const auto recC = TrackedAccounts::MakeRecord(*idCOpt, "AccC", "KnownC", TrackedAccounts::DiscoverySource::Import);

    TrackedAccounts::AccountRecord recCDupSamePlatformAndAccountId;
    recCDupSamePlatformAndAccountId.primaryId = "Steam|DIFFERENT_PRIMARY_FORMAT";
    recCDupSamePlatformAndAccountId.platform = TrackedAccounts::Platform::Steam;
    recCDupSamePlatformAndAccountId.platformAccountId = "76561198000000003";
    recCDupSamePlatformAndAccountId.nickname = "AccC_Duplicate";

    const std::vector<TrackedAccounts::AccountRecord> importBatch = {
        recA,
        recB,
        recBDup,
        recC,
        recCDupSamePlatformAndAccountId};

    const int inserted = dbManager->ImportTrackedAccounts(importBatch);
    EXPECT_EQ(inserted, 2);

    const auto list = dbManager->ListTrackedAccounts();
    EXPECT_EQ(list.size(), 3u);
}

TEST_F(TrackedAccountsDbTest, UpdateTrackedAccountLastSeenConditionallyUpdatesLastKnownName) {
    const auto idOpt = TrackedAccounts::FromPrimaryId("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(idOpt.has_value());
    const auto rec = TrackedAccounts::MakeRecord(*idOpt, "SeenTest", "InitialName", TrackedAccounts::DiscoverySource::Manual);
    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);

    EXPECT_TRUE(dbManager->UpdateTrackedAccountLastSeen("Epic|0123456789abcdef0123456789abcdef", "UpdatedName", 1700000100));
    auto fetched = dbManager->GetTrackedAccount("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->lastKnownName, "UpdatedName");
    EXPECT_EQ(fetched->lastSeenAtUnix, 1700000100);

    EXPECT_TRUE(dbManager->UpdateTrackedAccountLastSeen("Epic|0123456789abcdef0123456789abcdef", "", 1700000200));
    fetched = dbManager->GetTrackedAccount("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ(fetched->lastKnownName, "UpdatedName");
    EXPECT_EQ(fetched->lastSeenAtUnix, 1700000200);

    EXPECT_FALSE(dbManager->UpdateTrackedAccountLastSeen("Epic|non_existent_account", "Name", 1700000300));
}

TEST_F(TrackedAccountsDbTest, ExportLocalDataWritesTrackedAccountsJson) {
    const auto idOpt = TrackedAccounts::FromPrimaryId("Epic|0123456789abcdef0123456789abcdef");
    ASSERT_TRUE(idOpt.has_value());
    auto rec = TrackedAccounts::MakeRecord(*idOpt, "ExportNick", "ExportKnown", TrackedAccounts::DiscoverySource::Manual);
    rec.createdAtUnix = 1700000000;
    rec.lastSeenAtUnix = 1700000100;
    rec.sortOrder = 1;

    TrackedAccounts::PlaylistRank r;
    r.playlistKey = "2v2";
    r.mmr = 1250;
    r.rankTier = "Champion II";
    r.matchesPlayed = 40;
    r.source = "live";
    r.fetchedAtUnix = 1700000200;
    rec.ranks["2v2"] = r;

    ASSERT_EQ(dbManager->RegisterTrackedAccount(rec), TrackedAccountInsertResult::Inserted);

    std::string exportPath;
    std::string exportError;
    ASSERT_TRUE(dbManager->ExportLocalData(exportPath, exportError)) << exportError;

    const std::filesystem::path jsonPath = std::filesystem::path(exportPath) / "tracked_accounts.json";
    ASSERT_TRUE(std::filesystem::exists(jsonPath));

    std::ifstream f(jsonPath);
    ASSERT_TRUE(f.is_open());
    nlohmann::json parsed = nlohmann::json::parse(f, nullptr, false);
    ASSERT_TRUE(parsed.is_array());
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0]["primary_id"], "Epic|0123456789abcdef0123456789abcdef");
    EXPECT_EQ(parsed[0]["nickname"], "ExportNick");
    EXPECT_EQ(parsed[0]["platform"], "epic");
    EXPECT_EQ(parsed[0]["ranks"].size(), 1u);
    EXPECT_EQ(parsed[0]["ranks"][0]["playlist_key"], "2v2");
    EXPECT_EQ(parsed[0]["ranks"][0]["mmr"], 1250);

    std::error_code ec;
    std::filesystem::remove_all(exportPath, ec);
}
