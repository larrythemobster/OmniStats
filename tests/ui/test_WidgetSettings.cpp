#include "network/api/AccountClient.hpp"
#include <gtest/gtest.h>
#include "ui/config/WidgetSettings.hpp"
#include <algorithm>

TEST(WidgetSettings, OverlayTogglePreservesPlacementAndDoesNotAffectDashboard) {
    using Id = DashboardLayout::WidgetId;
    ConfigData config;
    config.tournament_reminder_minutes = 15;
    config.overlay_layout.containers.clear();
    WidgetSettings::SetEnabled(config, Id::Tournaments, false, true, 1280, 720);
    ASSERT_TRUE(WidgetSettings::Enabled(config, Id::Tournaments, false));
    ASSERT_EQ(config.overlay_layout.containers.size(), 1u);
    auto& card = config.overlay_layout.containers.front();
    card.x = 123;
    card.y = 234;
    card.w = 360;
    card.visibility.mode = OverlayLayout::Visibility::MenusOnly;
    WidgetSettings::SetEnabled(config, Id::Tournaments, false, false, 1280, 720);
    EXPECT_FALSE(WidgetSettings::Enabled(config, Id::Tournaments, false));
    EXPECT_EQ(config.tournament_reminder_minutes, 15);
    WidgetSettings::SetEnabled(config, Id::Tournaments, false, true, 1280, 720);
    ASSERT_EQ(config.overlay_layout.containers.size(), 1u);
    EXPECT_FLOAT_EQ(config.overlay_layout.containers.front().x, 123);
    EXPECT_FLOAT_EQ(config.overlay_layout.containers.front().y, 234);
    EXPECT_FLOAT_EQ(config.overlay_layout.containers.front().w, 360);
    EXPECT_EQ(config.overlay_layout.containers.front().visibility.mode, OverlayLayout::Visibility::MenusOnly);
    EXPECT_FALSE(WidgetSettings::Enabled(config, Id::Tournaments, true));
}
TEST(WidgetSettings, DashboardToggleRestoresZoneWithoutEnablingOverlay) {
    using Id = DashboardLayout::WidgetId;
    ConfigData config;
    config.overlay_layout.containers.clear();
    WidgetSettings::SetEnabled(config, Id::DemoTracker, true, true, 1280, 720);
    ASSERT_TRUE(WidgetSettings::Enabled(config, Id::DemoTracker, true));
    auto placement = std::find_if(config.dashboard_layout.widgets.begin(), config.dashboard_layout.widgets.end(), [](const auto& item) { return item.id == Id::DemoTracker; });
    ASSERT_NE(placement, config.dashboard_layout.widgets.end());
    placement->zone = DashboardLayout::Zone::Bottom;
    WidgetSettings::SetEnabled(config, Id::DemoTracker, true, false, 1280, 720);
    EXPECT_FALSE(WidgetSettings::Enabled(config, Id::DemoTracker, true));
    WidgetSettings::SetEnabled(config, Id::DemoTracker, true, true, 1280, 720);
    placement = std::find_if(config.dashboard_layout.widgets.begin(), config.dashboard_layout.widgets.end(), [](const auto& item) { return item.id == Id::DemoTracker; });
    ASSERT_NE(placement, config.dashboard_layout.widgets.end());
    EXPECT_EQ(placement->zone, DashboardLayout::Zone::Bottom);
    EXPECT_FALSE(WidgetSettings::Enabled(config, Id::DemoTracker, false));
}
TEST(WidgetSettings, AlwaysVisibilityWorksInMenusAndMatches) {
    OverlayLayout::ContainerConfig card;
    card.visibility.mode = OverlayLayout::Visibility::Always;
    OverlayLayout::VisibilityState state;
    EXPECT_TRUE(OverlayLayout::ContainerVisible(card, state, 100));
    state.inMatch = true;
    state.inMenus = false;
    EXPECT_TRUE(OverlayLayout::ContainerVisible(card, state, 100));
    card.visibility.hideDuringReplay = true;
    state.inReplay = true;
    EXPECT_FALSE(OverlayLayout::ContainerVisible(card, state, 100));
}

TEST(WidgetSettings, IsSignedInAcceptsConfigReferenceWithoutDeadlockInsideUpdate) {
    ConfigData conf;
    EXPECT_FALSE(AccountClient::Instance().IsSignedIn(conf));
    conf.account_refresh_token = "valid_token";
    EXPECT_TRUE(AccountClient::Instance().IsSignedIn(conf));
    Config::Update([&](ConfigData& c) {
        EXPECT_FALSE(AccountClient::Instance().IsSignedIn(c));
    },
                   false);
}

TEST(WidgetSettings, DockedWidgetsHaveIndependentVisibilityRules) {
    using Id = DashboardLayout::WidgetId;
    OverlayLayout::LayoutConfig layout;
    OverlayLayout::ContainerConfig stack;
    stack.id = "stack";
    stack.widgets = {Id::Tournaments, Id::PreviousGames};
    layout.containers = {stack};

    OverlayLayout::ContainerConfig::Visibility visTourn;
    visTourn.mode = OverlayLayout::Visibility::MenusOnly;
    layout.widgetVisibility[Id::Tournaments] = visTourn;

    OverlayLayout::ContainerConfig::Visibility visGames;
    visGames.mode = OverlayLayout::Visibility::AlwaysInMatch;
    layout.widgetVisibility[Id::PreviousGames] = visGames;

    OverlayLayout::VisibilityState state;
    state.inMatch = false;
    state.inMenus = true;
    const auto rTourn = OverlayLayout::GetWidgetVisibility(layout, Id::Tournaments);
    const auto rGames = OverlayLayout::GetWidgetVisibility(layout, Id::PreviousGames);
    EXPECT_TRUE(OverlayLayout::IsVisibilityActive(rTourn, state, 100));
    EXPECT_FALSE(OverlayLayout::IsVisibilityActive(rGames, state, 100));

    state.inMatch = true;
    state.inMenus = false;
    EXPECT_FALSE(OverlayLayout::IsVisibilityActive(rTourn, state, 100));
    EXPECT_TRUE(OverlayLayout::IsVisibilityActive(rGames, state, 100));
}
