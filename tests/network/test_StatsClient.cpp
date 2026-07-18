#include <gtest/gtest.h>
#include "network/StatsClient.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include <memory>

TEST(StatsClientTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    db->Initialize(":memory:");

    StatsClient client(state, fetcher, db);

    EXPECT_NO_THROW(client.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NO_THROW(client.Stop());
}

class MockDiscordManager : public DiscordManager {
  public:
    MockDiscordManager(std::shared_ptr<SessionState> s) : DiscordManager(s) {}
    void PushPresenceUpdate(const DiscordPresenceSnapshot& snap) override {
        pushCount++;
    }
    std::atomic<int> pushCount{0};
};

TEST(StatsClientTest, MatchEndNonBlockingAndDiscordPush) {
    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    db->Initialize(":memory:");

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    // Setup state
    state->game.myPrimaryId = "Steam|123";
    state->game.myTeam = 0;
    state->game.arenaName = "Stadium";
    state->game.matchGuid = "test_guid";
    state->game.roster["Steam|123"] = PlayerData{.primaryId = "Steam|123", .name = "Me", .team = 0, .mmr = 1000};

    // Send match ended event
    std::string matchEndedJson = R"({"Event": "MatchEnded", "Data": {"WinnerTeamNum": 0}})";

    auto tStart = std::chrono::steady_clock::now();
    client.HandleLine(matchEndedJson);
    auto duration = std::chrono::steady_clock::now() - tStart;

    // Verify it returned extremely fast (non-blocking)
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count(), 50);

    // Verify discord push count is exactly 1
    EXPECT_EQ(discord->pushCount.load(), 1);

    // Stop client to drain side effects queue
    client.Stop();
}

TEST(StatsClientTest, DisconnectClearsDiscordPresence) {
    ConfigData backup = Config::Read();
    Config::Update([](ConfigData& c) { c.host = "not_an_ip"; }, false);

    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    db->Initialize(":memory:");

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    client.Start();

    for (int i = 0; i < 100 && discord->pushCount.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(discord->pushCount.load(), 1);

    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);
}
