#include <gtest/gtest.h>
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include <memory>
#include <filesystem>
#include <chrono>
#include <thread>

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
