#include <gtest/gtest.h>
#include "core/TelemetryReducer.hpp"
#include "core/SessionState.hpp"
#include "core/Constants.hpp"
#include "core/Storage.hpp"
#include <memory>

TEST(TelemetryReducerStats, CountsEpicSaveAsSave) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1",
            .name = "P1",
            .team = 0};
    }

    nlohmann::json statFeedData;
    statFeedData["EventName"] = "EpicSave";
    statFeedData["Player"] = {
        {"PrimaryId", "Steam|1"},
        {"Name", "P1"}};

    reducer.Reduce(std::string(Constants::EVT_STATFEED), statFeedData);

    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    EXPECT_EQ(state->game.currentMatch.saves, 1);
    EXPECT_EQ(state->game.currentMatch.savesSelf, 1);
    ASSERT_TRUE(state->game.roster.count("Steam|1"));
    EXPECT_EQ(state->game.roster["Steam|1"].saves, 1);
}

TEST(TelemetryReducerStats, AddsDemoedSelfToSessionTotalsOnMatchEnd) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.roundEverStarted = true;
        state->game.localPlayerWasActive = true;
        state->game.maxPlayersSeen = 2;
        state->game.maxTeamPlayersSeen[0] = 1;
        state->game.maxTeamPlayersSeen[1] = 1;
        state->game.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1",
            .name = "P1",
            .team = 0};
        state->game.roster["Steam|2"] = PlayerData{
            .primaryId = "Steam|2",
            .name = "P2",
            .team = 1};
    }
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    nlohmann::json statFeedData;
    statFeedData["EventName"] = "Demolish";
    statFeedData["MainTarget"] = {
        {"PrimaryId", "Steam|2"},
        {"Name", "P2"}};
    statFeedData["SecondaryTarget"] = {
        {"PrimaryId", "Steam|1"},
        {"Name", "P1"}};
    reducer.Reduce(std::string(Constants::EVT_STATFEED), statFeedData);

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    EXPECT_EQ(state->game.currentMatch.demoedSelf, 1);
    EXPECT_EQ(state->game.sessionTotals.demoed, 1);
}
