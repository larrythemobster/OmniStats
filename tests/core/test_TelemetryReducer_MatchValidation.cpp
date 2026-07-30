#include <gtest/gtest.h>
#include "core/TelemetryReducer.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "network/MMRFetcher.hpp"
#include <memory>

namespace {
    void StartRankedOnesMatch(
        TelemetryReducer& reducer,
        const std::shared_ptr<SessionState>& state,
        const std::string& matchGuid,
        int localScore,
        int opponentScore,
        bool startRound = true,
        bool spectator = false,
        bool emitMatchCreated = true) {
        state->game.myPrimaryId = "Steam|1";
        state->game.roster["Steam|1"] =
            PlayerData{
                .primaryId = "Steam|1",
                .name = "P1",
                .team = 0,
                .mmr = 1200,
                .playlists = {{"1v1", 1200}},
                .playlistMatches = {{"1v1", 50}}};
        state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
        state->ui.graphMmrCategory.store(MmrCategory::OneVOne);

        if (emitMatchCreated) {
            reducer.Reduce(
                std::string(Constants::EVT_MATCH_CREATED),
                nlohmann::json{{"MatchGuid", matchGuid}});
        }
        reducer.Reduce(
            std::string(Constants::EVT_UPDATE_STATE),
            nlohmann::json{
                {"Game",
                 {{"Arena", "Stadium_P"},
                  {"bReplay", false},
                  {"bSpectator", spectator},
                  {"Teams",
                   nlohmann::json::array(
                       {{{"TeamNum", 0}, {"Score", localScore}},
                        {{"TeamNum", 1},
                         {"Score", opponentScore}}})}}},
                {"Players",
                 nlohmann::json::array(
                     {{{"PrimaryId", "Steam|1"},
                       {"TeamNum", 0},
                       {"Name", "P1"},
                       {"Boost", 100}},
                      {{"PrimaryId", "Steam|2"},
                       {"TeamNum", 1},
                       {"Name", "P2"}}})}});
        if (startRound) {
            reducer.Reduce(
                std::string(Constants::EVT_ROUND_STARTED),
                nlohmann::json{});
        }
    }

    void EnterGoalReplay(TelemetryReducer& reducer) {
        reducer.Reduce(
            std::string(Constants::EVT_UPDATE_STATE),
            nlohmann::json{
                {"Game",
                 {{"bReplay", true},
                  {"bSpectator", false}}}});
    }

    nlohmann::json CurrentMatchEvent(
        const std::shared_ptr<SessionState>& state,
        nlohmann::json data) {
        if (!data.contains("MatchGuid") &&
            !state->game.matchGuid.empty()) {
            data["MatchGuid"] = state->game.matchGuid;
        }
        return data;
    }
}

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
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    matchEndedData["Teams"] = nlohmann::json::array({nlohmann::json{{"TeamNum", 0}, {"Score", 3}},
                                                     nlohmann::json{{"TeamNum", 1}, {"Score", 2}}});

    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    state->game.roster["Steam|1"].playlists["1v1"] = 1200;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->matchGuid, "normal-win-guid");
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "1v1");
    EXPECT_EQ(effects.postMatchMmrRefresh->previousMmr, 1200);
    EXPECT_TRUE(effects.postMatchMmrRefresh->previousMmrIsPlaylistSpecific);
    EXPECT_TRUE(effects.postMatchMmrRefresh->won);
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

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    EXPECT_TRUE(effects.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_FALSE(effects.postMatchMmrRefresh->previousMmrIsPlaylistSpecific);
    EXPECT_FALSE(effects.postMatchMmrRefresh->won);
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
    },
                   false);
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
    },
                   false);
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
        "stadium_p");

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
        "stadium_p");

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

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    matchEndedData["Teams"] = nlohmann::json::array({nlohmann::json{{"TeamNum", 0}, {"Score", 3}},
                                                     nlohmann::json{{"TeamNum", 1}, {"Score", 2}}});

    reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));
    reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

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
    updateStateData["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                        {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
                                                        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}}});

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    updateStateDataFull["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                            {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
                                                            {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
                                                            {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}});

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
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    updateStateDataFull["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                            {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
                                                            {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
                                                            {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}});

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
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
        {"Players", nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                           {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
                                           {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}})}};

    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData1);

    // A player leaves, so lobby is now 1v1
    nlohmann::json updateStateData2 = {
        {"Game", {{"bSpectator", false}}},
        {"Players", nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                           {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}}})}};

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData2);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // Match ends, lobby never became full 2v2
    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}

TEST(TelemetryReducerMatchValidation, LeadingLocalForfeitIsLossNotScoreboardWin) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "early-leading-forfeit-guid", 2, 1);

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{{"bLocalPlayerForfeit", true}}));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.matchGuid,
              "early-leading-forfeit-guid");
    EXPECT_EQ(effects.saveSnapshot.winnerTeam, 1);
    EXPECT_EQ(effects.saveSnapshot.myTeam, 0);
    EXPECT_FALSE(effects.pendingDestroyedMatch.has_value());
}

TEST(TelemetryReducerMatchValidation, TrailingLocalForfeitUsesExplicitLossPath) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "early-trailing-forfeit-guid", 1, 2);

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{{"bLocalPlayerForfeit", true}}));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.winnerTeam, 1);
    EXPECT_EQ(effects.saveSnapshot.score[0], 1);
    EXPECT_EQ(effects.saveSnapshot.score[1], 2);
}

TEST(TelemetryReducerMatchValidation, TiedDestructionWaitsForTrackerThenSavesLoss) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "early-tied-forfeit-guid", 3, 3);

    SideEffects destroyed = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_FALSE(destroyed.saveMatch);
    ASSERT_TRUE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_EQ(
        destroyed.pendingDestroyedMatch->matchGuid,
        "early-tied-forfeit-guid");
    EXPECT_EQ(
        state->game.lastMatchVoidReason,
        "destroyed_pending_tracker_confirmation");

    SideEffects confirmed =
        reducer.ConfirmPendingDestroyedMatch(
            "early-tied-forfeit-guid", false);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
    ASSERT_TRUE(confirmed.saveMatch);
    EXPECT_EQ(confirmed.saveSnapshot.winnerTeam, 1);

    SideEffects duplicate =
        reducer.ConfirmPendingDestroyedMatch(
            "early-tied-forfeit-guid", false);
    EXPECT_FALSE(duplicate.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, MatchEndedFollowedByMatchDestroyedDoesNotDoubleSave) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "normal-ended-then-destroyed-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"]["bSpectator"] = false;
    updateStateData["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                        {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}});
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    SideEffects effects1 = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_TRUE(effects1.saveMatch);
    EXPECT_FALSE(state->game.lastMatchWasVoid);

    SideEffects effects2 = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_FALSE(effects2.saveMatch);
}

TEST(TelemetryReducerMatchValidation, DelayedMatchEndedResolvesPendingMatchExactlyOnce) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "delayed-ended-guid", 2, 1);

    SideEffects destroyed = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(destroyed.saveMatch);

    SideEffects ended = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
        {"MatchGuid", "delayed-ended-guid"},
        {"WinnerTeamNum", 1}}));
    ASSERT_TRUE(ended.saveMatch);
    ASSERT_TRUE(ended.resolvedDestroyedMatch.has_value());
    EXPECT_EQ(
        ended.resolvedDestroyedMatch->matchGuid,
        "delayed-ended-guid");
    EXPECT_FALSE(ended.resolvedDestroyedMatch->won);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects duplicateEnded = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
        {"MatchGuid", "delayed-ended-guid"},
        {"WinnerTeamNum", 1}}));
    SideEffects duplicateDestroyed = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    SideEffects duplicateConfirmation =
        reducer.ConfirmPendingDestroyedMatch(
            "delayed-ended-guid", false);

    EXPECT_FALSE(duplicateEnded.saveMatch);
    EXPECT_FALSE(duplicateDestroyed.saveMatch);
    EXPECT_FALSE(duplicateConfirmation.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}


TEST(TelemetryReducerMatchValidation, DuplicateMatchDestroyedKeepsOnePendingRecord) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "duplicate-destroyed-guid", 1, 0);

    SideEffects first = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    SideEffects second = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    ASSERT_TRUE(first.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(second.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(first.saveMatch);
    EXPECT_FALSE(second.saveMatch);

    SideEffects confirmed =
        reducer.ConfirmPendingDestroyedMatch(
            "duplicate-destroyed-guid", false);
    ASSERT_TRUE(confirmed.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, DestroyedBeforeRoundStartRemainsVoid) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "unstarted-destroyed-guid", 0, 0, false);

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_FALSE(effects.saveMatch);
    EXPECT_FALSE(effects.pendingDestroyedMatch.has_value());
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    EXPECT_EQ(state->game.lastMatchVoidReason, "round_never_started");
}

TEST(TelemetryReducerMatchValidation, SpectatorAndSavedReplayDestructionRemainVoid) {
    Storage::InitializeEnvironment();

    auto spectatorState = std::make_shared<SessionState>();
    TelemetryReducer spectatorReducer(spectatorState);
    StartRankedOnesMatch(
        spectatorReducer,
        spectatorState,
        "spectator-destroyed-guid",
        0,
        0,
        true,
        true);
    SideEffects spectatorEffects = spectatorReducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(spectatorState, nlohmann::json{}));
    EXPECT_FALSE(spectatorEffects.saveMatch);
    EXPECT_FALSE(
        spectatorEffects.pendingDestroyedMatch.has_value());
    EXPECT_EQ(spectatorState->game.sessionTotals.losses, 0);

    auto replayState = std::make_shared<SessionState>();
    TelemetryReducer replayReducer(replayState);
    StartRankedOnesMatch(
        replayReducer,
        replayState,
        "replay-destroyed-guid",
        0,
        0);
    replayReducer.Reduce(
        std::string(Constants::EVT_REPLAY_CREATED),
        nlohmann::json{});
    SideEffects replayEffects = replayReducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(replayState, nlohmann::json{}));
    EXPECT_FALSE(replayEffects.saveMatch);
    EXPECT_FALSE(replayEffects.pendingDestroyedMatch.has_value());
    EXPECT_EQ(replayState->game.sessionTotals.losses, 0);
    EXPECT_EQ(
        replayState->game.lastMatchVoidReason,
        "non_live_replay");
}

TEST(TelemetryReducerMatchValidation, LocalDepartureCreatesUnconfirmedPendingRecord) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "departure-pending-guid", 1, 1);

    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        nlohmann::json{
            {"Game", {{"bReplay", false}, {"bSpectator", false}}},
            {"Players",
             nlohmann::json::array(
                 {{{"PrimaryId", "Steam|2"},
                   {"TeamNum", 1},
                   {"Name", "P2"}}})}});
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    ASSERT_TRUE(effects.pendingDestroyedMatch.has_value());
    EXPECT_TRUE(
        effects.pendingDestroyedMatch->localPlayerDisappeared);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, OpponentLeavesMidMatchButRemainsInSavedSnapshot) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "opponent-left-guid"}});

    nlohmann::json updateStateDataFull;
    updateStateDataFull["Game"]["bSpectator"] = false;
    updateStateDataFull["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                            {{"PrimaryId", "Steam|2"}, {"TeamNum", 0}, {"Name", "P2"}},
                                                            {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
                                                            {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}});

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
    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, matchEndedData));

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
    updateStateData["Players"] = nlohmann::json::array({{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
                                                        {{"PrimaryId", "Steam|3"}, {"TeamNum", 1}, {"Name", "P3"}},
                                                        {{"PrimaryId", "Steam|4"}, {"TeamNum", 1}, {"Name", "P4"}}});
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);

    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    state->game.score[0] = 2;
    state->game.score[1] = 1;
    state->game.localPlayerWasActive = true;

    SideEffects effects = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "lobby_never_full");
}

TEST(TelemetryReducerMatchValidation, OpponentForfeitFinalizesOnceAndOwnsOneGraphPoint) {
    Storage::InitializeEnvironment();
    const ConfigData originalConfig = Config::Read();
    Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);

    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    state->game.myPrimaryId = "Steam|1";
    state->game.roster["Steam|1"].playlists["1v1"] = 1200;
    state->game.roster["Steam|1"].playlistMatches["1v1"] = 50;
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED),
                   nlohmann::json{{"MatchGuid", "opponent-forfeit-guid"}});

    nlohmann::json updateStateData;
    updateStateData["Game"] = {
        {"bSpectator", false},
        {"Arena", "Stadium_P"},
        {"SecondsRemaining", 240}};
    updateStateData["Players"] = nlohmann::json::array(
        {{{"PrimaryId", "Steam|1"}, {"TeamNum", 0}, {"Name", "P1"}, {"Boost", 100}},
         {{"PrimaryId", "Steam|2"}, {"TeamNum", 1}, {"Name", "P2"}}});
    state->game.myPrimaryId = "Steam|1";
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);
    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), updateStateData);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    // A Tracker refresh can arrive before MatchEnded. Finalization must retain
    // the snapshot captured when this GUID started, not the mutable live roster.
    state->game.roster["Steam|1"].playlists["1v1"] = 1230;
    state->game.roster["Steam|1"].playlistMatches["1v1"] = 51;

    const nlohmann::json forfeitData = {
        {"WinnerTeamNum", 0},
        {"bForfeit", true},
        {"Teams", nlohmann::json::array(
                      {{{"TeamNum", 0}, {"Score", 0}},
                       {{"TeamNum", 1}, {"Score", 0}}})}};
    SideEffects firstEffects =
        reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, forfeitData));

    ASSERT_TRUE(firstEffects.saveMatch);
    EXPECT_EQ(firstEffects.saveSnapshot.matchGuid, "opponent-forfeit-guid");
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
    ASSERT_TRUE(firstEffects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(firstEffects.postMatchMmrRefresh->playlist, "1v1");
    EXPECT_EQ(firstEffects.postMatchMmrRefresh->previousMmr, 1200);
    EXPECT_EQ(firstEffects.postMatchMmrRefresh->previousMatches, 50);
    EXPECT_TRUE(firstEffects.postMatchMmrRefresh->previousMmrIsPlaylistSpecific);
    EXPECT_TRUE(firstEffects.postMatchMmrRefresh->won);

    SideEffects duplicateEffects =
        reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    EXPECT_FALSE(duplicateEffects.saveMatch);
    EXPECT_FALSE(duplicateEffects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.sessionTotals.wins, 1);

    MMRFetcher fetcher(state);
    const auto& refresh = *firstEffects.postMatchMmrRefresh;
    fetcher.EnqueuePostMatch(refresh.primaryId,
                             refresh.name,
                             refresh.matchGuid,
                             refresh.playlist,
                             refresh.previousMmr,
                             refresh.previousMatches,
                             refresh.previousMmrIsPlaylistSpecific,
                             refresh.won);
    fetcher.ProcessPostMatchResponseForTests(refresh.matchGuid, 1200, 50);
    const auto points = fetcher.PlaylistMatchPointsForTests("1v1");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "opponent-forfeit-guid");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_FALSE(points[0].trackerCovered);

    Config::Update([&](ConfigData& config) { config = originalConfig; }, false);
}

TEST(TelemetryReducerMatchValidation, GoalReplayForfeitFinalizesLossImmediately) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "goal-replay-forfeit-guid", 2, 3);
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"bLocalPlayerForfeit", true}}));

    ASSERT_TRUE(destroyed.saveMatch);
    EXPECT_FALSE(destroyed.pendingDestroyedMatch.has_value());
    ASSERT_TRUE(destroyed.postMatchMmrRefresh.has_value());
    EXPECT_TRUE(
        destroyed.postMatchMmrRefresh->provisionalImmediately);
    EXPECT_EQ(
        destroyed.saveSnapshot.matchGuid,
        "goal-replay-forfeit-guid");
    EXPECT_EQ(destroyed.saveSnapshot.winnerTeam, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects duplicateConfirmation =
        reducer.ConfirmPendingDestroyedMatch(
            "goal-replay-forfeit-guid", false);
    EXPECT_FALSE(duplicateConfirmation.saveMatch);
}

TEST(TelemetryReducerMatchValidation, GoalReplayEndsAndNormalMatchFinalizationIsUnchanged) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "goal-replay-continues-guid", 2, 1);
    EnterGoalReplay(reducer);

    reducer.Reduce(
        std::string(Constants::EVT_GOAL_SCORED),
        nlohmann::json{});
    reducer.Reduce(
        std::string(Constants::EVT_STATFEED),
        nlohmann::json{
            {"EventName", "Save"},
            {"MainTarget",
             {{"PrimaryId", "Steam|1"}, {"Name", "P1"}}}});
    EXPECT_EQ(state->game.currentMatch.goals, 0);
    EXPECT_EQ(state->game.currentMatch.saves, 0);

    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        nlohmann::json{
            {"Game",
             {{"bReplay", false}, {"bSpectator", false}}}});
    reducer.Reduce(
        std::string(Constants::EVT_ROUND_STARTED),
        nlohmann::json{});
    SideEffects ended = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"WinnerTeamNum", 0}}));
    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));

    ASSERT_TRUE(ended.saveMatch);
    EXPECT_FALSE(destroyed.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, LeadingGoalReplayForfeitCountsExplicitLossNotScore) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "leading-replay-forfeit-guid", 4, 1);
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"bLocalPlayerForfeit", true}}));

    ASSERT_TRUE(destroyed.saveMatch);
    EXPECT_FALSE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_EQ(destroyed.saveSnapshot.score[0], 4);
    EXPECT_EQ(destroyed.saveSnapshot.score[1], 1);
    EXPECT_EQ(destroyed.saveSnapshot.winnerTeam, 1);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, TiedGoalReplayForfeitFinalizesExplicitLoss) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "tied-replay-forfeit-guid", 3, 3);
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"bLocalPlayerForfeit", true}}));

    ASSERT_TRUE(destroyed.saveMatch);
    EXPECT_FALSE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_EQ(destroyed.saveSnapshot.winnerTeam, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, GoalReplayDestructionStillRejectsLobbyNeverFull) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "replay-never-full-guid", 1, 0);
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    state->game.lobbyWasEverFull = false;
    state->game.maxTeamPlayersSeen = {1, 2};
    state->game.maxPlayersSeen = 3;
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_FALSE(destroyed.saveMatch);
    EXPECT_FALSE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_EQ(
        state->game.lastMatchVoidReason,
        "lobby_never_full");
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, GoalReplayDestructionStillRejectsUnstartedRound) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer,
        state,
        "replay-unstarted-guid",
        0,
        0,
        false);
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));

    EXPECT_FALSE(destroyed.saveMatch);
    EXPECT_FALSE(destroyed.pendingDestroyedMatch.has_value());
    EXPECT_EQ(
        state->game.lastMatchVoidReason,
        "round_never_started");
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, DuplicateGoalReplayDestructionSavesOnce) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "duplicate-replay-destroyed-guid", 2, 2);
    EnterGoalReplay(reducer);

    SideEffects first = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"bLocalPlayerForfeit", true}}));
    SideEffects duplicate = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        nlohmann::json{
            {"MatchGuid", "duplicate-replay-destroyed-guid"},
            {"bLocalPlayerForfeit", true}});

    ASSERT_TRUE(first.saveMatch);
    EXPECT_FALSE(first.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(duplicate.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(duplicate.saveMatch);
    SideEffects duplicateConfirmation =
        reducer.ConfirmPendingDestroyedMatch(
            "duplicate-replay-destroyed-guid", false);
    EXPECT_FALSE(duplicateConfirmation.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, DelayedEndAfterReplayForfeitCannotDuplicateMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "confirmed-replay-forfeit-guid", 3, 2);
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"bLocalPlayerForfeit", true}}));
    ASSERT_TRUE(destroyed.saveMatch);

    SideEffects delayed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "confirmed-replay-forfeit-guid"},
            {"WinnerTeamNum", 1},
            {"bForfeit", true}});

    EXPECT_FALSE(delayed.saveMatch);
    EXPECT_FALSE(delayed.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}
