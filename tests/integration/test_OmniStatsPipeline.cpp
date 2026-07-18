#include <gtest/gtest.h>
#include "core/SessionState.hpp"
#include "network/StatsClient.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "network/CurlImpersonate.hpp"
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
    }

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
    client->HandleLine(R"({"Event": "MatchDestroyed", "Data": {}})");

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
