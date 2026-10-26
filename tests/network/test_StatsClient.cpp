#include <gtest/gtest.h>
#include "network/api/StatsClient.hpp"
#include "network/mmr/MmrService.hpp"
#include "database/DatabaseManager.hpp"
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "core/config/Config.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

TEST(StatsClientTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MmrService>(state);
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
    void PushPresenceUpdate(const DiscordPresenceSnapshot&) override {
        pushCount++;
    }
    std::atomic<int> pushCount{0};
};

TEST(StatsClientTest, MatchEndNonBlockingAndDiscordPush) {
    auto state = std::make_shared<SessionState>();
    auto fetcher = std::make_shared<MmrService>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    // Setup state
    state->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "Steam|123";
        ms.myTeam = 0;
        ms.arenaName = "Stadium";
        ms.matchGuid = "test_guid";
        ms.inMatch = true;
        ms.roster["Steam|123"] = PlayerData{.primaryId = "Steam|123", .name = "Me", .team = 0, .mmr = 1000};
    });

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
    auto fetcher = std::make_shared<MmrService>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    auto discord = std::make_shared<MockDiscordManager>(state);

    StatsClient client(state, fetcher, db);
    client.SetDiscordManager(discord);

    // Seed active-match state that must not leak across a socket reconnect.
    state->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.inMatch = true;
        ms.matchGuid = "stale-disconnect-guid";
        ms.playlistId = 11;
        ms.legacyMaxPlayersSeen = 4;
        ms.legacyMaxTeamPlayersSeen = {2, 2};
        ms.roundEverStarted = true;
        ms.currentMatch.goals = 3;
        ms.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1", .name = "Me", .team = 0};
    });

    client.Start();

    for (int i = 0; i < 100 && discord->pushCount.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    EXPECT_EQ(discord->pushCount.load(), 1);
    auto snap = state->liveMatch->GetSnapshot();
    EXPECT_FALSE(snap->inMatch);
    EXPECT_TRUE(snap->matchGuid.empty());
    EXPECT_EQ(snap->playlistId, -1);
    EXPECT_EQ(snap->legacyMaxPlayersSeen, 0);
    EXPECT_EQ(snap->legacyMaxTeamPlayersSeen[0], 0);
    EXPECT_EQ(snap->legacyMaxTeamPlayersSeen[1], 0);
    EXPECT_FALSE(snap->roundEverStarted);
    EXPECT_EQ(snap->currentMatch.goals, 0);
    EXPECT_TRUE(snap->roster.empty());

    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);
}

namespace {
    void SeedFinishedSession(SessionState& state) {
        state.history.Mutate([](auto& history) {
            history.SelectMmrOwner("Steam|1");
            history.playlistInitialMmr["2v2"] = 1200;
            history.playlistHistoryY["2v2"] = {1200.0f, 1210.0f, 1220.0f};
            history.lifetimeMmrY = {1100.0f, 1200.0f, 1220.0f};
        });
        state.liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
            ms.myPrimaryId = "Steam|1";
            ms.sessionTotals.wins = 2;
            ms.sessionGamemodes["2v2"] = {2, 0, 2};
        });
        state.history.Mutate([&](auto& history) { state.syncSessionMmrChangeLocked(history); });
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
    auto fetcher = std::make_shared<MmrService>(state);
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    StatsClient client(state, fetcher, db);
    SeedFinishedSession(*state);

    client.Start();
    // With Rocket League running, the reset waits for the fifth disconnect.
    const bool reset = WaitFor([&] { return state->liveMatch->GetSnapshot()->sessionGeneration != 0; },
                               std::chrono::seconds(25));
    client.Stop();
    Config::Update([backup](ConfigData& c) { c = backup; }, false);

    ASSERT_TRUE(reset);
    auto snap = state->liveMatch->GetSnapshot();
    EXPECT_EQ(snap->sessionGeneration, 1u);
    EXPECT_EQ(snap->sessionTotals.wins, 0);
    EXPECT_TRUE(snap->sessionTotals.mmrChangeByPlaylist.empty());
    const auto history = state->history.GetSnapshot();
    EXPECT_TRUE(history->playlistHistoryY.empty());
    EXPECT_TRUE(history->playlistInitialMmr.empty());
    EXPECT_EQ(history->lifetimeMmrY, (std::vector<float>{1100.0f, 1200.0f, 1220.0f}));
    ASSERT_TRUE(snap->lastSessionRecap.valid);
    EXPECT_EQ(snap->lastSessionRecap.totals.wins, 2);
    EXPECT_EQ(snap->lastSessionRecap.totals.mmrChangeByPlaylist.at("2v2"), 20);
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
    auto fetcher = std::make_shared<MmrService>(state);
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
    auto snap = state->liveMatch->GetSnapshot();
    EXPECT_EQ(snap->sessionGeneration, 0u);
    EXPECT_EQ(snap->sessionTotals.wins, 2);
    EXPECT_EQ(snap->sessionTotals.mmrChangeByPlaylist.at("2v2"), 20);
    const auto history = state->history.GetSnapshot();
    EXPECT_EQ(history->playlistHistoryY.at("2v2"), (std::vector<float>{1200.0f, 1210.0f, 1220.0f}));
    EXPECT_EQ(history->playlistInitialMmr.at("2v2"), 1200);
    EXPECT_FALSE(snap->lastSessionRecap.valid);
}
