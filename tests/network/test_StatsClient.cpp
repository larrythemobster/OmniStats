#include <gtest/gtest.h>
#include "network/StatsClient.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

TEST(StatsClientTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

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
    ASSERT_TRUE(db->Initialize(":memory:"));

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    // Setup state
    state->game.myPrimaryId = "Steam|123";
    state->game.myTeam = 0;
    state->game.arenaName = "Stadium";
    state->game.matchGuid = "test_guid";
    state->game.inMatch = true;
    state->game.roster["Steam|123"] = PlayerData{.primaryId = "Steam|123", .name = "Me", .team = 0, .mmr = 1000};

    // Send match ended event
    std::string matchEndedJson = R"({"Event": "MatchEnded", "Data": {"MatchGuid": "test_guid", "WinnerTeamNum": 0}})";

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
    ASSERT_TRUE(db->Initialize(":memory:"));

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    // Seed active-match state that must not leak across a socket reconnect.
    state->game.inMatch = true;
    state->game.matchGuid = "stale-disconnect-guid";
    state->game.playlistId = 11;
    state->game.legacyMaxPlayersSeen = 4;
    state->game.legacyMaxTeamPlayersSeen = {2, 2};
    state->game.roundEverStarted = true;
    state->game.currentMatch.goals = 3;
    state->game.roster["Steam|1"] = PlayerData{
        .primaryId = "Steam|1", .name = "Me", .team = 0};

    client.Start();

    for (int i = 0; i < 100 && discord->pushCount.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(discord->pushCount.load(), 1);
    EXPECT_FALSE(state->game.inMatch);
    EXPECT_TRUE(state->game.matchGuid.empty());
    EXPECT_EQ(state->game.playlistId, -1);
    EXPECT_EQ(state->game.legacyMaxPlayersSeen, 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[0], 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[1], 0);
    EXPECT_FALSE(state->game.roundEverStarted);
    EXPECT_EQ(state->game.currentMatch.goals, 0);
    EXPECT_TRUE(state->game.roster.empty());

    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);
}

namespace {
    void SeedFinishedSession(SessionState& state) {
        std::unique_lock gameLock(state.game.mutex);
        std::unique_lock historyLock(state.history.mutex);
        state.game.myPrimaryId = "Steam|1";
        state.history.SelectMmrOwner("Steam|1");
        state.game.sessionTotals.wins = 2;
        state.game.sessionGamemodes["2v2"] = {2, 0, 2};
        state.history.playlistInitialMmr["2v2"] = 1200;
        state.history.playlistHistoryY["2v2"] = {1200.0f, 1210.0f, 1220.0f};
        state.history.lifetimeMmrY = {1100.0f, 1200.0f, 1220.0f};
        state.syncSessionMmrChangeLocked();
    }

    bool WaitFor(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    }
} // namespace

TEST(StatsClientTest, ClosingTheGameStartsANewSessionWhenResetIsEnabled) {
    ConfigData backup = Config::Read();
    Config::Update([](ConfigData& c) {
        c.host = "not_an_ip";
        c.reset_session_on_close = true;
        c.last_primary_id.clear();
        c.show_session_recap_on_close = false;
    },
                   false);

    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    StatsClient client(state, fetcher, db);
    SeedFinishedSession(*state);

    client.Start();
    // With Rocket League running, the reset waits for the fifth disconnect.
    const bool reset = WaitFor([&] { return state->game.sessionGeneration.load() != 0; },
                               std::chrono::seconds(25));
    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);

    ASSERT_TRUE(reset);
    EXPECT_EQ(state->game.sessionGeneration.load(), 1u);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_TRUE(state->game.sessionTotals.mmrChangeByPlaylist.empty());
    EXPECT_TRUE(state->history.playlistHistoryY.empty());
    EXPECT_TRUE(state->history.playlistInitialMmr.empty());
    EXPECT_EQ(state->history.lifetimeMmrY, (std::vector<float>{1100.0f, 1200.0f, 1220.0f}));
    ASSERT_TRUE(state->game.lastSessionRecap.valid);
    EXPECT_EQ(state->game.lastSessionRecap.totals.wins, 2);
    EXPECT_EQ(state->game.lastSessionRecap.totals.mmrChangeByPlaylist.at("2v2"), 20);
}

TEST(StatsClientTest, ClosingTheGameKeepsTheSessionWhenResetIsDisabled) {
    ConfigData backup = Config::Read();
    Config::Update([](ConfigData& c) {
        c.host = "not_an_ip";
        c.reset_session_on_close = false;
        c.last_primary_id.clear();
    },
                   false);

    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MMRFetcher>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto discord = std::make_shared<MockDiscordManager>(state);
    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);
    SeedFinishedSession(*state);

    client.Start();
    const bool disconnected = WaitFor([&] { return discord->pushCount.load() > 0; },
                                      std::chrono::seconds(5));
    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);

    ASSERT_TRUE(disconnected);
    EXPECT_EQ(state->game.sessionGeneration.load(), 0u);
    EXPECT_EQ(state->game.sessionTotals.wins, 2);
    EXPECT_EQ(state->game.sessionTotals.mmrChangeByPlaylist.at("2v2"), 20);
    EXPECT_EQ(state->history.playlistHistoryY.at("2v2"), (std::vector<float>{1200.0f, 1210.0f, 1220.0f}));
    EXPECT_EQ(state->history.playlistInitialMmr.at("2v2"), 1200);
    EXPECT_FALSE(state->game.lastSessionRecap.valid);
}
