#include <gtest/gtest.h>
#include "ui/WindowUtils.hpp"

TEST(WindowUtils, ComputeCenteredRect_CentersCorrectly) {
    RECT monitor{0, 0, 1920, 1080};
    RECT r = ComputeCenteredRect(monitor, 1024, 768);
    EXPECT_EQ(r.right - r.left, 1024);
    EXPECT_EQ(r.bottom - r.top, 768);
    EXPECT_EQ(r.left, (1920 - 1024) / 2);
    EXPECT_EQ(r.top, (1080 - 768) / 2);
}

TEST(WindowUtils, ComputeWindowStyles_Overlay) {
    LONG style = 0, ex = 0;
    // Start with neutral values
    style = WS_OVERLAPPEDWINDOW;
    ex = 0;
    ComputeWindowStyles(false, false, style, ex);
    // Overlay should set WS_POPUP and layered/topmost flags
    EXPECT_TRUE((style & WS_POPUP) != 0);
    EXPECT_TRUE((ex & WS_EX_TOPMOST) != 0);
    EXPECT_TRUE((ex & WS_EX_LAYERED) != 0);
}

TEST(WindowUtils, ComputeWindowStyles_SecondMonitor) {
    LONG style = WS_POPUP, ex = 0;
    ComputeWindowStyles(true, true, style, ex);
    EXPECT_TRUE((style & WS_OVERLAPPEDWINDOW) != 0);
    EXPECT_TRUE((ex & WS_EX_APPWINDOW) != 0);
}

TEST(WindowUtils, ShouldRaiseSecondMonitorWindow_TriggersOnlyOnTransition) {
    // True when transitioning to active in second-monitor mode while visible
    EXPECT_TRUE(ShouldRaiseSecondMonitorWindow(true, true, false, true));

    // False if RL was already active on previous frame (prevent repeated Z-order churn)
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(true, true, true, true));

    // False if RL is not active
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(true, false, false, true));
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(true, false, true, true));

    // False if window is not visible
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(true, true, false, false));

    // False if not in second monitor mode (overlay mode manages HWND_TOPMOST separately)
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(false, true, false, true));
}

TEST(WindowUtils, ShouldRaiseSecondMonitorWindow_SimulatedFocusLifecycle) {
    bool wasRLActive = false;
    bool secondMonitorMode = true;

    // Frame 1: RL is inactive (browser/Discord focused) -> do not raise
    bool isRLActive = false;
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(secondMonitorMode, isRLActive, wasRLActive, true));
    wasRLActive = isRLActive;

    // Frame 2: Alt+Tab back to RL -> raise dashboard
    isRLActive = true;
    EXPECT_TRUE(ShouldRaiseSecondMonitorWindow(secondMonitorMode, isRLActive, wasRLActive, true));
    wasRLActive = isRLActive;

    // Frame 3: Still in RL -> do not raise again
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(secondMonitorMode, isRLActive, wasRLActive, true));
    wasRLActive = isRLActive;

    // Frame 4: Alt+Tab to another window -> do not raise
    isRLActive = false;
    EXPECT_FALSE(ShouldRaiseSecondMonitorWindow(secondMonitorMode, isRLActive, wasRLActive, true));
    wasRLActive = isRLActive;

    // Frame 5: Alt+Tab directly back to RL -> raise dashboard again
    isRLActive = true;
    EXPECT_TRUE(ShouldRaiseSecondMonitorWindow(secondMonitorMode, isRLActive, wasRLActive, true));
    wasRLActive = isRLActive;
}
