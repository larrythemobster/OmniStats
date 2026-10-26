#include <gtest/gtest.h>
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "network/api/StatsClient.hpp"
#include "network/mmr/MmrService.hpp"
#include "database/DatabaseManager.hpp"
#include "network/http/CurlImpersonate.hpp"
#include "core/config/Config.hpp"
#include <memory>
#include <chrono>
#include <thread>
#include <cstdarg>

typedef size_t (*IntWriteCallbackType)(void*, size_t, size_t, void*);
static IntWriteCallbackType g_int_write_callback = nullptr;
static void* g_int_write_data = nullptr;
static std::string g_int_mock_response = "";
static long g_int_mock_response_code = 200;

static CURLcode int_mock_easy_setopt(CURL*, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_WRITEFUNCTION) {
        g_int_write_callback = va_arg(args, IntWriteCallbackType);
    } else if (option == CURLOPT_WRITEDATA) {
        g_int_write_data = va_arg(args, void*);
    }
    va_end(args);
    return CURLE_OK;
}

static CURLcode int_mock_easy_perform(CURL*) {
    if (g_int_write_callback && g_int_write_data && g_int_mock_response_code == 200) {
        g_int_write_callback((void*)g_int_mock_response.data(), 1, g_int_mock_response.size(), g_int_write_data);
    }
    return CURLE_OK;
}

static CURLcode int_mock_easy_getinfo(CURL*, CURLINFO info, ...) {
    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long* code = va_arg(args, long*);
        *code = g_int_mock_response_code;
    }
    va_end(args);
    return CURLE_OK;
}

static void int_mock_easy_cleanup(CURL*) {}
static void int_mock_slist_free_all(struct curl_slist*) {}
static struct curl_slist* int_mock_slist_append(struct curl_slist*, const char*) {
    return reinterpret_cast<struct curl_slist*>(1);
}
static CURL* int_mock_easy_init() {
    return reinterpret_cast<CURL*>(1);
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
        fetcher = std::make_shared<MmrService>(session);
        db = std::make_shared<DatabaseManager>(session);
        ASSERT_TRUE(db->Initialize(":memory:"));
        client = std::make_shared<StatsClient>(session, fetcher, db);

        // Setup Curl mocking for MmrService
        auto& curl = CurlImpersonate::Instance();
        original_perform = curl.easy_perform;
        original_getinfo = curl.easy_getinfo;
        original_setopt = curl.easy_setopt;
        original_cleanup = curl.easy_cleanup;
        original_slist_free_all = curl.slist_free_all;
        original_easy_escape = curl.easy_escape;
        original_free = curl.free_ptr;
        original_easy_impersonate = curl.easy_impersonate;
        original_ready = curl.IsReady();

        curl.easy_perform = int_mock_easy_perform;
        curl.easy_getinfo = int_mock_easy_getinfo;
        curl.easy_setopt = int_mock_easy_setopt;
        curl.easy_cleanup = int_mock_easy_cleanup;
        curl.slist_free_all = int_mock_slist_free_all;
        curl.slist_append = int_mock_slist_append;
        curl.easy_init = int_mock_easy_init;
        curl.easy_escape = [](CURL*, const char* val, int len) -> char* {
            static std::string escaped;
            escaped.assign(val, static_cast<size_t>(len));
            return escaped.data();
        };
        curl.free_ptr = [](void*) {};
        curl.easy_impersonate = [](CURL*, const char*, int) { return 0; };
        curl.SetReadyForTests(true);

        g_int_mock_response = R"({
            "data": {
                "segments": [
                    {
                        "type": "playlist",
                        "attributes": {"playlistId": 11},
                        "stats": {
                            "rating": {"value": 1200},
                            "tier": {"metadata": {"name": "Grand Champion I"}},
                            "division": {"metadata": {"name": "Division II"}}
                        }
                    }
                ]
            }
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
        curl.easy_escape = original_easy_escape;
        curl.free_ptr = original_free;
        curl.easy_impersonate = original_easy_impersonate;
        curl.SetReadyForTests(original_ready);
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
        session->liveMatch->UpdateMatchStateForTests([&](MatchState& ms) {
            ms.myPrimaryId = "steam|76561198000000001";
            ms.roster["steam|76561198000000001"] = PlayerData{
                .primaryId = "steam|76561198000000001",
                .name = "Hero",
                .team = 0,
                .mmr = 1200,
                .playlists = {{"1v1", 1200}},
                .playlistMatches = {{"1v1", previousMatches}}};
        });
        session->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
        session->ui.graphMmrCategory.store(MmrCategory::OneVOne);
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
    std::shared_ptr<MmrService> fetcher;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<StatsClient> client;

    pfn_curl_easy_perform original_perform;
    pfn_curl_easy_getinfo original_getinfo;
    pfn_curl_easy_setopt original_setopt;
    pfn_curl_easy_cleanup original_cleanup;
    pfn_curl_slist_free_all original_slist_free_all;
    pfn_curl_slist_append original_slist_append;
    pfn_curl_easy_init original_easy_init;
    pfn_curl_easy_escape original_easy_escape = nullptr;
    pfn_curl_free original_free = nullptr;
    pfn_curl_easy_impersonate original_easy_impersonate = nullptr;
    bool original_ready = false;
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

    session->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "steam|76561198000000001";
        ms.myTeam = 0;
    });

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
        auto snap = session->liveMatch->GetSnapshot();
        EXPECT_EQ(snap->currentMatch.savesSelf, 1);
        EXPECT_EQ(snap->currentMatch.shotsSelf, 1);
        EXPECT_EQ(snap->sessionTotals.wins, 1);
    }

    // 8. Verify the match was successfully written to SQLite
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto record = db->GetPlayerEncounterRecord("steam|76561198000000001", "epic|opponent2222");
    ASSERT_TRUE(record.has_value());
    // We were team 0, the opponent team 1, and we won.
    EXPECT_EQ(record->winsAgainst, 1);
    EXPECT_EQ(record->lossesAgainst, 0);
    EXPECT_EQ(record->GamesWith(), 0);

    // Finalization queues a lookup behind the save, so the live roster reflects
    // the stored record without in-memory increments.
    auto snap = session->liveMatch->GetSnapshot();
    const auto opponent = snap->roster.find("epic|opponent2222");
    ASSERT_NE(opponent, snap->roster.end());
    EXPECT_TRUE(opponent->second.hasLifetimeData);
    EXPECT_EQ(opponent->second.lifetimeWinsAgainst, 1);
    EXPECT_EQ(opponent->second.lifetimeWinsWith + opponent->second.lifetimeLossesWith, 0);
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

    session->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "steam|76561198000000001";
        ms.myTeam = 0;
        ms.localPlayerWasActive = true;
    });

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
        EXPECT_EQ(session->liveMatch->GetSnapshot()->sessionTotals.wins, 1);
        EXPECT_EQ(session->history.GetSnapshot()->recentSavedMatches.size(), 1);
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
        EXPECT_EQ(session->liveMatch->GetSnapshot()->sessionTotals.wins, 1);
        EXPECT_EQ(session->history.GetSnapshot()->recentSavedMatches.size(), 1);
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
        fetcher->HasPendingDestroyedMatch(
            "pipeline-replay-forfeit-guid"));
    {
        auto snap = session->liveMatch->GetSnapshot();
        EXPECT_EQ(snap->sessionTotals.wins, 0);
        EXPECT_EQ(snap->sessionTotals.losses, 1);
    }
    {
        const auto points =
            fetcher->PlaylistMatchPoints("1v1");
        ASSERT_EQ(points.size(), 1u);
        EXPECT_EQ(
            points[0].matchGuid,
            "pipeline-replay-forfeit-guid");
        EXPECT_EQ(points[0].mmr, 1191);
        EXPECT_FALSE(points[0].trackerCovered);
        EXPECT_TRUE(points[0].valueEstimated);
    }

    fetcher->ProcessPostMatchResponse(
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
        fetcher->PlaylistMatchPoints("1v1");
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
        fetcher->PlaylistMatchPoints("1v1").size(),
        1u);
    {
        auto snap = session->liveMatch->GetSnapshot();
        EXPECT_EQ(snap->sessionTotals.losses, 1);
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
        fetcher->HasPendingDestroyedMatch(
            "pipeline-catch-up-a"));

    std::this_thread::sleep_for(
        std::chrono::milliseconds(2));
    StartRankedOnesMatch(
        "pipeline-catch-up-b", 2, 1, 11);
    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"pipeline-catch-up-b","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":2},{"TeamNum":1,"Score":1}]}})");

    fetcher->ProcessPostMatchResponse(
        "pipeline-catch-up-b", 1200, 13);
    ASSERT_TRUE(WaitForDatabase(
        "pipeline_cumulative_catch_up_barrier"));

    {
        auto snap = session->liveMatch->GetSnapshot();
        EXPECT_EQ(snap->sessionTotals.wins, 1);
        EXPECT_EQ(snap->sessionTotals.losses, 1);
    }

    const auto points =
        fetcher->PlaylistMatchPoints("1v1");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "pipeline-catch-up-a");
    EXPECT_EQ(points[0].mmr, 1191);
    EXPECT_EQ(points[1].matchGuid, "pipeline-catch-up-b");
    EXPECT_EQ(points[1].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatch(
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
        auto snap = session->liveMatch->GetSnapshot();
        generationB = snap->activeMatchGeneration;
        ASSERT_EQ(snap->matchGuid, "partial-created-pipeline-b");
        ASSERT_TRUE(snap->roundEverStarted);
    }

    client->HandleLine(
        R"({"Event":"MatchCreated","Data":{}})");

    {
        auto snap = session->liveMatch->GetSnapshot();
        EXPECT_EQ(snap->activeMatchGeneration, generationB);
        EXPECT_EQ(snap->matchGuid, "partial-created-pipeline-b");
        EXPECT_TRUE(snap->roundEverStarted);
        EXPECT_TRUE(snap->legacyLobbyWasEverFull);
        EXPECT_EQ(snap->score[0], 3);
        EXPECT_EQ(snap->score[1], 2);
    }

    client->HandleLine(
        R"({"Event":"MatchEnded","Data":{"MatchGuid":"partial-created-pipeline-b","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":3},{"TeamNum":1,"Score":2}]}})");
    client->HandleLine(
        R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"partial-created-pipeline-b"}})");

    fetcher->ProcessPostMatchResponse(
        "partial-created-pipeline-b", 1218, 19);
    ASSERT_TRUE(WaitForDatabase(
        "partial_created_pipeline_barrier"));

    {
        EXPECT_EQ(session->liveMatch->GetSnapshot()->sessionTotals.wins, 2);
        EXPECT_EQ(session->liveMatch->GetSnapshot()->sessionTotals.losses, 0);
        EXPECT_EQ(session->history.GetSnapshot()->recentSavedMatches.size(), 2u);
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
        fetcher->PlaylistMatchPoints("1v1");
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

TEST_F(OmniStatsPipelineTest, TelemetryReducerPersistsPerPlayerAndLocalStatsAndPendingConfirmationYieldsOneStatsRow) {
    const std::string heroId = "steam|76561198000000001";
    const std::string oppId = "epic|opponent2222";

    StartRankedOnesMatch("pipeline-v2-stats-guid", 2, 1, 20);

    // Boost pickup (+33)
    client->HandleLine(R"({
        "Event": "UpdateState",
        "Data": {
            "Game": { "bReplay": false },
            "Players": [
                { "PrimaryId": "steam|76561198000000001", "Name": "Hero", "TeamNum": 0, "Boost": 20 },
                { "PrimaryId": "epic|opponent2222", "Name": "Opponent", "TeamNum": 1, "Boost": 80 }
            ]
        }
    })");
    client->HandleLine(R"({
        "Event": "UpdateState",
        "Data": {
            "Game": { "bReplay": false },
            "Players": [
                { "PrimaryId": "steam|76561198000000001", "Name": "Hero", "TeamNum": 0, "Boost": 65 },
                { "PrimaryId": "epic|opponent2222", "Name": "Opponent", "TeamNum": 1, "Boost": 80 }
            ]
        }
    })");

    // Statfeed & Goal events
    client->HandleLine(R"({"Event":"StatfeedEvent","Data":{"EventName":"Save","Player":{"PrimaryId":"steam|76561198000000001","Name":"Hero"}}})");
    client->HandleLine(R"({"Event":"StatfeedEvent","Data":{"EventName":"Shot","Player":{"PrimaryId":"steam|76561198000000001","Name":"Hero"}}})");
    client->HandleLine(R"({"Event":"StatfeedEvent","Data":{"EventName":"Demolish","MainTarget":{"PrimaryId":"steam|76561198000000001","Name":"Hero"},"SecondaryTarget":{"PrimaryId":"epic|opponent2222","Name":"Opponent"}}})");
    client->HandleLine(R"({"Event":"GoalScored","Data":{"Scorer":{"PrimaryId":"steam|76561198000000001","Name":"Hero"},"GoalSpeed":104.5,"GoalTime":19.0}})");
    client->HandleLine(R"({"Event":"GoalScored","Data":{"Scorer":{"PrimaryId":"epic|opponent2222","Name":"Opponent"},"GoalSpeed":95.0,"GoalTime":42.0}})");
    client->HandleLine(R"({"Event":"CrossbarHit","Data":{"ImpactForce":1200.0,"BallLastTouch":{"Player":{"PrimaryId":"steam|76561198000000001","Name":"Hero"}}}})");
    client->HandleLine(R"({"Event":"BallHit","Data":{"Ball":{"PostHitSpeed":115.0},"Players":[{"PrimaryId":"steam|76561198000000001","Name":"Hero"}]}})");

    // Destroyed before MatchEnded creates provisional pending row, then MatchEnded confirms it
    client->HandleLine(R"({"Event":"MatchDestroyed","Data":{"MatchGuid":"pipeline-v2-stats-guid"}})");
    client->HandleLine(R"({"Event":"MatchEnded","Data":{"MatchGuid":"pipeline-v2-stats-guid","WinnerTeamNum":0,"Teams":[{"TeamNum":0,"Score":2},{"TeamNum":1,"Score":1}]}})");
    ASSERT_TRUE(WaitForDatabase("pipeline_v2_stats_barrier"));

    const auto heroSeries = db->GetPlayerStatSeries(heroId, "1v1", 10);
    ASSERT_EQ(heroSeries.size(), 1u);
    EXPECT_EQ(heroSeries[0].matchGuid, "pipeline-v2-stats-guid");
    EXPECT_TRUE(heroSeries[0].win);
    EXPECT_EQ(heroSeries[0].goals, 1);
    EXPECT_EQ(heroSeries[0].saves, 1);
    EXPECT_EQ(heroSeries[0].shots, 1);
    EXPECT_EQ(heroSeries[0].demos, 1);
    ASSERT_TRUE(heroSeries[0].maxGoalSpeed.has_value());
    EXPECT_FLOAT_EQ(*heroSeries[0].maxGoalSpeed, 104.5f);
    ASSERT_TRUE(heroSeries[0].fastestGoalTime.has_value());
    EXPECT_FLOAT_EQ(*heroSeries[0].fastestGoalTime, 19.0f);
    EXPECT_EQ(heroSeries[0].boostCollected, 45);
    EXPECT_EQ(heroSeries[0].crossbars, 1);
    ASSERT_TRUE(heroSeries[0].hardestCrossbar.has_value());
    EXPECT_FLOAT_EQ(*heroSeries[0].hardestCrossbar, 1200.0f);
    ASSERT_TRUE(heroSeries[0].maxBallSpeed.has_value());
    EXPECT_FLOAT_EQ(*heroSeries[0].maxBallSpeed, 115.0f);

    const auto oppSeries = db->GetPlayerStatSeries(oppId, "1v1", 10);
    ASSERT_EQ(oppSeries.size(), 1u);
    EXPECT_EQ(oppSeries[0].goals, 1);

    sqlite3_stmt* stmt = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db->GetRawDb(), "SELECT COUNT(*) FROM MatchLocalStats;", -1, &stmt, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    EXPECT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);
}
