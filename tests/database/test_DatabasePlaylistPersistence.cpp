#include <gtest/gtest.h>
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include <chrono>
#include <filesystem>
#include <memory>

namespace {
    MatchSaveSnapshot MakeSnapshot(const std::string& guid,
                                   const std::string& primaryId,
                                   int playlistId,
                                   int legacyPlayerCount = 4) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.arenaAsset = "Stadium_P";
        snap.matchGuid = guid;
        snap.playlistId = playlistId;
        snap.gamemode = "intentionally-wrong-derived-value";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.legacyPlayerCount = legacyPlayerCount;
        snap.myPrimaryId = primaryId;
        return snap;
    }

    void AddPlayer(MatchSaveSnapshot& snap,
                   const std::string& id,
                   int team,
                   int bestMmr,
                   std::map<std::string, int> playlists = {}) {
        snap.roster[id] = PlayerData{
            .primaryId = id,
            .name = id,
            .team = team,
            .mmr = bestMmr,
            .playlists = std::move(playlists)};
    }

    std::string ScalarText(sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return {};
        std::string value;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) value = reinterpret_cast<const char*>(text);
        }
        sqlite3_finalize(stmt);
        return value;
    }

    int ScalarInt(sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -999999;
        int value = -999999;
        if (sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }
} // namespace

class DatabasePlaylistPersistenceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        state = std::make_shared<SessionState>();
        db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
    }

    std::shared_ptr<SessionState> state;
    std::shared_ptr<DatabaseManager> db;
};

TEST_F(DatabasePlaylistPersistenceTest, CasualExtraModePersistsPlaylistTruthAndCasualMmr) {
    const std::string primaryId = "Steam|casual-hoops";
    auto snap = MakeSnapshot("casual-hoops-guid", primaryId, 17, 3);
    AddPlayer(snap, primaryId, 0, 1500, {{"casual", 812}, {"hoops", 1337}});
    AddPlayer(snap, "Steam|opponent", 1, 1000);

    db->SaveMatch(snap);

    EXPECT_EQ(ScalarInt(db->GetRawDb(), "SELECT playlist_id FROM Matches LIMIT 1;"), 17);
    EXPECT_EQ(ScalarText(db->GetRawDb(), "SELECT gamemode FROM Matches LIMIT 1;"), "casual");

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(primaryId, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_FALSE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Casual");
    EXPECT_EQ(matches[0].mmr, 812);
}

TEST_F(DatabasePlaylistPersistenceTest, RankedExtraModeUsesItsOwnMmrBucket) {
    const std::string primaryId = "Steam|ranked-hoops";
    auto snap = MakeSnapshot("ranked-hoops-guid", primaryId, 27, 4);
    AddPlayer(snap, primaryId, 0, 1500, {{"casual", 812}, {"hoops", 1337}});
    AddPlayer(snap, "Steam|mate", 0, 1000);
    AddPlayer(snap, "Steam|opponent1", 1, 1000);
    AddPlayer(snap, "Steam|opponent2", 1, 1000);

    db->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(primaryId, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Hoops");
    EXPECT_EQ(matches[0].mmr, 1337);
}

TEST_F(DatabasePlaylistPersistenceTest, AuthoritativeCasualPlaylistDoesNotPersistObservedPlayerCountAsTruth) {
    const std::string primaryId = "Steam|casual-standard";
    // Even if only two players were observed, playlist 3 is authoritative.
    // The legacy player_count column must not become match truth for new rows.
    auto snap = MakeSnapshot("casual-standard-guid", primaryId, 3, 2);
    AddPlayer(snap, primaryId, 0, 900, {{"casual", 900}});
    AddPlayer(snap, "Steam|opponent", 1, 900);

    db->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(primaryId, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_FALSE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Casual");
    EXPECT_EQ(ScalarInt(db->GetRawDb(), "SELECT player_count FROM Matches LIMIT 1;"), 0);
}

TEST_F(DatabasePlaylistPersistenceTest, UnknownAuthoritativePlaylistIsNotReinferred) {
    const std::string primaryId = "Steam|unknown-playlist";
    auto snap = MakeSnapshot("unknown-playlist-guid", primaryId, 999, 4);
    snap.rosterMmrCategory = MmrCategory::TwoVTwo;
    AddPlayer(snap, primaryId, 0, 1000);
    AddPlayer(snap, "Steam|mate", 0, 1000);
    AddPlayer(snap, "Steam|opponent1", 1, 1000);
    AddPlayer(snap, "Steam|opponent2", 1, 1000);

    db->SaveMatch(snap);

    EXPECT_EQ(ScalarText(db->GetRawDb(), "SELECT gamemode FROM Matches LIMIT 1;"), "unknown");
    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(primaryId, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_FALSE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Unknown Playlist (999)");
}

TEST_F(DatabasePlaylistPersistenceTest, HeatseekerLifetimeHistoryFiltersInsteadOfMixingModes) {
    const std::string primaryId = "Steam|heatseeker-history";

    auto doubles = MakeSnapshot("doubles-history-guid", primaryId, 11, 4);
    AddPlayer(doubles, primaryId, 0, 1000, {{"2v2", 1000}});
    AddPlayer(doubles, "Steam|doubles-other", 1, 1000);
    db->SaveMatch(doubles);

    auto heatseeker = MakeSnapshot("heatseeker-history-guid", primaryId, 43, 4);
    AddPlayer(heatseeker, primaryId, 0, 1200, {{"heatseeker", 1200}});
    AddPlayer(heatseeker, "Steam|heat-other", 1, 1000);
    db->SaveMatch(heatseeker);

    std::vector<float> x;
    std::vector<float> y;
    db->GetLifetimeMmrHistory(primaryId, "heatseeker", x, y);
    ASSERT_EQ(y.size(), 1u);
    EXPECT_FLOAT_EQ(y[0], 1200.0f);
}

TEST(DatabasePlaylistMigrationTest, AddsPlaylistIdWithoutBreakingLegacyRows) {
    namespace fs = std::filesystem;
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path path = fs::temp_directory_path() /
                          ("omnistats_playlist_migration_" + unique + ".db");

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, R"(
        CREATE TABLE Matches (
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
        CREATE TABLE MatchPlayers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id INTEGER,
            primary_id TEXT,
            name TEXT,
            team INTEGER,
            mmr INTEGER,
            is_opponent BOOLEAN DEFAULT 0
        );
        INSERT INTO Matches
            (arena, our_score, their_score, win, match_guid, gamemode, player_count)
        VALUES ('DFH Stadium', 2, 1, 1, 'legacy-guid', 'casual', 4);
        INSERT INTO MatchPlayers
            (match_id, primary_id, name, team, mmr, is_opponent)
        VALUES (1, 'Steam|legacy', 'Legacy', 0, 777, 0);
    )",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    {
        auto state = std::make_shared<SessionState>();
        auto db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(path.string()));

        bool foundPlaylistColumn = false;
        bool foundMmrEstimatedColumn = false;
        sqlite3_stmt* pragma = nullptr;
        ASSERT_EQ(sqlite3_prepare_v2(db->GetRawDb(), "PRAGMA table_info(Matches);", -1, &pragma, nullptr), SQLITE_OK);
        while (sqlite3_step(pragma) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(pragma, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == "playlist_id") {
                foundPlaylistColumn = true;
                break;
            }
        }
        sqlite3_finalize(pragma);
        ASSERT_EQ(sqlite3_prepare_v2(db->GetRawDb(), "PRAGMA table_info(MatchPlayers);", -1, &pragma, nullptr), SQLITE_OK);
        while (sqlite3_step(pragma) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(pragma, 1);
            if (name && std::string(reinterpret_cast<const char*>(name)) == "mmr_estimated") {
                foundMmrEstimatedColumn = true;
                break;
            }
        }
        sqlite3_finalize(pragma);
        EXPECT_TRUE(foundPlaylistColumn);
        EXPECT_TRUE(foundMmrEstimatedColumn);
        EXPECT_EQ(ScalarInt(db->GetRawDb(), "SELECT playlist_id IS NULL FROM Matches WHERE match_guid='legacy-guid';"), 1);
        EXPECT_EQ(ScalarInt(db->GetRawDb(), "SELECT mmr_estimated FROM MatchPlayers WHERE primary_id='Steam|legacy';"), 0);

        std::vector<SessionMatchSummary> matches;
        db->GetRecentMatchHistory("Steam|legacy", matches, 10);
        ASSERT_EQ(matches.size(), 1u);
        EXPECT_FALSE(matches[0].ranked);
        EXPECT_EQ(matches[0].mode, "Doubles");
        EXPECT_EQ(matches[0].mmr, 777);
    }

    std::error_code ec;
    fs::remove(path, ec);
}

TEST_F(DatabasePlaylistPersistenceTest, EstimatedPostMatchMmrIsLabeledAndExcludedFromLifetimeUntilExact) {
    const std::string primaryId = "Steam|estimated-history";
    auto snap = MakeSnapshot("estimated-history-guid", primaryId, 11, 4);
    snap.localMmrNeedsReconciliation = true;
    AddPlayer(snap, primaryId, 0, 1200, {{"2v2", 1200}});
    AddPlayer(snap, "Steam|opponent", 1, 1200);
    db->SaveMatch(snap);

    EXPECT_EQ(ScalarInt(db->GetRawDb(),
                        "SELECT mmr_estimated FROM MatchPlayers WHERE primary_id='Steam|estimated-history';"),
              1);

    ASSERT_TRUE(db->UpdateMatchPlayerMmr(
        snap.matchGuid, primaryId, 1209, true));
    std::vector<SessionMatchSummary> recent;
    db->GetRecentMatchHistory(primaryId, recent, 10);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].mmr, 1209);
    EXPECT_TRUE(recent[0].mmrEstimated);

    std::vector<float> x;
    std::vector<float> y;
    db->GetLifetimeMmrHistory(primaryId, "2v2", x, y);
    EXPECT_TRUE(y.empty());

    ASSERT_TRUE(db->UpdateMatchPlayerMmr(
        snap.matchGuid, primaryId, 1211, false));
    db->GetRecentMatchHistory(primaryId, recent, 10);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].mmr, 1211);
    EXPECT_FALSE(recent[0].mmrEstimated);

    db->GetLifetimeMmrHistory(primaryId, "2v2", x, y);
    ASSERT_EQ(y.size(), 1u);
    EXPECT_FLOAT_EQ(y[0], 1211.0f);
}
