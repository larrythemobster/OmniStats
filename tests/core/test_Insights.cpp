#include <gtest/gtest.h>

#include "core/Insights.hpp"

namespace {
    constexpr int64_t kStart = 1'800'000'000;
    constexpr int64_t kGame = 10 * 60;

    // Builds a chronological run of games ten minutes apart; Break() starts a new sitting.
    struct Timeline {
        std::vector<MatchOutcome> matches;
        int64_t clock = kStart;

        Timeline& Play(std::initializer_list<bool> results, const char* playlist = "Doubles", int hour = 20) {
            for (bool win : results) {
                clock += kGame;
                matches.push_back({clock, hour, playlist, win});
            }
            return *this;
        }
        Timeline& Break() {
            clock += Insights::kSessionGapSeconds + 1;
            return *this;
        }
    };
}

TEST(InsightsTest, LossStreakBucketsResetAtSessionBoundaries) {
    Timeline t;
    t.Play({false, false, true}).Break().Play({true, false});

    const TrendsReport report = Insights::ComputeTrends(t.matches);

    EXPECT_EQ(report.sessions, 2);
    EXPECT_EQ(report.games, 5);
    // Game 2 follows one loss; game 3 follows two losses. The first game of the
    // second sitting follows nothing, and its successor follows a win.
    EXPECT_EQ(report.afterStreak[1].games, 1);
    EXPECT_EQ(report.afterStreak[2].games, 1);
    EXPECT_EQ(report.afterStreak[2].wins, 1);
    EXPECT_EQ(report.afterStreak[0].games, 1);
    EXPECT_EQ(report.afterStreak[0].wins, 0);
    EXPECT_EQ(report.afterTwoPlusLosses.games, 1);
}

TEST(InsightsTest, SessionGameBucketsCountFromEachSittingStart) {
    Timeline t;
    t.Play({true, true, true, true}).Break().Play({false});

    const TrendsReport report = Insights::ComputeTrends(t.matches);

    EXPECT_EQ(report.bySessionGame[0].games, 4); // three from the first sitting, one from the second
    EXPECT_EQ(report.bySessionGame[1].games, 1);
}

TEST(InsightsTest, TiltWarningNeedsEnoughGamesAndAClearDrop) {
    // Twelve sittings of L L L: every third game follows two losses and loses.
    // A six-win sitting after each lifts the overall rate well above that.
    Timeline tilted;
    for (int i = 0; i < 12; ++i)
        tilted.Play({false, false, false}).Break().Play({true, true, true, true, true, true}).Break();
    const TrendsReport report = Insights::ComputeTrends(tilted.matches);
    ASSERT_GE(report.afterTwoPlusLosses.games, Insights::kTiltMinimumGames);
    EXPECT_FLOAT_EQ(report.afterTwoPlusLosses.WinRate(), 0.0f);
    EXPECT_FALSE(report.tiltWarning.empty());

    Timeline small;
    small.Play({false, false, false, true, true, true});
    EXPECT_TRUE(Insights::ComputeTrends(small.matches).tiltWarning.empty());
}

TEST(InsightsTest, PlaylistsAndTimeOfDayBucketByLabelAndHour) {
    Timeline t;
    t.Play({true}, "Duel", 2).Play({false, false}, "Doubles", 13).Play({true}, "Doubles", 23);

    const TrendsReport report = Insights::ComputeTrends(t.matches);

    ASSERT_EQ(report.byPlaylist.size(), 2u);
    EXPECT_EQ(report.byPlaylist[0].label, "Doubles");
    EXPECT_EQ(report.byPlaylist[0].games, 3);
    EXPECT_EQ(report.byTimeOfDay[0].games, 1); // 02:00
    EXPECT_EQ(report.byTimeOfDay[2].games, 2); // 13:00
    EXPECT_EQ(report.byTimeOfDay[3].wins, 1);  // 23:00
}

TEST(InsightsTest, RankPeopleFiltersByRelationshipAndOrdersByGames) {
    std::vector<PersonRecord> people = {
        {"a", "Alpha", 1, 0, 0, 0, 10},
        {"b", "Bravo", 3, 2, 0, 1, 20},
        {"c", "Charlie", 0, 0, 4, 4, 30},
    };

    const auto mates = Insights::RankPeople(people, Insights::PeopleFilter::Teammates, 10);
    ASSERT_EQ(mates.size(), 2u);
    EXPECT_EQ(mates[0].primaryId, "b");
    EXPECT_EQ(mates[1].primaryId, "a");

    const auto rivals = Insights::RankPeople(people, Insights::PeopleFilter::Rivals, 1);
    ASSERT_EQ(rivals.size(), 1u);
    EXPECT_EQ(rivals[0].primaryId, "c");
}

TEST(InsightsTest, GapBucketsClassifyExactBoundariesAndPercentages) {
    // Bucket boundaries:
    //   Much stronger: gap >= +75
    //   Stronger:      +25 < gap < +75
    //   Even:          -25 <= gap <= +25
    //   Weaker:        -75 < gap < -25
    //   Much weaker:   gap <= -75
    std::vector<MatchMmrContext> matches = {
        {kStart + 600, "2v2", true, 1000, false, 1000.0, 1075.0, 2, 2},   // gap = +75.0 -> Much stronger
        {kStart + 1200, "2v2", false, 1000, false, 1000.0, 1100.0, 2, 2}, // gap = +100.0 -> Much stronger
        {kStart + 1800, "2v2", true, 1000, false, 1000.0, 1074.9, 2, 2},  // gap = +74.9 -> Stronger
        {kStart + 2400, "2v2", true, 1000, false, 1000.0, 1025.1, 2, 2},  // gap = +25.1 -> Stronger
        {kStart + 3000, "2v2", false, 1000, false, 1000.0, 1025.0, 2, 2}, // gap = +25.0 -> Even
        {kStart + 3600, "2v2", true, 1000, false, 1000.0, 975.0, 2, 2},   // gap = -25.0 -> Even
        {kStart + 4200, "2v2", false, 1000, false, 1000.0, 974.9, 2, 2},  // gap = -25.1 -> Weaker
        {kStart + 4800, "2v2", false, 1000, false, 1000.0, 925.1, 2, 2},  // gap = -74.9 -> Weaker
        {kStart + 5400, "2v2", true, 1000, false, 1000.0, 925.0, 2, 2},   // gap = -75.0 -> Much weaker
    };

    const GapReport report = Insights::ComputeGapTrends(matches, "2v2");

    ASSERT_EQ(report.buckets.size(), 5u);
    EXPECT_EQ(report.games, 9);
    EXPECT_EQ(report.wins, 5);

    EXPECT_EQ(report.buckets[0].label, "Much stronger (+75 or more)");
    EXPECT_EQ(report.buckets[0].games, 2);
    EXPECT_EQ(report.buckets[0].wins, 1);
    EXPECT_FLOAT_EQ(report.buckets[0].WinRate(), 0.5f);

    EXPECT_EQ(report.buckets[1].label, "Stronger (+25 to +75)");
    EXPECT_EQ(report.buckets[1].games, 2);
    EXPECT_EQ(report.buckets[1].wins, 2);
    EXPECT_FLOAT_EQ(report.buckets[1].WinRate(), 1.0f);

    EXPECT_EQ(report.buckets[2].label, "Even (±25)");
    EXPECT_EQ(report.buckets[2].games, 2);
    EXPECT_EQ(report.buckets[2].wins, 1);
    EXPECT_FLOAT_EQ(report.buckets[2].WinRate(), 0.5f);

    EXPECT_EQ(report.buckets[3].label, "Weaker (-25 to -75)");
    EXPECT_EQ(report.buckets[3].games, 2);
    EXPECT_EQ(report.buckets[3].wins, 0);
    EXPECT_FLOAT_EQ(report.buckets[3].WinRate(), 0.0f);

    EXPECT_EQ(report.buckets[4].label, "Much weaker (-75 or less)");
    EXPECT_EQ(report.buckets[4].games, 1);
    EXPECT_EQ(report.buckets[4].wins, 1);
    EXPECT_FLOAT_EQ(report.buckets[4].WinRate(), 1.0f);
}

TEST(InsightsTest, GapTrendsExcludeIncompleteLobbiesAndUnmatchedPlaylists) {
    std::vector<MatchMmrContext> matches = {
        {kStart + 600, "Doubles", true, 1020, false, 1000.0, 1040.0, 2, 2},   // complete 2v2, teammate=980 -> carry=+40
        {kStart + 1200, "Doubles", false, 1000, false, 1000.0, 1040.0, 2, 1}, // incomplete opponent count
        {kStart + 1800, "Doubles", false, 1000, false, 1000.0, 1040.0, 1, 2}, // incomplete team count
        {kStart + 2400, "Standard", true, 1100, false, 1100.0, 1150.0, 3, 2}, // incomplete 3v3
        {kStart + 3000, "Duel", false, 900, false, 900.0, 960.0, 1, 1},       // complete 1v1
        {kStart + 3600, "Casual", true, 1500, false, 1500.0, 1600.0, 2, 2},   // casual excluded
    };

    const GapReport all = Insights::ComputeGapTrends(matches, "All");
    EXPECT_EQ(all.games, 2);
    EXPECT_EQ(all.wins, 1);
    EXPECT_EQ(all.buckets[1].games, 2);
    EXPECT_EQ(all.carryGames, 1);
    EXPECT_DOUBLE_EQ(all.AvgCarry(), 40.0);

    const GapReport doublesOnly = Insights::ComputeGapTrends(matches, "2v2");
    EXPECT_EQ(doublesOnly.games, 1);
    EXPECT_EQ(doublesOnly.wins, 1);

    const GapReport standardOnly = Insights::ComputeGapTrends(matches, "3v3");
    EXPECT_EQ(standardOnly.games, 0);
    EXPECT_EQ(standardOnly.statusMessage, "Not enough ranked data");
}

TEST(InsightsTest, GapDeltasRequireConsecutiveUnestimatedMatchesWithinTwoHours) {
    std::vector<MatchMmrContext> matches = {
        // Match 1: win in Stronger bucket (+40 gap), followed 10m later by Match 2 (+12 MMR)
        {kStart, "2v2", true, 1000, false, 1000.0, 1040.0, 2, 2},
        // Match 2: loss in Stronger bucket, followed 10m later by Match 3 (-8 MMR)
        {kStart + 600, "2v2", false, 1012, false, 1012.0, 1050.0, 2, 2},
        // Match 3: win in Stronger bucket, but Match 4 is >2h later -> skipped
        {kStart + 1200, "2v2", true, 1004, false, 1004.0, 1045.0, 2, 2},
        // Match 4 (>2h gap): win in Stronger bucket, followed by estimated Match 5 -> skipped
        {kStart + 1200 + Insights::kSessionGapSeconds + 1, "2v2", true, 1050, false, 1050.0, 1090.0, 2, 2},
        // Match 5 (estimated): loss in Stronger bucket, followed by Match 6 -> skipped for both pairs
        {kStart + 1800 + Insights::kSessionGapSeconds + 1, "2v2", false, 1060, true, 1060.0, 1100.0, 2, 2},
        // Match 6: terminal match
        {kStart + 2400 + Insights::kSessionGapSeconds + 1, "2v2", true, 1052, false, 1052.0, 1090.0, 2, 2},
    };

    const GapReport report = Insights::ComputeGapTrends(matches, "2v2");
    const GapBucket& stronger = report.buckets[1];
    EXPECT_EQ(stronger.games, 6);
    EXPECT_EQ(stronger.winDeltas, 1);
    EXPECT_DOUBLE_EQ(stronger.AvgWinDelta(), 12.0);
    EXPECT_EQ(stronger.lossDeltas, 1);
    EXPECT_DOUBLE_EQ(stronger.AvgLossDelta(), -8.0);
}

TEST(InsightsTest, WilsonIntervalAndCalloutGateRequireSampleSizeAndSignificance) {
    const ConfidenceInterval empty = Insights::WilsonInterval(0, 0);
    EXPECT_DOUBLE_EQ(empty.low, 0.0);
    EXPECT_DOUBLE_EQ(empty.high, 0.0);

    const ConfidenceInterval half = Insights::WilsonInterval(50, 100);
    EXPECT_NEAR(half.low, 0.4038, 1e-3);
    EXPECT_NEAR(half.high, 0.5962, 1e-3);
    EXPECT_FALSE(half.Excludes(0.5));

    const ConfidenceInterval highWin = Insights::WilsonInterval(24, 30);
    EXPECT_GT(highWin.low, 0.5);
    EXPECT_TRUE(highWin.Excludes(0.5));

    // 29 wins in 29 games is below the n>=30 gate: no callout.
    std::vector<MatchMmrContext> underSample;
    for (int i = 0; i < 29; ++i)
        underSample.push_back({kStart + i * 600, "2v2", true, 1000, false, 1000.0, 1040.0, 2, 2});
    EXPECT_TRUE(Insights::ComputeGapTrends(underSample, "2v2").callout.empty());

    // 18 wins in 30 games (60%) has n>=30, but 95% Wilson CI includes 50%: no callout.
    std::vector<MatchMmrContext> noisy;
    for (int i = 0; i < 30; ++i)
        noisy.push_back({kStart + i * 600, "2v2", i < 18, 1000, false, 1000.0, 1040.0, 2, 2});
    EXPECT_TRUE(Insights::ComputeGapTrends(noisy, "2v2").callout.empty());

    // 24 wins in 30 games (80%) has n>=30 and CI excludes 50%: callout fires.
    std::vector<MatchMmrContext> significant;
    for (int i = 0; i < 30; ++i)
        significant.push_back({kStart + i * 600, "2v2", i < 24, 1000, false, 1000.0, 1040.0, 2, 2});
    const GapReport sigReport = Insights::ComputeGapTrends(significant, "2v2");
    EXPECT_NE(sigReport.callout.find("stronger teams in 2v2"), std::string::npos);
}
