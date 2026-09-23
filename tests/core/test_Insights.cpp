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
