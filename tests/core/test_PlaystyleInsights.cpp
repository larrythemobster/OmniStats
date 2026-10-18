#include <gtest/gtest.h>

#include "core/game/PlaystyleInsights.hpp"

namespace {

    AnalyticsMatchRow CreateMatchSample(bool win,
                                        float bpm,
                                        float supersonicPct,
                                        float avgHitSpeed,
                                        int hits,
                                        const std::string& playlist = "2v2",
                                        int playlistId = 11,
                                        const std::string& arena = "DFH Stadium") {
        AnalyticsMatchRow row;
        row.win = win;
        row.playlist = playlist;
        row.playlistId = playlistId;
        row.arena = arena;
        row.myTeam = 0;
        row.ourScore = win ? 3 : 1;
        row.theirScore = win ? 1 : 3;

        MatchAnalytics ma;
        ma.playSeconds = 300.0f;
        ma.possessionSeconds[0] = 160.0f;
        ma.possessionSeconds[1] = 140.0f;

        PlayerAnalytics pa;
        pa.primaryId = "local_player";
        pa.team = 0;
        pa.hits = hits;
        pa.hitSpeedSum = avgHitSpeed * static_cast<float>(hits);
        pa.aerialHits = 3;
        pa.firstTouches = 2;
        pa.demosInflicted = 1;
        pa.demosTaken = 1;

        CarTelemetry ct;
        ct.sampledSeconds = 300.0f;
        ct.distance = 300.0f * (55.0f / 0.036f);
        ct.boostUsed = static_cast<int>(bpm * (300.0f / 60.0f));
        ct.boostIntegral = 15000.0f;
        ct.zeroBoostSeconds = 30.0f;
        ct.fullBoostSeconds = 30.0f;
        ct.supersonicSeconds = (supersonicPct / 100.0f) * 300.0f;
        ct.boostSpeedSeconds = 60.0f;
        ct.slowSeconds = 150.0f;
        ct.airSeconds = 45.0f;
        ct.wallSeconds = 30.0f;
        ct.bigPads = 12;
        ct.smallPads = 25;
        ct.powerslideCount = 35;

        pa.car = ct;
        ma.players.push_back(pa);

        KickoffRecord ko1;
        ko1.possessionTeam = 0;
        KickoffRecord ko2;
        ko2.possessionTeam = 1;
        ma.kickoffs.push_back(ko1);
        ma.kickoffs.push_back(ko2);

        row.analytics = std::move(ma);
        return row;
    }

} // namespace

TEST(PlaystyleInsightsTest, EmptyMatchesYieldEmptyState) {
    const auto report = Playstyle::ComputePlaystyle({}, "All");
    EXPECT_EQ(report.totalMatchesWithTelemetry, 0);
    EXPECT_FALSE(report.emptyMessage.empty());
    EXPECT_TRUE(report.topSplits.empty());
    EXPECT_TRUE(report.recentForm.empty());
}

TEST(PlaystyleInsightsTest, MatchesWithoutCarTelemetryAreSkipped) {
    std::vector<AnalyticsMatchRow> matches;
    AnalyticsMatchRow rowWithoutAnalytics;
    matches.push_back(rowWithoutAnalytics);

    AnalyticsMatchRow rowWithPlayerNoCar;
    MatchAnalytics ma;
    PlayerAnalytics pa;
    pa.primaryId = "p1";
    ma.players.push_back(pa);
    rowWithPlayerNoCar.analytics = ma;
    matches.push_back(rowWithPlayerNoCar);

    const auto report = Playstyle::ComputePlaystyle(matches, "All");
    EXPECT_EQ(report.totalMatchesWithTelemetry, 0);
    EXPECT_FALSE(report.emptyMessage.empty());
}

TEST(PlaystyleInsightsTest, PlaylistFilteringWorks) {
    std::vector<AnalyticsMatchRow> matches;
    matches.push_back(CreateMatchSample(true, 400.0f, 20.0f, 65.0f, 30, "2v2", 11));
    matches.push_back(CreateMatchSample(false, 350.0f, 15.0f, 60.0f, 25, "1v1", 10));
    matches.push_back(CreateMatchSample(true, 420.0f, 22.0f, 70.0f, 35, "3v3", 13));

    const auto allReport = Playstyle::ComputePlaystyle(matches, "All");
    EXPECT_EQ(allReport.totalMatchesWithTelemetry, 3);

    const auto doublesReport = Playstyle::ComputePlaystyle(matches, "2v2");
    EXPECT_EQ(doublesReport.totalMatchesWithTelemetry, 1);

    const auto duelReport = Playstyle::ComputePlaystyle(matches, "1v1");
    EXPECT_EQ(duelReport.totalMatchesWithTelemetry, 1);

    const auto standardReport = Playstyle::ComputePlaystyle(matches, "3v3");
    EXPECT_EQ(standardReport.totalMatchesWithTelemetry, 1);
}

TEST(PlaystyleInsightsTest, RecentFormLast10VsPrevious40) {
    std::vector<AnalyticsMatchRow> matches;
    // 10 recent matches with high BPM (450)
    for (int i = 0; i < 10; ++i) {
        matches.push_back(CreateMatchSample(true, 450.0f, 25.0f, 70.0f, 40));
    }
    // 20 older matches with lower BPM (350)
    for (int i = 0; i < 20; ++i) {
        matches.push_back(CreateMatchSample(false, 350.0f, 15.0f, 60.0f, 30));
    }

    const auto report = Playstyle::ComputePlaystyle(matches, "All");
    EXPECT_EQ(report.totalMatchesWithTelemetry, 30);
    EXPECT_EQ(report.recentCount, 10);
    EXPECT_EQ(report.previousCount, 20);

    ASSERT_GE(report.recentForm.size(), 4u);
    // Boost per minute
    EXPECT_EQ(report.recentForm[0].label, "Boost per minute");
    EXPECT_EQ(report.recentForm[0].recentVal, "450");
    EXPECT_EQ(report.recentForm[0].previousVal, "350");
    EXPECT_EQ(report.recentForm[0].delta, "+100");
    EXPECT_EQ(report.recentForm[0].tone, 1);

    // Supersonic
    EXPECT_EQ(report.recentForm[1].label, "Supersonic");
    EXPECT_EQ(report.recentForm[1].recentVal, "25%");
    EXPECT_EQ(report.recentForm[1].previousVal, "15%");
    EXPECT_EQ(report.recentForm[1].delta, "+10.0%");
    EXPECT_EQ(report.recentForm[1].tone, 1);
}

TEST(PlaystyleInsightsTest, WinsSplitsRequiresEightWinsAndLosses) {
    std::vector<AnalyticsMatchRow> matches;
    // 7 wins and 8 losses -> not enough wins!
    for (int i = 0; i < 7; ++i)
        matches.push_back(CreateMatchSample(true, 400.0f, 20.0f, 65.0f, 30));
    for (int i = 0; i < 8; ++i)
        matches.push_back(CreateMatchSample(false, 350.0f, 15.0f, 60.0f, 25));

    const auto report = Playstyle::ComputePlaystyle(matches, "All");
    EXPECT_EQ(report.winsWithTelemetry, 7);
    EXPECT_EQ(report.lossesWithTelemetry, 8);
    EXPECT_TRUE(report.topSplits.empty());
    EXPECT_NE(report.splitsEmptyMessage.find("need 1 more wins"), std::string::npos);
}

TEST(PlaystyleInsightsTest, WinsSplitsComputesStandardizedEffectSize) {
    std::vector<AnalyticsMatchRow> matches;
    // 10 wins with high BPM (500) and 10 losses with lower BPM (300)
    for (int i = 0; i < 10; ++i)
        matches.push_back(CreateMatchSample(true, 500.0f, 25.0f, 75.0f, 40));
    for (int i = 0; i < 10; ++i)
        matches.push_back(CreateMatchSample(false, 300.0f, 15.0f, 55.0f, 25));

    const auto report = Playstyle::ComputePlaystyle(matches, "All");
    EXPECT_EQ(report.winsWithTelemetry, 10);
    EXPECT_EQ(report.lossesWithTelemetry, 10);
    EXPECT_TRUE(report.splitsEmptyMessage.empty());
    EXPECT_FALSE(report.topSplits.empty());
    EXPECT_LE(report.topSplits.size(), 5u);

    // First split should have positive effect size
    EXPECT_GT(report.topSplits[0].effectSize, 0.0f);
    EXPECT_FALSE(report.topSplits[0].sentence.empty());
}

TEST(PlaystyleInsightsTest, AnalyticsTrendsClutchArenasKickoffsLength) {
    std::vector<AnalyticsMatchRow> matches;
    // Match 1: OT win in DFH Stadium
    auto m1 = CreateMatchSample(true, 400.0f, 20.0f, 65.0f, 30, "2v2", 11, "DFH Stadium");
    m1.analytics->endedInOvertime = true;
    m1.ourScore = 3;
    m1.theirScore = 2; // one-goal game
    GoalRecord g1;
    g1.team = 0; // scored first
    m1.analytics->goals.push_back(g1);
    matches.push_back(m1);

    // Match 2: Regulation loss in DFH Stadium
    auto m2 = CreateMatchSample(false, 380.0f, 18.0f, 60.0f, 28, "2v2", 11, "DFH Stadium");
    m2.ourScore = 0;
    m2.theirScore = 3;
    GoalRecord g2;
    g2.team = 1; // conceded first
    m2.analytics->goals.push_back(g2);
    matches.push_back(m2);

    // Match 3: Win in Mannfield
    auto m3 = CreateMatchSample(true, 410.0f, 22.0f, 68.0f, 32, "2v2", 11, "Mannfield");
    matches.push_back(m3);

    const auto report = Playstyle::ComputeAnalyticsTrends(matches, "All");
    EXPECT_EQ(report.overtimeGames.games, 1);
    EXPECT_EQ(report.overtimeGames.wins, 1);
    EXPECT_EQ(report.oneGoalGames.games, 1);
    EXPECT_EQ(report.scoredFirst.games, 1);
    EXPECT_EQ(report.concededFirst.games, 1);
    EXPECT_EQ(report.totalKickoffs, 6);        // 2 kickoffs per match * 3 matches
    EXPECT_EQ(report.kickoffPossessionWon, 3); // 1 per match * 3 matches
    EXPECT_EQ(report.regulationGames.games, 2);
    EXPECT_EQ(report.overtimeLengthGames.games, 1);
}
