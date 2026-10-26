#include <gtest/gtest.h>

#include <algorithm>
#include <array>

#include "core/game/RankProgress.hpp"
#include "network/mmr/MmrService.hpp"

namespace {
    constexpr std::array<RankProgress::Threshold, 5> kTable = {{
        {-100, "Bronze I", "Div I"},
        {100, "Bronze I", "Div II"},
        {200, "Bronze II", "Div I"},
        {300, "Bronze II", "Div II"},
        {400, "Supersonic Legend", ""},
    }};
}

TEST(RankProgressTest, BelowBoundaryReportsDistancesToBothNeighbours) {
    const auto p = RankProgress::Compute(kTable, 290, "");
    ASSERT_EQ(p.status, RankProgress::Status::Ranked);
    EXPECT_EQ(p.rank, "Bronze II Div I");
    ASSERT_TRUE(p.floor && p.next && p.previous);
    EXPECT_EQ(p.floor->mmr, 200);
    EXPECT_EQ(p.aboveFloor, 90);
    EXPECT_EQ(p.next->label, "Bronze II Div II");
    EXPECT_EQ(p.toNext, 10);
    EXPECT_EQ(p.previous->label, "Bronze I Div II");
}

TEST(RankProgressTest, ExactlyAtBoundaryBelongsToTheHigherDivision) {
    const auto p = RankProgress::Compute(kTable, 300, "");
    EXPECT_EQ(p.rank, "Bronze II Div II");
    EXPECT_EQ(p.aboveFloor, 0);
    ASSERT_TRUE(p.next);
    EXPECT_EQ(p.toNext, 100);

    const auto above = RankProgress::Compute(kTable, 301, "");
    EXPECT_EQ(above.rank, "Bronze II Div II");
    EXPECT_EQ(above.aboveFloor, 1);
    EXPECT_EQ(above.toNext, 99);
}

TEST(RankProgressTest, ReportedDivisionWinsOverTableAndExposesDemotionBuffer) {
    // Rocket League's demotion buffer can keep a player below their division's floor.
    const auto p = RankProgress::Compute(kTable, 195, "Bronze II Div I");
    EXPECT_EQ(p.rank, "Bronze II Div I");
    ASSERT_TRUE(p.floor);
    EXPECT_EQ(p.aboveFloor, -5);
    EXPECT_EQ(p.toNext, 105);

    const auto promotedEarly = RankProgress::Compute(kTable, 305, "Bronze II Div I");
    EXPECT_EQ(promotedEarly.toNext, -5);

    const auto unknownLabel = RankProgress::Compute(kTable, 195, "Mystery Rank");
    EXPECT_EQ(unknownLabel.rank, "Bronze I Div II");
}

TEST(RankProgressTest, TopAndBottomOfTheLadderOmitTheMissingSide) {
    const auto top = RankProgress::Compute(kTable, 450, "Supersonic Legend");
    EXPECT_EQ(top.rank, "Supersonic Legend");
    EXPECT_FALSE(top.next);
    ASSERT_TRUE(top.floor && top.previous);
    EXPECT_EQ(top.aboveFloor, 50);
    EXPECT_EQ(top.previous->label, "Bronze II Div II");

    const auto bottom = RankProgress::Compute(kTable, 50, "");
    EXPECT_EQ(bottom.rank, "Bronze I Div I");
    EXPECT_FALSE(bottom.floor);
    EXPECT_FALSE(bottom.previous);
    ASSERT_TRUE(bottom.next);
    EXPECT_EQ(bottom.toNext, 50);
}

TEST(RankProgressTest, PlacementAndMissingDataProduceNoDistances) {
    const auto placement = RankProgress::Compute(kTable, 250, "Unranked");
    EXPECT_EQ(placement.status, RankProgress::Status::Placement);
    EXPECT_FALSE(placement.floor);
    EXPECT_FALSE(placement.next);

    EXPECT_EQ(RankProgress::Compute(kTable, 0, "").status, RankProgress::Status::Unavailable);
    EXPECT_EQ(RankProgress::Compute(kTable, -20, "Bronze I Div I").status, RankProgress::Status::Unavailable);
    EXPECT_EQ(RankProgress::Compute({}, 1200, "Champion II Div I").status, RankProgress::Status::Unavailable);
}

TEST(RankProgressTest, PlaylistTablesAreSelectedPerPlaylist) {
    EXPECT_TRUE(MmrService::RankThresholdsForPlaylist("best").empty());
    EXPECT_TRUE(MmrService::RankThresholdsForPlaylist("casual").empty());
    EXPECT_TRUE(MmrService::RankThresholdsForPlaylist("not-a-playlist").empty());

    const auto duel = RankProgress::Compute(MmrService::RankThresholdsForPlaylist("1v1"), 1000, "");
    const auto doubles = RankProgress::Compute(MmrService::RankThresholdsForPlaylist("2v2"), 1000, "");
    ASSERT_EQ(duel.status, RankProgress::Status::Ranked);
    ASSERT_EQ(doubles.status, RankProgress::Status::Ranked);
    ASSERT_TRUE(duel.next && doubles.next);
    EXPECT_EQ(duel.rank, MmrService::GetRankTierForPlaylistMmr("1v1", 1000));
    EXPECT_EQ(doubles.rank, MmrService::GetRankTierForPlaylistMmr("2v2", 1000));
    EXPECT_NE(duel.rank, doubles.rank);
    EXPECT_NE(duel.next->mmr, doubles.next->mmr);
}

TEST(RankProgressTest, GrandChampionAndSupersonicLegendUseTheirOwnRungs) {
    const auto table = MmrService::RankThresholdsForPlaylist("2v2");
    const auto ssl = std::find_if(table.begin(), table.end(), [](const auto& t) { return std::string_view(t.tier) == "Supersonic Legend"; });
    ASSERT_NE(ssl, table.end());
    const auto gc3d4 = ssl - 1;

    const auto almost = RankProgress::Compute(table, gc3d4->minMmr + 1, "Grand Champion III Div IV");
    EXPECT_EQ(almost.rank, "Grand Champion III Div IV");
    ASSERT_TRUE(almost.next);
    EXPECT_EQ(almost.next->label, "Supersonic Legend");
    EXPECT_EQ(almost.toNext, ssl->minMmr - gc3d4->minMmr - 1);

    const auto top = RankProgress::Compute(table, ssl->minMmr + 40, "Supersonic Legend");
    EXPECT_FALSE(top.next);
    ASSERT_TRUE(top.previous);
    EXPECT_EQ(top.previous->label, "Grand Champion III Div IV");
}

TEST(RankProgressTest, ExpandsRangeOnlyForNearbyAdjacentBoundaries) {
    float minV = 250.0f, maxV = 290.0f;
    RankProgress::ExpandRangeForBoundaries(kTable, 280.0f, 60.0f, minV, maxV);
    EXPECT_LT(minV, 200.0f);
    EXPECT_GT(maxV, 300.0f);

    minV = 270.0f;
    maxV = 290.0f;
    RankProgress::ExpandRangeForBoundaries(kTable, 280.0f, 60.0f, minV, maxV);
    EXPECT_FLOAT_EQ(minV, 270.0f);
    EXPECT_GT(maxV, 300.0f);
}

TEST(RankProgressTest, GraphBoundariesMapOntoThePlotAndSkipTheLadderFloor) {
    RankProgress::GraphLayoutOptions options;
    options.plotTopPct = 10.0f;
    options.plotBottomPct = 88.0f;
    const auto boundaries = RankProgress::LayoutGraphBoundaries(kTable, -150.0f, 320.0f, 290.0f, options);
    std::vector<int> mmrs;
    for (const auto& b : boundaries)
        mmrs.push_back(b.boundary.mmr);
    EXPECT_EQ(mmrs, (std::vector<int>{300, 200, 100}));

    const auto& top = boundaries.front();
    EXPECT_NEAR(top.topPct, 88.0f - (300.0f + 150.0f) / 470.0f * 78.0f, 0.01f);
    EXPECT_TRUE(top.labeled);
    EXPECT_TRUE(top.boundary.tierStart == false);
    EXPECT_TRUE(boundaries[1].boundary.tierStart);
}

TEST(RankProgressTest, CrowdedGraphsKeepTierStartsAndAdjacentDivisionsWithoutLabelOverlap) {
    const auto table = MmrService::RankThresholdsForPlaylist("2v2");
    RankProgress::GraphLayoutOptions options;
    options.reserved = {{0.0f, 8.0f}, {87.0f, 100.0f}};
    const float current = 1200.0f;
    const auto boundaries = RankProgress::LayoutGraphBoundaries(table, 700.0f, 1700.0f, current, options);
    ASSERT_FALSE(boundaries.empty());
    EXPECT_LE(boundaries.size(), options.maxLines);

    const auto progress = RankProgress::Compute(table, static_cast<int>(current), "");
    ASSERT_TRUE(progress.floor && progress.next);
    const auto has = [&](int mmr) {
        return std::any_of(boundaries.begin(), boundaries.end(), [&](const auto& b) { return b.boundary.mmr == mmr; });
    };
    EXPECT_TRUE(has(progress.floor->mmr));
    EXPECT_TRUE(has(progress.next->mmr));

    std::vector<std::pair<float, float>> labels;
    for (const auto& b : boundaries) {
        EXPECT_TRUE(b.boundary.tierStart || b.boundary.mmr == progress.floor->mmr || b.boundary.mmr == progress.next->mmr);
        if (b.labeled) labels.emplace_back(b.labelTopPct, b.labelTopPct + options.labelHeightPct);
    }
    EXPECT_LE(labels.size(), options.maxLabels);
    for (size_t i = 0; i < labels.size(); ++i) {
        EXPECT_GE(labels[i].first, 8.0f);
        EXPECT_LE(labels[i].second, 87.0f);
        for (size_t j = i + 1; j < labels.size(); ++j)
            EXPECT_FALSE(labels[i].first < labels[j].second && labels[j].first < labels[i].second);
    }
}

TEST(RankProgressTest, CollidingLabelsKeepTheBoundaryNearestTheCurrentRating) {
    constexpr std::array<RankProgress::Threshold, 3> close = {{
        {-100, "Bronze I", "Div I"},
        {500, "Bronze I", "Div II"},
        {501, "Bronze I", "Div III"},
    }};
    RankProgress::GraphLayoutOptions options;
    options.labelHeightPct = 40.0f;
    const auto boundaries = RankProgress::LayoutGraphBoundaries(close, 480.0f, 520.0f, 499.0f, options);
    ASSERT_EQ(boundaries.size(), 2u);
    const auto labeled = std::count_if(boundaries.begin(), boundaries.end(), [](const auto& b) { return b.labeled; });
    EXPECT_EQ(labeled, 1);
    const auto nearest = std::find_if(boundaries.begin(), boundaries.end(), [](const auto& b) { return b.boundary.mmr == 500; });
    ASSERT_NE(nearest, boundaries.end());
    EXPECT_TRUE(nearest->labeled);
}

TEST(RankProgressTest, GraphBandsTileThePlotHighestFirstAndClipAtTheEdges) {
    const auto bands = RankProgress::LayoutGraphBands(kTable, 150.0f, 450.0f, 0.0f, 100.0f);
    ASSERT_EQ(bands.size(), 4u);
    EXPECT_EQ(bands[0].label, "Supersonic Legend");
    EXPECT_FLOAT_EQ(bands[0].topPct, 0.0f);
    EXPECT_NEAR(bands[0].bottomPct, 100.0f / 6.0f, 0.01f);
    EXPECT_EQ(bands[1].label, "Bronze II Div II");
    EXPECT_NEAR(bands[1].bottomPct, 50.0f, 0.01f);
    EXPECT_EQ(bands[3].label, "Bronze I Div II");
    EXPECT_FLOAT_EQ(bands[3].bottomPct, 100.0f);
    for (size_t i = 1; i < bands.size(); ++i)
        EXPECT_FLOAT_EQ(bands[i].topPct, bands[i - 1].bottomPct);

    // Ratings below the table still sit in its lowest division.
    const auto below = RankProgress::LayoutGraphBands(kTable, -300.0f, -150.0f, 10.0f, 90.0f);
    ASSERT_EQ(below.size(), 1u);
    EXPECT_EQ(below[0].label, "Bronze I Div I");
    EXPECT_FLOAT_EQ(below[0].topPct, 10.0f);
    EXPECT_FLOAT_EQ(below[0].bottomPct, 90.0f);
}
