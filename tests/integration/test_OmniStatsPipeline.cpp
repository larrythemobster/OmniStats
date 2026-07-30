#include <gtest/gtest.h>
#include "core/SessionState.hpp"
#include "network/StatsClient.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "network/CurlImpersonate.hpp"
#include "core/Config.hpp"
#include <memory>
#include <chrono>
#include <thread>
#include <cstdarg>

typedef size_t (*IntWriteCallbackType)(void*, size_t, size_t, void*);
static IntWriteCallbackType g_int_write_callback = nullptr;
static void* g_int_write_data = nullptr;
static std::string g_int_mock_response = "";
static long g_int_mock_response_code = 200;

static int int_mock_easy_setopt(void* curl, int option, ...) {
    va_list args;
    va_start(args, option);
    if (option == CI_CURLOPT_WRITEFUNCTION) {
        g_int_write_callback = va_arg(args, IntWriteCallbackType);
    } else if (option == CI_CURLOPT_WRITEDATA) {
        g_int_write_data = va_arg(args, void*);
    }
    va_end(args);
    return 0;
}

static int int_mock_easy_perform(void* curl) {
    if (g_int_write_callback && g_int_write_data && g_int_mock_response_code == 200) {
        g_int_write_callback((void*)g_int_mock_response.data(), 1, g_int_mock_response.size(), g_int_write_data);
    }
    return 0; // CURLE_OK
}

static int int_mock_easy_getinfo(void* curl, int info, ...) {
    va_list args;
    va_start(args, info);
    if (info == CI_CURLINFO_RESPONSE_CODE) {
        long* code = va_arg(args, long*);
        *code = g_int_mock_response_code;
    }
    va_end(args);
    return 0;
}

static void int_mock_easy_cleanup(void* curl) {}
static void int_mock_slist_free_all(void* list) {}
static void* int_mock_slist_append(void* list, const char* str) {
    return (void*)1;
}
static void* int_mock_easy_init() {
    return (void*)1;
}

class OmniStatsPipelineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        originalConfig = Config::Read();
        Config::Update(
            [](ConfigData& config) {
                config.enable_mmr_tracking = true;
            },
            false);
        session = std::make_shared<SessionState>();
        fetcher = std::make_shared<MMRFetcher>(session);
        db = std::make_shared<DatabaseManager>(session);
        db->Initialize(":memory:");
        client = std::make_shared<StatsClient>(session, fetcher, db);

        // Setup Curl mocking for MMRFetcher
        auto& curl = CurlImpersonate::Instance();
        original_perform = curl.easy_perform;
        original_getinfo = curl.easy_getinfo;
        original_setopt = curl.easy_setopt;
        original_cleanup = curl.easy_cleanup;
        original_slist_free_all = curl.slist_free_all;
        original_slist_append = curl.slist_append;
        original_easy_init = curl.easy_init;

        curl.easy_perform = int_mock_easy_perform;
        curl.easy_getinfo = int_mock_easy_getinfo;
        curl.easy_setopt = int_mock_easy_setopt;
        curl.easy_cleanup = int_mock_easy_cleanup;
        curl.slist_free_all = int_mock_slist_free_all;
        curl.slist_append = int_mock_slist_append;
        curl.easy_init = int_mock_easy_init;

        g_int_mock_response = R"({
            "current_mmr": 1250,
            "playlist": "2v2"
        })";
        g_int_mock_response_code = 200;
    }

    void TearDown() override {
        auto& curl = CurlImpersonate::Instance();
        curl.easy_perform = original_perform;
        curl.easy_getinfo = original_getinfo;
        curl.easy_setopt = original_setopt;
        curl.easy_cleanup = original_cleanup;
        curl.slist_free_all = original_slist_free_all;
        curl.slist_append = original_slist_append;
        curl.easy_init = original_easy_init;
        Config::Update(
            [this](ConfigData& config) {
                config = originalConfig;
            },
            false);
    }

    void StartRankedOnesMatch(
        const std::string& matchGuid,
        int localScore,
        int opponentScore,
        int previousMatches = 11) {
        {
            std::unique_lock<std::shared_mutex> lock(
                session->game.mutex);
            session->game.myPrimaryId =
                "steam|76561198000000001";
            session->game.roster
                ["steam|76561198000000001"] =
                PlayerData{
                    .primaryId =
                        "steam|76561198000000001",
                    .name = "Hero",
                    .team = 0,
                    .mmr = 1200,
                    .playlists = {{"1v1", 1200}},
                    .playlistMatches = {{"1v1", previousMatches}}};
            session->ui.rosterMmrCategory.store(
                MmrCategory::OneVOne);
            session->ui.graphMmrCategory.store(
                MmrCategory::OneVOne);
        }

        client->HandleLine(
            nlohmann::json{
                {"Event", "MatchCreated"},
                {"Data", {{"MatchGuid", matchGuid}}}}
                .dump());
        client->HandleLine(
            nlohmann::json{
                {"Event", "UpdateState"},
                {"Data",
                 {{"Game",
                   {{"Arena", "stadium_p"},
                    {"bReplay", false},
                    {"bSpectator", false},
                    {"Teams",
                     nlohmann::json::array(
                         {{{"TeamNum", 0},
                           {"Score", localScore}},
                          {{"TeamNum", 1},
                           {"Score", opponentScore}}})}}},
                  {"Players",
                   nlohmann::json::array(
                       {{{"PrimaryId",
                          "steam|76561198000000001"},
                         {"Name", "Hero"},
                         {"TeamNum", 0},
                         {"Boost", 100}},
                        {{"PrimaryId",
                          "epic|opponent2222"},
                         {"Name", "Opponent"},
                         {"TeamNum", 1},
                         {"Boost", 80}}})}}}}
                .dump());
        client->HandleLine(
            R"({"Event":"RoundStarted","Data":{}})");
    }

    bool WaitForDatabase(const std::string& barrierKey) {
        db->AsyncSetSetting(barrierKey, "complete");
        for (int attempt = 0;
             attempt < 200 &&
             db->GetSetting(barrierKey, "") != "complete";
             ++attempt) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }
        return db->GetSetting(barrierKey, "") == "complete";
    }

    ConfigData originalConfig;
    std::shared_ptr<SessionState> session;
    std::shared_ptr<MMRFetcher> fetcher;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<StatsClient> client;

    pfn_curl_easy_perform original_perform;
    pfn_curl_easy_getinfo original_getinfo;
    pfn_curl_easy_setopt original_setopt;
    pfn_curl_easy_cleanup original_cleanup;
    pfn_curl_slist_free_all original_slist_free_all;
    pfn_curl_slist_append original_slist_append;
    pfn_curl_easy_init original_easy_init;
};

TEST_F(OmniStatsPipelineTest, FullLifecycleMatchFlow) {
    // 1. Game Created
    client->HandleLine(R"({
        "Event": "MatchCreated",
        "Data": {
            "MatchGuid": "match-guid-pipeline-123"
        }
    })");

    // 2. State update containing our primary id and opponent
    client->HandleLine(R"({
        "Event": "UpdateState",
        "Data": {
            "Game": {
                "Arena": "stadium_p",
                "bReplay": false,
                "Teams": [
                    {"TeamNum": 0, "Score": 0},
                    {"TeamNum": 1, "Score": 0}
                ]
            },
            "Players": [
                {
                    "PrimaryId": "steam|76561198000000001",
                    "Name": "Hero",
                    "TeamNum": 0,
                    "Boost": 100
                },
                {
                    "PrimaryId": "epic|opponent2222",
                    "Name": "Villian",
                    "TeamNum": 1,
                    "Boost": 80
                }
            ]
        }
    })");

    // Let the process of elimination identify "steam|76561198000000001" as local player
    // Since there is only one PC platform player with boost in team 0
    {
        std::unique_lock<std::shared_mutex> lock(session->game.mutex);
        session->game.myPrimaryId = "steam|76561198000000001";
        session->game.myTeam = 0;
    }

    // 3. Round Starts
    client->HandleLine(R"({"Event": "RoundStarted", "Data": {}})");

    // 4. Boost pick up event
    client->HandleLine(R"({
        "Event": "UpdateState",
        "Data": {
            "Game": { "bReplay": false },
            "Players": [
                { "PrimaryId": "steam|76561198000000001", "Name": "Hero", "TeamNum": 0, "Boost": 33 },
                { "PrimaryId": "epic|opponent2222", "Name": "Villian", "TeamNum": 1, "Boost": 100 }
            ]
        }
    })");

    // 5. Stat feeds (Goals, Saves)
    client->HandleLine(R"({
        "Event": "StatfeedEvent",
        "Data": {
            "EventName": "Save",
            "Player": { "PrimaryId": "steam|76561198000000001", "Name": "Hero" }
        }
    })");

    client->HandleLine(R"({
        "Event": "StatfeedEvent",
        "Data": {
            "EventName": "Shot",
            "Player": { "PrimaryId": "steam|76561198000000001", "Name": "Hero" }
        }
    })");

    // 6. Match Ended (Score 3-2 for Hero's team)
    client->HandleLine(R"({
        "Event": "MatchEnded",
        "Data": {
            "MatchGuid": "match-guid-pipeline-123",
            "WinnerTeamNum": 0,
            "Teams": [
                {"TeamNum": 0, "Score": 3},
                {"TeamNum": 1, "Score": 2}
            ]
        }
    })");

    // 7. Verify in-memory SessionState updates
    {
        std::shared_lock<std::shared_mutex> lock(session->game.mutex);
        EXPECT_EQ(session->game.currentMatch.savesSelf, 1);
        EXPECT_EQ(session->game.currentMatch.shotsSelf, 1);
        EXPECT_EQ(session->game.sessionTotals.wins, 1);
    }

    // 8. Verify the match was successfully written to SQLite
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int winsWith = 0, lossesWith = 0, winsAgainst = 0, lossesAgainst = 0;
    db->GetPlayerEncounterRecord("epic|opponent2222", winsWith, lossesWith, winsAgainst, lossesAgainst);

    // We were team 0, opponent was team 1. We won.
    // So we won against the opponent.
    EXPECT_EQ(winsAgainst, 1);
    EXPECT_EQ(lossesAgainst, 0);
}

TEST_F(OmniStatsPipelineTest, MatchEndedFollowedByMatchDestroyedDoesNotDoubleSave) {
    // 1. MatchCreated
    client->HandleLine(R"({
        "Event": "MatchCreated",
        "Data": {
            "MatchGuid": "double-save-pipeline-guid"
        }
    })");

    // 2. UpdateState with full lobby
    client->HandleLine(R"({
        "Event": "UpdateState",
        "Data": {
            "Game": {
                "Arena": "stadium_p",
                "bReplay": false,
                "Teams": [
                    {"TeamNum": 0, "Score": 0},
                    {"TeamNum": 1, "Score": 0}
                ]
            },
            "Players": [
                {
                    "PrimaryId": "steam|76561198000000001",
                    "Name": "Hero",
                    "TeamNum": 0,
                    "Boost": 100
                },
                {
                    "PrimaryId": "epic|opponent2222",
                    "Name": "Villian",
                    "TeamNum": 1,
                    "Boost": 80
                }
            ]
        }
    })");

    {
        std::unique_lock<std::shared_mutex> lock(session->game.mutex);
        session->game.myPrimaryId = "steam|76561198000000001";
        session->game.myTeam = 0;
        session->game.localPlayerWasActive = true;
    }

    // 3. Round Starts
    client->HandleLine(R"({"Event": "RoundStarted", "Data": {}})");

    // 4. Match Ended (Score 3-2 for Hero's team)
    client->HandleLine(R"({
        "Event": "MatchEnded",
        "Data": {
            "MatchGuid": "double-save-pipeline-guid",
            "WinnerTeamNum": 0,
            "Teams": [
                {"TeamNum": 0, "Score": 3},
                {"TeamNum": 1, "Score": 2}
            ]
        }
    })");

    // Wait for the async database write to complete
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify session totals and recent history size
    {
        std::shared_lock<std::shared_mutex> lock1(session->game.mutex);
        std::shared_lock<std::shared_mutex> lock2(session->history.mutex);
        EXPECT_EQ(session->game.sessionTotals.wins, 1);
        EXPECT_EQ(session->history.recentSavedMatches.size(), 1);
    }

    // Check DB to verify there is only 1 match record
    {
        std::vector<SessionMatchSummary> matches;
        db->GetRecentMatchHistory("steam|76561198000000001", matches, 5);
        EXPECT_EQ(matches.size(), 1);
    }

    // 5. Match Destroyed (Back to Menu)
    client->HandleLine(R"({"Event": "MatchDestroyed", "Data": {"MatchGuid":"double-save-pipeline-guid"}})");

    // Wait again to make sure no second save was enqueued/processed
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify no second save, no second win, no duplicate recent saved match
    {
        std::shared_lock<std::shared_mutex> lock1(session->game.mutex);
        std::shared_lock<std::shared_mutex> lock2(session->history.mutex);
        EXPECT_EQ(session->game.sessionTotals.wins, 1);
        EXPECT_EQ(session->history.recentSavedMatches.size(), 1);
    }

    // Check DB to verify there is still only 1 match record
    {
        std::vector<SessionMatchSummary> matches;
        db->GetRecentMatchHistory("steam|76561198000000001", matches, 5);
        EXPECT_EQ(matches.size(), 1);
    }
}

TEST_F(
    OmniStatsPipelineTest,
    ReplayForfeitCreatesEstimatedLossPointBeforeTrackerPublishes) {
    StartRankedOnesMatch("", 3, 3, 50);

    client->HandleLine(
        R"({"Event":"UpdateState","Data":{"Game":{"bReplay":true,"bSpectator":false}}})");

    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"pipeline-replay-forfeit-guid","bLocalPlayerForfeit":true}})");

    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "pipeline-replay-forfeit-guid"));
    {
        std::shared_lock<std::shared_mutex> lock(
            session->game.mutex);
        EXPECT_EQ(session->game.sessionTotals.wins, 0);
        EXPECT_EQ(session->game.sessionTotals.losses, 1);
    }
    {
        const auto points =
            fetcher->PlaylistMatchPointsForTests("1v1");
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(
            points[0].matchGuid,
            "pipeline-replay-forfeit-guid");
        EXPECT_EQ(points[0].mmr, 1191);
        EXPECT_FALSE(points[0].trackerCovered);
        EXPECT_TRUE(points[0].valueEstimated);
    }

    fetcher->ProcessPostMatchResponseForTests(
        "pipeline-replay-forfeit-guid", 1191, 51);
    ASSERT_TRUE(WaitForDatabase(
        "pipeline_replay_forfeit_barrier"));

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(
        "steam|76561198000000001", matches, 5);
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_FALSE(matches[0].win);
    {
        std::lock_guard<std::mutex> lock(
            session->ui.dbStatsMutex);
        EXPECT_EQ(
            session->ui.cachedDbStats.currentWins, 0);
        EXPECT_EQ(
            session->ui.cachedDbStats.currentLosses, 1);
        EXPECT_EQ(
            session->ui.cachedDbStats.longestLosses, 1);
    }
    const auto confirmedPoints =
        fetcher->PlaylistMatchPointsForTests("1v1");
    ASSERT_EQ(confirmedPoints.size(), 1u);
    EXPECT_EQ(confirmedPoints[0].mmr, 1191);
    EXPECT_TRUE(confirmedPoints[0].trackerCovered);
    EXPECT_FALSE(confirmedPoints[0].valueEstimated);

    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"pipeline-replay-forfeit-guid","WinnerTeamNum":1}})");
    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"pipeline-replay-forfeit-guid"}})");
    ASSERT_TRUE(WaitForDatabase(
        "pipeline_replay_forfeit_duplicate_barrier"));

    matches.clear();
    db->GetRecentMatchHistory(
        "steam|76561198000000001", matches, 5);
    EXPECT_EQ(matches.size(), 1u);
    EXPECT_EQ(
        fetcher->PlaylistMatchPointsForTests("1v1").size(),
        1u);
    {
        std::shared_lock<std::shared_mutex> lock(
            session->game.mutex);
        EXPECT_EQ(session->game.sessionTotals.losses, 1);
    }
}

TEST_F(
    OmniStatsPipelineTest,
    CumulativeTrackerCatchUpKeepsReplayForfeitLossBeforeNormalWin) {
    StartRankedOnesMatch(
        "pipeline-catch-up-a", 1, 1, 11);
    client->HandleLine(
        R"({"Event":"UpdateState","Data":{"Game":{"bReplay":true,"bSpectator":false}}})");
    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"pipeline-catch-up-a","bLocalPlayerForfeit":true}})");
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "pipeline-catch-up-a"));

    std::this_thread::sleep_for(
        std::chrono::milliseconds(2));
    StartRankedOnesMatch(
        "pipeline-catch-up-b", 2, 1, 11);
    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"pipeline-catch-up-b","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":2},{"TeamNum":1,"Score":1}]}})");

    fetcher->ProcessPostMatchResponseForTests(
        "pipeline-catch-up-b", 1200, 13);
    ASSERT_TRUE(WaitForDatabase(
        "pipeline_cumulative_catch_up_barrier"));

    {
        std::shared_lock<std::shared_mutex> lock(
            session->game.mutex);
        EXPECT_EQ(session->game.sessionTotals.wins, 1);
        EXPECT_EQ(session->game.sessionTotals.losses, 1);
    }

    const auto points =
        fetcher->PlaylistMatchPointsForTests("1v1");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "pipeline-catch-up-a");
    EXPECT_EQ(points[0].mmr, 1199);
    EXPECT_EQ(points[1].matchGuid, "pipeline-catch-up-b");
    EXPECT_EQ(points[1].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "pipeline-catch-up-a"));

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(
        "steam|76561198000000001", matches, 5);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_TRUE(matches[0].win);
    EXPECT_FALSE(matches[1].win);
    {
        std::lock_guard<std::mutex> lock(
            session->ui.dbStatsMutex);
        EXPECT_EQ(
            session->ui.cachedDbStats.currentWins, 1);
        EXPECT_EQ(
            session->ui.cachedDbStats.currentLosses, 0);
        EXPECT_EQ(
            session->ui.cachedDbStats.longestWins, 1);
        EXPECT_EQ(
            session->ui.cachedDbStats.longestLosses, 1);
    }

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            db->GetRawDb(),
            "SELECT COUNT(DISTINCT match_guid) "
            "FROM Matches WHERE match_guid IN "
            "('pipeline-catch-up-a', "
            "'pipeline-catch-up-b');",
            -1,
            &stmt,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);
}

TEST_F(
    OmniStatsPipelineTest,
    PartialMatchCreatedKeepsTwoSavedMatchesAndOwnedMmrPoints) {
    StartRankedOnesMatch(
        "partial-created-pipeline-a", 2, 1, 17);
    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"partial-created-pipeline-a","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":2},{"TeamNum":1,"Score":1}]}})");
    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"partial-created-pipeline-a"}})");

    std::this_thread::sleep_for(
        std::chrono::milliseconds(2));
    StartRankedOnesMatch(
        "partial-created-pipeline-b", 3, 2, 17);
    uint64_t generationB = 0;
    {
        std::shared_lock<std::shared_mutex> lock(
            session->game.mutex);
        generationB =
            session->game.activeMatchGeneration;
        ASSERT_EQ(
            session->game.matchGuid,
            "partial-created-pipeline-b");
        ASSERT_TRUE(session->game.roundEverStarted);
    }

    client->HandleLine(
        R"({"Event":"MatchCreated","Data":{}})");

    {
        std::shared_lock<std::shared_mutex> lock(
            session->game.mutex);
        EXPECT_EQ(
            session->game.activeMatchGeneration,
            generationB);
        EXPECT_EQ(
            session->game.matchGuid,
            "partial-created-pipeline-b");
        EXPECT_TRUE(session->game.roundEverStarted);
        EXPECT_TRUE(session->game.lobbyWasEverFull);
        EXPECT_EQ(session->game.score[0], 3);
        EXPECT_EQ(session->game.score[1], 2);
    }

    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"partial-created-pipeline-b","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":3},{"TeamNum":1,"Score":2}]}})");
    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"partial-created-pipeline-b"}})");

    fetcher->ProcessPostMatchResponseForTests(
        "partial-created-pipeline-b", 1218, 19);
    ASSERT_TRUE(WaitForDatabase(
        "partial_created_pipeline_barrier"));

    {
        std::shared_lock<std::shared_mutex> gameLock(
            session->game.mutex);
        std::shared_lock<std::shared_mutex> historyLock(
            session->history.mutex);
        EXPECT_EQ(session->game.sessionTotals.wins, 2);
        EXPECT_EQ(session->game.sessionTotals.losses, 0);
        EXPECT_EQ(
            session->history.recentSavedMatches.size(),
            2u);
    }

    std::vector<SessionMatchSummary> matches;
    db->GetRecentMatchHistory(
        "steam|76561198000000001", matches, 5);
    ASSERT_EQ(matches.size(), 2u);
    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(
            db->GetRawDb(),
            "SELECT COUNT(DISTINCT match_guid) "
            "FROM Matches WHERE match_guid IN "
            "('partial-created-pipeline-a', "
            "'partial-created-pipeline-b');",
            -1,
            &stmt,
            nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    const auto points =
        fetcher->PlaylistMatchPointsForTests("1v1");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(
        points[0].matchGuid,
        "partial-created-pipeline-a");
    EXPECT_EQ(
        points[1].matchGuid,
        "partial-created-pipeline-b");
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[1].trackerCovered);
}
