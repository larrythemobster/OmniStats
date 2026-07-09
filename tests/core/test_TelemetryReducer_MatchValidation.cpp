#include <gtest/gtest.h>
#include "core/TelemetryReducer.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include <memory>

TEST(TelemetryReducerMatchValidation, SpectatorBugDoesNotCount) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "spectator-bug-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with bSpectator = true
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = true;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}};
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. MatchEnded
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "local_player_spectator_bug");
}

TEST(TelemetryReducerMatchValidation, NoShowLobbyDoesNotCount) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "noshow-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with incomplete lobby (e.g. 2v2 but only 3 players)
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}}; // Local
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}};
    nlohmann::json p3 = {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    updateStateData["Players"].push_back(p3);
    
    // Set identity to P1
    state->game.myPrimaryId = "Steam|1";
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. RoundStarted
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // 4. MatchEnded
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    
    // Set category to 2v2 to trigger full team check
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}

TEST(TelemetryReducerMatchValidation, RoundNeverStartedDoesNotCount) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "noround-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with full lobby
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}}; // Local
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    
    state->game.myPrimaryId = "Steam|1";
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. MatchEnded (No RoundStarted event)
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "round_never_started");
}

TEST(TelemetryReducerMatchValidation, NormalWinCounts) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "normal-win-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with full lobby
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}}; // Local
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    
    state->game.myPrimaryId = "Steam|1";
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. RoundStarted
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // 4. MatchEnded
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0; // Local team wins
    matchEndedData["Teams"] = nlohmann::json::array({
        nlohmann::json{{"TeamNum", 0}, {"Score", 3}},
        nlohmann::json{{"TeamNum", 1}, {"Score", 2}}
    });
    
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, OnesMatchDoesNotTrackGoalParticipation) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "ones-goal-participation-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();

    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}};
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.currentMatch.goalsSelf = 2;

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.teamGoals, 0);
    EXPECT_EQ(state->game.sessionTotals.goalParticipations, 0);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, NormalLossCounts) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "normal-loss-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with full lobby
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}}; // Local
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    
    state->game.myPrimaryId = "Steam|1";
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. RoundStarted
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // 4. MatchEnded
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 1; // Opponent team wins
    
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, ActivePlaylistSwitchesOncePerMatch) {
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) { c.auto_switch_mmr_category = true; }, false);
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "playlist-switch-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}});

    nlohmann::json partialUpdateStateData;
    partialUpdateStateData["Game"]["bSpectator"] = false;
    partialUpdateStateData["Players"] = nlohmann::json::array();
    partialUpdateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    partialUpdateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}});

    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), partialUpdateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::Best);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::ThreeVThree);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::TwoVTwo);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::TwoVTwo);

    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::OneVOne);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::OneVOne);

    matchCreatedData["MatchGuid"] = "playlist-switch-guid-2";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData3sLoading = updateStateData;
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData3sLoading);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::TwoVTwo);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::TwoVTwo);

    updateStateData3sLoading["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|5"}, {"TeamNum", 0}, {"Name", "P5"}});
    updateStateData3sLoading["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|6"}, {"TeamNum", 1}, {"Name", "P6"}});
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData3sLoading);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::ThreeVThree);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::ThreeVThree);
}

TEST(TelemetryReducerMatchValidation, ActivePlaylistAutoSwitchCanBeDisabled) {
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) { c.auto_switch_mmr_category = false; }, false);
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "playlist-switch-disabled-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}});

    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::Best);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::ThreeVThree);

    Config::Update([](ConfigData& c) { c.auto_switch_mmr_category = true; }, false);
}

TEST(TelemetryReducerMatchValidation, ExtraArenaAutoSwitchesBeforePlayerCount) {
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.show_extra_playlists = true;
    }, false);
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "hoops-map-switch-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Game"]["Arena"] = "hoops_dunkhouse_p";
    updateStateData["Players"] = nlohmann::json::array();
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}});

    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::Hoops);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::Hoops);
}

TEST(TelemetryReducerMatchValidation, SnowMapAutoSwitchesToSnowDay) {
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.show_extra_playlists = true;
    }, false);
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "snow-map-switch-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Game"]["Arena"] = "UtopiaStadium_Snow_P";
    updateStateData["Players"] = nlohmann::json::array();
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}});

    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::SnowDay);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::SnowDay);
}

TEST(TelemetryReducerMatchValidation, RumbleManualSelectionInfersRumbleOnStandardArena) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();

    // Manually set category to Rumble
    state->ui.rosterMmrCategory.store(MmrCategory::Rumble);
    state->ui.graphMmrCategory.store(MmrCategory::Rumble);

    // Verify that InferFromSnapshot correctly infers "rumble" even on a standard arena
    std::string gamemode = GamemodeUtils::InferFromSnapshot(
        6, 6,
        state->ui.rosterMmrCategory.load(),
        state->ui.graphMmrCategory.load(),
        "stadium_p"
    );

    EXPECT_EQ(gamemode, "rumble");
}

TEST(TelemetryReducerMatchValidation, HeatseekerManualSelectionInfersHeatseekerOnStandardArena) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();

    // Manually set category to Heatseeker
    state->ui.rosterMmrCategory.store(MmrCategory::Heatseeker);
    state->ui.graphMmrCategory.store(MmrCategory::Heatseeker);

    // Verify that InferFromSnapshot correctly infers "heatseeker" even on a standard arena
    std::string gamemode = GamemodeUtils::InferFromSnapshot(
        6, 6,
        state->ui.rosterMmrCategory.load(),
        state->ui.graphMmrCategory.load(),
        "stadium_p"
    );

    EXPECT_EQ(gamemode, "heatseeker");
}

TEST(TelemetryReducerMatchValidation, DisconnectAfterValidStartCounts) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // 1. MatchCreated
    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "disconnect-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    // 2. UpdateState with full lobby (2v2)
    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    
    nlohmann::json p1 = {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}}; // Local
    nlohmann::json p2 = {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}};
    nlohmann::json p3 = {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}};
    nlohmann::json p4 = {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}};
    updateStateData["Players"].push_back(p1);
    updateStateData["Players"].push_back(p2);
    updateStateData["Players"].push_back(p3);
    updateStateData["Players"].push_back(p4);
    
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);

    // 3. RoundStarted
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // 4. UpdateState with one player missing (disconnect)
    nlohmann::json updateStateData2 = updateStateData;
    updateStateData2["Players"].erase(3); // Remove P4
    
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData2);

    // 5. MatchEnded
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, MatchSummaryResultSurvivesMatchDestroyed) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    nlohmann::json matchCreatedData;
    matchCreatedData["MatchGuid"] = "summary-survives-destroyed-guid";
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), matchCreatedData);

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array();
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}});
    updateStateData["Players"].push_back(nlohmann::json{{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}});

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    matchEndedData["Teams"] = nlohmann::json::array({
        nlohmann::json{{"TeamNum", 0}, {"Score", 3}},
        nlohmann::json{{"TeamNum", 1}, {"Score", 2}}
    });

    reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);
    reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_FALSE(state->game.inMatch);
    EXPECT_EQ(state->game.myTeam, -1);
    EXPECT_EQ(state->game.matchSummaryMyTeam, 0);
    EXPECT_EQ(state->game.matchSummaryWinnerTeam, 0);
    EXPECT_EQ(state->game.matchSummaryScore[0], 3);
    EXPECT_EQ(state->game.matchSummaryScore[1], 2);
    EXPECT_TRUE(state->ui.showMatchSummary);
}

TEST(TelemetryReducerMatchValidation, LobbyNeverFillsVoid) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "never-fills-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}}
    });

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}

TEST(TelemetryReducerMatchValidation, LobbyFillsThenPlayerLeavesCounts) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "fills-then-leaves-guid"}});

    nlohmann::json updateStateDataFull;
    updateStateDataFull["Game"]["bSpectator"] = false;
    updateStateDataFull["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
        {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}
    });

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataFull);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // Verify lobby was detected as full
    EXPECT_TRUE(state->game.lobbyWasEverFull);

    // Player leaves mid-game (so update state contains fewer players)
    nlohmann::json updateStateDataLeft = updateStateDataFull;
    updateStateDataLeft["Players"].erase(3); // Remove P4

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataLeft);

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, LobbyFillsPlayerLeavesPlayerRejoinsCounts) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "fills-leaves-rejoins-guid"}});

    nlohmann::json updateStateDataFull;
    updateStateDataFull["Game"]["bSpectator"] = false;
    updateStateDataFull["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
        {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}
    });

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataFull);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // Player leaves
    nlohmann::json updateStateDataLeft = updateStateDataFull;
    updateStateDataLeft["Players"].erase(3); // Remove P4
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataLeft);

    // Player rejoins
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataFull);

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
}

TEST(TelemetryReducerMatchValidation, PlayerLeavesBeforeFullLobbyIsVoid) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "leaves-before-full-guid"}});

    // Lobby starts as 1v2 (for 2v2)
    nlohmann::json updateStateData1 = {
        {"Game", {{"bSpectator", false}}},
        {"Players", nlohmann::json::array({
            {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
            {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
            {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}
        })}
    };

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData1);

    // A player leaves, so lobby is now 1v1
    nlohmann::json updateStateData2 = {
        {"Game", {{"bSpectator", false}}},
        {"Players", nlohmann::json::array({
            {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
            {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}}
        })}
    };

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData2);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // Match ends, lobby never became full 2v2
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}

TEST(TelemetryReducerMatchValidation, MatchDestroyedWithLeadingScoreSavesWin) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "early-win-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}
    });
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.matchSummaryWinnerTeam, 0);
}

TEST(TelemetryReducerMatchValidation, MatchDestroyedWithTrailingScoreSavesLoss) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "early-loss-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}
    });
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 1;
    state->game.score[1] = 2;
    state->game.localPlayerWasActive = true;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.matchSummaryWinnerTeam, 1);
}

TEST(TelemetryReducerMatchValidation, MatchDestroyedWithTiedScoreDoesNotCount) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "early-tie-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}
    });
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 1;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "destroyed_tied_score_no_winner");
}

TEST(TelemetryReducerMatchValidation, MatchEndedFollowedByMatchDestroyedDoesNotDoubleSave) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "normal-ended-then-destroyed-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}
    });
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects1 = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_TRUE(effects1.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);

    SideEffects effects2 = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_FALSE(effects2.saveMatch);
}

TEST(TelemetryReducerMatchValidation, OpponentLeavesMidMatchButRemainsInSavedSnapshot) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "opponent-left-guid"}});

    nlohmann::json updateStateDataFull;
    updateStateDataFull["Game"]["bSpectator"] = false;
    updateStateDataFull["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
        {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}
    });

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataFull);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_EQ(state->game.roster.size(), 4);

    std::this_thread::sleep_for(std::chrono::milliseconds(5100));

    nlohmann::json updateStateDataLeft = updateStateDataFull;
    updateStateDataLeft["Players"].erase(3); // Remove P4
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateDataLeft);

    EXPECT_EQ(state->game.roster.count("Steam|4"), 0);
    EXPECT_EQ(state->game.roster.size(), 3);
    EXPECT_EQ(state->game.matchRoster.count("Steam|4"), 1);

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);

    EXPECT_EQ(effects.saveSnapshot.roster.size(), 4);
    EXPECT_EQ(effects.saveSnapshot.roster.count("Steam|4"), 1);
}

TEST(TelemetryReducerMatchValidation, MatchDestroyedWithLobbyNeverFullVoids) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "never-full-destroy-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({
        {{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
        {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}
    });
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{});

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}
