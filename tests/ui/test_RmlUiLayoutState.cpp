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

    EXPECT_EQ(layout.version, OverlayLayout::kCurrentLayoutVersion);
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

TEST(RmlUiLayoutState, ContainerVisibleEvaluatorTableCoversModesBoundariesReplayAndStaleTimestamps) {
    using Mode = OverlayLayout::Visibility::Mode;
    using Event = OverlayLayout::Visibility::Event;
    using Timestamps = OverlayLayout::VisibilityTimestamps;

    struct Case {
        const char* name;
        Mode mode;
        Event event;
        int seconds;
        bool alsoWhileKeyHeld;
        bool hideDuringReplay;
        int64_t nowMs;
        Timestamps timestamps;
        bool keyHeld;
        bool inMatch;
        bool inReplay;
        bool inMenus;
        bool expectedVisible;
    };

    const Timestamps activeMatchTs{
        .matchStartMs = 1000,
        .firstCountdownMs = 2000,
        .countdownMs = 5000,
        .goalMs = 10000,
        .matchEndMs = 20000,
        .podiumMs = 22000};

    const Case cases[] = {
        // KeyHeld
        {"KeyHeld_NotHeld", Mode::KeyHeld, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, false, true, false, false, false},
        {"KeyHeld_HeldInMatch", Mode::KeyHeld, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, true, true, false, false, true},
        {"KeyHeld_HeldInMenus", Mode::KeyHeld, Event::FirstCountdown, 8, true, false, 3000, {}, true, false, false, true, true},
        {"KeyHeld_HiddenDuringReplay", Mode::KeyHeld, Event::FirstCountdown, 8, true, true, 3000, activeMatchTs, true, true, true, false, false},
        {"KeyHeld_VisibleDuringReplayWhenFlagOff", Mode::KeyHeld, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, true, true, true, false, true},

        // AlwaysInMatch
        {"AlwaysInMatch_InMatch", Mode::AlwaysInMatch, Event::FirstCountdown, 8, false, false, 3000, activeMatchTs, false, true, false, false, true},
        {"AlwaysInMatch_InMenusNoKey", Mode::AlwaysInMatch, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, false, false, false, true, false},
        {"AlwaysInMatch_InMenusKeyHeldAllowed", Mode::AlwaysInMatch, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, true, false, false, true, true},
        {"AlwaysInMatch_InMenusKeyHeldDisabled", Mode::AlwaysInMatch, Event::FirstCountdown, 8, false, false, 3000, activeMatchTs, true, false, false, true, false},
        {"AlwaysInMatch_HiddenDuringReplay", Mode::AlwaysInMatch, Event::FirstCountdown, 8, true, true, 3000, activeMatchTs, false, true, true, false, false},

        // MenusOnly
        {"MenusOnly_InMenus", Mode::MenusOnly, Event::FirstCountdown, 8, false, false, 3000, {}, false, false, false, true, true},
        {"MenusOnly_InMatchNoKey", Mode::MenusOnly, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, false, true, false, false, false},
        {"MenusOnly_InMatchKeyHeldAllowed", Mode::MenusOnly, Event::FirstCountdown, 8, true, false, 3000, activeMatchTs, true, true, false, false, true},
        {"MenusOnly_InMatchKeyHeldDisabled", Mode::MenusOnly, Event::FirstCountdown, 8, false, false, 3000, activeMatchTs, true, true, false, false, false},
        {"MenusOnly_NotInMenusAndNotInMatch", Mode::MenusOnly, Event::FirstCountdown, 8, false, false, 3000, {}, false, false, false, false, false},

        // AfterEvent: MatchStart (matchStartMs = 1000, 8s => [1000, 9000))
        {"AfterMatchStart_AtStart", Mode::AfterEvent, Event::MatchStart, 8, false, false, 1000, activeMatchTs, false, true, false, false, true},
        {"AfterMatchStart_BeforeBoundary", Mode::AfterEvent, Event::MatchStart, 8, false, false, 8999, activeMatchTs, false, true, false, false, true},
        {"AfterMatchStart_ExactBoundary8s", Mode::AfterEvent, Event::MatchStart, 8, false, false, 9000, activeMatchTs, false, true, false, false, false},

        // AfterEvent: FirstCountdown (firstCountdownMs = 2000, 8s => [2000, 10000))
        {"AfterFirstCountdown_BeforeEvent", Mode::AfterEvent, Event::FirstCountdown, 8, false, false, 1999, activeMatchTs, false, true, false, false, false},
        {"AfterFirstCountdown_AtEvent", Mode::AfterEvent, Event::FirstCountdown, 8, false, false, 2000, activeMatchTs, false, true, false, false, true},
        {"AfterFirstCountdown_BeforeBoundary", Mode::AfterEvent, Event::FirstCountdown, 8, false, false, 9999, activeMatchTs, false, true, false, false, true},
        {"AfterFirstCountdown_ExactBoundary8s", Mode::AfterEvent, Event::FirstCountdown, 8, false, false, 10000, activeMatchTs, false, true, false, false, false},
        {"AfterFirstCountdown_ExpiredButKeyHeldAllowed", Mode::AfterEvent, Event::FirstCountdown, 8, true, false, 15000, activeMatchTs, true, true, false, false, true},
        {"AfterFirstCountdown_ExpiredKeyHeldDisabled", Mode::AfterEvent, Event::FirstCountdown, 8, false, false, 15000, activeMatchTs, true, true, false, false, false},

        // AfterEvent: Countdown (countdownMs = 5000, 5s => [5000, 10000))
        {"AfterCountdown_InsideWindow", Mode::AfterEvent, Event::Countdown, 5, false, false, 9999, activeMatchTs, false, true, false, false, true},
        {"AfterCountdown_ExactBoundary5s", Mode::AfterEvent, Event::Countdown, 5, false, false, 10000, activeMatchTs, false, true, false, false, false},

        // AfterEvent: Goal (goalMs = 10000, 10s => [10000, 20000))
        {"AfterGoal_InsideWindow", Mode::AfterEvent, Event::Goal, 10, false, false, 15000, activeMatchTs, false, true, false, false, true},
        {"AfterGoal_HiddenDuringReplay", Mode::AfterEvent, Event::Goal, 10, false, true, 15000, activeMatchTs, false, true, true, false, false},
        {"AfterGoal_ExactBoundary10s", Mode::AfterEvent, Event::Goal, 10, false, false, 20000, activeMatchTs, false, true, false, false, false},

        // AfterEvent: MatchEnd (matchEndMs = 20000, 15s => [20000, 35000))
        {"AfterMatchEnd_InsideWindow", Mode::AfterEvent, Event::MatchEnd, 15, false, false, 34999, activeMatchTs, false, true, false, false, true},
        {"AfterMatchEnd_ExactBoundary15s", Mode::AfterEvent, Event::MatchEnd, 15, false, false, 35000, activeMatchTs, false, true, false, false, false},

        // AfterEvent: Podium (podiumMs = 22000, 20s => [22000, 42000))
        {"AfterPodium_InsideWindow", Mode::AfterEvent, Event::Podium, 20, false, false, 41999, activeMatchTs, false, false, false, true, true},
        {"AfterPodium_ExactBoundary20s", Mode::AfterEvent, Event::Podium, 20, false, false, 42000, activeMatchTs, false, false, false, true, false},

        // Stale timestamps from a previous match must not trigger
        {"StaleGoal_PreviousMatchBeforeNewMatchStart", Mode::AfterEvent, Event::Goal, 10, false, false, 12000,
         Timestamps{.matchStartMs = 11000, .goalMs = 9000}, false, true, false, false, false},
        {"StaleCountdown_PreviousMatchBeforeNewMatchStart", Mode::AfterEvent, Event::Countdown, 10, false, false, 12000,
         Timestamps{.matchStartMs = 11000, .countdownMs = 9000}, false, true, false, false, false},
        {"StaleFirstCountdown_InMenusAfterMatchDestroyed", Mode::AfterEvent, Event::FirstCountdown, 10, false, false, 5000,
         Timestamps{.matchStartMs = 1000, .firstCountdownMs = 2000}, false, false, false, true, false},
        {"StalePodium_PreviousMatchBeforeNewMatchStart", Mode::AfterEvent, Event::Podium, 20, false, false, 15000,
         Timestamps{.matchStartMs = 12000, .podiumMs = 10000}, false, true, false, false, false},
        {"StalePodium_OlderThanLatestMatchEnd", Mode::AfterEvent, Event::Podium, 20, false, false, 21000,
         Timestamps{.matchStartMs = 1000, .matchEndMs = 20000, .podiumMs = 15000}, false, true, false, false, false},

        // Seconds clamping (< 5 -> 5s, > 30 -> 30s)
        {"ClampedLowSeconds_4sStillVisibleAt2sInput", Mode::AfterEvent, Event::Goal, 2, false, false, 14999, activeMatchTs, false, true, false, false, true},
        {"ClampedLowSeconds_5sBoundaryAt2sInput", Mode::AfterEvent, Event::Goal, 2, false, false, 15000, activeMatchTs, false, true, false, false, false},
        {"ClampedHighSeconds_29sStillVisibleAt99sInput", Mode::AfterEvent, Event::Goal, 99, false, false, 39999, activeMatchTs, false, true, false, false, true},
        {"ClampedHighSeconds_30sBoundaryAt99sInput", Mode::AfterEvent, Event::Goal, 99, false, false, 40000, activeMatchTs, false, true, false, false, false},
    };

    for (const auto& tc : cases) {
        OverlayLayout::ContainerConfig container;
        container.id = "test";
        container.visibility = {
            .mode = tc.mode,
            .event = tc.event,
            .seconds = tc.seconds,
            .alsoWhileKeyHeld = tc.alsoWhileKeyHeld,
            .hideDuringReplay = tc.hideDuringReplay};

        OverlayLayout::VisibilityState state{
            .timestamps = tc.timestamps,
            .keyHeld = tc.keyHeld,
            .inMatch = tc.inMatch,
            .inReplay = tc.inReplay,
            .inMenus = tc.inMenus,
            .editMode = false};

        EXPECT_EQ(OverlayLayout::ContainerVisible(container, state, tc.nowMs), tc.expectedVisible) << tc.name;

        // Edit mode always shows every container regardless of rules.
        state.editMode = true;
        EXPECT_TRUE(OverlayLayout::ContainerVisible(container, state, tc.nowMs)) << tc.name << " (editMode)";
    }
}

TEST(RmlUiLayoutState, NextVisibilityChangeMsReturnsExactDeadlinesAndNulloptWhenIdle) {
    using Mode = OverlayLayout::Visibility::Mode;
    using Event = OverlayLayout::Visibility::Event;

    OverlayLayout::ContainerConfig keyHeldContainer;
    keyHeldContainer.id = "roster";
    keyHeldContainer.visibility.mode = Mode::KeyHeld;

    OverlayLayout::ContainerConfig lobbyContainer;
    lobbyContainer.id = "lobby_ranks";
    lobbyContainer.visibility.mode = Mode::AfterEvent;
    lobbyContainer.visibility.event = Event::FirstCountdown;
    lobbyContainer.visibility.seconds = 8;
    lobbyContainer.visibility.alsoWhileKeyHeld = true;
    lobbyContainer.visibility.hideDuringReplay = true;

    OverlayLayout::ContainerConfig goalContainer;
    goalContainer.id = "goal_card";
    goalContainer.visibility.mode = Mode::AfterEvent;
    goalContainer.visibility.event = Event::Goal;
    goalContainer.visibility.seconds = 5;
    goalContainer.visibility.alsoWhileKeyHeld = false;

    OverlayLayout::LayoutConfig layout;
    layout.containers = {keyHeldContainer, lobbyContainer, goalContainer};

    OverlayLayout::VisibilityState state;
    state.inMatch = true;
    state.inMenus = false;

    // Idle: no timestamps set -> no wake deadline.
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(layout, state, 1000), std::nullopt);

    // Active FirstCountdown window [2000, 10000): wakes at 10000.
    state.timestamps.matchStartMs = 1000;
    state.timestamps.firstCountdownMs = 2000;
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(lobbyContainer, state, 2000), std::optional<int64_t>(10000));
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(lobbyContainer, state, 9999), std::optional<int64_t>(10000));
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(lobbyContainer, state, 10000), std::nullopt);

    // While key is held and alsoWhileKeyHeld is true, timer expiry will not change visibility.
    state.keyHeld = true;
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(lobbyContainer, state, 5000), std::nullopt);
    state.keyHeld = false;

    // While hidden by goal replay, timer expiry will not change visibility.
    state.inReplay = true;
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(lobbyContainer, state, 5000), std::nullopt);
    state.inReplay = false;

    // Multiple active windows in layout: earliest deadline wins (goal at 4000 + 5000 = 9000 < 10000).
    state.timestamps.goalMs = 4000;
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(layout, state, 5000), std::optional<int64_t>(9000));
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(layout, state, 9000), std::optional<int64_t>(10000));
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(layout, state, 10000), std::nullopt);

    // Stale timestamp from previous match requests no wake deadline.
    state.timestamps.matchStartMs = 20000;
    EXPECT_EQ(OverlayLayout::NextVisibilityChangeMs(layout, state, 21000), std::nullopt);
}
