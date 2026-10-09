#include <gtest/gtest.h>
#include "core/TelemetryReducer.hpp"
#include "core/SessionState.hpp"
#include "core/Constants.hpp"
#include "core/Storage.hpp"
#include <memory>
#include <thread>
#include <chrono>

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

TEST(TelemetryReducerStats, AttributesNameOnlyEventPayloadsToRosterPlayersByNameAndTeam) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.roster["Steam|1"] = PlayerData{.primaryId = "Steam|1", .name = "Me", .team = 0};
        state->game.roster["Epic|2"] = PlayerData{.primaryId = "Epic|2", .name = "Twin", .team = 0};
        state->game.roster["PS4|3"] = PlayerData{.primaryId = "PS4|3", .name = "Twin", .team = 1};
        state->game.roster["Unknown|Bot"] = PlayerData{.primaryId = "Unknown|Bot", .name = "Bot", .team = 1};
    }

    const auto statfeed = [&](const char* eventName, const char* name, int team) {
        nlohmann::json data;
        data["EventName"] = eventName;
        data["MainTarget"] = {{"Name", name}, {"Shortcut", 1}, {"TeamNum", team}};
        reducer.Reduce(std::string(Constants::EVT_STATFEED), data);
    };
    statfeed("Shot", "Me", 0);
    statfeed("Save", "Twin", 1);
    statfeed("Assist", "Twin", 0);
    statfeed("Demolish", "Bot", 1);

    nlohmann::json goal;
    goal["GoalSpeed"] = 80.0;
    goal["Scorer"] = {{"Name", "Me"}, {"Shortcut", 1}, {"TeamNum", 0}};
    reducer.Reduce(std::string(Constants::EVT_GOAL_SCORED), goal);

    nlohmann::json ambiguous;
    ambiguous["EventName"] = "Shot";
    ambiguous["MainTarget"] = {{"Name", "Twin"}};
    reducer.Reduce(std::string(Constants::EVT_STATFEED), ambiguous);

    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    EXPECT_EQ(state->game.roster["Steam|1"].shots, 1);
    EXPECT_EQ(state->game.roster["Steam|1"].goals, 1);
    EXPECT_EQ(state->game.currentMatch.shotsSelf, 1);
    EXPECT_EQ(state->game.currentMatch.goalsSelf, 1);
    EXPECT_EQ(state->game.roster["PS4|3"].saves, 1);
    EXPECT_EQ(state->game.roster["PS4|3"].shots, 0);
    EXPECT_EQ(state->game.roster["Epic|2"].assists, 1);
    EXPECT_EQ(state->game.roster["Epic|2"].shots, 0);
    EXPECT_EQ(state->game.roster["Unknown|Bot"].demos, 1);
    EXPECT_EQ(state->game.currentMatch.shots, 2);
}

TEST(TelemetryReducerStats, CapturesScoreboardScoreFromUpdateStateAndLeavesUnseenScoresUnknown) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    const auto update = [&](int myScore) {
        reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE),
                       nlohmann::json{{"Game", {{"Arena", "Stadium_P"}, {"bReplay", false}, {"bSpectator", false}}},
                                      {"Players",
                                       {{{"PrimaryId", "Steam|1"}, {"Name", "Me"}, {"TeamNum", 0}, {"Score", myScore}},
                                        {{"PrimaryId", "Epic|2"}, {"Name", "Opp"}, {"TeamNum", 1}}}}});
    };
    update(120);
    update(412);

    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    ASSERT_TRUE(state->game.matchRoster.count("Steam|1"));
    EXPECT_EQ(state->game.matchRoster["Steam|1"].score, 412);
    ASSERT_TRUE(state->game.matchRoster.count("Epic|2"));
    EXPECT_EQ(state->game.matchRoster["Epic|2"].score, -1);
}

TEST(TelemetryReducerStats, AddsDemoedSelfToSessionTotalsOnMatchEnd) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.inMatch = true;
        state->game.roundEverStarted = true;
        state->game.localPlayerWasActive = true;
        state->game.legacyMaxPlayersSeen = 2;
        state->game.legacyMaxTeamPlayersSeen[0] = 1;
        state->game.legacyMaxTeamPlayersSeen[1] = 1;
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

TEST(TelemetryReducerStats, SessionDemolitionsDoNotDoubleCountFinalizedMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.inMatch = true;
        state->game.roundEverStarted = true;
        state->game.localPlayerWasActive = true;
        state->game.legacyMaxPlayersSeen = 2;
        state->game.legacyMaxTeamPlayersSeen[0] = 1;
        state->game.legacyMaxTeamPlayersSeen[1] = 1;
        state->game.sessionTotals.demos = 5;
        state->game.sessionTotals.demoed = 4;
        state->game.currentMatch.demosSelf = 3;
        state->game.currentMatch.demoedSelf = 2;
        state->game.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1",
            .name = "P1",
            .team = 0};
        state->game.roster["Steam|2"] = PlayerData{
            .primaryId = "Steam|2",
            .name = "P2",
            .team = 1};

        const DemolitionCounts liveCounts =
            CalculateSessionDemolitionCounts(
                state->game.sessionTotals,
                state->game.currentMatch,
                state->game.matchFinalized);
        EXPECT_EQ(liveCounts.demos, 8);
        EXPECT_EQ(liveCounts.demoed, 6);
    }
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    nlohmann::json matchEndedData;
    matchEndedData["WinnerTeamNum"] = 0;
    reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), matchEndedData);

    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    ASSERT_TRUE(state->game.matchFinalized);
    const DemolitionCounts finalCounts =
        CalculateSessionDemolitionCounts(
            state->game.sessionTotals,
            state->game.currentMatch,
            state->game.matchFinalized);
    EXPECT_EQ(state->game.sessionTotals.demos, 8);
    EXPECT_EQ(state->game.sessionTotals.demoed, 6);
    EXPECT_EQ(finalCounts.demos, 8);
    EXPECT_EQ(finalCounts.demoed, 6);
}

TEST(TelemetryReducerStats, SessionBoostUsesOnlyLocalPlayerAndFinalizesOnce) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.matchGuid = "boost-finalize-guid";
        state->game.inMatch = true;
        state->game.roundEverStarted = true;
        state->game.localPlayerWasActive = true;
        state->game.legacyMaxPlayersSeen = 2;
        state->game.legacyMaxTeamPlayersSeen = {1, 1};
        state->game.legacyLobbyWasEverFull = true;
        state->game.sessionTotals.boostPickedUp = 10;
        state->game.currentMatch.boostPickedUp = 125;
        state->game.currentMatch.boostPickedUpSelf = 25;
        state->game.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1", .name = "P1", .team = 0};
        state->game.roster["Steam|2"] = PlayerData{
            .primaryId = "Steam|2", .name = "P2", .team = 1};

        EXPECT_EQ(
            CalculateSessionBoostPickedUp(
                state->game.sessionTotals,
                state->game.currentMatch,
                false),
            35);
    }
    state->ui.rosterMmrCategory.store(MmrCategory::OneVOne);

    SideEffects effects = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "boost-finalize-guid"},
                       {"WinnerTeamNum", 0}});

    ASSERT_TRUE(effects.saveMatch);
    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    EXPECT_EQ(state->game.sessionTotals.boostPickedUp, 35);
    EXPECT_EQ(
        CalculateSessionBoostPickedUp(
            state->game.sessionTotals,
            state->game.currentMatch,
            state->game.matchFinalized),
        35);
}

TEST(TelemetryReducerStats, VoidedMatchDoesNotLeakLiveBoostIntoSessionTotal) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.matchGuid = "boost-void-guid";
        state->game.inMatch = true;
        state->game.roundEverStarted = true;
        state->game.localPlayerWasActive = false; // forces a void result
        state->game.sessionTotals.boostPickedUp = 10;
        state->game.currentMatch.boostPickedUp = 125;
        state->game.currentMatch.boostPickedUpSelf = 25;
        state->game.roster["Steam|1"] = PlayerData{
            .primaryId = "Steam|1", .name = "P1", .team = 0};
    }

    SideEffects effects = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "boost-void-guid"},
                       {"WinnerTeamNum", 0}});

    EXPECT_FALSE(effects.saveMatch);
    std::shared_lock<std::shared_mutex> lock(state->game.mutex);
    EXPECT_EQ(state->game.sessionTotals.boostPickedUp, 10);
}

TEST(TelemetryReducerStats, LifecycleEventsWriteSteadyClockTimestampsAndGoalReplayFlags) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    const int64_t matchStartMs = state->ui.lastMatchStartMs.load();
    EXPECT_GT(matchStartMs, 0);
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), 0);
    EXPECT_EQ(state->ui.lastCountdownMs.load(), 0);
    EXPECT_EQ(state->ui.lastGoalMs.load(), 0);
    EXPECT_EQ(state->ui.lastMatchEndMs.load(), 0);
    EXPECT_EQ(state->ui.lastPodiumMs.load(), 0);
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_MATCH_INITIALIZED), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    const int64_t firstCountdownMs = state->ui.firstCountdownOfMatchMs.load();
    EXPECT_GE(firstCountdownMs, matchStartMs);
    EXPECT_EQ(state->ui.lastCountdownMs.load(), firstCountdownMs);

    // First UpdateState with Arena during initial countdown must preserve UI timestamps.
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        nlohmann::json{{"Game", {{"Arena", "Stadium_P"}, {"bReplay", false}, {"bSpectator", false}}}});
    EXPECT_EQ(state->ui.lastMatchStartMs.load(), matchStartMs);
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), firstCountdownMs);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_COUNTDOWN_BEGIN), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    const int64_t round1CountdownMs = state->ui.lastCountdownMs.load();
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), firstCountdownMs);
    EXPECT_GE(round1CountdownMs, firstCountdownMs);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), firstCountdownMs);
    EXPECT_EQ(state->ui.lastCountdownMs.load(), round1CountdownMs);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_GOAL_SCORED), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    const int64_t goalMs = state->ui.lastGoalMs.load();
    EXPECT_GE(goalMs, round1CountdownMs);

    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    EXPECT_TRUE(state->ui.inGoalReplay.load());

    // GoalReplayWillEnd is not a clear.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_WILL_END), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    EXPECT_TRUE(state->ui.inGoalReplay.load());

    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_END), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "vis-guid-1"}, {"WinnerTeamNum", 0}});
    const int64_t matchEndMs = state->ui.lastMatchEndMs.load();
    EXPECT_GE(matchEndMs, goalMs);
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_PODIUM_START), nlohmann::json{{"MatchGuid", "vis-guid-1"}});
    const int64_t podiumMs = state->ui.lastPodiumMs.load();
    EXPECT_GE(podiumMs, matchEndMs);
    EXPECT_EQ(state->ui.matchSummaryStartMs.load(), podiumMs);
}

TEST(TelemetryReducerStats, SkippedGoalReplayClearsOnCountdownRoundMatchEndDestroyAndNewMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "replay-clear-1"}});

    // Skipped replay cleared by next CountdownBegin.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_COUNTDOWN_BEGIN), nlohmann::json{});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    // Cleared by RoundStarted.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    // Cleared by MatchEnded.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED), nlohmann::json{{"MatchGuid", "replay-clear-1"}, {"WinnerTeamNum", 0}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    // Cleared by MatchDestroyed on an active match.
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "replay-clear-2"}});
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{{"MatchGuid", "replay-clear-2"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), 0);

    // Cleared by new match while a goal replay is active in the prior match.
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "replay-clear-3"}});
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "replay-clear-4"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());
}

TEST(TelemetryReducerStats, FirstCountdownFallbacksAndResetPerMatch) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    // Fallback 1: no MatchInitialized -> first CountdownBegin sets firstCountdownOfMatchMs.
    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "fb-guid-1"}});
    reducer.Reduce(std::string(Constants::EVT_COUNTDOWN_BEGIN), nlohmann::json{{"MatchGuid", "fb-guid-1"}});
    const int64_t firstCd1 = state->ui.firstCountdownOfMatchMs.load();
    ASSERT_GT(firstCd1, 0);
    EXPECT_EQ(state->ui.lastCountdownMs.load(), firstCd1);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_COUNTDOWN_BEGIN), nlohmann::json{{"MatchGuid", "fb-guid-1"}});
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), firstCd1);
    EXPECT_GT(state->ui.lastCountdownMs.load(), firstCd1);

    // Fallback 2: new match via MatchDestroyed + RoundStarted with no countdown events.
    reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{{"MatchGuid", "fb-guid-1"}});
    EXPECT_EQ(state->ui.firstCountdownOfMatchMs.load(), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{{"MatchGuid", "fb-guid-2"}});
    const int64_t firstCd2 = state->ui.firstCountdownOfMatchMs.load();
    EXPECT_GT(firstCd2, firstCd1);
    EXPECT_EQ(state->ui.lastCountdownMs.load(), firstCd2);

    // Reset via new GUID arriving on CountdownBegin without MatchCreated.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_COUNTDOWN_BEGIN), nlohmann::json{{"MatchGuid", "fb-guid-3"}});
    const int64_t firstCd3 = state->ui.firstCountdownOfMatchMs.load();
    EXPECT_GT(firstCd3, firstCd2);
    EXPECT_EQ(state->ui.lastGoalMs.load(), 0);
}

TEST(TelemetryReducerStats, RejectedOrDuplicateTerminalEventsDoNotRestartVisibilityWindows) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{{"MatchGuid", "vis-term-1"}});

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "vis-term-1"}, {"WinnerTeamNum", 0}});
    const int64_t endMs = state->ui.lastMatchEndMs.load();
    ASSERT_GT(endMs, 0);

    // Match-winning goal replay after MatchEnded (e.g. OT golden goal) still sets inGoalReplay
    // for the current match, while stale/other GUIDs are rejected.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "other-stale-guid"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_END), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    // PodiumStart also clears an active post-MatchEnded goal replay and stamps lastPodiumMs.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    reducer.Reduce(std::string(Constants::EVT_PODIUM_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());
    const int64_t podiumMs = state->ui.lastPodiumMs.load();
    ASSERT_GE(podiumMs, endMs);

    // Duplicate MatchEnded and PodiumStart for the already-finalized match must not overwrite timestamps.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "vis-term-1"}, {"WinnerTeamNum", 0}});
    EXPECT_EQ(state->ui.lastMatchEndMs.load(), endMs);

    reducer.Reduce(std::string(Constants::EVT_PODIUM_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_EQ(state->ui.lastPodiumMs.load(), podiumMs);
    EXPECT_EQ(state->ui.matchSummaryStartMs.load(), podiumMs);

    // Late GoalScored for the finalized match must still be ignored.
    reducer.Reduce(std::string(Constants::EVT_GOAL_SCORED), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_EQ(state->ui.lastGoalMs.load(), 0);

    // MatchDestroyed clears an active post-MatchEnded goal replay, and once inMatch == false
    // any late GoalReplayStart, MatchEnded, or PodiumStart is rejected.
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_TRUE(state->ui.inGoalReplay.load());
    reducer.Reduce(std::string(Constants::EVT_MATCH_DESTROYED), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    ASSERT_FALSE(state->game.inMatch.load());
    EXPECT_FALSE(state->ui.inGoalReplay.load());

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    reducer.Reduce(std::string(Constants::EVT_GOAL_REPLAY_START), nlohmann::json{{"MatchGuid", "vis-term-1"}});
    EXPECT_FALSE(state->ui.inGoalReplay.load());
    reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "vis-late-guid"}, {"WinnerTeamNum", 0}});
    reducer.Reduce(std::string(Constants::EVT_PODIUM_START), nlohmann::json{{"MatchGuid", "vis-late-guid"}});
    EXPECT_EQ(state->ui.lastMatchEndMs.load(), endMs);
    EXPECT_EQ(state->ui.lastPodiumMs.load(), podiumMs);
}

TEST(TelemetryReducerStats, AuthoritativeUpdateStateCountersOverrideStatfeedAndAbsentFieldsFallBack) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", "override-guid-1"}});
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{{"MatchGuid", "override-guid-1"}});
    {
        std::unique_lock<std::shared_mutex> lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|1";
        state->game.myTeam = 0;
        state->game.roster["Steam|1"] = PlayerData{.primaryId = "Steam|1", .name = "Me", .team = 0};
        state->game.roster["Epic|2"] = PlayerData{.primaryId = "Epic|2", .name = "Opp", .team = 1};
    }

    reducer.Reduce(std::string(Constants::EVT_STATFEED),
                   nlohmann::json{{"EventName", "Shot"}, {"MainTarget", {{"Name", "Me"}, {"PrimaryId", "Steam|1"}, {"TeamNum", 0}}}});
    reducer.Reduce(std::string(Constants::EVT_STATFEED),
                   nlohmann::json{{"EventName", "Assist"}, {"MainTarget", {{"Name", "Me"}, {"PrimaryId", "Steam|1"}, {"TeamNum", 0}}}});
    reducer.Reduce(std::string(Constants::EVT_STATFEED),
                   nlohmann::json{{"EventName", "Save"}, {"MainTarget", {{"Name", "Opp"}, {"PrimaryId", "Epic|2"}, {"TeamNum", 1}}}});

    {
        std::shared_lock<std::shared_mutex> lock(state->game.mutex);
        EXPECT_EQ(state->game.roster["Steam|1"].shots, 1);
        EXPECT_EQ(state->game.roster["Steam|1"].assists, 1);
        EXPECT_EQ(state->game.currentMatch.shotsSelf, 1);
        EXPECT_EQ(state->game.currentMatch.assistsSelf, 1);
        EXPECT_EQ(state->game.roster["Epic|2"].saves, 1);
    }

    // Assists (Steam|1) and Saves (Epic|2) are absent and must keep statfeed values.
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        nlohmann::json{{"Game", {{"Arena", "Stadium_P"}, {"bReplay", false}, {"bSpectator", false}, {"PlaylistId", 11}}},
                       {"Players",
                        {{{"PrimaryId", "Steam|1"},
                          {"Name", "Me"},
                          {"TeamNum", 0},
                          {"Goals", 2},
                          {"Shots", 4},
                          {"Touches", 32},
                          {"CarTouches", 28},
                          {"Score", 420}},
                         {{"PrimaryId", "Epic|2"},
                          {"Name", "Opp"},
                          {"TeamNum", 1},
                          {"Touches", 19},
                          {"CarTouches", 15},
                          {"Score", 180}}}}});

    {
        std::shared_lock<std::shared_mutex> lock(state->game.mutex);
        const auto& me = state->game.roster["Steam|1"];
        EXPECT_EQ(me.goals, 2);
        EXPECT_EQ(me.shots, 4);
        EXPECT_EQ(me.assists, 1);
        EXPECT_EQ(state->game.currentMatch.goalsSelf, 2);
        EXPECT_EQ(state->game.currentMatch.shotsSelf, 4);
        EXPECT_EQ(state->game.currentMatch.assistsSelf, 1);
        ASSERT_TRUE(me.touches.has_value());
        EXPECT_EQ(*me.touches, 32);
        ASSERT_TRUE(me.carTouches.has_value());
        EXPECT_EQ(*me.carTouches, 28);
        EXPECT_EQ(me.score, 420);

        const auto& opp = state->game.roster["Epic|2"];
        EXPECT_EQ(opp.saves, 1);
        ASSERT_TRUE(opp.touches.has_value());
        EXPECT_EQ(*opp.touches, 19);
        ASSERT_TRUE(opp.carTouches.has_value());
        EXPECT_EQ(*opp.carTouches, 15);
        EXPECT_EQ(opp.score, 180);
    }

    SideEffects endEffects = reducer.Reduce(
        std::string(Constants::EVT_MATCH_ENDED),
        nlohmann::json{{"MatchGuid", "override-guid-1"}, {"WinnerTeamNum", 0}});

    EXPECT_TRUE(endEffects.saveMatch);
    ASSERT_TRUE(endEffects.saveSnapshot.roster.count("Steam|1"));
    ASSERT_TRUE(endEffects.saveSnapshot.roster["Steam|1"].touches.has_value());
    EXPECT_EQ(*endEffects.saveSnapshot.roster["Steam|1"].touches, 32);
    ASSERT_TRUE(endEffects.saveSnapshot.roster["Steam|1"].carTouches.has_value());
    EXPECT_EQ(*endEffects.saveSnapshot.roster["Steam|1"].carTouches, 28);

    ASSERT_TRUE(endEffects.saveSnapshot.roster.count("Epic|2"));
    ASSERT_TRUE(endEffects.saveSnapshot.roster["Epic|2"].touches.has_value());
    EXPECT_EQ(*endEffects.saveSnapshot.roster["Epic|2"].touches, 19);
    ASSERT_TRUE(endEffects.saveSnapshot.roster["Epic|2"].carTouches.has_value());
    EXPECT_EQ(*endEffects.saveSnapshot.roster["Epic|2"].carTouches, 15);
}

namespace {
    void SendClock(TelemetryReducer& reducer, float clock, bool overtime) {
        reducer.Reduce(
            std::string(Constants::EVT_UPDATE_STATE),
            nlohmann::json{{"Game",
                            {{"Arena", "Stadium_P"},
                             {"bReplay", false},
                             {"bSpectator", false},
                             {"bOvertime", overtime},
                             {"PlaylistId", 11},
                             {"TimeSeconds", clock}}},
                           {"Players",
                            {{{"PrimaryId", "Steam|1"}, {"Name", "Me"}, {"TeamNum", 0}},
                             {{"PrimaryId", "Epic|2"}, {"Name", "Opp"}, {"TeamNum", 1}}}}});
    }

    void StartTimedMatch(TelemetryReducer& reducer, SessionState& state, const std::string& guid) {
        reducer.Reduce(std::string(Constants::EVT_MATCH_CREATED), nlohmann::json{{"MatchGuid", guid}});
        {
            std::unique_lock<std::shared_mutex> lock(state.game.mutex);
            state.game.myPrimaryId = "Steam|1";
            state.game.myTeam = 0;
            state.game.roster["Steam|1"] = PlayerData{.primaryId = "Steam|1", .name = "Me", .team = 0};
            state.game.roster["Epic|2"] = PlayerData{.primaryId = "Epic|2", .name = "Opp", .team = 1};
        }
        reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{{"MatchGuid", guid}});
    }
} // namespace

TEST(TelemetryReducerStats, RegulationDurationFollowsGameClockNotWallTime) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    auto now = std::chrono::steady_clock::time_point{std::chrono::seconds(2000)};
    reducer.SetClockForTests([&]() { return now; });

    StartTimedMatch(reducer, *state, "regular-guid-1");
    SendClock(reducer, 300.0f, false);
    now += std::chrono::seconds(200);
    SendClock(reducer, 150.0f, false);
    // Goal replays and kickoff countdowns add wall time while the clock is frozen.
    now += std::chrono::seconds(60);
    SendClock(reducer, 150.0f, false);
    now += std::chrono::seconds(160);
    SendClock(reducer, 0.0f, false);

    SideEffects endEffects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED),
                                            nlohmann::json{{"MatchGuid", "regular-guid-1"}, {"WinnerTeamNum", 0}});

    EXPECT_TRUE(endEffects.saveMatch);
    EXPECT_NEAR(endEffects.saveSnapshot.durationSeconds, 300.0f, 0.01f);
    EXPECT_FLOAT_EQ(endEffects.saveSnapshot.overtimeSeconds, 0.0f);
    EXPECT_FLOAT_EQ(state->game.matchSummaryOvertimeSeconds, 0.0f);
}

TEST(TelemetryReducerStats, ForfeitDurationIsClockElapsed) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    StartTimedMatch(reducer, *state, "forfeit-guid-1");
    SendClock(reducer, 300.0f, false);
    SendClock(reducer, 172.0f, false);

    SideEffects endEffects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED),
                                            nlohmann::json{{"MatchGuid", "forfeit-guid-1"}, {"WinnerTeamNum", 0}});

    EXPECT_TRUE(endEffects.saveMatch);
    EXPECT_NEAR(endEffects.saveSnapshot.durationSeconds, 128.0f, 0.01f);
}

TEST(TelemetryReducerStats, OvertimeAddsUnpausedTimeAfterFlip) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);
    auto now = std::chrono::steady_clock::time_point{std::chrono::seconds(1000)};
    reducer.SetClockForTests([&]() { return now; });

    StartTimedMatch(reducer, *state, "timing-guid-1");
    SendClock(reducer, 300.0f, false);
    now += std::chrono::seconds(400);
    SendClock(reducer, 0.0f, false);
    SendClock(reducer, 0.0f, true);
    EXPECT_TRUE(state->game.bOvertime);

    now += std::chrono::seconds(20);
    reducer.Reduce(std::string(Constants::EVT_MATCH_PAUSED), nlohmann::json{});
    now += std::chrono::seconds(15);
    reducer.Reduce(std::string(Constants::EVT_MATCH_UNPAUSED), nlohmann::json{});
    now += std::chrono::seconds(25);

    SideEffects endEffects = reducer.Reduce(std::string(Constants::EVT_MATCH_ENDED),
                                            nlohmann::json{{"MatchGuid", "timing-guid-1"}, {"WinnerTeamNum", 0}});

    EXPECT_TRUE(endEffects.saveMatch);
    EXPECT_NEAR(endEffects.saveSnapshot.overtimeSeconds, 45.0f, 0.01f);
    EXPECT_NEAR(endEffects.saveSnapshot.durationSeconds, 345.0f, 0.01f);
    EXPECT_NEAR(state->game.matchSummaryOvertimeSeconds, 45.0f, 0.01f);
}
