#include <benchmark/benchmark.h>
#include "network/StatsClient.hpp"
#include "core/SessionState.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include <memory>

static void BM_ParseUpdateState(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(session);
    auto db = std::make_shared<DatabaseManager>(session);
    db->Initialize(":memory:");
    StatsClient client(session, fetcher, db);

    // Simulated Rocket League state update JSON line
    std::string line = R"({
        "Event": "game:update_state",
        "Data": {
            "Game": {
                "Arena": "stadium_p",
                "bReplay": false,
                "bSpectator": false,
                "Teams": [
                    {"TeamNum": 0, "Score": 3},
                    {"TeamNum": 1, "Score": 2}
                ]
            },
            "Players": [
                {
                    "PrimaryId": "steam|123456789",
                    "Name": "Player1",
                    "TeamNum": 0,
                    "Boost": 100
                },
                {
                    "PrimaryId": "epic|987654321",
                    "Name": "Player2",
                    "TeamNum": 1,
                    "Boost": 50
                }
            ]
        }
    })";

    for (auto _ : state) {
        client.HandleLine(line);
    }
}
BENCHMARK(BM_ParseUpdateState)->Unit(benchmark::kMicrosecond);

static void BM_ParseStatFeed(benchmark::State& state) {
    auto session = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(session);
    auto db = std::make_shared<DatabaseManager>(session);
    db->Initialize(":memory:");
    StatsClient client(session, fetcher, db);

    // Preset player in roster
    {
        std::unique_lock<std::shared_mutex> lock(session->game.mutex);
        session->game.roster["steam|123456789"] = PlayerData{
            .primaryId = "steam|123456789",
            .name = "Player1",
            .team = 0};
    }

    std::string line = R"({
        "Event": "game:statfeed",
        "Data": {
            "EventName": "Save",
            "Player": {
                "PrimaryId": "steam|123456789",
                "Name": "Player1"
            }
        }
    })";

    for (auto _ : state) {
        client.HandleLine(line);
    }
}
BENCHMARK(BM_ParseStatFeed)->Unit(benchmark::kMicrosecond);
