#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, ReplaySavingKeepsInlineLegacySaveReplayBindControl) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.auto_save_replays = true;
    config.key_save_replay = 0x52;

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    std::string html = RenderIntegrationSettings(controller);
    const std::string bindTarget = SaveReplayBindTarget();
    EXPECT_NE(html.find("data-action='capture-bind' data-bind='" + bindTarget + "'"), std::string::npos);
    EXPECT_NE(html.find("data-action='clear-bind' data-bind='" + bindTarget + "'"), std::string::npos);

    BeginSaveReplayKeyCapture(controller);
    html = RenderIntegrationSettings(controller);
    EXPECT_NE(html.find("Press a key..."), std::string::npos);
    EXPECT_NE(html.find("data-action='cancel-bind' data-bind='" + bindTarget + "'"), std::string::npos);

    config.auto_save_replays = false;
    controller.Update(config);
    html = RenderIntegrationSettings(controller);
    EXPECT_EQ(html.find("Save Replay Bind"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, MenuKeyCaptureUpdatesConfigAndControllerSnapshot) {
    auto state = std::make_shared<SessionState>();
    Config::Update([](ConfigData& c) { c.key_menu = 0x41; });
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    CaptureMenuKey(controller, state, 0x42);

    EXPECT_EQ(Config::Read().key_menu, 0x42);
    EXPECT_EQ(ConfiguredMenuKey(controller), 0x42);
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
}

TEST_F(RmlUiControllerStateTest, CapturedKeyChangesOnlySelectedShortcut) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const ConfigData before = Config::Read();

    BeginOverlayKeyCapture(controller);
    EXPECT_TRUE(state->ui.inputCaptureActive.load());
    state->ui.lastKeyboardKeyPressed.store(VK_F10);
    controller.Update(Config::Read());

    const ConfigData after = Config::Read();
    EXPECT_EQ(after.key_overlay, VK_F10);
    EXPECT_EQ(after.key_menu, before.key_menu);
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
}

TEST_F(RmlUiControllerStateTest, EscapeCancelsShortcutCaptureWithoutChangingBinding) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const int previous = Config::Read().key_overlay;

    BeginOverlayKeyCapture(controller);
    state->ui.lastKeyboardKeyPressed.store(VK_ESCAPE);
    controller.Update(Config::Read());

    EXPECT_EQ(Config::Read().key_overlay, previous);
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
}

TEST_F(RmlUiControllerStateTest, ClosingSettingsCancelsPendingShortcutWithoutChangingBinding) {
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    const int previous = Config::Read().key_overlay;

    BeginOverlayKeyCapture(controller);
    ASSERT_TRUE(state->ui.inputCaptureActive.load());
    state->ui.showMenu.store(false);
    state->ui.lastKeyboardKeyPressed.store(VK_F10);
    controller.Update(Config::Read());

    EXPECT_FALSE(state->ui.inputCaptureActive.load());
    EXPECT_EQ(Config::Read().key_overlay, previous);
}

TEST_F(RmlUiControllerStateTest, RawControllerFallbackOnlyUpdatesSelectedBinding) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const ConfigData before = Config::Read();

    state->ui.controllerIsGameController.store(false);
    BeginCycleControllerCapture(controller);
    state->ui.lastRawControllerButtonPressed.store(9);
    controller.Update(Config::Read());

    const ConfigData after = Config::Read();
    EXPECT_TRUE(after.gamepad_cycle_raw);
    EXPECT_EQ(after.gamepad_cycle_raw_button, 9);
    EXPECT_EQ(after.gamepad_overlay, before.gamepad_overlay);
    EXPECT_EQ(after.gamepad_overlay_raw, before.gamepad_overlay_raw);
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
}

TEST_F(RmlUiControllerStateTest, UncapturedKeyboardAndGamepadInputDoNotInvalidateSettings) {
    WarpDevice warp;
    if (!warp.Create(1200, 900)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    const ConfigData config = Config::Read();
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1200, 900, 1.0f));
    controller.Update(config);
    controller.Render();
    auto* before = SettingsRoot(controller)->QuerySelector("[data-setting='require_rl_focus']");
    ASSERT_NE(before, nullptr);

    state->ui.lastKeyboardKeyPressed.store('Q');
    state->ui.lastControllerButtonPressed.store(3);
    state->ui.lastRawControllerButtonPressed.store(8);
    controller.Update(config, false);
    controller.Render();
    EXPECT_EQ(SettingsRoot(controller)->QuerySelector("[data-setting='require_rl_focus']"), before);

    state->ui.lastKeyboardKeyPressed.store('Z');
    state->ui.lastControllerButtonPressed.store(12);
    state->ui.lastRawControllerButtonPressed.store(20);
    controller.Update(config, false);
    controller.Render();
    EXPECT_EQ(SettingsRoot(controller)->QuerySelector("[data-setting='require_rl_focus']"), before);
}

TEST_F(RmlUiControllerStateTest, EscapeNeutralizesDataBindingBraces) {
    EXPECT_EQ(EscapeText("{{match_saves}}"), "&#123;&#123;match_saves&#125;&#125;");
    EXPECT_EQ(EscapeText("<b>&'\""), "&lt;b&gt;&amp;&#39;&quot;");
}
