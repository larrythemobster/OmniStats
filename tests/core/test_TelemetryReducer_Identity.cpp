#include <gtest/gtest.h>
#include "core/TelemetryReducer.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include "core/Constants.hpp"
#include "database/DatabaseManager.hpp"
#include <memory>

// This test exercises the identity auto-switch behavior in TelemetryReducer.
// It simulates an UpdateState payload where a saved primary id is present but
// not seen in the lobby while local player feeds (PC platform players) exist.

TEST(TelemetryReducerIdentity, AutoSwitchesWhenSavedIdMissing) {
    Storage::InitializeEnvironment();
    // Ensure clean config
    Config::Update([](ConfigData& c) { c.last_primary_id = ""; }, true);

    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // Start with a saved id in config and apply into state
    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|12345"; }, true);
    {
        std::unique_lock<std::shared_mutex> lk(state->game.mutex);
        state->game.myPrimaryId = "Steam|12345";
    }

    // Build a Players array where the saved id is NOT present but a local PC feed exists
    nlohmann::json data;
    data["Players"] = nlohmann::json::array();

    // Local feed 1 (Epic)
    nlohmann::json p1;
    p1["PrimaryId"] = "Epic|98765";
    p1["Name"] = "PlayerLocal";
    p1["TeamNum"] = 0;
    p1["Boost"] = 50; // presence of Boost marks it as local feed on PC
    data["Players"].push_back(p1);

    // Non-local console feed
    nlohmann::json p2;
    p2["PrimaryId"] = "PS4|222";
    p2["Name"] = "Other";
    p2["TeamNum"] = 1;
    data["Players"].push_back(p2);

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), data);

    // Expect that the state switched to the Epic|98765 account
    EXPECT_EQ(state->game.myPrimaryId, "Epic|98765");
    // Expect SideEffects requested lifetime/history refresh for the new id
    EXPECT_TRUE(effects.fetchLifetimeHistory);
    EXPECT_EQ(effects.lifetimePrimaryId, "Epic|98765");
    EXPECT_TRUE(effects.refreshDbStats);
    EXPECT_EQ(effects.refreshStatsPrimaryId, "Epic|98765");

    // Clean up
    Config::Update([](ConfigData& c) { c.last_primary_id = ""; }, true);
}
