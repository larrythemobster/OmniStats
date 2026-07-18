#include <gtest/gtest.h>

#include "core/Config.hpp"
#include "core/Storage.hpp"

namespace {

    constexpr int kLeftStickUp = 22;
    constexpr int kLeftStickLeft = 24;
    constexpr int kRightStickUp = 26;
    constexpr int kRightStickRight = 29;

    class ControllerBindingsConfigTest : public ::testing::Test {
      protected:
        void SetUp() override {
            Storage::InitializeEnvironment();
            originalConfig_ = Config::Read();
        }

        void TearDown() override {
            Config::Update([this](ConfigData& config) {
                config = originalConfig_;
            });
        }

        ConfigData originalConfig_;
    };

    TEST(ControllerBindingsDefaultsTest, NewActionsRemainUnboundByDefault) {
        const ConfigData defaults;

        EXPECT_EQ(defaults.gamepad_overlay, 4);
        EXPECT_FALSE(defaults.gamepad_overlay_raw);
        EXPECT_EQ(defaults.gamepad_overlay_raw_button, 4);

        EXPECT_EQ(defaults.gamepad_cycle, -1);
        EXPECT_FALSE(defaults.gamepad_cycle_raw);
        EXPECT_EQ(defaults.gamepad_cycle_raw_button, -1);

        EXPECT_EQ(defaults.gamepad_expand, -1);
        EXPECT_FALSE(defaults.gamepad_expand_raw);
        EXPECT_EQ(defaults.gamepad_expand_raw_button, -1);

        EXPECT_EQ(defaults.gamepad_session, -1);
        EXPECT_FALSE(defaults.gamepad_session_raw);
        EXPECT_EQ(defaults.gamepad_session_raw_button, -1);

        EXPECT_EQ(defaults.gamepad_menu, -1);
        EXPECT_FALSE(defaults.gamepad_menu_raw);
        EXPECT_EQ(defaults.gamepad_menu_raw_button, -1);
    }

    TEST_F(ControllerBindingsConfigTest, PersistsStandardRawAndStickBindings) {
        Config::Update([](ConfigData& config) {
            config.gamepad_overlay = kLeftStickUp;
            config.gamepad_overlay_raw = false;
            config.gamepad_overlay_raw_button = -1;

            config.gamepad_cycle = kRightStickRight;
            config.gamepad_cycle_raw = false;
            config.gamepad_cycle_raw_button = -1;

            config.gamepad_expand = -1;
            config.gamepad_expand_raw = true;
            config.gamepad_expand_raw_button = 9;

            config.gamepad_session = kLeftStickLeft;
            config.gamepad_session_raw = false;
            config.gamepad_session_raw_button = -1;

            config.gamepad_menu = kRightStickUp;
            config.gamepad_menu_raw = true;
            config.gamepad_menu_raw_button = 7;
        },
                       false);
        Config::Save();

        Config::Update([](ConfigData& config) {
            config.gamepad_overlay = 0;
            config.gamepad_overlay_raw = true;
            config.gamepad_overlay_raw_button = 0;
            config.gamepad_cycle = 0;
            config.gamepad_cycle_raw = true;
            config.gamepad_cycle_raw_button = 0;
            config.gamepad_expand = 0;
            config.gamepad_expand_raw = false;
            config.gamepad_expand_raw_button = 0;
            config.gamepad_session = 0;
            config.gamepad_session_raw = true;
            config.gamepad_session_raw_button = 0;
            config.gamepad_menu = 0;
            config.gamepad_menu_raw = false;
            config.gamepad_menu_raw_button = 0;
        },
                       false);

        Config::Load();
        const ConfigData loaded = Config::Read();

        EXPECT_EQ(loaded.gamepad_overlay, kLeftStickUp);
        EXPECT_FALSE(loaded.gamepad_overlay_raw);
        EXPECT_EQ(loaded.gamepad_overlay_raw_button, -1);

        EXPECT_EQ(loaded.gamepad_cycle, kRightStickRight);
        EXPECT_FALSE(loaded.gamepad_cycle_raw);
        EXPECT_EQ(loaded.gamepad_cycle_raw_button, -1);

        EXPECT_EQ(loaded.gamepad_expand, -1);
        EXPECT_TRUE(loaded.gamepad_expand_raw);
        EXPECT_EQ(loaded.gamepad_expand_raw_button, 9);

        EXPECT_EQ(loaded.gamepad_session, kLeftStickLeft);
        EXPECT_FALSE(loaded.gamepad_session_raw);
        EXPECT_EQ(loaded.gamepad_session_raw_button, -1);

        EXPECT_EQ(loaded.gamepad_menu, kRightStickUp);
        EXPECT_TRUE(loaded.gamepad_menu_raw);
        EXPECT_EQ(loaded.gamepad_menu_raw_button, 7);
    }

    TEST_F(ControllerBindingsConfigTest, PersistsClearedBindings) {
        Config::Update([](ConfigData& config) {
            config.gamepad_overlay = -1;
            config.gamepad_overlay_raw = false;
            config.gamepad_overlay_raw_button = -1;
            config.gamepad_cycle = -1;
            config.gamepad_cycle_raw = false;
            config.gamepad_cycle_raw_button = -1;
            config.gamepad_expand = -1;
            config.gamepad_expand_raw = false;
            config.gamepad_expand_raw_button = -1;
            config.gamepad_session = -1;
            config.gamepad_session_raw = false;
            config.gamepad_session_raw_button = -1;
            config.gamepad_menu = -1;
            config.gamepad_menu_raw = false;
            config.gamepad_menu_raw_button = -1;
        },
                       false);
        Config::Save();

        Config::Update([](ConfigData& config) {
            config.gamepad_overlay = 4;
            config.gamepad_cycle = 1;
            config.gamepad_expand = 2;
            config.gamepad_session = 3;
            config.gamepad_menu = 4;
        },
                       false);

        Config::Load();
        const ConfigData loaded = Config::Read();

        EXPECT_EQ(loaded.gamepad_overlay, -1);
        EXPECT_EQ(loaded.gamepad_overlay_raw_button, -1);
        EXPECT_EQ(loaded.gamepad_cycle, -1);
        EXPECT_EQ(loaded.gamepad_cycle_raw_button, -1);
        EXPECT_EQ(loaded.gamepad_expand, -1);
        EXPECT_EQ(loaded.gamepad_expand_raw_button, -1);
        EXPECT_EQ(loaded.gamepad_session, -1);
        EXPECT_EQ(loaded.gamepad_session_raw_button, -1);
        EXPECT_EQ(loaded.gamepad_menu, -1);
        EXPECT_EQ(loaded.gamepad_menu_raw_button, -1);
    }

} // namespace
