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
    struct ScopedConfigRestore {
        ConfigData original = Config::Read();

        ~ScopedConfigRestore() {
            Config::Update(
                [this](ConfigData& config) { config = original; },
                false);
        }
    };

    nlohmann::json TeamUpdate(
        int teamSize,
        nlohmann::json game = nlohmann::json::object()) {
        if (!game.contains("bReplay")) game["bReplay"] = false;
        if (!game.contains("bSpectator")) game["bSpectator"] = false;

        nlohmann::json players = nlohmann::json::array();
        for (int team = 0; team < 2; ++team) {
            for (int index = 0; index < teamSize; ++index) {
                const int number = team * teamSize + index + 1;
                nlohmann::json player = {
                    {"PrimaryId", "Steam|" + std::to_string(number)},
                    {"TeamNum", team},
                    {"Name", "P" + std::to_string(number)}};
                if (number == 1) player["Boost"] = 100;
                players.push_back(std::move(player));
            }
        }
        return nlohmann::json{
            {"Game", std::move(game)},
            {"Players", std::move(players)}};
    }
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
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.graph_follow_current_playlist = true;
    },
                   false);
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
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "3v3";
        c.graph_follow_current_playlist = false;
    },
                   false);
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
}

TEST(TelemetryReducerMatchValidation, ExtraArenaAutoSwitchesBeforePlayerCount) {
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.show_extra_playlists = true;
        c.graph_follow_current_playlist = true;
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
        c.graph_follow_current_playlist = true;
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

TEST(
    TelemetryReducerMatchValidation,
    GraphFollowsInferredRankedOneVersusOneIndependently) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.mmr_category = "best";
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "2v2";
        c.graph_follow_current_playlist = true;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    TelemetryReducer reducer(state);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "graph-follow-ones"}});
    reducer.Reduce(
        std::string(Constants::EVT_ROUND_STARTED),
        nlohmann::json{});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        TeamUpdate(1));

    EXPECT_EQ(
        state->ui.rosterMmrCategory.load(),
        MmrCategory::Best);
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::OneVOne);
    EXPECT_EQ(Config::Read().graph_mmr_category, "2v2");
}

TEST(
    TelemetryReducerMatchValidation,
    GraphFollowsMultipleInferredRankedPlaylists) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "1v1";
        c.graph_follow_current_playlist = true;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    TelemetryReducer reducer(state);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "graph-follow-twos"}});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        TeamUpdate(2));
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::TwoVTwo);
    EXPECT_EQ(
        state->ui.rosterMmrCategory.load(),
        MmrCategory::Best);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "graph-follow-threes"}});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        TeamUpdate(3));
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::ThreeVThree);
    EXPECT_EQ(
        state->ui.rosterMmrCategory.load(),
        MmrCategory::Best);
    EXPECT_EQ(Config::Read().graph_mmr_category, "1v1");
}

TEST(
    TelemetryReducerMatchValidation,
    LiveFollowAndGraphFollowCanBeEnabledIndependently) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.graph_mmr_category = "3v3";
        c.graph_follow_current_playlist = false;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);
    TelemetryReducer reducer(state);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "live-only-follow"}});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        TeamUpdate(2));

    EXPECT_EQ(
        state->ui.rosterMmrCategory.load(),
        MmrCategory::TwoVTwo);
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::ThreeVThree);
}

TEST(
    TelemetryReducerMatchValidation,
    DisabledGraphFollowKeepsConfiguredFallback) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "2v2";
        c.graph_follow_current_playlist = false;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Best);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    TelemetryReducer reducer(state);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "graph-follow-disabled"}});
    reducer.Reduce(
        std::string(Constants::EVT_ROUND_STARTED),
        nlohmann::json{});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        TeamUpdate(1));

    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::TwoVTwo);
    EXPECT_EQ(
        state->ui.rosterMmrCategory.load(),
        MmrCategory::Best);
}

TEST(
    TelemetryReducerMatchValidation,
    RepeatedTelemetryDoesNotReassignSameFollowedPlaylist) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "1v1";
        c.graph_follow_current_playlist = true;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    TelemetryReducer reducer(state);
    const nlohmann::json update = TeamUpdate(2);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{{"MatchGuid", "graph-follow-repeat"}});
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        update);
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::TwoVTwo);

    state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        update);
    EXPECT_EQ(
        state->ui.graphMmrCategory.load(),
        MmrCategory::ThreeVThree);
    EXPECT_EQ(Config::Read().graph_mmr_category, "1v1");
}

TEST(
    TelemetryReducerMatchValidation,
    UnsupportedContextsDoNotSwitchFollowedGraph) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "3v3";
        c.graph_follow_current_playlist = true;
    },
                   false);

    const auto expectUnchanged =
        [](const std::string& guid,
           const nlohmann::json& update,
           bool savedReplay) {
            auto state = std::make_shared<SessionState>();
            state->ui.graphMmrCategory.store(
                MmrCategory::ThreeVThree);
            TelemetryReducer reducer(state);
            reducer.Reduce(
                std::string(Constants::EVT_MATCH_CREATED),
                nlohmann::json{{"MatchGuid", guid}});
            if (savedReplay) {
                reducer.Reduce(
                    std::string(Constants::EVT_REPLAY_CREATED),
                    nlohmann::json{});
            }
            reducer.Reduce(
                std::string(Constants::EVT_UPDATE_STATE),
                update);
            EXPECT_EQ(
                state->ui.graphMmrCategory.load(),
                MmrCategory::ThreeVThree);
        };

    expectUnchanged(
        "freeplay-context",
        TeamUpdate(
            2,
            nlohmann::json{
                {"bTraining", true},
                {"TrainingType", "Freeplay"}}),
        false);
    expectUnchanged(
        "training-context",
        TeamUpdate(
            2,
            nlohmann::json{
                {"bTraining", true},
                {"TrainingType", "CustomTraining"}}),
        false);
    expectUnchanged(
        "saved-replay-context",
        TeamUpdate(2),
        true);
    expectUnchanged(
        "spectator-context",
        TeamUpdate(
            2,
            nlohmann::json{{"bSpectator", true}}),
        false);
    expectUnchanged(
        "private-context",
        TeamUpdate(
            2,
            nlohmann::json{{"bPrivateMatch", true}}),
        false);
    expectUnchanged(
        "casual-context",
        TeamUpdate(
            2,
            nlohmann::json{{"bCasualMatch", true}}),
        false);
    expectUnchanged(
        "unknown-context",
        nlohmann::json{
            {"Game", nlohmann::json::object()},
            {"Players", nlohmann::json::array()}},
        false);

    nlohmann::json incomplete = TeamUpdate(1);
    incomplete["Players"].push_back(
        nlohmann::json{
            {"PrimaryId", "Steam|3"},
            {"TeamNum", 0},
            {"Name", "P3"}});
    expectUnchanged(
        "incomplete-context",
        incomplete,
        false);
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

TEST(TelemetryReducerMatchValidation, StaleMatchEndedDoesNotFinalizeNextMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "stale-ended-match-a", 1, 1);

    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());
    SideEffects confirmedA =
        reducer.ConfirmPendingDestroyedMatch(
            "stale-ended-match-a", false);
    ASSERT_TRUE(confirmedA.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    StartRankedOnesMatch(
        reducer, state, "stale-ended-match-b", 0, 0);
    ASSERT_EQ(state->game.matchGuid, "stale-ended-match-b");
    ASSERT_FALSE(state->game.matchFinalized);

    SideEffects staleA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "stale-ended-match-a"},
                                                                                                              {"WinnerTeamNum", 0},
                                                                                                              {"Teams",
                                                                                                               nlohmann::json::array(
                                                                                                                   {{{"TeamNum", 0}, {"Score", 5}},
                                                                                                                    {{"TeamNum", 1}, {"Score", 0}}})}}));

    EXPECT_FALSE(staleA.saveMatch);
    EXPECT_FALSE(staleA.resolvedDestroyedMatch.has_value());
    EXPECT_EQ(state->game.matchGuid, "stale-ended-match-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects endedB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "stale-ended-match-b"},
                                                                                                              {"WinnerTeamNum", 0},
                                                                                                              {"Teams",
                                                                                                               nlohmann::json::array(
                                                                                                                   {{{"TeamNum", 0}, {"Score", 2}},
                                                                                                                    {{"TeamNum", 1}, {"Score", 1}}})}}));

    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(
        endedB.saveSnapshot.matchGuid,
        "stale-ended-match-b");
    EXPECT_EQ(endedB.saveSnapshot.winnerTeam, 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
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

TEST(TelemetryReducerMatchValidation, ExplicitGuidFinalizesCurrentMatchExactlyOnce) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "explicit-current-guid", 2, 1);

    const nlohmann::json ended = {
        {"MatchGuid", "explicit-current-guid"},
        {"WinnerTeamNum", 0}};
    SideEffects first = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, ended));
    SideEffects duplicate = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, ended));

    ASSERT_TRUE(first.saveMatch);
    EXPECT_EQ(
        first.saveSnapshot.matchGuid,
        "explicit-current-guid");
    EXPECT_FALSE(duplicate.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, ExplicitGuidFinalizesPendingMatchWithoutTouchingNewerMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "pending-explicit-a", 1, 1);
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());

    StartRankedOnesMatch(
        reducer, state, "pending-explicit-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;

    SideEffects endedA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "pending-explicit-a"},
                                                                                                              {"WinnerTeamNum", 1}}));

    ASSERT_TRUE(endedA.saveMatch);
    ASSERT_TRUE(endedA.resolvedDestroyedMatch.has_value());
    EXPECT_EQ(
        endedA.saveSnapshot.matchGuid,
        "pending-explicit-a");
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_EQ(state->game.matchGuid, "pending-explicit-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects duplicateA =
        reducer.ConfirmPendingDestroyedMatch(
            "pending-explicit-a", false);
    EXPECT_FALSE(duplicateA.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, MissingGuidFinalizesOnlyUnambiguousCurrentMatchExactlyOnce) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(reducer, state, "", 3, 2);

    const nlohmann::json ended = {
        {"WinnerTeamNum", 0}};
    SideEffects first = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, ended));
    SideEffects duplicate = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, ended));

    ASSERT_TRUE(first.saveMatch);
    EXPECT_FALSE(duplicate.saveMatch);
    EXPECT_FALSE(duplicate.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, MissingGuidCannotCrossPendingMatchGeneration) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "ambiguous-generation-a", 1, 1);
    const uint64_t generationA =
        state->game.activeMatchGeneration;
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());

    StartRankedOnesMatch(
        reducer, state, "ambiguous-generation-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    ASSERT_GT(generationB, generationA);

    SideEffects ambiguous = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"WinnerTeamNum", 0},
            {"bForfeit", true},
            {"Teams",
             nlohmann::json::array(
                 {{{"TeamNum", 0}, {"Score", 5}},
                  {{"TeamNum", 1}, {"Score", 0}}})}});

    EXPECT_FALSE(ambiguous.saveMatch);
    EXPECT_FALSE(ambiguous.postMatchMmrRefresh.has_value());
    EXPECT_FALSE(
        ambiguous.resolvedDestroyedMatch.has_value());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_EQ(state->game.matchGuid, "ambiguous-generation-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects confirmedA =
        reducer.ConfirmPendingDestroyedMatch(
            "ambiguous-generation-a", false);
    ASSERT_TRUE(confirmedA.saveMatch);
    EXPECT_EQ(
        confirmedA.saveSnapshot.matchGuid,
        "ambiguous-generation-a");
}

TEST(TelemetryReducerMatchValidation, NormalLifecycleSavesOnceAndStartsNextGenerationCleanly) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "normal-lifecycle-a", 2, 1);
    const uint64_t generationA =
        state->game.activeMatchGeneration;

    SideEffects endedA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "normal-lifecycle-a"},
                                                                                                              {"WinnerTeamNum", 0}}));
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                      {"MatchGuid", "normal-lifecycle-a"}}));
    ASSERT_TRUE(endedA.saveMatch);
    EXPECT_FALSE(destroyedA.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);

    StartRankedOnesMatch(
        reducer, state, "normal-lifecycle-b", 0, 0);
    EXPECT_GT(state->game.activeMatchGeneration, generationA);
    EXPECT_EQ(state->game.matchGuid, "normal-lifecycle-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
}

TEST(TelemetryReducerMatchValidation, EarlyExitDelayedMissingGuidEventDoesNotFinalizeNewMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "early-exit-a", 2, 2);
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());

    StartRankedOnesMatch(
        reducer, state, "early-exit-b", 0, 0);
    SideEffects delayed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"WinnerTeamNum", 1}});

    EXPECT_FALSE(delayed.saveMatch);
    EXPECT_FALSE(delayed.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.matchGuid, "early-exit-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects explicitA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                 {"MatchGuid", "early-exit-a"},
                                                                                                                 {"WinnerTeamNum", 1}}));
    ASSERT_TRUE(explicitA.saveMatch);

    SideEffects explicitB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                 {"MatchGuid", "early-exit-b"},
                                                                                                                 {"WinnerTeamNum", 0}}));
    ASSERT_TRUE(explicitB.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, RepeatedMatchCreatedDoesNotAdvanceGeneration) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "generation-repeat-a", 0, 0);
    const uint64_t generationA =
        state->game.activeMatchGeneration;

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{
            {"MatchGuid", "generation-repeat-a"}});
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationA);
    EXPECT_TRUE(state->game.roundEverStarted);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{
            {"MatchGuid", "generation-repeat-b"}});
    EXPECT_GT(
        state->game.activeMatchGeneration,
        generationA);
    EXPECT_FALSE(state->game.roundEverStarted);
    EXPECT_EQ(state->game.matchGuid, "generation-repeat-b");
}

TEST(TelemetryReducerMatchValidation, MissingGuidMatchCreatedPreservesActiveIdentifiedLifecycle) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "partial-created-guid", 3, 2);
    const uint64_t generation =
        state->game.activeMatchGeneration;
    state->game.currentMatch.goals = 2;
    auto localPlayer =
        state->game.roster.find("Steam|1");
    ASSERT_NE(localPlayer, state->game.roster.end());
    localPlayer->second.goals = 1;
    ASSERT_EQ(
        state->game.preMatchMmrByGuid.count(
            "partial-created-guid"),
        1u);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{});

    EXPECT_EQ(
        state->game.activeMatchGeneration, generation);
    EXPECT_EQ(
        state->game.matchGuid, "partial-created-guid");
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_TRUE(state->game.roundEverStarted);
    EXPECT_TRUE(state->game.lobbyWasEverFull);
    EXPECT_TRUE(state->game.localPlayerWasActive);
    EXPECT_EQ(state->game.myPrimaryId, "Steam|1");
    EXPECT_EQ(state->game.myTeam, 0);
    EXPECT_EQ(state->game.score[0], 3);
    EXPECT_EQ(state->game.score[1], 2);
    EXPECT_EQ(state->game.currentMatch.goals, 2);
    ASSERT_NE(
        state->game.roster.find("Steam|1"),
        state->game.roster.end());
    EXPECT_EQ(
        state->game.roster.at("Steam|1").goals, 1);
    ASSERT_EQ(
        state->game.preMatchMmrByGuid.count(
            "partial-created-guid"),
        1u);
    EXPECT_EQ(
        state->game.preMatchMmrByGuid
            .at("partial-created-guid")
            .playlistMmrs.at("1v1"),
        1200);

    const nlohmann::json ended = {
        {"MatchGuid", "partial-created-guid"},
        {"WinnerTeamNum", 0}};
    SideEffects first = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED), ended);
    SideEffects duplicate = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED), ended);

    ASSERT_TRUE(first.saveMatch);
    EXPECT_EQ(
        first.saveSnapshot.matchGuid,
        "partial-created-guid");
    EXPECT_FALSE(duplicate.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, LateMatchGuidEnrichmentPreservesActiveLifecycle) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(reducer, state, "", 2, 1);
    const uint64_t generation =
        state->game.activeMatchGeneration;
    state->game.currentMatch.shots = 4;
    auto localPlayer =
        state->game.roster.find("Steam|1");
    ASSERT_NE(localPlayer, state->game.roster.end());
    localPlayer->second.saves = 2;
    ASSERT_EQ(
        state->game.preMatchMmrByGuid.count(""), 1u);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{
            {"MatchGuid", "late-enriched-guid"}});

    EXPECT_EQ(
        state->game.activeMatchGeneration, generation);
    EXPECT_EQ(
        state->game.matchGuid, "late-enriched-guid");
    EXPECT_TRUE(state->game.roundEverStarted);
    EXPECT_TRUE(state->game.lobbyWasEverFull);
    EXPECT_TRUE(state->game.localPlayerWasActive);
    EXPECT_EQ(state->game.myTeam, 0);
    EXPECT_EQ(state->game.score[0], 2);
    EXPECT_EQ(state->game.score[1], 1);
    EXPECT_EQ(state->game.currentMatch.shots, 4);
    EXPECT_EQ(
        state->game.roster.at("Steam|1").saves, 2);
    EXPECT_EQ(
        state->game.preMatchMmrByGuid.count(""), 0u);
    ASSERT_EQ(
        state->game.preMatchMmrByGuid.count(
            "late-enriched-guid"),
        1u);
    EXPECT_EQ(
        state->game.preMatchMmrByGuid
            .at("late-enriched-guid")
            .playlistMatches.at("1v1"),
        50);

    SideEffects ended = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "late-enriched-guid"},
            {"WinnerTeamNum", 0}});

    ASSERT_TRUE(ended.saveMatch);
    EXPECT_EQ(
        ended.saveSnapshot.matchGuid,
        "late-enriched-guid");
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, InactiveMissingGuidMatchCreatedStartsOfflineLifecycle) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "departed-online-guid", 2, 1);
    const uint64_t onlineGeneration =
        state->game.activeMatchGeneration;
    SideEffects endedOnline = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"WinnerTeamNum", 0}}));
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(endedOnline.saveMatch);
    ASSERT_FALSE(state->game.inMatch);

    StartRankedOnesMatch(reducer, state, "", 1, 2);

    EXPECT_GT(
        state->game.activeMatchGeneration,
        onlineGeneration);
    EXPECT_TRUE(state->game.matchGuid.empty());
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_TRUE(state->game.roundEverStarted);
    SideEffects endedOffline = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"WinnerTeamNum", 1}});

    ASSERT_TRUE(endedOffline.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, ReconnectCannotApplyMissingGuidToNewIdentifiedMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "reconnect-a", 1, 1);
    reducer.OnTelemetryDisconnected();

    StartRankedOnesMatch(
        reducer, state, "reconnect-b", 0, 0);

    SideEffects staleMissing = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"WinnerTeamNum", 1}});

    EXPECT_FALSE(staleMissing.saveMatch);
    EXPECT_FALSE(
        staleMissing.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.matchGuid, "reconnect-b");
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects endedB = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "reconnect-b"},
            {"WinnerTeamNum", 0}});
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, StaleMatchDestroyedCannotFinalizeOrDestroyNewerMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "stale-destroyed-a", 1, 1);
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());

    StartRankedOnesMatch(
        reducer, state, "stale-destroyed-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    SideEffects staleDestroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                           {"MatchGuid", "stale-destroyed-a"},
                                                                                                                           {"WinnerTeamNum", 1}}));

    EXPECT_FALSE(staleDestroyedA.saveMatch);
    EXPECT_FALSE(
        staleDestroyedA.pendingDestroyedMatch.has_value());
    EXPECT_EQ(state->game.matchGuid, "stale-destroyed-b");
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects confirmedA =
        reducer.ConfirmPendingDestroyedMatch(
            "stale-destroyed-a", false);
    ASSERT_TRUE(confirmedA.saveMatch);
    EXPECT_EQ(
        confirmedA.saveSnapshot.matchGuid,
        "stale-destroyed-a");
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_FALSE(state->game.matchFinalized);
}

TEST(TelemetryReducerMatchValidation, ConfirmedDestroyedMatchStillBlocksDelayedMissingGuidTerminal) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "confirmed-before-delay-a", 1, 1);

    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                      {"MatchGuid", "confirmed-before-delay-a"}}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());
    SideEffects confirmedA =
        reducer.ConfirmPendingDestroyedMatch(
            "confirmed-before-delay-a", false);
    ASSERT_TRUE(confirmedA.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    StartRankedOnesMatch(
        reducer, state, "confirmed-before-delay-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    SideEffects delayedMissing = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"WinnerTeamNum", 1},
            {"bForfeit", true},
            {"Teams",
             nlohmann::json::array(
                 {{{"TeamNum", 0}, {"Score", 0}},
                  {{"TeamNum", 1}, {"Score", 5}}})}});

    EXPECT_FALSE(delayedMissing.saveMatch);
    EXPECT_FALSE(
        delayedMissing.postMatchMmrRefresh.has_value());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_EQ(
        state->game.matchGuid,
        "confirmed-before-delay-b");
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects endedB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "confirmed-before-delay-b"},
                                                                                                              {"WinnerTeamNum", 0}}));
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, ExplicitPendingEndKeepsBarrierAgainstMissingDuplicate) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "explicit-then-missing-a", 1, 1);
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                      {"MatchGuid", "explicit-then-missing-a"}}));
    ASSERT_TRUE(destroyedA.pendingDestroyedMatch.has_value());

    StartRankedOnesMatch(
        reducer, state, "explicit-then-missing-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    SideEffects explicitA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                 {"MatchGuid", "explicit-then-missing-a"},
                                                                                                                 {"WinnerTeamNum", 1}}));
    ASSERT_TRUE(explicitA.saveMatch);
    ASSERT_TRUE(explicitA.resolvedDestroyedMatch.has_value());
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects missingDuplicateA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"WinnerTeamNum", 1},
            {"bForfeit", true},
            {"Teams",
             nlohmann::json::array(
                 {{{"TeamNum", 0}, {"Score", 0}},
                  {{"TeamNum", 1}, {"Score", 5}}})}});

    EXPECT_FALSE(missingDuplicateA.saveMatch);
    EXPECT_FALSE(
        missingDuplicateA.postMatchMmrRefresh.has_value());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_EQ(
        state->game.matchGuid,
        "explicit-then-missing-b");
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 0);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    SideEffects endedB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "explicit-then-missing-b"},
                                                                                                              {"WinnerTeamNum", 0}}));
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
}

TEST(TelemetryReducerMatchValidation, NormalDepartureKeepsBarrierAgainstDelayedMissingDuplicate) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "normal-departure-a", 2, 1);

    SideEffects endedA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "normal-departure-a"},
                                                                                                              {"WinnerTeamNum", 0}}));
    ASSERT_TRUE(endedA.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                                      {"MatchGuid", "normal-departure-a"}}));
    EXPECT_FALSE(destroyedA.saveMatch);

    StartRankedOnesMatch(
        reducer, state, "normal-departure-b", 0, 0);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    SideEffects delayedDuplicateA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"WinnerTeamNum", 1},
            {"bForfeit", true},
            {"Teams",
             nlohmann::json::array(
                 {{{"TeamNum", 0}, {"Score", 0}},
                  {{"TeamNum", 1}, {"Score", 5}}})}});

    EXPECT_FALSE(delayedDuplicateA.saveMatch);
    EXPECT_FALSE(
        delayedDuplicateA.postMatchMmrRefresh.has_value());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_EQ(state->game.matchGuid, "normal-departure-b");
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_FALSE(state->game.matchFinalized);
    EXPECT_EQ(state->game.score[0], 0);
    EXPECT_EQ(state->game.score[1], 0);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects endedB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{
                                                                                                              {"MatchGuid", "normal-departure-b"},
                                                                                                              {"WinnerTeamNum", 0}}));
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 2);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);
}

TEST(TelemetryReducerMatchValidation, ConsecutiveCleanGuidlessMatchesFinalizeAndDepart) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    StartRankedOnesMatch(reducer, state, "", 2, 1);
    const uint64_t generationA =
        state->game.activeMatchGeneration;
    SideEffects endedA = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{{"WinnerTeamNum", 0}}));
    SideEffects destroyedA = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));
    ASSERT_TRUE(endedA.saveMatch);
    EXPECT_FALSE(destroyedA.saveMatch);
    EXPECT_FALSE(state->game.inMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);

    StartRankedOnesMatch(reducer, state, "", 1, 2);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    ASSERT_GT(generationB, generationA);
    SideEffects endedB = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), CurrentMatchEvent(state, nlohmann::json{{"WinnerTeamNum", 1}}));
    SideEffects destroyedB = reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), CurrentMatchEvent(state, nlohmann::json{}));

    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_FALSE(destroyedB.saveMatch);
    EXPECT_FALSE(state->game.inMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{});
    EXPECT_GT(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_TRUE(state->game.inMatch);
}

TEST(TelemetryReducerMatchValidation, MissingGuidMatchCreatedAfterReconnectPreservesIdentifiedLifecycle) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    StartRankedOnesMatch(
        reducer, state, "reconnect-identified-a", 1, 1);
    const uint64_t generation =
        state->game.activeMatchGeneration;
    reducer.OnTelemetryDisconnected();
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_CREATED),
        nlohmann::json{});

    EXPECT_EQ(
        state->game.activeMatchGeneration, generation);
    EXPECT_EQ(
        state->game.matchGuid, "reconnect-identified-a");
    EXPECT_TRUE(state->game.roundEverStarted);

    SideEffects missing = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"WinnerTeamNum", 0}});
    EXPECT_FALSE(missing.saveMatch);
    EXPECT_FALSE(state->game.matchFinalized);

    SideEffects explicitEnd = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "reconnect-identified-a"},
            {"WinnerTeamNum", 0}});
    ASSERT_TRUE(explicitEnd.saveMatch);
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, StaleMatchEndedVoidGuidCannotAttachToNewGuidlessMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "ended-void-match-a", 0, 0, false);

    SideEffects endedA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        CurrentMatchEvent(
            state,
            nlohmann::json{{"WinnerTeamNum", 0}}));
    EXPECT_FALSE(endedA.saveMatch);
    EXPECT_EQ(
        state->game.lastMatchVoidReason,
        "round_never_started");
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));

    StartRankedOnesMatch(
        reducer, state, "", 1, 0, true, false, false);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    ASSERT_TRUE(state->game.matchGuid.empty());

    SideEffects staleA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "ended-void-match-a"},
            {"WinnerTeamNum", 1}});
    EXPECT_FALSE(staleA.saveMatch);
    EXPECT_TRUE(state->game.matchGuid.empty());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects endedB = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "ended-void-match-b"},
            {"WinnerTeamNum", 0}});
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(
        endedB.saveSnapshot.matchGuid,
        "ended-void-match-b");
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, StaleVoidedGuidCannotAttachToNewGuidlessMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "voided-match-a", 0, 0, false);
    SideEffects destroyedA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        CurrentMatchEvent(state, nlohmann::json{}));
    EXPECT_FALSE(destroyedA.saveMatch);
    EXPECT_EQ(
        state->game.lastMatchVoidReason,
        "round_never_started");

    StartRankedOnesMatch(
        reducer, state, "", 1, 0, true, false, false);
    const uint64_t generationB =
        state->game.activeMatchGeneration;
    ASSERT_TRUE(state->game.matchGuid.empty());

    SideEffects staleA = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "voided-match-a"},
            {"WinnerTeamNum", 1}});
    EXPECT_FALSE(staleA.saveMatch);
    EXPECT_TRUE(state->game.matchGuid.empty());
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generationB);
    EXPECT_TRUE(state->game.inMatch);
    EXPECT_EQ(state->game.sessionTotals.losses, 0);

    SideEffects endedB = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{
            {"MatchGuid", "active-match-b"},
            {"WinnerTeamNum", 0}});
    ASSERT_TRUE(endedB.saveMatch);
    EXPECT_EQ(
        endedB.saveSnapshot.matchGuid,
        "active-match-b");
    EXPECT_EQ(state->game.sessionTotals.wins, 1);
}

TEST(TelemetryReducerMatchValidation, TerminalGuidEnrichesArenaStartedReplayForfeit) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    StartRankedOnesMatch(
        reducer, state, "", 3, 2, true, false, false);
    const uint64_t generation =
        state->game.activeMatchGeneration;
    EnterGoalReplay(reducer);

    SideEffects destroyed = reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        nlohmann::json{
            {"MatchGuid", "late-terminal-guid"},
            {"bLocalPlayerForfeit", true}});

    ASSERT_GT(generation, 0u);
    ASSERT_TRUE(destroyed.saveMatch);
    EXPECT_EQ(
        destroyed.saveSnapshot.matchGuid,
        "late-terminal-guid");
    ASSERT_TRUE(destroyed.postMatchMmrRefresh.has_value());
    EXPECT_EQ(
        destroyed.postMatchMmrRefresh->matchGuid,
        "late-terminal-guid");
    EXPECT_TRUE(
        destroyed.postMatchMmrRefresh->provisionalImmediately);
    EXPECT_FALSE(
        destroyed.pendingDestroyedMatch.has_value());
    EXPECT_FALSE(state->game.inMatch);
    EXPECT_EQ(
        state->game.activeMatchGeneration,
        generation);
    EXPECT_EQ(state->game.sessionTotals.losses, 1);
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
