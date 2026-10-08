#include <gtest/gtest.h>
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include <memory>
#include <filesystem>
#include <chrono>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>
#include "core/Storage.hpp"

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
    EXPECT_EQ(matches[0].matchGuid, "guid_3");
    EXPECT_EQ(matches[0].mode, "Doubles");
    EXPECT_EQ(matches[0].ourScore, 5);
    EXPECT_EQ(matches[0].theirScore, 0);
    EXPECT_EQ(matches[0].mmr, 1000);
    EXPECT_TRUE(matches[0].win);

    EXPECT_TRUE(matches[1].ranked);
    EXPECT_EQ(matches[1].matchGuid, "guid_2");
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
    snap.legacyPlayerCount = 4;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::Hoops;
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

TEST_F(DatabaseManagerTest, GraphSelectionDoesNotOverrideMatchMode) {
    const std::string pid = "Steam|graph-mode";
    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "graph-mode-independent-guid";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 2;
    snap.score[1] = 1;
    snap.legacyPlayerCount = 6;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::Best;
    for (int index = 0; index < 6; ++index) {
        const std::string playerId =
            index == 0
                ? pid
                : "Steam|graph-mode-" + std::to_string(index);
        snap.roster[playerId] = PlayerData{
            .primaryId = playerId,
            .name = "Player" + std::to_string(index),
            .team = index < 3 ? 0 : 1,
            .mmr = 1000};
    }

    dbManager->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_TRUE(matches[0].ranked);
    EXPECT_EQ(matches[0].mode, "Standard");
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
    snap.legacyPlayerCount = 4;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::TwoVTwo;
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

TEST_F(DatabaseManagerTest, UpdatesSavedLocalPlayerMmrByMatchGuid) {
    const std::string pid = "Steam|123456";

    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "post-match-mmr-guid";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 3;
    snap.score[1] = 1;
    snap.legacyPlayerCount = 2;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::OneVOne;
    snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1200};
    snap.roster["Steam|opponent"] = PlayerData{.primaryId = "Steam|opponent", .name = "Player2", .team = 1, .mmr = 1180};
    dbManager->SaveMatch(snap);

    ASSERT_TRUE(dbManager->UpdateMatchPlayerMmr(snap.matchGuid, pid, 1211));

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].mmr, 1211);
}

TEST_F(DatabaseManagerTest, ConfirmedMmrRefreshReplacesPendingHistoryPlaceholder) {
    const std::string pid = "Steam|pending-history";
    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "pending-history-db-guid";
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 4;
    snap.score[1] = 2;
    snap.legacyPlayerCount = 2;
    snap.myPrimaryId = pid;
    snap.rosterMmrCategory = MmrCategory::OneVOne;
    snap.roster[pid] = PlayerData{
        .primaryId = pid,
        .name = "Player",
        .team = 0,
        .mmr = 1200};
    snap.roster["Steam|opponent"] = PlayerData{
        .primaryId = "Steam|opponent",
        .name = "Opponent",
        .team = 1,
        .mmr = 1190};

    {
        std::unique_lock<std::shared_mutex> lock(
            sessionState->history.mutex);
        SessionMatchSummary pending;
        pending.matchGuid = snap.matchGuid;
        pending.mode = "Duel";
        pending.ourScore = 4;
        pending.theirScore = 2;
        pending.pendingTrackerConfirmation = true;
        sessionState->history.pendingRecentMatches.push_back(
            std::move(pending));
    }

    dbManager->SaveMatch(snap);
    dbManager->AsyncUpdateMatchPlayerMmr(
        snap.matchGuid, pid, 1211);
    dbManager->AsyncSetSetting(
        "pending_history_barrier", "complete");

    for (int attempt = 0;
         attempt < 200 &&
         dbManager->GetSetting("pending_history_barrier", "") !=
             "complete";
         ++attempt) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }
    ASSERT_EQ(
        dbManager->GetSetting("pending_history_barrier", ""),
        "complete");

    std::shared_lock<std::shared_mutex> lock(
        sessionState->history.mutex);
    EXPECT_TRUE(
        sessionState->history.pendingRecentMatches.empty());
    ASSERT_EQ(
        sessionState->history.recentSavedMatches.size(), 1u);
    EXPECT_EQ(
        sessionState->history.recentSavedMatches[0].matchGuid,
        snap.matchGuid);
    EXPECT_EQ(
        sessionState->history.recentSavedMatches[0].mmr,
        1211);
    EXPECT_FALSE(
        sessionState->history.recentSavedMatches[0]
            .pendingTrackerConfirmation);
}

TEST_F(DatabaseManagerTest, AsyncSavePublishesOrderedStreakCacheAndLeavesItClean) {
    const std::string pid = "Steam|streak-player";
    auto makeMatch = [&](const std::string& guid, bool win) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.myTeam = 0;
        snap.winnerTeam = win ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = win ? 2 : 1;
        snap.score[1] = win ? 1 : 2;
        snap.legacyPlayerCount = 2;
        snap.myPrimaryId = pid;
        snap.rosterMmrCategory = MmrCategory::OneVOne;
        snap.roster[pid] =
            PlayerData{.primaryId = pid, .name = "Player", .team = 0, .mmr = 1200};
        snap.roster["Steam|opponent"] =
            PlayerData{.primaryId = "Steam|opponent", .name = "Opponent", .team = 1, .mmr = 1200};
        return snap;
    };

    sessionState->ui.dbStatsDirty.store(true);
    dbManager->AsyncSaveMatch(makeMatch("streak-win-1", true));
    dbManager->AsyncSaveMatch(makeMatch("streak-win-2", true));
    dbManager->AsyncSaveMatch(makeMatch("streak-win-3", true));
    dbManager->AsyncSaveMatch(makeMatch("streak-loss-1", false));
    dbManager->AsyncSetSetting("streak_test_barrier", "complete");

    for (int attempt = 0;
         attempt < 200 && dbManager->GetSetting("streak_test_barrier", "") != "complete";
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(dbManager->GetSetting("streak_test_barrier", ""), "complete");

    CachedDbStats cached;
    {
        std::lock_guard<std::mutex> lock(sessionState->ui.dbStatsMutex);
        cached = sessionState->ui.cachedDbStats;
    }
    EXPECT_EQ(cached.currentWins, 0);
    EXPECT_EQ(cached.currentLosses, 1);
    EXPECT_EQ(cached.longestWins, 3);
    EXPECT_EQ(cached.longestLosses, 1);
    EXPECT_FALSE(sessionState->ui.dbStatsDirty.load());
}

TEST_F(DatabaseManagerTest, EarlyLossPersistsOnceBeforeFollowingWin) {
    const std::string pid = "Steam|ordered-streak-player";
    const int64_t baseTimeMs = 1'800'000'000'000;
    auto makeMatch = [&](const std::string& guid,
                         bool win,
                         int64_t endedAtUnixMs) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.myTeam = 0;
        snap.winnerTeam = win ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = win ? 2 : 1;
        snap.score[1] = win ? 1 : 2;
        snap.legacyPlayerCount = 2;
        snap.myPrimaryId = pid;
        snap.rosterMmrCategory = MmrCategory::OneVOne;
        snap.endedAtUnixMs = endedAtUnixMs;
        snap.roster[pid] =
            PlayerData{
                .primaryId = pid,
                .name = "Player",
                .team = 0,
                .mmr = 1200};
        snap.roster["Steam|opponent"] =
            PlayerData{
                .primaryId = "Steam|opponent",
                .name = "Opponent",
                .team = 1,
                .mmr = 1200};
        return snap;
    };

    dbManager->SaveMatch(
        makeMatch("ordered-win-1", true, baseTimeMs + 1000));
    dbManager->SaveMatch(
        makeMatch("ordered-win-2", true, baseTimeMs + 2000));
    dbManager->SaveMatch(
        makeMatch("ordered-win-3", true, baseTimeMs + 3000));
    const MatchSaveSnapshot earlyLoss =
        makeMatch(
            "ordered-early-loss",
            false,
            baseTimeMs + 4000);
    dbManager->SaveMatch(earlyLoss);
    dbManager->SaveMatch(earlyLoss);
    dbManager->SaveMatch(
        makeMatch(
            "ordered-following-win",
            true,
            baseTimeMs + 5000));

    int wins = 0;
    int losses = 0;
    int games = 0;
    dbManager->GetGamemodeStats(
        pid, "1v1", wins, losses, games);
    EXPECT_EQ(wins, 4);
    EXPECT_EQ(losses, 1);
    EXPECT_EQ(games, 5);

    int currentWins = 0;
    int currentLosses = 0;
    int longestWins = 0;
    int longestLosses = 0;
    dbManager->GetStreakStats(
        pid,
        currentWins,
        currentLosses,
        longestWins,
        longestLosses);
    EXPECT_EQ(currentWins, 1);
    EXPECT_EQ(currentLosses, 0);
    EXPECT_EQ(longestWins, 3);
    EXPECT_EQ(longestLosses, 1);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    ASSERT_EQ(matches.size(), 5u);
    EXPECT_TRUE(matches[0].win);
    EXPECT_FALSE(matches[1].win);
    EXPECT_TRUE(matches[2].win);
}

TEST_F(DatabaseManagerTest, PendingDestroyedMatchCountsInStreakAndConfirmationReplacesResult) {
    const std::string pid = "Steam|pending-streak-player";
    const int64_t baseTimeMs = 1'800'000'000'000;
    auto makeMatch = [&](const std::string& guid, bool win, int64_t endedAtUnixMs) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.playlistId = 11;
        snap.myTeam = 1;
        snap.winnerTeam = win ? 1 : 0;
        snap.validResult = true;
        snap.score[0] = win ? 0 : 4;
        snap.score[1] = win ? 4 : 0;
        snap.myPrimaryId = pid;
        snap.endedAtUnixMs = endedAtUnixMs;
        snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player", .team = 1, .mmr = 1015};
        return snap;
    };
    auto streak = [&]() {
        int currentWins = 0, currentLosses = 0, longestWins = 0, longestLosses = 0;
        dbManager->GetStreakStats(pid, currentWins, currentLosses, longestWins, longestLosses);
        return currentWins > 0 ? currentWins : -currentLosses;
    };

    dbManager->SaveMatch(makeMatch("pending-win", true, baseTimeMs + 1000));
    dbManager->SaveMatch(makeMatch("pending-loss-1", false, baseTimeMs + 2000));
    MatchSaveSnapshot provisional = makeMatch("pending-destroyed", false, baseTimeMs + 3000);
    provisional.resultPending = true;
    dbManager->SaveMatch(provisional);
    EXPECT_EQ(streak(), -2);

    dbManager->SaveMatch(provisional);
    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    ASSERT_EQ(matches.size(), 3u);

    dbManager->SaveMatch(makeMatch("pending-destroyed", true, baseTimeMs + 3000));
    EXPECT_EQ(streak(), 1);
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    ASSERT_EQ(matches.size(), 3u);
    EXPECT_EQ(matches[0].matchGuid, "pending-destroyed");
    EXPECT_TRUE(matches[0].win);
    EXPECT_EQ(matches[0].ourScore, 4);

    dbManager->SaveMatch(makeMatch("pending-destroyed", false, baseTimeMs + 3000));
    EXPECT_EQ(streak(), 1);
}

TEST_F(DatabaseManagerTest, VoidConfirmationRemovesPendingDestroyedMatch) {
    const std::string pid = "Steam|pending-void-player";
    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "pending-void";
    snap.playlistId = 11;
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.resultPending = true;
    snap.score[0] = 3;
    snap.myPrimaryId = pid;
    snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Player", .team = 0, .mmr = 1000};
    dbManager->SaveMatch(snap);

    snap.resultPending = false;
    snap.validResult = false;
    snap.voidReason = "non_live_replay";
    dbManager->SaveMatch(snap);

    std::vector<SessionMatchSummary> matches;
    dbManager->GetRecentMatchHistory(pid, matches, 10);
    EXPECT_TRUE(matches.empty());
}

static void RemoveTestDbFiles(const std::string& base) {
    std::error_code ec;
    std::filesystem::remove(base, ec);
    std::filesystem::remove(base + "-wal", ec);
    std::filesystem::remove(base + "-shm", ec);
}

TEST_F(DatabaseManagerTest, MergeDatabaseRejectsInvalidOrMissingFile) {
    auto resultEmpty = dbManager->MergeDatabase("");
    EXPECT_FALSE(resultEmpty.success);
    EXPECT_EQ(resultEmpty.error, "No database file selected.");

    auto resultMissing = dbManager->MergeDatabase("non_existent_file_12345.db");
    EXPECT_FALSE(resultMissing.success);
    EXPECT_NE(resultMissing.error.find("does not exist"), std::string::npos);
}

TEST_F(DatabaseManagerTest, MergeDatabaseRejectsSelfMerge) {
    std::string testPath = "test_self_merge.db";
    RemoveTestDbFiles(testPath);

    {
        auto fileDb = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(fileDb->Initialize(testPath));
        auto result = fileDb->MergeDatabase(testPath);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.error, "Cannot merge the active database into itself.");
    }

    RemoveTestDbFiles(testPath);
}

TEST_F(DatabaseManagerTest, MergeDatabaseImportsMatchesAndPlayers) {
    std::string sourcePath = "test_source_merge.db";
    RemoveTestDbFiles(sourcePath);

    const std::string pid = "Steam|merge_user";
    {
        auto sourceDb = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(sourceDb->Initialize(sourcePath));

        MatchSaveSnapshot match1;
        match1.arenaName = "DFH Stadium";
        match1.matchGuid = "merge_guid_1";
        match1.myTeam = 0;
        match1.winnerTeam = 0;
        match1.validResult = true;
        match1.score[0] = 4;
        match1.score[1] = 2;
        match1.myPrimaryId = pid;
        match1.playlistId = 10;
        match1.gamemode = "1v1";
        match1.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1100};
        match1.roster["Steam|opp1"] = PlayerData{.primaryId = "Steam|opp1", .name = "Opp1", .team = 1, .mmr = 1080};
        sourceDb->SaveMatch(match1);

        MatchSaveSnapshot match2;
        match2.arenaName = "Mannfield";
        match2.matchGuid = "merge_guid_2";
        match2.myTeam = 0;
        match2.winnerTeam = 1;
        match2.validResult = true;
        match2.score[0] = 1;
        match2.score[1] = 3;
        match2.myPrimaryId = pid;
        match2.playlistId = 10;
        match2.gamemode = "1v1";
        match2.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1090};
        match2.roster["Steam|opp2"] = PlayerData{.primaryId = "Steam|opp2", .name = "Opp2", .team = 1, .mmr = 1110};
        sourceDb->SaveMatch(match2);
    }

    int wins = 0, losses = 0, games = 0;
    dbManager->GetGamemodeStats(pid, "1v1", wins, losses, games);
    EXPECT_EQ(games, 0);

    auto mergeResult = dbManager->MergeDatabase(sourcePath);
    EXPECT_TRUE(mergeResult.success);
    EXPECT_EQ(mergeResult.matchesImported, 2);
    EXPECT_EQ(mergeResult.matchesSkipped, 0);
    EXPECT_EQ(mergeResult.playersImported, 4);

    dbManager->GetGamemodeStats(pid, "1v1", wins, losses, games);
    EXPECT_EQ(games, 2);
    EXPECT_EQ(wins, 1);
    EXPECT_EQ(losses, 1);

    std::vector<SessionMatchSummary> recent;
    dbManager->GetRecentMatchHistory(pid, recent, 10);
    EXPECT_EQ(recent.size(), 2u);
    RemoveTestDbFiles(sourcePath);
}

TEST_F(DatabaseManagerTest, MergeDatabaseDeduplicatesMatches) {
    std::string sourcePath = "test_source_dedup.db";
    RemoveTestDbFiles(sourcePath);
    const std::string pid = "Steam|dedup_user";
    MatchSaveSnapshot match1;
    match1.arenaName = "DFH Stadium";
    match1.matchGuid = "shared_guid_1";
    match1.myTeam = 0;
    match1.winnerTeam = 0;
    match1.validResult = true;
    match1.score[0] = 3;
    match1.score[1] = 0;
    match1.myPrimaryId = pid;
    match1.gamemode = "1v1";
    match1.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 1000};

    MatchSaveSnapshot match2;
    match2.arenaName = "Utopia Coliseum";
    match2.matchGuid = "new_guid_2";
    match2.myTeam = 0;
    match2.winnerTeam = 1;
    match2.validResult = true;
    match2.score[0] = 2;
    match2.score[1] = 4;
    match2.myPrimaryId = pid;
    match2.gamemode = "1v1";
    match2.roster[pid] = PlayerData{.primaryId = pid, .name = "Player1", .team = 0, .mmr = 990};

    // Target already has match1
    dbManager->SaveMatch(match1);

    // Source has match1 AND match2
    {
        auto sourceDb = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(sourceDb->Initialize(sourcePath));
        sourceDb->SaveMatch(match1);
        sourceDb->SaveMatch(match2);
    }

    auto firstMerge = dbManager->MergeDatabase(sourcePath);
    EXPECT_TRUE(firstMerge.success);
    EXPECT_EQ(firstMerge.matchesImported, 1);
    EXPECT_EQ(firstMerge.matchesSkipped, 1);

    // Merge again: both should be skipped as duplicates
    auto secondMerge = dbManager->MergeDatabase(sourcePath);
    EXPECT_TRUE(secondMerge.success);
    EXPECT_EQ(secondMerge.matchesImported, 0);
    EXPECT_EQ(secondMerge.matchesSkipped, 2);

    RemoveTestDbFiles(sourcePath);
}

TEST_F(DatabaseManagerTest, MergeDatabaseHandlesLegacySchema) {
    std::string legacyPath = "test_legacy_schema.db";
    RemoveTestDbFiles(legacyPath);

    // Create legacy sqlite database without playlist_id and mmr_estimated columns
    sqlite3* rawDb = nullptr;
    ASSERT_EQ(sqlite3_open(legacyPath.c_str(), &rawDb), SQLITE_OK);

    const char* schema = R"(
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
        INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, gamemode, player_count)
        VALUES ('2024-01-01 12:00:00', 'DFH Stadium', 5, 2, 1, 'legacy_guid_1', '1v1', 2);
        INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent)
        VALUES (1, 'Steam|legacy_user', 'LegacyPlayer', 0, 1200, 0);
    )";

    char* errMsg = nullptr;
    ASSERT_EQ(sqlite3_exec(rawDb, schema, nullptr, nullptr, &errMsg), SQLITE_OK);
    sqlite3_close(rawDb);

    auto result = dbManager->MergeDatabase(legacyPath);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.matchesImported, 1);
    EXPECT_EQ(result.playersImported, 1);

    int wins = 0, losses = 0, games = 0;
    dbManager->GetGamemodeStats("Steam|legacy_user", "1v1", wins, losses, games);
    EXPECT_EQ(games, 1);
    EXPECT_EQ(wins, 1);

    RemoveTestDbFiles(legacyPath);
}

TEST_F(DatabaseManagerTest, PeopleRecordsCountOnlySharedMatchesAndUseLatestName) {
    const std::string me = "Steam|me";
    auto saveMatch = [&](const std::string& guid, bool win, const std::string& owner, const std::string& mateName) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.myTeam = 0;
        snap.winnerTeam = win ? 0 : 1;
        snap.validResult = true;
        snap.myPrimaryId = owner;
        snap.roster[owner] = PlayerData{.primaryId = owner, .name = "Owner", .team = 0, .mmr = 1000};
        snap.roster["Steam|mate"] = PlayerData{.primaryId = "Steam|mate", .name = mateName, .team = 0, .mmr = 1000};
        snap.roster["Steam|rival"] = PlayerData{.primaryId = "Steam|rival", .name = "Rival", .team = 1, .mmr = 1000};
        snap.roster["Unknown|bot"] = PlayerData{.primaryId = "Unknown|bot", .name = "Bot", .team = 1, .mmr = 0};
        dbManager->SaveMatch(snap);
    };
    saveMatch("people-1", true, me, "OldName");
    saveMatch("people-2", false, me, "NewName");
    saveMatch("people-3", true, "Steam|someone-else", "Elsewhere");
    sqlite3_exec(dbManager->GetRawDb(), "UPDATE Matches SET timestamp = datetime('now', '-2 hours') WHERE match_guid = 'people-1';", nullptr, nullptr, nullptr);

    std::vector<PersonRecord> people;
    dbManager->GetPeopleRecords(me, people);

    ASSERT_EQ(people.size(), 2u);
    const auto find = [&](const std::string& id) {
        return *std::find_if(people.begin(), people.end(), [&](const PersonRecord& p) { return p.primaryId == id; });
    };
    const PersonRecord mate = find("Steam|mate");
    EXPECT_EQ(mate.name, "NewName");
    EXPECT_EQ(mate.winsWith, 1);
    EXPECT_EQ(mate.lossesWith, 1);
    EXPECT_EQ(mate.GamesAgainst(), 0);
    const PersonRecord rival = find("Steam|rival");
    EXPECT_EQ(rival.winsAgainst, 1);
    EXPECT_EQ(rival.lossesAgainst, 1);
    EXPECT_EQ(rival.GamesWith(), 0);
}

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

TEST_F(DatabaseManagerTest, MigratesAllHistoricalSchemasToV2WithBackupsAndIdempotency) {
    struct HistoricalSchemaCase {
        const char* label;
        const char* ddlAndSeed;
        int initialUserVersion;
    };

    const HistoricalSchemaCase cases[] = {
        {"no_playlist_id",
         R"(
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
            INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, gamemode, player_count)
            VALUES ('2025-01-10 12:00:00', 'DFH Stadium', 3, 1, 1, 'hist-guid-1', '2v2', 4),
                   ('2025-01-10 12:10:00', 'Mannfield', 1, 2, 0, 'hist-guid-2', '2v2', 4);
            INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent)
            VALUES (1, 'Steam|hist', 'Hero', 0, 1100, 0),
                   (1, 'Steam|opp', 'Opp', 1, 1090, 1),
                   (2, 'Steam|hist', 'Hero', 0, 1091, 0);
         )",
         0},
        {"no_result_pending",
         R"(
            CREATE TABLE Matches (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
                arena TEXT,
                our_score INTEGER,
                their_score INTEGER,
                win BOOLEAN,
                match_guid TEXT,
                playlist_id INTEGER,
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
                mmr_estimated BOOLEAN DEFAULT 0,
                is_opponent BOOLEAN DEFAULT 0
            );
            CREATE TABLE Settings (key TEXT PRIMARY KEY, value TEXT);
            INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count)
            VALUES ('2025-02-10 12:00:00', 'DFH Stadium', 4, 2, 1, 'hist-guid-1', 11, '2v2', 0),
                   ('2025-02-10 12:10:00', 'Mannfield', 0, 3, 0, 'hist-guid-2', 11, '2v2', 0);
            INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent)
            VALUES (1, 'Steam|hist', 'Hero', 0, 1150, 0, 0),
                   (1, 'Steam|opp', 'Opp', 1, 1140, 0, 1),
                   (2, 'Steam|hist', 'Hero', 0, 1139, 0, 0);
         )",
         0},
        {"current_v1",
         R"(
            CREATE TABLE Matches (
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
            CREATE TABLE MatchPlayers (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                match_id INTEGER,
                primary_id TEXT,
                name TEXT,
                team INTEGER,
                mmr INTEGER,
                mmr_estimated BOOLEAN DEFAULT 0,
                is_opponent BOOLEAN DEFAULT 0
            );
            CREATE TABLE Settings (key TEXT PRIMARY KEY, value TEXT);
            PRAGMA user_version = 1;
            INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, result_pending)
            VALUES ('2025-03-10 12:00:00', 'DFH Stadium', 2, 1, 1, 'hist-guid-1', 11, '2v2', 0, 0),
                   ('2025-03-10 12:10:00', 'Mannfield', 2, 3, 0, 'hist-guid-2', 11, '2v2', 0, 0);
            INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent)
            VALUES (1, 'Steam|hist', 'Hero', 0, 1200, 0, 0),
                   (1, 'Steam|opp', 'Opp', 1, 1190, 0, 1),
                   (2, 'Steam|hist', 'Hero', 0, 1189, 0, 0);
         )",
         1},
    };

    for (const auto& tc : cases) {
        const std::string path = std::string("test_mig_") + tc.label + ".db";
        const std::string backupPath = path + ".bak-v" + std::to_string(tc.initialUserVersion);
        RemoveTestDbFiles(path);
        std::filesystem::remove(backupPath);

        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, tc.ddlAndSeed, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(raw);

        {
            auto db = std::make_shared<DatabaseManager>(sessionState);
            ASSERT_TRUE(db->Initialize(path)) << tc.label;
            EXPECT_EQ(db->GetSchemaVersion(), 3) << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 2) << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers;"), 3) << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(),
                                     "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name IN ('MatchPlayerStats','MatchLocalStats');"),
                      2)
                << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(),
                                     "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_mp_primary_match';"),
                      1)
                << tc.label;
            EXPECT_TRUE(std::filesystem::exists(backupPath)) << tc.label;
        }

        // Re-running Initialize on an already-migrated v2 DB is idempotent.
        {
            auto db = std::make_shared<DatabaseManager>(sessionState);
            ASSERT_TRUE(db->Initialize(path)) << tc.label;
            EXPECT_EQ(db->GetSchemaVersion(), 3) << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 2) << tc.label;
            EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers;"), 3) << tc.label;
        }

        RemoveTestDbFiles(path);
        std::filesystem::remove(backupPath);
    }
}

TEST_F(DatabaseManagerTest, MigrationV2FailureRollsBackAndKeepsRunningOnV1Tables) {
    const std::string path = "test_mig_v2_fail.db";
    const std::string backupPath = path + ".bak-v0";
    RemoveTestDbFiles(path);
    std::filesystem::remove(backupPath);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    // Create a view named MatchLocalStats so migration v2 fails mid-transaction after creating MatchPlayerStats.
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
        CREATE VIEW MatchLocalStats AS SELECT 1 AS dummy;
    )",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    {
        auto db = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db->Initialize(path));
        // Migration v1 committed, migration v2 rolled back completely.
        EXPECT_EQ(db->GetSchemaVersion(), 1);
        EXPECT_EQ(QueryIntScalar(db->GetRawDb(),
                                 "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='MatchPlayerStats';"),
                  0);

        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "v1-fallback-guid";
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.myPrimaryId = "Steam|v1user";
        snap.roster["Steam|v1user"] = PlayerData{.primaryId = "Steam|v1user", .name = "Player", .team = 0, .mmr = 1000, .goals = 2};
        db->SaveMatch(snap);

        std::vector<SessionMatchSummary> recent;
        db->GetRecentMatchHistory("Steam|v1user", recent, 10);
        ASSERT_EQ(recent.size(), 1u);
        EXPECT_EQ(recent[0].matchGuid, "v1-fallback-guid");
        EXPECT_TRUE(db->GetPlayerStatSeries("Steam|v1user").empty());
    }

    RemoveTestDbFiles(path);
    std::filesystem::remove(backupPath);
}

TEST_F(DatabaseManagerTest, GetRecentMatchHistoryExcludesMatchesAccountDidNotPlay) {
    const std::string accountA = "Steam|account-a";
    const std::string accountB = "Steam|account-b";

    auto saveForAccount = [&](const std::string& guid, const std::string& accountId) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.playlistId = 10;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 3;
        snap.score[1] = 1;
        snap.myPrimaryId = accountId;
        snap.roster[accountId] = PlayerData{.primaryId = accountId, .name = "Player", .team = 0, .mmr = 1050};
        snap.roster["Steam|opp"] = PlayerData{.primaryId = "Steam|opp", .name = "Opp", .team = 1, .mmr = 1040};
        dbManager->SaveMatch(snap);
    };

    saveForAccount("match-a-1", accountA);
    saveForAccount("match-b-1", accountB);
    saveForAccount("match-a-2", accountA);

    std::vector<SessionMatchSummary> matchesA;
    dbManager->GetRecentMatchHistory(accountA, matchesA, 10);
    ASSERT_EQ(matchesA.size(), 2u);
    EXPECT_EQ(matchesA[0].matchGuid, "match-a-2");
    EXPECT_EQ(matchesA[1].matchGuid, "match-a-1");

    std::vector<SessionMatchSummary> matchesB;
    dbManager->GetRecentMatchHistory(accountB, matchesB, 10);
    ASSERT_EQ(matchesB.size(), 1u);
    EXPECT_EQ(matchesB[0].matchGuid, "match-b-1");
}

TEST_F(DatabaseManagerTest, SaveMatchAndGetPlayerStatSeriesPersistStatsAndNullUnobservedFields) {
    const std::string pid = "Steam|stats-player";

    MatchSaveSnapshot snap1;
    snap1.arenaName = "DFH Stadium";
    snap1.matchGuid = "stats-series-1";
    snap1.playlistId = 11; // Ranked Doubles (2v2)
    snap1.myTeam = 0;
    snap1.winnerTeam = 0;
    snap1.validResult = true;
    snap1.score[0] = 3;
    snap1.score[1] = 1;
    snap1.myPrimaryId = pid;
    snap1.endedAtUnixMs = 1'800'000'001'000;
    snap1.localStats.boostPickedUpSelf = 320;
    snap1.localStats.demoedSelf = 1;
    snap1.localStats.crossbarsSelf = 2;
    snap1.localStats.maxImpactForceSelf = 1450.0f;
    snap1.localStats.maxBallSpeedSelf = 112.5f;
    snap1.localStats.ownGoalsSelf = 0;
    snap1.roster[pid] = PlayerData{
        .primaryId = pid,
        .name = "Hero",
        .team = 0,
        .mmr = 1210,
        .goals = 2,
        .saves = 3,
        .shots = 5,
        .demos = 1,
        .assists = 1,
        .maxGoalSpeed = 108.0f,
        .fastestGoalTime = 14.5f,
    };
    snap1.roster["Steam|opp"] = PlayerData{
        .primaryId = "Steam|opp",
        .name = "Opponent",
        .team = 1,
        .mmr = 1200,
        .goals = 1,
        .saves = 1,
        .shots = 2,
        .demos = 1,
        .assists = 0,
    };
    dbManager->SaveMatch(snap1);

    MatchSaveSnapshot snap2 = snap1;
    snap2.matchGuid = "stats-series-2";
    snap2.playlistId = 10; // Ranked Duel (1v1)
    snap2.endedAtUnixMs = 1'800'000'002'000;
    snap2.localStats = {};
    snap2.roster[pid].goals = 0;
    snap2.roster[pid].maxGoalSpeed = 0.0f;
    snap2.roster[pid].fastestGoalTime = 0.0f;
    dbManager->SaveMatch(snap2);

    const auto allSeries = dbManager->GetPlayerStatSeries(pid, "", 10);
    ASSERT_EQ(allSeries.size(), 2u);
    EXPECT_EQ(allSeries[0].matchGuid, "stats-series-2");
    EXPECT_FALSE(allSeries[0].score.has_value());
    EXPECT_FALSE(allSeries[0].touches.has_value());
    EXPECT_FALSE(allSeries[0].carTouches.has_value());
    EXPECT_FALSE(allSeries[0].maxGoalSpeed.has_value());
    EXPECT_FALSE(allSeries[0].fastestGoalTime.has_value());
    EXPECT_FALSE(allSeries[0].hardestCrossbar.has_value());
    EXPECT_FALSE(allSeries[0].maxBallSpeed.has_value());
    EXPECT_FALSE(allSeries[0].durationSeconds.has_value());
    EXPECT_FALSE(allSeries[0].overtimeSeconds.has_value());

    const auto doublesSeries = dbManager->GetPlayerStatSeries(pid, "2v2", 10);
    ASSERT_EQ(doublesSeries.size(), 1u);
    const auto& r = doublesSeries[0];
    EXPECT_EQ(r.matchGuid, "stats-series-1");
    EXPECT_EQ(r.goals, 2);
    EXPECT_EQ(r.assists, 1);
    EXPECT_EQ(r.saves, 3);
    EXPECT_EQ(r.shots, 5);
    EXPECT_EQ(r.demos, 1);
    EXPECT_FALSE(r.score.has_value());
    EXPECT_FALSE(r.touches.has_value());
    EXPECT_FALSE(r.carTouches.has_value());
    ASSERT_TRUE(r.maxGoalSpeed.has_value());
    EXPECT_FLOAT_EQ(*r.maxGoalSpeed, 108.0f);
    ASSERT_TRUE(r.fastestGoalTime.has_value());
    EXPECT_FLOAT_EQ(*r.fastestGoalTime, 14.5f);
    EXPECT_TRUE(r.hasLocalStats);
    EXPECT_EQ(r.boostCollected, 320);
    EXPECT_EQ(r.demoed, 1);
    EXPECT_EQ(r.crossbars, 2);
    ASSERT_TRUE(r.hardestCrossbar.has_value());
    EXPECT_FLOAT_EQ(*r.hardestCrossbar, 1450.0f);
    ASSERT_TRUE(r.maxBallSpeed.has_value());
    EXPECT_FLOAT_EQ(*r.maxBallSpeed, 112.5f);
    EXPECT_EQ(r.ownGoals, 0);
    EXPECT_EQ(r.statsVersion, 1);

    int detailedCount = 0;
    std::string sinceDate;
    dbManager->GetDetailedStatsSummary(detailedCount, sinceDate);
    EXPECT_EQ(detailedCount, 2);
    EXPECT_FALSE(sinceDate.empty());
}

TEST_F(DatabaseManagerTest, PendingDestroyedMatchConfirmationUpsertsExactlyOneStatsRow) {
    const std::string pid = "Steam|pending-stats-player";

    MatchSaveSnapshot provisional;
    provisional.arenaName = "DFH Stadium";
    provisional.matchGuid = "pending-stats-guid";
    provisional.playlistId = 11;
    provisional.myTeam = 0;
    provisional.winnerTeam = 1;
    provisional.validResult = true;
    provisional.resultPending = true;
    provisional.score[0] = 1;
    provisional.score[1] = 2;
    provisional.myPrimaryId = pid;
    provisional.localStats.boostPickedUpSelf = 150;
    provisional.roster[pid] = PlayerData{.primaryId = pid, .name = "Hero", .team = 0, .mmr = 1100, .goals = 1, .saves = 2};
    provisional.roster["Steam|opp"] = PlayerData{.primaryId = "Steam|opp", .name = "Opp", .team = 1, .mmr = 1100, .goals = 2};
    dbManager->SaveMatch(provisional);

    MatchSaveSnapshot confirmed = provisional;
    confirmed.resultPending = false;
    confirmed.winnerTeam = 0;
    confirmed.score[0] = 3;
    confirmed.score[1] = 2;
    confirmed.localStats.boostPickedUpSelf = 210;
    confirmed.roster[pid].goals = 3;
    confirmed.roster[pid].saves = 4;
    dbManager->SaveMatch(confirmed);

    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchLocalStats;"), 1);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayerStats;"), 2);

    const auto series = dbManager->GetPlayerStatSeries(pid, "", 10);
    ASSERT_EQ(series.size(), 1u);
    EXPECT_TRUE(series[0].win);
    EXPECT_EQ(series[0].goals, 3);
    EXPECT_EQ(series[0].saves, 4);
    EXPECT_EQ(series[0].boostCollected, 210);
}

TEST_F(DatabaseManagerTest, MergeDatabaseV1ToV2AndV2ToV2WithoutDuplicatesAndExportDeleteParity) {
    const std::string v1SourcePath = "test_merge_v1_source.db";
    const std::string v2SourcePath = "test_merge_v2_source.db";
    RemoveTestDbFiles(v1SourcePath);
    RemoveTestDbFiles(v2SourcePath);

    const std::string pid = "Steam|parity-user";

    // 1. Create a v1 source DB (no MatchPlayerStats / MatchLocalStats tables)
    {
        sqlite3* raw = nullptr;
        ASSERT_EQ(sqlite3_open(v1SourcePath.c_str(), &raw), SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(raw, R"(
            CREATE TABLE Matches (
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
            CREATE TABLE MatchPlayers (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                match_id INTEGER,
                primary_id TEXT,
                name TEXT,
                team INTEGER,
                mmr INTEGER,
                mmr_estimated BOOLEAN DEFAULT 0,
                is_opponent BOOLEAN DEFAULT 0
            );
            INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count)
            VALUES ('2025-05-01 10:00:00', 'DFH Stadium', 2, 1, 1, 'v1-only-guid', 11, '2v2', 0);
            INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent)
            VALUES (1, 'Steam|parity-user', 'Hero', 0, 1100, 0, 0);
        )",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);
        sqlite3_close(raw);
    }

    // 2. Create a v2 source DB with detailed stats
    {
        auto v2Source = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(v2Source->Initialize(v2SourcePath));
        MatchSaveSnapshot snap;
        snap.arenaName = "Mannfield";
        snap.matchGuid = "v2-stats-guid";
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 4;
        snap.score[1] = 1;
        snap.myPrimaryId = pid;
        snap.localStats.boostPickedUpSelf = 280;
        snap.localStats.crossbarsSelf = 1;
        snap.localStats.maxImpactForceSelf = 900.0f;
        snap.roster[pid] = PlayerData{
            .primaryId = pid, .name = "Hero", .team = 0, .mmr = 1120, .goals = 3, .saves = 2, .shots = 6, .demos = 1, .assists = 1};
        v2Source->SaveMatch(snap);
    }

    // Merge v1 -> v2
    const auto mergeV1 = dbManager->MergeDatabase(v1SourcePath);
    EXPECT_TRUE(mergeV1.success) << mergeV1.error;
    EXPECT_EQ(mergeV1.matchesImported, 1);

    // Merge v2 -> v2 (twice to verify deduplication by match_guid)
    const auto mergeV2First = dbManager->MergeDatabase(v2SourcePath);
    EXPECT_TRUE(mergeV2First.success) << mergeV2First.error;
    EXPECT_EQ(mergeV2First.matchesImported, 1);

    const auto mergeV2Second = dbManager->MergeDatabase(v2SourcePath);
    EXPECT_TRUE(mergeV2Second.success) << mergeV2Second.error;
    EXPECT_EQ(mergeV2Second.matchesImported, 0);
    EXPECT_EQ(mergeV2Second.matchesSkipped, 1);

    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 2);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayerStats;"), 1);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchLocalStats;"), 1);

    const auto series = dbManager->GetPlayerStatSeries(pid, "2v2", 10);
    ASSERT_EQ(series.size(), 1u);
    EXPECT_EQ(series[0].matchGuid, "v2-stats-guid");
    EXPECT_EQ(series[0].goals, 3);
    EXPECT_EQ(series[0].saves, 2);
    EXPECT_EQ(series[0].boostCollected, 280);

    // ExportLocalData includes stats per player and local_stats per match
    Storage::InitializeEnvironment();
    std::string exportPath;
    std::string exportError;
    ASSERT_TRUE(dbManager->ExportLocalData(exportPath, exportError)) << exportError;
    {
        std::ifstream jsonIn(exportPath + "matches.json");
        ASSERT_TRUE(jsonIn.is_open());
        const auto exported = nlohmann::json::parse(jsonIn);
        ASSERT_EQ(exported.size(), 2u);
        const auto v1MatchIt = std::find_if(exported.begin(), exported.end(), [](const auto& m) {
            return m["match_guid"] == "v1-only-guid";
        });
        const auto v2MatchIt = std::find_if(exported.begin(), exported.end(), [](const auto& m) {
            return m["match_guid"] == "v2-stats-guid";
        });
        ASSERT_NE(v1MatchIt, exported.end());
        ASSERT_NE(v2MatchIt, exported.end());
        EXPECT_TRUE((*v1MatchIt)["local_stats"].is_null());
        EXPECT_TRUE((*v1MatchIt)["players"][0]["stats"].is_null());

        ASSERT_TRUE((*v2MatchIt)["local_stats"].is_object());
        EXPECT_EQ((*v2MatchIt)["local_stats"]["boost_collected"], 280);
        EXPECT_EQ((*v2MatchIt)["local_stats"]["crossbars"], 1);
        ASSERT_TRUE((*v2MatchIt)["players"][0]["stats"].is_object());
        EXPECT_EQ((*v2MatchIt)["players"][0]["stats"]["goals"], 3);
        EXPECT_EQ((*v2MatchIt)["players"][0]["stats"]["saves"], 2);
        EXPECT_TRUE((*v2MatchIt)["players"][0]["stats"]["score"].is_null());
        EXPECT_TRUE((*v2MatchIt)["players"][0]["stats"]["touches"].is_null());
    }
    std::error_code ec;
    std::filesystem::remove_all(exportPath, ec);

    // DeleteLocalMatchHistory clears v2 tables as well
    std::string deleteError;
    ASSERT_TRUE(dbManager->DeleteLocalMatchHistory(deleteError)) << deleteError;
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 0);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers;"), 0);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchPlayerStats;"), 0);
    EXPECT_EQ(QueryIntScalar(dbManager->GetRawDb(), "SELECT COUNT(*) FROM MatchLocalStats;"), 0);

    RemoveTestDbFiles(v1SourcePath);
    RemoveTestDbFiles(v2SourcePath);
}

TEST_F(DatabaseManagerTest, GetMatchMmrContextAggregatesLobbiesAndExcludesOtherAccounts) {
    const std::string me = "Steam|me";

    // Match 1: complete 2v2 lobby, me=1020, mate=980 (teamAvg=1000), opp1=1040, opp2=1060 (oppAvg=1050)
    {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "mmr-ctx-1";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.playlistId = 11;
        snap.gamemode = "2v2";
        snap.endedAtUnixMs = 1'700'000'000'000LL;
        snap.myPrimaryId = me;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Me", .team = 0, .mmr = 1020};
        snap.roster["Steam|mate"] = PlayerData{.primaryId = "Steam|mate", .name = "Mate", .team = 0, .mmr = 980};
        snap.roster["Steam|opp1"] = PlayerData{.primaryId = "Steam|opp1", .name = "Opp1", .team = 1, .mmr = 1040};
        snap.roster["Steam|opp2"] = PlayerData{.primaryId = "Steam|opp2", .name = "Opp2", .team = 1, .mmr = 1060};
        dbManager->SaveMatch(snap);
    }

    // Match 2: incomplete 2v2 lobby (opp2 has mmr=0) and local player's MMR is estimated
    {
        MatchSaveSnapshot snap;
        snap.arenaName = "Mannfield";
        snap.matchGuid = "mmr-ctx-2";
        snap.myTeam = 0;
        snap.winnerTeam = 1;
        snap.validResult = true;
        snap.playlistId = 11;
        snap.gamemode = "2v2";
        snap.endedAtUnixMs = 1'700'000'600'000LL;
        snap.myPrimaryId = me;
        snap.localMmrNeedsReconciliation = true;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Me", .team = 0, .mmr = 1030};
        snap.roster["Steam|mate"] = PlayerData{.primaryId = "Steam|mate", .name = "Mate", .team = 0, .mmr = 990};
        snap.roster["Steam|opp1"] = PlayerData{.primaryId = "Steam|opp1", .name = "Opp1", .team = 1, .mmr = 1050};
        snap.roster["Steam|opp2"] = PlayerData{.primaryId = "Steam|opp2", .name = "Opp2", .team = 1, .mmr = 0};
        dbManager->SaveMatch(snap);
    }

    // Match 3: complete 1v1 lobby
    {
        MatchSaveSnapshot snap;
        snap.arenaName = "Champions Field";
        snap.matchGuid = "mmr-ctx-3";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.playlistId = 10;
        snap.gamemode = "1v1";
        snap.endedAtUnixMs = 1'700'001'200'000LL;
        snap.myPrimaryId = me;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Me", .team = 0, .mmr = 900};
        snap.roster["Steam|duel_opp"] = PlayerData{.primaryId = "Steam|duel_opp", .name = "DuelOpp", .team = 1, .mmr = 940};
        dbManager->SaveMatch(snap);
    }

    // Match 4: played by another account, must be excluded for `me`
    {
        MatchSaveSnapshot snap;
        snap.arenaName = "Utopia Coliseum";
        snap.matchGuid = "mmr-ctx-other";
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.playlistId = 10;
        snap.gamemode = "1v1";
        snap.endedAtUnixMs = 1'700'001'800'000LL;
        snap.myPrimaryId = "Steam|someone-else";
        snap.roster["Steam|someone-else"] = PlayerData{.primaryId = "Steam|someone-else", .name = "Other", .team = 0, .mmr = 1100};
        snap.roster["Steam|other-opp"] = PlayerData{.primaryId = "Steam|other-opp", .name = "OtherOpp", .team = 1, .mmr = 1120};
        dbManager->SaveMatch(snap);
    }

    std::vector<MatchMmrContext> rows;
    dbManager->GetMatchMmrContext(me, rows);

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].playlist, "Doubles");
    EXPECT_TRUE(rows[0].win);
    EXPECT_EQ(rows[0].myMmr, 1020);
    EXPECT_FALSE(rows[0].mmrEstimated);
    EXPECT_DOUBLE_EQ(rows[0].teamAvg, 1000.0);
    EXPECT_DOUBLE_EQ(rows[0].oppAvg, 1050.0);
    EXPECT_EQ(rows[0].teamCount, 2);
    EXPECT_EQ(rows[0].oppCount, 2);

    EXPECT_EQ(rows[1].playlist, "Doubles");
    EXPECT_FALSE(rows[1].win);
    EXPECT_EQ(rows[1].myMmr, 1030);
    EXPECT_TRUE(rows[1].mmrEstimated);
    EXPECT_DOUBLE_EQ(rows[1].teamAvg, 1010.0);
    EXPECT_DOUBLE_EQ(rows[1].oppAvg, 1050.0);
    EXPECT_EQ(rows[1].teamCount, 2);
    EXPECT_EQ(rows[1].oppCount, 1);

    EXPECT_EQ(rows[2].playlist, "Duel");
    EXPECT_TRUE(rows[2].win);
    EXPECT_EQ(rows[2].myMmr, 900);
    EXPECT_FALSE(rows[2].mmrEstimated);
    EXPECT_DOUBLE_EQ(rows[2].teamAvg, 900.0);
    EXPECT_DOUBLE_EQ(rows[2].oppAvg, 940.0);
    EXPECT_EQ(rows[2].teamCount, 1);
    EXPECT_EQ(rows[2].oppCount, 1);

    const GapReport report = Insights::ComputeGapTrends(rows, "All");
    EXPECT_EQ(report.games, 2);
    EXPECT_EQ(report.wins, 2);
}

TEST_F(DatabaseManagerTest, MigrationV2ToV3CreatesSessionsTableAndMatchSessionIndex) {
    const std::string path = "test_mig_v2_to_v3.db";
    const std::string backupPath = path + ".bak-v2";
    RemoveTestDbFiles(path);
    std::filesystem::remove(backupPath);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.c_str(), &raw), SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, R"(
        CREATE TABLE Matches (
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
        CREATE TABLE MatchPlayers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            match_id INTEGER,
            primary_id TEXT,
            name TEXT,
            team INTEGER,
            mmr INTEGER,
            mmr_estimated BOOLEAN DEFAULT 0,
            is_opponent BOOLEAN DEFAULT 0
        );
        CREATE TABLE Settings (key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE MatchPlayerStats (
            match_id INTEGER NOT NULL REFERENCES Matches(id) ON DELETE CASCADE,
            primary_id TEXT NOT NULL,
            score INTEGER, goals INTEGER, assists INTEGER, saves INTEGER, shots INTEGER, demos INTEGER,
            touches INTEGER, car_touches INTEGER, max_goal_speed REAL, fastest_goal_time REAL,
            PRIMARY KEY (match_id, primary_id)
        );
        CREATE TABLE MatchLocalStats (
            match_id INTEGER PRIMARY KEY REFERENCES Matches(id) ON DELETE CASCADE,
            boost_collected INTEGER, demoed INTEGER, crossbars INTEGER, hardest_crossbar REAL,
            max_ball_speed REAL, own_goals INTEGER, duration_seconds REAL, overtime_seconds REAL,
            stats_version INTEGER NOT NULL DEFAULT 1
        );
        PRAGMA user_version = 2;
        INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, result_pending)
        VALUES ('2026-01-10 18:00:00', 'DFH Stadium', 3, 1, 1, 'v2-m1', 11, '2v2', 0, 0),
               ('2026-01-10 18:10:00', 'Mannfield', 1, 2, 0, 'v2-m2', 11, '2v2', 0, 0);
        INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent)
        VALUES (1, 'Steam|v2user', 'Hero', 0, 1100, 0, 0),
               (2, 'Steam|v2user', 'Hero', 0, 1091, 0, 0);
    )",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    sqlite3_close(raw);

    {
        auto db = std::make_shared<DatabaseManager>(sessionState);
        ASSERT_TRUE(db->Initialize(path));
        EXPECT_EQ(db->GetSchemaVersion(), 3);
        EXPECT_TRUE(std::filesystem::exists(backupPath));
        EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='Sessions';"), 1);
        EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_matches_session';"), 1);

        const auto sessions = db->ListSessions("Steam|v2user", 0, 10);
        ASSERT_EQ(sessions.size(), 1u);
        EXPECT_EQ(sessions[0].source, "backfill");
        EXPECT_EQ(sessions[0].totals.wins, 1);
        EXPECT_EQ(sessions[0].totals.losses, 1);
        ASSERT_TRUE(sessions[0].totals.mmrChangeByPlaylist.count("2v2"));
        EXPECT_EQ(sessions[0].totals.mmrChangeByPlaylist.at("2v2"), -9);
        EXPECT_EQ(QueryIntScalar(db->GetRawDb(), "SELECT COUNT(*) FROM Matches WHERE session_id IS NOT NULL;"), 2);
    }

    RemoveTestDbFiles(path);
    std::filesystem::remove(backupPath);
}

TEST_F(DatabaseManagerTest, BackfillMatchesComputeTrendsSittingCountAndIsIdempotent) {
    const std::string pid = "Steam|backfill-user";
    const int64_t t0 = 1'750'000'000;
    const int64_t gap = Insights::kSessionGapSeconds;

    // 3 sittings: [t0, t0+600, t0+1200], break >2h, [t1, t1+500], break >2h, [t2]
    const int64_t t1 = t0 + 1200 + gap + 10;
    const int64_t t2 = t1 + 500 + gap + 60;
    const std::vector<std::pair<int64_t, bool>> schedule = {
        {t0, true},
        {t0 + 600, true},
        {t0 + 1200, false},
        {t1, false},
        {t1 + 500, true},
        {t2, true},
    };

    for (size_t i = 0; i < schedule.size(); ++i) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "bf-guid-" + std::to_string(i);
        snap.playlistId = 11;
        snap.gamemode = "2v2";
        snap.myTeam = 0;
        snap.winnerTeam = schedule[i].second ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = schedule[i].second ? 3 : 1;
        snap.score[1] = schedule[i].second ? 1 : 3;
        snap.myPrimaryId = pid;
        snap.endedAtUnixMs = schedule[i].first * 1000;
        snap.roster[pid] = PlayerData{.primaryId = pid, .name = "Hero", .team = 0, .mmr = 0, .goals = 1, .saves = 2};
        snap.roster["Steam|opp"] = PlayerData{.primaryId = "Steam|opp", .name = "Opp", .team = 1, .mmr = 0};
        dbManager->SaveMatch(snap);
    }

    std::vector<MatchOutcome> outcomes;
    dbManager->GetMatchOutcomes(pid, outcomes);
    const TrendsReport trends = Insights::ComputeTrends(outcomes);
    ASSERT_EQ(trends.sessions, 3);

    const int createdFirst = dbManager->BackfillSessions(2); // chunk size 2 tests cross-chunk sitting merge
    EXPECT_EQ(createdFirst, trends.sessions);

    const int createdSecond = dbManager->BackfillSessions(2);
    EXPECT_EQ(createdSecond, 0);

    const auto sessions = dbManager->ListSessions(pid, 0, 20);
    ASSERT_EQ(static_cast<int>(sessions.size()), trends.sessions);
    EXPECT_EQ(sessions[0].totals.wins, 1);
    EXPECT_EQ(sessions[0].totals.losses, 0);
    EXPECT_FALSE(sessions[0].HasKnownMmrChange());

    EXPECT_EQ(sessions[1].totals.wins, 1);
    EXPECT_EQ(sessions[1].totals.losses, 1);

    EXPECT_EQ(sessions[2].totals.wins, 2);
    EXPECT_EQ(sessions[2].totals.losses, 1);
    EXPECT_EQ(sessions[2].totals.goals, 3);
    EXPECT_EQ(sessions[2].totals.saves, 6);
}

TEST_F(DatabaseManagerTest, LateConfirmationUpdatesStoredSessionTotals) {
    const std::string pid = "Steam|late-confirm";
    SessionRecap recap;
    recap.valid = true;
    recap.account = pid;
    recap.startedAtUnix = 1'760'000'000;
    recap.endedAtUnix = 1'760'001'200;
    recap.sessionGeneration = 4;
    recap.totals.wins = 2;
    recap.totals.losses = 0;
    recap.totals.goals = 4;
    recap.gamemodes["2v2"] = {2, 0, 2};

    const int64_t sid = dbManager->SaveSession(recap);
    ASSERT_GT(sid, 0);

    // Destroyed match from generation 4 confirms later as a loss with 1 goal.
    recap.totals.losses = 1;
    recap.totals.goals = 5;
    recap.totals.mmrChangeByPlaylist["2v2"] = 11;
    recap.gamemodes["2v2"] = {2, 1, 3};
    ASSERT_TRUE(dbManager->UpdateSession(recap));

    const auto all = dbManager->ListSessions(pid, 0, 10);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].id, sid);
    EXPECT_EQ(all[0].totals.wins, 2);
    EXPECT_EQ(all[0].totals.losses, 1);
    EXPECT_EQ(all[0].totals.goals, 5);
    EXPECT_EQ(all[0].NetMmrChange(), 11);
}
