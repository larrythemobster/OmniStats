#include <gtest/gtest.h>
#include "ui/window/ForegroundDetector.hpp"

TEST(ForegroundDetectorTest, IsRocketLeagueWindow_MatchesUnrealEngineClass) {
    EXPECT_TRUE(IsRocketLeagueWindow("LaunchUnrealUWindowsClient", ""));
    EXPECT_TRUE(IsRocketLeagueWindow("LaunchUnrealUWindowsClient", "Some Custom Title"));
}

TEST(ForegroundDetectorTest, IsRocketLeagueWindow_ExcludesBrowserAndExplorerClasses) {
    EXPECT_FALSE(IsRocketLeagueWindow("Chrome_WidgetWin_1", "Rocket League"));
    EXPECT_FALSE(IsRocketLeagueWindow("MozillaWindowClass", "Rocket League (64-bit, DX11)"));
    EXPECT_FALSE(IsRocketLeagueWindow("IEFrame", "Rocket League (32-bit, DX11)"));
    EXPECT_FALSE(IsRocketLeagueWindow("CabinetWClass", "Rocket League"));
}

TEST(ForegroundDetectorTest, IsRocketLeagueWindow_MatchesKnownTitlesOnOtherClasses) {
    EXPECT_TRUE(IsRocketLeagueWindow("Window", "Rocket League (64-bit, DX11)"));
    EXPECT_TRUE(IsRocketLeagueWindow("Window", "Rocket League (32-bit, DX11)"));
    EXPECT_TRUE(IsRocketLeagueWindow("Window", "Rocket League"));
    EXPECT_FALSE(IsRocketLeagueWindow("Window", "Rocket League Sideswipe"));
    EXPECT_FALSE(IsRocketLeagueWindow("Window", "Discord"));
}

TEST(ForegroundDetectorTest, ClassifyForegroundWindow_DetectsOverlayAndOmniStatsWindows) {
    const DWORD myPid = 1234;
    EXPECT_EQ(ClassifyForegroundWindow("OmniStats_Class", "OmniStats", myPid, myPid, true, false),
              ForegroundKind::Overlay);
    EXPECT_EQ(ClassifyForegroundWindow("SomeDialog", "Settings", myPid, myPid, false, false),
              ForegroundKind::Overlay);
}

TEST(ForegroundDetectorTest, ClassifyForegroundWindow_DetectsRocketLeague) {
    const DWORD myPid = 1234;
    const DWORD rlPid = 5678;
    EXPECT_EQ(ClassifyForegroundWindow("LaunchUnrealUWindowsClient", "Rocket League", rlPid, myPid, false, false),
              ForegroundKind::RocketLeague);
    EXPECT_EQ(ClassifyForegroundWindow("AnyClass", "AnyTitle", rlPid, myPid, false, true),
              ForegroundKind::RocketLeague);
}

TEST(ForegroundDetectorTest, ClassifyForegroundWindow_DetectsNoneAndOther) {
    const DWORD myPid = 1234;
    EXPECT_EQ(ClassifyForegroundWindow("", "", 0, myPid, false, false),
              ForegroundKind::None);
    EXPECT_EQ(ClassifyForegroundWindow("Notepad", "Untitled - Notepad", 9999, myPid, false, false),
              ForegroundKind::Other);
}

TEST(ForegroundDetectorTest, ResolveRocketLeagueFocus_StateTransitions) {
    bool lastWasRL = false;

    // Rocket League gains focus
    EXPECT_TRUE(ResolveRocketLeagueFocus(ForegroundKind::RocketLeague, false, lastWasRL));
    EXPECT_TRUE(lastWasRL);

    // Overlay clicked after Rocket League -> inherits focus
    EXPECT_TRUE(ResolveRocketLeagueFocus(ForegroundKind::Overlay, false, lastWasRL));
    EXPECT_TRUE(lastWasRL);

    // Other window gains focus -> loses focus
    EXPECT_FALSE(ResolveRocketLeagueFocus(ForegroundKind::Other, false, lastWasRL));
    EXPECT_FALSE(lastWasRL);

    // Overlay clicked after other window -> does NOT inherit focus
    EXPECT_FALSE(ResolveRocketLeagueFocus(ForegroundKind::Overlay, false, lastWasRL));
    EXPECT_FALSE(lastWasRL);

    // Rocket League minimized -> unavailable
    EXPECT_FALSE(ResolveRocketLeagueFocus(ForegroundKind::RocketLeague, true, lastWasRL));
    EXPECT_FALSE(lastWasRL);
}
