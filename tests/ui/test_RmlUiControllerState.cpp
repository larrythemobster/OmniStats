#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, ExpiredToastClearsInsteadOfBecomingPermanent) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);

    SeedExpiredToast(controller);
    ASSERT_TRUE(HasToastMessage(controller));

    RebuildVisibleUi(controller);

    EXPECT_FALSE(HasToastMessage(controller));
    EXPECT_EQ(ToastDeadline(controller), 0);
}

TEST_F(RmlUiControllerStateTest, HeadlessControllerDoesNotClearForeignRmlInterfaces) {
    RmlSystemInterfaceWin32 sentinel;
    Rml::SystemInterface* previous = Rml::GetSystemInterface();
    Rml::SetSystemInterface(&sentinel);
    {
        auto state = std::make_shared<SessionState>();
        RmlUiController controller(state, nullptr);
    }
    EXPECT_EQ(Rml::GetSystemInterface(), &sentinel);
    Rml::SetSystemInterface(previous);
}

TEST_F(RmlUiControllerStateTest, DynamicRmlTextEscapesMarkupAndSanitizesInvalidUtf8) {
    const std::string input = std::string("<&\"'>") + "\xC0\xAF" + "ok" + "\xF0\x9F\x98\x80" + "\xED\xA0\x80";
    const std::string escaped = EscapeText(input);

    EXPECT_EQ(escaped, "&lt;&amp;&quot;&#39;&gt;\xEF\xBF\xBD\xEF\xBF\xBDok\xF0\x9F\x98\x80\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_F(RmlUiControllerStateTest, EffectivePrimaryTriggersOneDeduplicatedDbRefresh) {
    auto state = std::make_shared<SessionState>();
    int lifetimeCalls = 0;
    int refreshCalls = 0;
    std::string lifetimePrimary;
    std::string refreshPrimary;
    DatabaseHooks hooks;
    hooks.onAsyncGetLifetimeMmr = [&](const std::string& primary, const std::string&) {
        ++lifetimeCalls;
        lifetimePrimary = primary;
    };
    hooks.onAsyncRefreshDbStats = [&](const std::string& primary) {
        ++refreshCalls;
        refreshPrimary = primary;
    };
    auto db = std::make_shared<DatabaseManager>(state, std::move(hooks));

    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|TEST123"; }, true);
    ConfigData config = Config::Read();
    RmlUiController controller(state, db);

    controller.Update(config);
    EXPECT_EQ(lifetimeCalls, 1);
    EXPECT_EQ(refreshCalls, 1);
    EXPECT_EQ(lifetimePrimary, "Steam|TEST123");
    EXPECT_EQ(refreshPrimary, "Steam|TEST123");

    controller.Update(config);
    EXPECT_EQ(lifetimeCalls, 1);
    EXPECT_EQ(refreshCalls, 1);
}

TEST_F(RmlUiControllerStateTest, InvalidDpiScaleFallsBackSafely) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);

    controller.SetDpiScale(std::numeric_limits<float>::quiet_NaN());
    EXPECT_FLOAT_EQ(DpiScale(controller), 1.0f);
    controller.SetDpiScale(-10.0f);
    EXPECT_FLOAT_EQ(DpiScale(controller), 1.0f);
    controller.SetDpiScale(1.5f);
    EXPECT_FLOAT_EQ(DpiScale(controller), 1.5f);
}

TEST_F(RmlUiControllerStateTest, DependentCardDetailControlsStayDisabledWithParentCards) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ConfigData config = Config::Read();
    config.show_previous_games_summary = false;
    config.show_gamemode_breakdown = false;
    controller.Update(config);

    const std::string html = RenderCardSettings(controller);
    EXPECT_NE(html.find("data-setting='previous_games_limit' disabled='disabled'"), std::string::npos);
    EXPECT_NE(html.find("data-setting='gamemode_breakdown_scope' disabled='disabled'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, HotReloadAppliesEditedStylesheet) {
    WarpDevice warp;
    if (!warp.Create(800, 600)) GTEST_SKIP() << "WARP not available.";
    const std::filesystem::path source = std::filesystem::path(OMNISTATS_SOURCE_DIR) / "resources" / "rml";
    const std::filesystem::path dev = std::filesystem::temp_directory_path() / "omnistats_rml_dev";
    std::filesystem::remove_all(dev);
    std::filesystem::copy(source, dev);
    SetEnvironmentVariableA("OMNISTATS_RML_DIR", dev.string().c_str());

    auto state = std::make_shared<SessionState>();
    {
        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 800, 600, 1.0f));
        controller.Update(Config::Read(), false);
        controller.Render();
        EXPECT_FLOAT_EQ(Document(controller)->GetProperty<float>("opacity"), 1.0f);

        std::ofstream(dev / "omnistats.rcss", std::ios::app) << "\nbody { opacity: 0.5; }\n";
        ReloadUi(controller);
        controller.Render();
        EXPECT_FLOAT_EQ(Document(controller)->GetProperty<float>("opacity"), 0.5f);
    }
    SetEnvironmentVariableA("OMNISTATS_RML_DIR", nullptr);
    std::filesystem::remove_all(dev);
}

TEST_F(RmlUiControllerStateTest, DeferredSelectRebuildSurvivesBlockedUpdateUntilInteractionEnds) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* root = SettingsRoot(controller);
    ASSERT_NE(root, nullptr);
    ASSERT_EQ(root->QuerySelector("[data-setting='widget_vis_event:live-roster']"), nullptr);

    auto* modeSelect = dynamic_cast<Rml::ElementFormControl*>(
        root->QuerySelector("[data-setting='widget_vis_mode:live-roster']"));
    ASSERT_NE(modeSelect, nullptr);
    modeSelect->SetValue("after_event");
    EXPECT_TRUE(HasDeferredSelectRebuild(controller));

    SimulateOverlayDragStart(controller, "main_stack", 1490.0f, 0.0f);
    controller.Update(Config::Read(), true, Config::Revision());
    EXPECT_TRUE(HasDeferredSelectRebuild(controller));
    EXPECT_EQ(SettingsRoot(controller)->QuerySelector("[data-setting='widget_vis_event:live-roster']"), nullptr);

    ClearOverlayDrag(controller);
    controller.Update(Config::Read(), false, Config::Revision());
    controller.Render();
    EXPECT_FALSE(HasDeferredSelectRebuild(controller));
    EXPECT_NE(SettingsRoot(controller)->QuerySelector("[data-setting='widget_vis_event:live-roster']"), nullptr);
    EXPECT_NE(SettingsRoot(controller)->QuerySelector("[data-setting='widget_vis_seconds:live-roster']"), nullptr);
}
