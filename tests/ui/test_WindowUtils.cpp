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
