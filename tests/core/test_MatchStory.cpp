#include <gtest/gtest.h>

#include "core/game/MatchAnalytics.hpp"
#include "core/game/MatchStory.hpp"
#include "core/state/SessionState.hpp"

TEST(MatchStoryTest, EmptyWhenNoNotableEvents) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 1;
    detail.theirScore = 0;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;
    analytics.possessionSeconds = {150.0f, 150.0f};

    PlayerAnalytics me;
    me.primaryId = "Steam|test_me";
    me.team = 0;
    me.hits = 20;
    analytics.players.push_back(me);

    detail.accountPrimaryId = "Steam|test_me";
    detail.analytics = analytics;

    const auto story = MatchStory::BuildMatchStory(detail);
    EXPECT_TRUE(story.empty());
}

TEST(MatchStoryTest, DetectsComeback) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 3;
    detail.theirScore = 2;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    // Opponents score two first
    GoalRecord g1;
    g1.team = 1;
    g1.playSeconds = 30.0f;
    analytics.goals.push_back(g1);

    GoalRecord g2;
    g2.team = 1;
    g2.playSeconds = 60.0f;
    analytics.goals.push_back(g2);

    // We score three to win
    GoalRecord g3;
    g3.team = 0;
    g3.playSeconds = 90.0f;
    analytics.goals.push_back(g3);

    GoalRecord g4;
    g4.team = 0;
    g4.playSeconds = 120.0f;
    analytics.goals.push_back(g4);

    GoalRecord g5;
    g5.team = 0;
    g5.playSeconds = 150.0f;
    analytics.goals.push_back(g5);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_FALSE(story.empty());
    EXPECT_EQ(story[0], "Came back from a 2-goal deficit to win.");
}

TEST(MatchStoryTest, DetectsOneGoalComeback) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 2;
    detail.theirScore = 1;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    GoalRecord g1;
    g1.team = 1;
    g1.playSeconds = 40.0f;
    analytics.goals.push_back(g1);

    GoalRecord g2;
    g2.team = 0;
    g2.playSeconds = 80.0f;
    analytics.goals.push_back(g2);

    GoalRecord g3;
    g3.team = 0;
    g3.playSeconds = 120.0f;
    analytics.goals.push_back(g3);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_FALSE(story.empty());
    EXPECT_EQ(story[0], "Came back from a 1-goal deficit to win.");
}

TEST(MatchStoryTest, DetectsBlownLead) {
    MatchDetail detail;
    detail.found = true;
    detail.win = false;
    detail.ourScore = 2;
    detail.theirScore = 3;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    // We lead 2-0
    GoalRecord g1;
    g1.team = 0;
    g1.playSeconds = 30.0f;
    analytics.goals.push_back(g1);

    GoalRecord g2;
    g2.team = 0;
    g2.playSeconds = 60.0f;
    analytics.goals.push_back(g2);

    // Opponents score 3
    GoalRecord g3;
    g3.team = 1;
    g3.playSeconds = 90.0f;
    analytics.goals.push_back(g3);

    GoalRecord g4;
    g4.team = 1;
    g4.playSeconds = 120.0f;
    analytics.goals.push_back(g4);

    GoalRecord g5;
    g5.team = 1;
    g5.playSeconds = 150.0f;
    analytics.goals.push_back(g5);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_FALSE(story.empty());
    EXPECT_EQ(story[0], "Surrendered a 2-goal lead.");
}

TEST(MatchStoryTest, DetectsOvertimeAndForfeit) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 4;
    detail.theirScore = 1;

    MatchAnalytics analytics;
    analytics.playSeconds = 240.0f;
    analytics.endClockSeconds = 60.0f; // Ended early with 1:00 left

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 1u);
    bool foundForfeit = false;
    for (const auto& line : story) {
        if (line.find("Opponents forfeited with 1:00 remaining.") != std::string::npos) foundForfeit = true;
    }
    EXPECT_TRUE(foundForfeit);
}

TEST(MatchStoryTest, DetectsLateWinner) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 2;
    detail.theirScore = 1;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    GoalRecord g1;
    g1.team = 1;
    g1.playSeconds = 60.0f;
    g1.clockSeconds = 240.0f;
    analytics.goals.push_back(g1);

    GoalRecord g2;
    g2.team = 0;
    g2.playSeconds = 180.0f;
    g2.clockSeconds = 120.0f;
    analytics.goals.push_back(g2);

    // Decisive goal scored with 15 seconds remaining
    GoalRecord g3;
    g3.team = 0;
    g3.playSeconds = 285.0f;
    g3.clockSeconds = 15.0f;
    analytics.goals.push_back(g3);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 1u);
    bool foundLateWinner = false;
    for (const auto& line : story) {
        if (line.find("Scored the match-winning goal with 0:15 remaining.") != std::string::npos) foundLateWinner = true;
    }
    EXPECT_TRUE(foundLateWinner);
}

TEST(MatchStoryTest, DetectsPlayerDeparture) {
    MatchDetail detail;
    detail.found = true;
    detail.win = false;
    detail.ourScore = 1;
    detail.theirScore = 3;
    detail.accountPrimaryId = "Steam|user";

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    PlayerDeparture dep;
    dep.name = "Alex";
    dep.primaryId = "Steam|teammate";
    dep.team = 0;
    dep.clockSeconds = 150.0f;
    analytics.departures.push_back(dep);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 1u);
    EXPECT_EQ(story[0], "Teammate Alex left the match with 2:30 remaining.");
}

TEST(MatchStoryTest, DetectsKickoffGoalsAndFastestHit) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 3;
    detail.theirScore = 0;
    detail.accountPrimaryId = "Steam|user";

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    KickoffRecord k;
    k.goalTeam = 0;
    analytics.kickoffs.push_back(k);

    PlayerAnalytics me;
    me.primaryId = "Steam|user";
    me.team = 0;
    me.maxHitSpeed = 112.0f;
    analytics.players.push_back(me);

    PlayerAnalytics opp;
    opp.primaryId = "Steam|opp";
    opp.team = 1;
    opp.maxHitSpeed = 95.0f;
    analytics.players.push_back(opp);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 2u);
    EXPECT_EQ(story[0], "Scored 1 goal within 10 seconds of kickoff.");
    EXPECT_EQ(story[1], "Recorded the fastest hit in the match.");
}

TEST(MatchStoryTest, DetectsDemoDifferential) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 2;
    detail.theirScore = 1;
    detail.accountPrimaryId = "Steam|user";

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;

    PlayerAnalytics me;
    me.primaryId = "Steam|user";
    me.team = 0;
    me.demosInflicted = 5;
    analytics.players.push_back(me);

    PlayerAnalytics opp;
    opp.primaryId = "Steam|opp";
    opp.team = 1;
    opp.demosInflicted = 1;
    analytics.players.push_back(opp);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 1u);
    EXPECT_EQ(story[0], "Your team led demolitions 5 to 1.");
}

TEST(MatchStoryTest, LimitsStoryToTopFive) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 4;
    detail.theirScore = 2;
    detail.overtimeSeconds = 45.0f;
    detail.accountPrimaryId = "Steam|user";

    MatchAnalytics analytics;
    analytics.playSeconds = 345.0f;
    analytics.possessionSeconds = {250.0f, 95.0f}; // 72% possession in win (prio 65)

    // Comeback from 2 down (prio 110)
    GoalRecord g1{10.0f, 290.0f, false, 1};
    GoalRecord g2{20.0f, 280.0f, false, 1};
    GoalRecord g3{30.0f, 270.0f, false, 0};
    GoalRecord g4{40.0f, 260.0f, false, 0};
    GoalRecord g5{50.0f, 250.0f, false, 0};
    GoalRecord g6{60.0f, 240.0f, false, 0};
    analytics.goals = {g1, g2, g3, g4, g5, g6};

    // Kickoff goal (prio 75)
    KickoffRecord k;
    k.goalTeam = 0;
    analytics.kickoffs.push_back(k);

    // Departure (prio 70)
    PlayerDeparture dep;
    dep.name = "OppPlayer";
    dep.primaryId = "Steam|opp2";
    dep.team = 1;
    dep.clockSeconds = 60.0f;
    analytics.departures.push_back(dep);

    // Player with 80% touches (prio 72) and fastest hit (prio 68) and 20% zero boost (prio 66)
    PlayerAnalytics me;
    me.primaryId = "Steam|user";
    me.team = 0;
    me.hits = 40;
    me.maxHitSpeed = 110.0f;
    me.demosInflicted = 6; // demo diff 6-0 (prio 62)
    CarTelemetry car;
    car.sampledSeconds = 300.0f;
    car.zeroBoostSeconds = 60.0f;
    me.car = car;
    analytics.players.push_back(me);

    PlayerAnalytics tm;
    tm.primaryId = "Steam|teammate";
    tm.team = 0;
    tm.hits = 10;
    analytics.players.push_back(tm);

    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    EXPECT_EQ(story.size(), 5u);
    // Highest priority candidate should be first (comeback: prio 110)
    EXPECT_EQ(story[0], "Came back from a 2-goal deficit to win.");
    // Second highest should be overtime (prio 90)
    EXPECT_EQ(story[1], "Won in overtime (+0:45).");
}

TEST(MatchStoryTest, DetectsBoostStarvationAndHighPossession) {
    MatchDetail detail;
    detail.found = true;
    detail.win = false;
    detail.ourScore = 1;
    detail.theirScore = 2;

    MatchAnalytics analytics;
    analytics.playSeconds = 300.0f;
    analytics.possessionSeconds = {200.0f, 100.0f}; // 67% possession

    PlayerAnalytics me;
    me.primaryId = "Steam|user";
    me.team = 0;
    CarTelemetry car;
    car.sampledSeconds = 300.0f;
    car.zeroBoostSeconds = 60.0f; // 20% at zero boost
    me.car = car;
    analytics.players.push_back(me);

    detail.accountPrimaryId = "Steam|user";
    detail.analytics = analytics;
    MatchDetailPlayer p;
    p.team = 0;
    detail.ourTeam.push_back(p);

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_GE(story.size(), 2u);

    bool foundPossession = false;
    bool foundBoost = false;
    for (const auto& line : story) {
        if (line.find("possession despite the loss") != std::string::npos) foundPossession = true;
        if (line.find("zero boost") != std::string::npos) foundBoost = true;
    }
    EXPECT_TRUE(foundPossession);
    EXPECT_TRUE(foundBoost);
}

TEST(MatchStoryTest, LegacyMatchFallback) {
    MatchDetail detail;
    detail.found = true;
    detail.win = true;
    detail.ourScore = 2;
    detail.theirScore = 1;
    detail.overtimeSeconds = 85.0f;

    MatchLocalStatsRow local;
    local.crossbars = 3;
    detail.localStats = local;

    const auto story = MatchStory::BuildMatchStory(detail);
    ASSERT_EQ(story.size(), 2u);
    EXPECT_EQ(story[0], "Won in overtime (+1:25).");
    EXPECT_EQ(story[1], "Hit the crossbar 3 times.");
}
