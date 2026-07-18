#include <gtest/gtest.h>
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include <memory>
#include <filesystem>

class DatabaseManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sessionState = std::make_shared<SessionState>();
        dbManager = std::make_shared<DatabaseManager>(sessionState);
        // Initialize in-memory database to avoid disk I/O
        ASSERT_TRUE(dbManager->Initialize(":memory:"));
    }

    std::shared_ptr<SessionState> sessionState;
    std::shared_ptr<DatabaseManager> dbManager;
};

TEST_F(DatabaseManagerTest, InitializeCreatesTables) {
    // Already initialized in SetUp. We can verify it by writing a setting.
    EXPECT_TRUE(dbManager->SetSetting("test_key", "test_value"));
    EXPECT_EQ(dbManager->GetSetting("test_key", "default"), "test_value");
    EXPECT_EQ(dbManager->GetSetting("missing_key", "default"), "default");
}

TEST_F(DatabaseManagerTest, SaveMatchDoesNotCrash) {
    MatchSaveSnapshot snap;
    snap.arenaName = "test_arena";
    EXPECT_NO_THROW(dbManager->SaveMatch(snap));
}

TEST_F(DatabaseManagerTest, QueueShutdownAndSnapshotCorrectness) {
    std::string testDbPath = "test_queue_shutdown.db";
    if (std::filesystem::exists(testDbPath)) {
        std::filesystem::remove(testDbPath);
    }

    {
        auto fileDb = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(fileDb->Initialize(testDbPath));

        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "guid_123";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.myPrimaryId = "Steam|123456";
        snap.roster["Steam|123456"] = PlayerData{.primaryId = "Steam|123456", .name = "Player1", .team = 0, .mmr = 1000};
        snap.roster["Steam|654321"] = PlayerData{.primaryId = "Steam|654321", .name = "Player2", .team = 1, .mmr = 950};

        fileDb->AsyncSaveMatch(snap);
        // fileDb destructor will join worker and ensure queue drains
    }

    // Now verify the data was saved correctly
    {
        auto fileDb = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(fileDb->Initialize(testDbPath));

        int wins = 0, losses = 0, games = 0;
        fileDb->GetGamemodeStats("Steam|123456", "1v1", wins, losses, games);
        EXPECT_EQ(wins, 1);
        EXPECT_EQ(games, 1);
    }

    if (std::filesystem::exists(testDbPath)) {
        std::filesystem::remove(testDbPath);
    }
}

TEST_F(DatabaseManagerTest, GetStreakStatsCalculatesLongestLossStreak) {
    std::string pid = "Steam|123456";

    auto saveMatch = [&](const std::string& guid, bool win) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.myTeam = 0;
        snap.winnerTeam = win ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = win ? 3 : 1;
        snap.score[1] = win ? 1 : 3;
        snap.myPrimaryId = pid;
        snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1000};
        dbManager->SaveMatch(snap);
    };

    saveMatch("guid_1", true);
    saveMatch("guid_2", false);
    saveMatch("guid_3", false);
    saveMatch("guid_4", true);
    saveMatch("guid_5", false);

    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now', '-4 minutes') WHERE id = 1;", nullptr, nullptr, nullptr);
    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now', '-3 minutes') WHERE id = 2;", nullptr, nullptr, nullptr);
    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now', '-2 minutes') WHERE id = 3;", nullptr, nullptr, nullptr);
    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now', '-1 minutes') WHERE id = 4;", nullptr, nullptr, nullptr);
    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now') WHERE id = 5;", nullptr, nullptr, nullptr);

    int curWin = 0, curLoss = 0, longestWin = 0, longestLoss = 0;
    dbManager->GetStreakStats(pid, curWin, curLoss, longestWin, longestLoss);

    EXPECT_EQ(curWin, 0);
    EXPECT_EQ(curLoss, 1);
    EXPECT_EQ(longestWin, 1);
    EXPECT_EQ(longestLoss, 2);
}

TEST_F(DatabaseManagerTest, GetRecentMatchHistoryReturnsNewestSavedMatches) {
    std::string pid = "Steam|123456";

    auto saveMatch = [&](const std::string& guid, bool win, int ourScore, int theirScore, MmrCategory category) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.myTeam = 0;
        snap.winnerTeam = win ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = ourScore;
        snap.score[1] = theirScore;
        snap.myPrimaryId = pid;
        snap.rosterMmrCategory = category;
        snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1000};
        snap.roster["Steam|teammate"] = PlayerData{.primaryId = "Steam|teammate", .name = "Player2", .team = 0, .mmr = 1000};
        snap.roster["Steam|opponent1"] = PlayerData{.primaryId = "Steam|opponent1", .name = "Player3", .team = 1, .mmr = 1000};
        snap.roster["Steam|opponent2"] = PlayerData{.primaryId = "Steam|opponent2", .name = "Player4", .team = 1, .mmr = 1000};
        dbManager->SaveMatch(snap);
    };

    saveMatch("guid_1", true, 3, 1, MmrCategory::TwoVTwo);
    saveMatch("guid_2", false, 2, 4, MmrCategory::TwoVTwo);
    saveMatch("guid_3", true, 5, 0, MmrCategory::Casual);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);

    ASSERT_EQ(matches.size(), 3u);
    EXPECT_FALSE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Doubles");
    EXPECT_EQ(matches[0].ourScore, 5);
    EXPECT_EQ(matches[0].theirScore, 0);
    EXPECT_EQ(matches[0].mmr, 1000);
    EXPECT_TRUE(matches[0].win);

    EXPECT_TRUE(matches[1].ranked);
    EXPECT_EQ(matches[1].mode, "Doubles");
    EXPECT_EQ(matches[1].ourScore, 2);
    EXPECT_EQ(matches[1].theirScore, 4);
    EXPECT_EQ(matches[1].mmr, 1000);
    EXPECT_FALSE(matches[1].win);
}

TEST_F(DatabaseManagerTest, ExtraPlaylistSelectionDoesNotOverrideStandardArena) {
    std::string pid = "Steam|123456";

    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "standard-with-hoops-selected-guid";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 3;
    snap.score[1] = 1;
    snap.maxPlayersSeen = 4;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::Hoops;
    snap.graphMmrCategory = MmrCategory::Hoops;
    snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1000};
    snap.roster["Steam|teammate"] = PlayerData{.primaryId = "Steam|teammate", .name = "Player2", .team = 0, .mmr = 1000};
    snap.roster["Steam|opponent1"] = PlayerData{.primaryId = "Steam|opponent1", .name = "Player3", .team = 1, .mmr = 1000};
    snap.roster["Steam|opponent2"] = PlayerData{.primaryId = "Steam|opponent2", .name = "Player4", .team = 1, .mmr = 1000};

    dbManager->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);

    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Doubles");
}

TEST_F(DatabaseManagerTest, ExtraArenaOverridesStandardPlayerCount) {
    std::string pid = "Steam|123456";

    MatchSaveSnapshot snap;
    snap.arenaName = "DunkHouse";
    snap.arenaAsset = "hoops_dunkhouse_p";
    snap.matchGuid = "hoops-map-guid";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 3;
    snap.score[1] = 1;
    snap.maxPlayersSeen = 4;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::TwoVTwo;
    snap.graphMmrCategory = MmrCategory::TwoVTwo;
    snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1000};
    snap.roster["Steam|teammate"] = PlayerData{.primaryId = "Steam|teammate", .name = "Player2", .team = 0, .mmr = 1000};
    snap.roster["Steam|opponent1"] = PlayerData{.primaryId = "Steam|opponent1", .name = "Player3", .team = 1, .mmr = 1000};
    snap.roster["Steam|opponent2"] = PlayerData{.primaryId = "Steam|opponent2", .name = "Player4", .team = 1, .mmr = 1000};

    dbManager->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);

    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Hoops");
}
