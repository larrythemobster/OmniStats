#include <gtest/gtest.h>
#include "platform/OverlayHoldState.hpp"

TEST(OverlayHoldStateTest, EitherInputSourceCanKeepOverlayVisible) {
    OverlayHoldState holds;
    EXPECT_FALSE(holds.Visible());
    holds.Set(OverlayHoldSource::Keyboard, true);
    EXPECT_TRUE(holds.Visible());
    holds.Set(OverlayHoldSource::Gamepad, true);
    holds.Set(OverlayHoldSource::Keyboard, false);
    EXPECT_TRUE(holds.Visible());
    holds.Set(OverlayHoldSource::Gamepad, false);
    EXPECT_FALSE(holds.Visible());
}

TEST(OverlayHoldStateTest, DisconnectedGamepadCannotLatchKeyboardRelease) {
    OverlayHoldState holds;
    holds.Set(OverlayHoldSource::Keyboard, true);
    holds.Set(OverlayHoldSource::Gamepad, false);
    holds.Set(OverlayHoldSource::Keyboard, false);
    EXPECT_FALSE(holds.Visible());
    holds.Set(OverlayHoldSource::Gamepad, true);
    holds.Set(OverlayHoldSource::Gamepad, false);
    EXPECT_FALSE(holds.Visible());
}
