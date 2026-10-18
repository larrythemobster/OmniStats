#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "core/game/MatchAnalytics.hpp"

namespace {
    using nlohmann::json;

    json Car(const char* id, const char* name, int team, int shortcut, int boost, json extra = json::object()) {
        json p{{"PrimaryId", id}, {"Name", name}, {"TeamNum", team}, {"Shortcut", shortcut}, {"Boost", boost}, {"bHasCar", true}, {"bOnGround", true}, {"Speed", 40.0}};
        for (auto& [k, v] : extra.items())
            p[k] = v;
        return p;
    }

    json Remote(const char* id, const char* name, int team, int shortcut) {
        return {{"PrimaryId", id}, {"Name", name}, {"TeamNum", team}, {"Shortcut", shortcut}};
    }

    json State(json players, float clock, int ballTeam = 255, bool replay = false) {
        // A one-element braced list of json copy-constructs the element instead of making an array.
        if (!players.is_array()) players = json::array({players});
        return {{"Players", std::move(players)},
                {"Game", {{"TimeSeconds", clock}, {"bOvertime", false}, {"bReplay", replay}, {"Ball", {{"Speed", 50.0}, {"TeamNum", ballTeam}}}}}};
    }

    json Hit(const char* name, int team, int shortcut, float x, float y, float z, float speed) {
        return {{"Players", json::array({{{"Name", name}, {"TeamNum", team}, {"Shortcut", shortcut}}})},
                {"Ball", {{"PostHitSpeed", speed}, {"Location", {{"X", x}, {"Y", y}, {"Z", z}}}}}};
    }

    json Goal(const char* scorer, int team, int shortcut) {
        return {{"Scorer", {{"Name", scorer}, {"TeamNum", team}, {"Shortcut", shortcut}}}, {"GoalSpeed", 90.0}};
    }

    // Streams `seconds` of 0.1 s UpdateState packets built by `make(clock)`.
    template <typename F>
    double Play(MatchAnalyticsTracker& t, double now, float seconds, float& clock, F make) {
        for (int i = 0; i < static_cast<int>(seconds * 10.0f + 0.5f); ++i) {
            now += 0.1;
            clock -= 0.1f;
            t.OnUpdateState(make(clock), now);
        }
        return now;
    }
}

TEST(MatchAnalyticsTest, BoostAccountingIgnoresKickoffAndRespawnRefills) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    t.OnUpdateState(State({Car("Steam|1", "Me", 0, 1, 33)}, clock), now);
    t.OnRoundStarted(now);
    int boost = 33;
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });
    boost = 45; // small pad
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });
    boost = 100; // big pad
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });
    boost = 60;
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });
    now = Play(t, now, 0.5f, clock, [&](float c) {
        return State({Car("Steam|1", "Me", 0, 1, 0, {{"bDemolished", true}})}, c);
    });
    boost = 33; // respawn refill
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });
    t.OnGoalScored(Goal("Me", 0, 1));
    t.OnCountdownBegin();
    t.OnRoundStarted(now);
    boost = 33; // kickoff reset after the round ended at 33
    Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 0, 1, boost)}, c); });

    const MatchAnalytics a = t.Snapshot();
    const auto* me = a.FindPlayer("Steam|1");
    ASSERT_NE(me, nullptr);
    ASSERT_TRUE(me->car.has_value());
    EXPECT_EQ(me->car->boostCollected, 12 + 55);
    EXPECT_EQ(me->car->boostUsed, 40);
    EXPECT_EQ(me->car->smallPads, 1);
    EXPECT_EQ(me->car->bigPads, 1);
    EXPECT_NEAR(me->car->demolishedSeconds, 0.5f, 0.01f);
}

TEST(MatchAnalyticsTest, BoostPickupEventsReplaceDeltaPadHeuristicAndCountSteals) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    t.OnUpdateState(State({Car("Steam|1", "Me", 1, 1, 33)}, clock), now);
    t.OnRoundStarted(now);
    now = Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 1, 1, 33)}, c); });
    Play(t, now, 0.1f, clock, [&](float c) { return State({Car("Steam|1", "Me", 1, 1, 60)}, c); });
    // Orange attacks toward negative Y, so a pad at Y=-3000 is on the opponent half.
    t.OnBoostPickup({{"Player", {{"Name", "Me"}, {"TeamNum", 1}, {"Shortcut", 1}}},
                     {"Location", {{"X", 0}, {"Y", -3000}, {"Z", 70}}},
                     {"BoostType", "BoostType_Pad"}});
    t.OnBoostPickup({{"Player", {{"Name", "Me"}, {"TeamNum", 1}, {"Shortcut", 1}}},
                     {"Location", {{"X", 3584}, {"Y", 0}, {"Z", 73}}},
                     {"BoostType", "BoostType_Pill"}});

    const MatchAnalytics a = t.Snapshot();
    const auto& c = *a.FindPlayer("Steam|1")->car;
    EXPECT_TRUE(c.padsFromPickupEvents);
    EXPECT_EQ(c.smallPads, 1);
    EXPECT_EQ(c.bigPads, 1);
    EXPECT_EQ(c.stolenSmallPads, 1);
    EXPECT_EQ(c.stolenBigPads, 0);
}

TEST(MatchAnalyticsTest, CarSpeedUnitsAreNormalizedFromObservedRange) {
    auto run = [](float rawSpeed) {
        MatchAnalyticsTracker t;
        float clock = 300.0f;
        double now = 0.0;
        t.OnUpdateState(State({Car("Steam|1", "Me", 0, 1, 50)}, clock), now);
        t.OnRoundStarted(now);
        Play(t, now, 1.0f, clock, [&](float c) {
            return State({Car("Steam|1", "Me", 0, 1, 50, {{"Speed", rawSpeed}, {"bSupersonic", false}})}, c);
        });
        return *t.Snapshot().FindPlayer("Steam|1")->car;
    };
    const CarTelemetry uu = run(1800.0f);
    const CarTelemetry kph = run(64.8f);
    EXPECT_NEAR(uu.AverageSpeed(), 64.8f, 0.5f);
    EXPECT_NEAR(kph.AverageSpeed(), 64.8f, 0.5f);
    EXPECT_NEAR(uu.boostSpeedSeconds, 1.0f, 0.05f);
    EXPECT_NEAR(kph.boostSpeedSeconds, 1.0f, 0.05f);
    EXPECT_NEAR(uu.distance, 1800.0f, 20.0f);
}

TEST(MatchAnalyticsTest, MovementStatesSplitGroundWallAirAndCountPowerslides) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    t.OnUpdateState(State({Car("Steam|1", "Me", 0, 1, 50)}, clock), now);
    t.OnRoundStarted(now);
    auto with = [&](json extra) {
        return [&, extra](float c) { return State({Car("Steam|1", "Me", 0, 1, 50, extra)}, c); };
    };
    now = Play(t, now, 1.0f, clock, with({{"bOnGround", true}, {"bPowersliding", true}}));
    now = Play(t, now, 0.5f, clock, with({{"bOnGround", true}, {"bOnWall", true}}));
    now = Play(t, now, 0.5f, clock, with({{"bOnGround", false}, {"bPowersliding", true}}));
    const auto c = *t.Snapshot().FindPlayer("Steam|1")->car;
    EXPECT_NEAR(c.groundSeconds, 1.0f, 0.05f);
    EXPECT_NEAR(c.wallSeconds, 0.5f, 0.05f);
    EXPECT_NEAR(c.airSeconds, 0.5f, 0.05f);
    EXPECT_EQ(c.powerslideCount, 2);
}

TEST(MatchAnalyticsTest, PausesReplaysAndDeadBallTimeDoNotCountAsPlay) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    auto state = [&](float c) { return State({Remote("Steam|1", "Me", 0, 1)}, c, 0); };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    now = Play(t, now, 2.0f, clock, state);
    t.OnPaused(now);
    now = Play(t, now, 3.0f, clock, state);
    t.OnUnpaused(now);
    now = Play(t, now, 1.0f, clock, state);
    t.OnGoalScored(Goal("Me", 0, 1));
    now = Play(t, now, 4.0f, clock, [&](float c) { return State({Remote("Steam|1", "Me", 0, 1)}, c, 0, true); });

    const MatchAnalytics a = t.Snapshot();
    EXPECT_NEAR(a.playSeconds, 3.0f, 0.05f);
    EXPECT_NEAR(a.pausedSeconds, 3.0f, 0.05f);
    EXPECT_EQ(a.pauses, 1);
    EXPECT_NEAR(a.possessionSeconds[0], 3.0f, 0.05f);
}

TEST(MatchAnalyticsTest, JoiningMidRoundStartsTrackingOnceTheClockMoves) {
    MatchAnalyticsTracker t;
    float clock = 200.0f;
    double now = 0.0;
    auto state = [&](float c) { return State({Remote("Steam|1", "Me", 0, 1)}, c, 1); };
    now = Play(t, now, 2.0f, clock, state);
    EXPECT_NEAR(t.Snapshot().playSeconds, 1.9f, 0.05f);
}

TEST(MatchAnalyticsTest, HitsAreAttributedByThirdFromEachTeamsPerspective) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    auto state = [&](float c) { return State({Remote("Steam|1", "Blue", 0, 1), Remote("Epic|2", "Orange", 1, 2)}, c); };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    t.OnBallHit(Hit("Blue", 0, 1, 0, -4000, 93, 60), now);
    now = Play(t, now, 1.0f, clock, state);
    t.OnBallHit(Hit("Orange", 1, 2, 0, -4000, 900, 120), now);
    now = Play(t, now, 1.0f, clock, state);
    t.OnBallHit(Hit("Blue", 0, 1, 0, 0, 400, 80), now);

    const MatchAnalytics a = t.Snapshot();
    const auto* blue = a.FindPlayer("Steam|1");
    const auto* orange = a.FindPlayer("Epic|2");
    EXPECT_EQ(blue->hits, 2);
    EXPECT_EQ(blue->hitsByThird[0], 1);
    EXPECT_EQ(blue->hitsByThird[1], 1);
    EXPECT_EQ(blue->aerialHits, 1);
    EXPECT_EQ(blue->firstTouches, 1);
    EXPECT_EQ(orange->hitsByThird[2], 1);
    EXPECT_EQ(orange->highAerialHits, 1);
    EXPECT_EQ(orange->hardHits, 1);
    EXPECT_FLOAT_EQ(blue->AverageHitSpeed(), 70.0f);
    EXPECT_EQ(a.kickoffs.front().firstTouchTeam, 0);
}

TEST(MatchAnalyticsTest, KickoffRecordsPossessionAfterThreeSecondsAndQuickGoals) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    int ballTeam = 255;
    auto state = [&](float c) { return State({Remote("Steam|1", "Blue", 0, 1), Remote("Epic|2", "Orange", 1, 2)}, c, ballTeam); };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    t.OnBallHit(Hit("Blue", 0, 1, 0, 0, 93, 50), now);
    ballTeam = 0;
    now = Play(t, now, 1.0f, clock, state);
    t.OnBallHit(Hit("Orange", 1, 2, 0, 0, 93, 50), now);
    ballTeam = 1;
    now = Play(t, now, 3.0f, clock, state);
    t.OnGoalScored(Goal("Orange", 1, 2));

    const MatchAnalytics a = t.Snapshot();
    ASSERT_EQ(a.kickoffs.size(), 1u);
    EXPECT_EQ(a.kickoffs[0].firstTouchTeam, 0);
    EXPECT_EQ(a.kickoffs[0].possessionTeam, 1);
    EXPECT_EQ(a.kickoffs[0].goalTeam, 1);
    const ScorelineSummary s = AnalyzeScoreline(a, 0, false);
    EXPECT_EQ(s.goalsAfterKickoffAgainst, 1);
    EXPECT_EQ(s.goalsAfterKickoffFor, 0);
}

TEST(MatchAnalyticsTest, StatfeedCountsEveryEventAndTracksDemoVictims) {
    MatchAnalyticsTracker t;
    t.OnUpdateState(State({Remote("Steam|1", "Blue", 0, 1), Remote("Epic|2", "Orange", 1, 2)}, 300.0f), 0.0);
    t.OnStatfeed({{"EventName", "Demolish"},
                  {"MainTarget", {{"Name", "Blue"}, {"TeamNum", 0}, {"Shortcut", 1}}},
                  {"SecondaryTarget", {{"Name", "Orange"}, {"TeamNum", 1}, {"Shortcut", 2}}}});
    t.OnStatfeed({{"EventName", "AerialGoal"}, {"MainTarget", {{"Name", "Blue"}, {"TeamNum", 0}, {"Shortcut", 1}}}});

    const MatchAnalytics a = t.Snapshot();
    EXPECT_EQ(a.FindPlayer("Steam|1")->demosInflicted, 1);
    EXPECT_EQ(a.FindPlayer("Epic|2")->demosTaken, 1);
    EXPECT_EQ(a.FindPlayer("Steam|1")->StatEvent("AerialGoal"), 1);
    EXPECT_EQ(a.FindPlayer("Steam|1")->StatEvent("Demolish"), 1);
}

TEST(MatchAnalyticsTest, ScorelineDetectsComebacksBlownLeadsAndLateWinners) {
    MatchAnalytics a;
    auto goal = [&](int team, float clock, bool ot = false) {
        GoalRecord g;
        g.team = team;
        g.clockSeconds = clock;
        g.overtime = ot;
        a.goals.push_back(g);
    };
    goal(1, 250);
    goal(1, 200);
    goal(0, 120);
    goal(0, 50);
    goal(0, 20);

    const ScorelineSummary won = AnalyzeScoreline(a, 0, true);
    EXPECT_TRUE(won.comeback);
    EXPECT_EQ(won.maxDeficit, 2);
    EXPECT_TRUE(won.lateWinner);
    EXPECT_EQ(won.lastMinuteGoalsFor, 2);

    const ScorelineSummary lost = AnalyzeScoreline(a, 1, false);
    EXPECT_TRUE(lost.blownLead);
    EXPECT_FALSE(lost.comeback);
    EXPECT_EQ(lost.maxLead, 2);

    MatchAnalytics ot;
    GoalRecord g;
    g.team = 0;
    g.clockSeconds = 12.0f;
    g.overtime = true;
    ot.goals.push_back(g);
    const ScorelineSummary otWin = AnalyzeScoreline(ot, 0, true);
    EXPECT_TRUE(otWin.overtime);
    EXPECT_FALSE(otWin.lateWinner);
}

TEST(MatchAnalyticsTest, EndingWithRegulationTimeLeftIsAnEarlyEnd) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    auto state = [&](float c) { return State({Remote("Steam|1", "Me", 0, 1)}, c, 0); };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    Play(t, now, 1.0f, clock, state);
    t.OnMatchEnded();
    EXPECT_TRUE(t.Snapshot().EndedEarly());
}

TEST(MatchAnalyticsTest, JsonRoundTripPreservesStoredAnalytics) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    auto state = [&](float c) {
        return State({Car("Steam|1", "Me", 0, 1, 50), Remote("Epic|2", "Opp", 1, 2)}, c, 0);
    };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    t.OnBallHit(Hit("Me", 0, 1, 10, 20, 300, 77), now);
    now = Play(t, now, 20.0f, clock, state);
    json goal = Goal("Me", 0, 1);
    goal["Assister"] = {{"Name", "Opp"}, {"TeamNum", 1}, {"Shortcut", 2}};
    goal["ImpactLocation"] = {{"X", 0}, {"Y", 0}, {"Z", 0}};
    t.OnGoalScored(goal);
    t.OnGoalScored({{"Scorer", {{"Name", ""}, {"Shortcut", 0}, {"TeamNum", 0}}},
                    {"GoalSpeed", 0.0},
                    {"ImpactLocation", {{"X", 1}, {"Y", 5100}, {"Z", 200}}}});
    t.OnStatfeed({{"EventName", "Shot"}, {"MainTarget", {{"Name", "Me"}, {"TeamNum", 0}, {"Shortcut", 1}}}});
    t.OnPlayerLeft({{"PlayerName", "Opp"}, {"PrimaryId", "Epic|2"}});

    const MatchAnalytics original = t.Snapshot();
    const auto parsed = MatchAnalyticsFromJson(json::parse(MatchAnalyticsToJson(original).dump()));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_NEAR(parsed->playSeconds, original.playSeconds, 0.05f);
    ASSERT_EQ(parsed->players.size(), original.players.size());
    const auto* me = parsed->FindPlayer("Steam|1");
    ASSERT_NE(me, nullptr);
    EXPECT_EQ(me->StatEvent("Shot"), 1);
    ASSERT_TRUE(me->car.has_value());
    EXPECT_NEAR(me->car->AverageBoost(), 50.0f, 0.1f);
    ASSERT_EQ(parsed->hits.size(), 1u);
    EXPECT_EQ(parsed->hits[0].playerId, "Steam|1");
    ASSERT_EQ(parsed->goals.size(), 1u);
    EXPECT_EQ(parsed->goals[0].assisterName, "Opp");
    ASSERT_TRUE(parsed->goals[0].location.has_value());
    EXPECT_FLOAT_EQ(parsed->goals[0].location->y, 5100.0f);
    EXPECT_EQ(parsed->goals[0].scoreAfter[0], 1);
    EXPECT_EQ(parsed->possessionTimeline.size(), original.possessionTimeline.size());
    ASSERT_EQ(parsed->departures.size(), 1u);
    EXPECT_EQ(parsed->departures[0].team, 1);
}

TEST(MatchAnalyticsTest, UnknownFormatVersionIsRejected) {
    EXPECT_FALSE(MatchAnalyticsFromJson(json{{"v", MatchAnalytics::kFormatVersion + 1}}).has_value());
}

TEST(MatchAnalyticsTest, OmittedCarFieldsMeanZeroAndOpponentsWithoutCarStateAreNotTracked) {
    MatchAnalyticsTracker t;
    float clock = 300.0f;
    double now = 0.0;
    // Live payloads drop zero and false values; opponents never carry bHasCar outside replays.
    auto state = [&](float c) {
        return State({{{"PrimaryId", "Steam|1"}, {"Name", "Me"}, {"TeamNum", 0}, {"Shortcut", 1}, {"bHasCar", true}},
                      {{"PrimaryId", "Epic|2"}, {"Name", "Opp"}, {"TeamNum", 1}, {"Shortcut", 2}}},
                     c, 0);
    };
    t.OnUpdateState(state(clock), now);
    t.OnRoundStarted(now);
    Play(t, now, 1.0f, clock, state);

    const MatchAnalytics a = t.Snapshot();
    const auto& me = *a.FindPlayer("Steam|1")->car;
    EXPECT_NEAR(me.zeroBoostSeconds, 1.0f, 0.05f);
    EXPECT_NEAR(me.airSeconds, 1.0f, 0.05f);
    EXPECT_NEAR(me.slowSeconds, 1.0f, 0.05f);
    EXPECT_FALSE(a.FindPlayer("Epic|2")->car.has_value());
}

TEST(MatchAnalyticsTest, GoalReplayEchoSuppliesLocationWithoutAddingAGoal) {
    MatchAnalyticsTracker t;
    t.OnUpdateState(State({Remote("Steam|1", "Me", 1, 6)}, 300.0f), 0.0);
    t.OnRoundStarted(0.0);
    json live = Goal("Me", 1, 6);
    live["ImpactLocation"] = {{"X", 0.0}, {"Y", 0.0}, {"Z", 0.0}};
    t.OnGoalScored(live);
    t.OnGoalScored({{"Scorer", {{"Name", ""}, {"Shortcut", 0}, {"TeamNum", 0}}},
                    {"GoalSpeed", 0.0},
                    {"GoalTime", 0},
                    {"ImpactLocation", {{"X", -536.0}, {"Y", -5247.8}, {"Z", 121.4}}}});

    const MatchAnalytics a = t.Snapshot();
    ASSERT_EQ(a.goals.size(), 1u);
    EXPECT_EQ(a.goals[0].team, 1);
    EXPECT_EQ(a.goals[0].scoreAfter[1], 1);
    EXPECT_EQ(a.goals[0].scoreAfter[0], 0);
    ASSERT_TRUE(a.goals[0].location.has_value());
    EXPECT_FLOAT_EQ(a.goals[0].location->y, -5247.8f);
}

TEST(MatchAnalyticsTest, ReusedShortcutAfterBotReplacementResolvesByName) {
    MatchAnalyticsTracker t;
    t.OnUpdateState(State({Remote("Unknown|0|0", "Middy", 1, 6)}, 300.0f), 0.0);
    t.OnUpdateState(State({Remote("Epic|me", "Me", 1, 6)}, 299.0f), 1.0);
    t.OnBallHit(Hit("Middy", 1, 6, 0, 0, 93, 50), 1.0);
    t.OnBallHit(Hit("Me", 1, 6, 0, 0, 93, 50), 1.0);

    const MatchAnalytics a = t.Snapshot();
    EXPECT_EQ(a.FindPlayer("Unknown|Middy")->hits, 1);
    EXPECT_EQ(a.FindPlayer("Epic|me")->hits, 1);
}

TEST(MatchAnalyticsTest, ConsecutiveContactFramesCountAsOneTouchWithTheHardestSpeed) {
    MatchAnalyticsTracker t;
    t.OnUpdateState(State({Remote("Steam|1", "Me", 0, 1)}, 300.0f), 0.0);
    t.OnRoundStarted(0.0);
    // One push as recorded: a hit message per physics frame, 10-20 ms apart.
    t.OnBallHit(Hit("Me", 0, 1, 0, 0, 93, 40), 1.000);
    t.OnBallHit(Hit("Me", 0, 1, 0, 0, 93, 110), 1.015);
    t.OnBallHit(Hit("Me", 0, 1, 0, 0, 93, 70), 1.040);
    // A separate dribble touch 0.2 s later is a new touch in the game's count.
    t.OnBallHit(Hit("Me", 0, 1, 0, 0, 93, 60), 1.240);

    const MatchAnalytics a = t.Snapshot();
    const PlayerAnalytics& p = *a.FindPlayer("Steam|1");
    EXPECT_EQ(p.hits, 2);
    EXPECT_EQ(p.hardHits, 1);
    EXPECT_FLOAT_EQ(p.AverageHitSpeed(), 85.0f);
}

TEST(MatchAnalyticsTest, ReplayEndLocationIsKeptAfterInstantSkipAndRejectedOnTheWrongSide) {
    MatchAnalyticsTracker t;
    t.OnUpdateState(State({Remote("Steam|1", "Me", 1, 6), Remote("Epic|2", "Opp", 0, 1)}, 300.0f), 0.0);
    t.OnRoundStarted(0.0);
    const json echoScorer = {{"Name", ""}, {"Shortcut", 0}, {"TeamNum", 0}};
    // Recorded online: a replay skipped after 1.4 s still echoed, repeating this goal's own location.
    json first = Goal("Me", 1, 6);
    first["ImpactLocation"] = {{"X", -806.2}, {"Y", -5412.6}, {"Z", 102.7}};
    t.OnGoalScored(first);
    t.OnGoalScored({{"Scorer", echoScorer}, {"ImpactLocation", {{"X", -806.2}, {"Y", -5412.6}, {"Z", 102.7}}}});
    t.OnRoundStarted(1.0);
    // An echo at the wrong end of the field for the scoring team is not used.
    t.OnGoalScored(Goal("Opp", 0, 1));
    t.OnGoalScored({{"Scorer", echoScorer}, {"ImpactLocation", {{"X", 100.0}, {"Y", -5200.0}, {"Z", 100.0}}}});

    const MatchAnalytics a = t.Snapshot();
    ASSERT_EQ(a.goals.size(), 2u);
    ASSERT_TRUE(a.goals[0].location.has_value());
    EXPECT_FLOAT_EQ(a.goals[0].location->y, -5412.6f);
    EXPECT_FALSE(a.goals[1].location.has_value());
    EXPECT_EQ(a.goals[1].team, 0);
}
