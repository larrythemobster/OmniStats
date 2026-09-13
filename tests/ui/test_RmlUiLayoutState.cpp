#include <gtest/gtest.h>

#include <set>

#include "core/DashboardLayoutConfig.hpp"
#include "core/OverlayLayoutConfig.hpp"

TEST(RmlUiLayoutState, DefaultDashboardContainsEveryWidgetExactlyOnce) {
    auto layout = DashboardLayout::DefaultLayout();
    DashboardLayout::Sanitize(layout);

    std::set<DashboardLayout::WidgetId> widgets;
    for (const auto& placement : layout.widgets)
        widgets.insert(placement.id);

    EXPECT_EQ(layout.widgets.size(), 9u);
    EXPECT_EQ(widgets.size(), 9u);
}

TEST(RmlUiLayoutState, OverlaySanitizeRemovesDuplicateWidgetsAndEmptyContainers) {
    OverlayLayout::LayoutConfig layout;
    layout.containers = {
        {.id = "one", .x = 10.0f, .y = 10.0f, .widgets = {DashboardLayout::WidgetId::LiveRoster, DashboardLayout::WidgetId::SessionStats}},
        {.id = "two", .x = 20.0f, .y = 20.0f, .widgets = {DashboardLayout::WidgetId::LiveRoster}},
        {.id = "empty", .x = 30.0f, .y = 30.0f, .widgets = {}}};

    OverlayLayout::Sanitize(layout);

    ASSERT_EQ(layout.containers.size(), 1u);
    EXPECT_EQ(layout.containers.front().id, "one");
    ASSERT_EQ(layout.containers.front().widgets.size(), 2u);
}

TEST(RmlUiLayoutState, OverlaySanitizeV3ResetsLobbyRanksDimensions) {
    OverlayLayout::LayoutConfig layout;
    layout.version = 2;
    layout.containers = {
        {.id = "lobby_ranks", .x = 10.0f, .y = 10.0f, .w = 636.0f, .h = 220.0f, .widgets = {DashboardLayout::WidgetId::LobbyRanks}},
        {.id = "other", .x = 20.0f, .y = 20.0f, .w = 300.0f, .h = 200.0f, .widgets = {DashboardLayout::WidgetId::LiveRoster}}};

    OverlayLayout::Sanitize(layout);

    EXPECT_EQ(layout.version, 3);
    ASSERT_EQ(layout.containers.size(), 2u);
    EXPECT_EQ(layout.containers[0].w, 0.0f);
    EXPECT_EQ(layout.containers[0].h, 0.0f);
    EXPECT_EQ(layout.containers[1].w, 300.0f);
    EXPECT_EQ(layout.containers[1].h, 200.0f);
}

TEST(RmlUiLayoutState, DashboardSanitizeRestoresMissingWidgetsAndBoundsColumnWeight) {
    DashboardLayout::LayoutConfig layout;
    layout.leftColumnWeight = 4.0f;
    layout.widgets = {{DashboardLayout::WidgetId::MmrGraph, DashboardLayout::Zone::Top, 99, 220.0f, false}};

    DashboardLayout::Sanitize(layout);

    EXPECT_FLOAT_EQ(layout.leftColumnWeight, 0.75f);
    EXPECT_EQ(layout.widgets.size(), 9u);
}
