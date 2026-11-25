#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, ExternalSettingsCloseClearsSensitiveTransientUiState) {
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    SetSettingsTransientState(controller);
    ASSERT_TRUE(HasSettingsTransientState(controller));
    state->ui.showMenu.store(false);
    controller.Update(Config::Read());

    EXPECT_FALSE(HasSettingsTransientState(controller));
}

TEST_F(RmlUiControllerStateTest, SettingsOpenKeepsLegacyOverlayMoveWithoutEnablingDockEditMode) {
    ConfigData config = Config::Read();
    ASSERT_FALSE(config.overlay_layout.containers.empty());

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    state->ui.dashboardLayoutEditMode.store(false);
    RmlUiController controller(state, nullptr);
    controller.Update(config);

    const std::string html = RenderOverlayContainer(controller, config.overlay_layout.containers.front(), false);
    EXPECT_NE(html.find("data-action='overlay-drag'"), std::string::npos);
    EXPECT_EQ(html.find("data-action='overlay-resize'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, LiveTelemetryDoesNotInvalidateOpenSettingsDom) {
    WarpDevice warp;
    if (!warp.Create(1200, 900)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    state->ui.showMenu.store(true);

    state->liveMatch->Update([](MatchState& ms) {
        PlayerData local;
        local.primaryId = "Epic|local";
        local.name = "Local Player";
        local.team = 0;
        local.fetched = true;
        local.mmr = 1200;
        local.playlists["2v2"] = 1200;
        local.playlistTiers["2v2"] = "Diamond I Division I";
        ms.myPrimaryId = local.primaryId;
        ms.roster[local.primaryId] = local;
    });

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1200, 900, 1.0f));
    controller.Update(config);
    controller.Render();
    auto* before = SettingsRoot(controller)->QuerySelector("[data-setting='require_rl_focus']");
    ASSERT_NE(before, nullptr);

    // Ordinary match telemetry changes the game version and overlay, but none
    // of the identity/rank fields rendered by the General settings page. The
    // Settings DOM should therefore remain stable instead of being recreated
    // under the mouse every telemetry tick.
    state->liveMatch->Update([](MatchState& ms) {
        ms.currentMatch.goals += 1;
        ms.currentMatch.shots += 2;
        ms.sessionTotals.goals += 1;
    });
    controller.Update(config);
    controller.Render();
    EXPECT_EQ(SettingsRoot(controller)->QuerySelector("[data-setting='require_rl_focus']"), before);
}

TEST_F(RmlUiControllerStateTest, ChangingUiScaleSelectDoesNotCrashOrCorruptState) {
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);

    ConfigData config = Config::Read();
    config.ui_scale = 1.0f;
    controller.Update(config);

    // Verify switching ui_scale through different options including 0.85
    config.ui_scale = 0.85f;
    EXPECT_NO_THROW(controller.Update(config));
    EXPECT_FLOAT_EQ(GetConfigUiScale(controller), 0.85f);

    config.ui_scale = 0.75f;
    EXPECT_NO_THROW(controller.Update(config));
    EXPECT_FLOAT_EQ(GetConfigUiScale(controller), 0.75f);

    config.ui_scale = 1.0f;
    EXPECT_NO_THROW(controller.Update(config));
    EXPECT_FLOAT_EQ(GetConfigUiScale(controller), 1.0f);
}

// App text size scales the card contents, so the card box must scale with it.
// When it did not, low scales left dead space inside a full-size box (shrinking
// the screen area a card could be dragged into) and high scales overflowed it.
TEST_F(RmlUiControllerStateTest, OverlayCardGeometryFollowsUiScale) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);

    Config::Update([](ConfigData& c) {
        c.overlay_layout.containers.clear();
        OverlayLayout::ContainerConfig container;
        container.id = "scaled";
        container.x = 0.0f;
        container.y = 0.0f;
        container.w = 600.0f;
        container.h = 400.0f;
        container.widgets = {DashboardLayout::WidgetId::SessionStats};
        c.overlay_layout.containers.push_back(container);
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    state->ui.dashboardLayoutEditMode.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));

    const auto renderedSize = [&](float scale) {
        ConfigData config = Config::Read();
        config.ui_scale = scale;
        controller.Update(config);
        controller.Render();
        auto* card = OverlayRoot(controller)->QuerySelector("[data-container='scaled']");
        EXPECT_NE(card, nullptr) << "scale " << scale;
        return card ? Rml::Vector2f(card->GetOffsetWidth(), card->GetOffsetHeight()) : Rml::Vector2f(0.0f, 0.0f);
    };

    // Height auto-fits the visible widgets outside edit mode; both the explicit
    // width and the auto height must track the text size.
    const Rml::Vector2f full = renderedSize(1.0f);
    EXPECT_NEAR(full.x, 600.0f, 1.0f);
    EXPECT_GT(full.y, 1.0f);

    const Rml::Vector2f reducedSize = renderedSize(0.75f);
    EXPECT_NEAR(reducedSize.x, 450.0f, 1.0f);
    EXPECT_NEAR(reducedSize.y, full.y * 0.75f, 2.0f);

    const Rml::Vector2f large = renderedSize(1.5f);
    EXPECT_NEAR(large.x, 900.0f, 1.0f);
    EXPECT_NEAR(large.y, full.y * 1.5f, 2.0f);
    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, DpiChangeRebuildsOverlayGeometry) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);
    Config::Update([](ConfigData& c) {
        c.ui_scale = 1.0f;
        c.overlay_layout.containers.clear();
        OverlayLayout::ContainerConfig container;
        container.id = "dpi-test";
        container.x = 120.0f;
        container.y = 80.0f;
        container.w = 600.0f;
        container.h = 400.0f;
        container.widgets = {DashboardLayout::WidgetId::SessionStats};
        c.overlay_layout.containers.push_back(container);
    },
                   true);
    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    state->ui.dashboardLayoutEditMode.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Render();
    controller.SetDpiScale(1.5f);
    controller.Render();
    auto* card = OverlayRoot(controller)->QuerySelector("[data-container='dpi-test']");
    ASSERT_NE(card, nullptr);
    EXPECT_NEAR(card->GetAbsoluteLeft(), 120.0f, 1.0f);
    EXPECT_NEAR(card->GetAbsoluteTop(), 80.0f, 1.0f);
    DestroyWindow(hwnd);
}

// Picking a scale in the `<select>` writes the new value into the controller's
// config and defers the rest of the work to avoid a use-after-free inside
// WidgetDropDown. The deferred pass still has to push the new ratio into the
// RmlUi context: when it did not, text rendered at the old scale while every
// card position was computed for the new one, throwing cards off screen.
TEST_F(RmlUiControllerStateTest, DeferredScaleChangeUpdatesContextRatio) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));

    ConfigData config = Config::Read();
    config.ui_scale = 1.0f;
    controller.Update(config);
    ASSERT_FLOAT_EQ(RmlContext(controller)->GetDensityIndependentPixelRatio(), 1.0f);

    // Simulate the deferred `<select>` path: the new scale lands in config
    // without anyone calling SetDpiScale.
    Config::Update([](ConfigData& c) { c.ui_scale = 1.5f; }, true);
    config = Config::Read();
    controller.Update(config);
    EXPECT_FLOAT_EQ(RmlContext(controller)->GetDensityIndependentPixelRatio(), 1.5f);

    DestroyWindow(hwnd);
}

// Each font role must restyle only its own text, including the UI-font header
// cells nested over mono columns, and a font that is not installed must fall
// back to the packaged default instead of rendering nothing.
TEST_F(RmlUiControllerStateTest, FontRolesApplyIndependentlyAndFallBack) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP D3D11 device unavailable";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1280, 720, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);

    Config::Update([](ConfigData& c) { c.ResetFonts(); }, true);
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1280, 720, 1.0f));

    Rml::Element* probe = Document(controller)->AppendChild(Document(controller)->CreateElement("div"));
    probe->SetInnerRML("<div id='fp-body'>a</div><div id='fp-mono' class='mono'>1</div>"
                       "<div class='match-header'><div id='fp-header' class='match-score'>b</div></div>"
                       "<div id='fp-title' class='card-title'>c</div>");
    auto family = [&](const char* id) {
        RmlContext(controller)->Update();
        Rml::Element* element = Document(controller)->GetElementById(id);
        return element ? Rml::StringUtilities::ToLower(element->GetComputedValues().font_family()) : std::string("<missing>");
    };
    auto apply = [&](std::string ui, std::string mono, std::string display) {
        Config::Update([&](ConfigData& c) {
            c.font_ui = ui;
            c.font_mono = mono;
            c.font_display = display;
        },
                       true);
        controller.Update(Config::Read());
    };

    EXPECT_EQ(family("fp-body"), "inter");
    EXPECT_EQ(family("fp-mono"), "jetbrains mono");
    EXPECT_EQ(family("fp-header"), "inter");
    EXPECT_EQ(family("fp-title"), "russo one");

    apply("", "Russo One", "");
    EXPECT_EQ(family("fp-body"), "inter");
    EXPECT_EQ(family("fp-mono"), "russo one");
    EXPECT_EQ(family("fp-header"), "inter");
    EXPECT_EQ(family("fp-title"), "russo one");

    apply("Arial", "", "JetBrains Mono");
    EXPECT_EQ(family("fp-body"), "omnistats-system arial");
    EXPECT_EQ(family("fp-mono"), "jetbrains mono");
    EXPECT_EQ(family("fp-header"), "omnistats-system arial");
    EXPECT_EQ(family("fp-title"), "jetbrains mono");
    state->notes.PublishLoaded(false, {}, {});
    OpenNote(controller, "Steam|font-note", "Font Note");
    controller.Render();
    RmlContext(controller)->Update();
    ASSERT_NE(NoteDocument(controller), nullptr);
    Rml::Element* noteName = NoteDocument(controller)->QuerySelector(".player-note-name");
    ASSERT_NE(noteName, nullptr);
    EXPECT_EQ(Rml::StringUtilities::ToLower(NoteDocument(controller)->GetComputedValues().font_family()), "omnistats-system arial");
    EXPECT_EQ(Rml::StringUtilities::ToLower(noteName->GetComputedValues().font_family()), "jetbrains mono");

    apply("No Such Font OmniStats", "", "");
    EXPECT_EQ(family("fp-body"), "inter");

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, SettingsWindowStaysInsideClientAreaAfterResize) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    controller.Resize(1223, 1032, 1.0f);
    controller.Update(Config::Read());
    controller.Render();

    auto* window = SettingsRoot(controller)->QuerySelector(".settings-window");
    ASSERT_NE(window, nullptr);
    EXPECT_GE(window->GetAbsoluteOffset().x, 0.0f);
    EXPECT_LE(window->GetAbsoluteOffset().x + window->GetOffsetWidth(), 1223.0f);
    EXPECT_LE(window->GetAbsoluteOffset().y + window->GetOffsetHeight(), 1032.0f);

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, LongSettingsPagesScrollAndPreservePositionAfterChanges) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }

    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    ASSERT_TRUE(hwnd != nullptr);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));

    // The footer must remain reachable on every page, including Cards.
    state->ui.showMenu.store(true);
    controller.Update(Config::Read());
    controller.Render();

    // 2. Iterate through all settings pages
    for (int page = 0; page < 6; ++page) {
        SetSettingsPage(controller, page);
        controller.Update(Config::Read());
        controller.Render();
        auto* root = SettingsRoot(controller);
        ASSERT_NE(root, nullptr);
        auto* body = root->QuerySelector(".settings-page");
        auto* footer = root->QuerySelector(".settings-footer");
        ASSERT_NE(body, nullptr);
        ASSERT_NE(footer, nullptr);
        EXPECT_GT(body->GetClientHeight(), 0.0f);
        EXPECT_LE(footer->GetAbsoluteOffset().y + footer->GetOffsetHeight(), 1080.0f);
        if (page == 0) {
            ASSERT_GT(body->GetScrollHeight(), body->GetClientHeight());
            body->SetScrollTop(220.0f);
            const float offset = body->GetScrollTop();
            ASSERT_GT(offset, 0.0f);
            RefreshSettings(controller);
            body = SettingsRoot(controller)->QuerySelector(".settings-page");
            ASSERT_NE(body, nullptr);
            EXPECT_FLOAT_EQ(body->GetScrollTop(), offset);
        }
        if (page == 4) {
            auto* token = root->QuerySelector("[data-setting='ballchasing_token']");
            ASSERT_NE(token, nullptr);
            ASSERT_TRUE(token->Focus());
            const std::string value = "qa-token-with-many-characters";
            for (char ch : value) {
                controller.ProcessWindowMessage(hwnd, WM_CHAR, static_cast<WPARAM>(ch), 0);
                state->liveMatch->Update([](MatchState&) {});
                // Mirror Overlay::RunLoop: the render loop reports the config
                // revision it observed, so a local edit is not mistaken for an
                // external change that would rebuild the focused Settings DOM.
                controller.Update(Config::Read(), true, Config::Revision());
                controller.Render();
            }
            EXPECT_EQ(Config::Read().ballchasing_token, value);
            EXPECT_EQ(SettingsRoot(controller)->QuerySelector("[data-setting='ballchasing_token']"), token);
        }
    }

    // 3. Close settings (like pressing F5 or Escape again)
    state->ui.showMenu.store(false);
    controller.Update(Config::Read());
    controller.Render();

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, SettingsRefreshPreservesSelectOptionsAndUpdatesSelection) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }

    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    ASSERT_TRUE(hwnd != nullptr);
    Config::Update([](ConfigData& c) { c.season_reminder_days = 3; }, true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    state->ui.showMenu.store(true);
    controller.Update(Config::Read());
    controller.Render();

    auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(
        SettingsRoot(controller)->QuerySelector("[data-setting='season_reminder_days']"));
    ASSERT_NE(select, nullptr);
    ASSERT_EQ(select->GetNumOptions(), 4);
    EXPECT_EQ(select->GetValue(), "3");

    const std::vector<int> selections = {7, 14, 0, 3, 7};
    const std::vector<std::string> optionValues = {"0", "3", "7", "14"};
    for (int days : selections) {
        Config::Update([days](ConfigData& c) { c.season_reminder_days = days; }, true);
        controller.Update(Config::Read(), true, Config::Revision());
        RefreshSettings(controller);
        RefreshSettings(controller);
        auto* current = dynamic_cast<Rml::ElementFormControlSelect*>(
            SettingsRoot(controller)->QuerySelector("[data-setting='season_reminder_days']"));
        ASSERT_EQ(current, select);
        ASSERT_EQ(current->GetNumOptions(), static_cast<int>(optionValues.size()));
        EXPECT_EQ(current->GetValue(), std::to_string(days));
        for (int i = 0; i < current->GetNumOptions(); ++i) {
            ASSERT_NE(current->GetOption(i), nullptr);
            EXPECT_EQ(current->GetOption(i)->GetAttribute<Rml::String>("value", ""), optionValues[i]);
        }
    }
    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, SettingsDisclosureRetainsFontOptionsAndPageLabels) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP device creation not available in this environment.";
    }
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr,
                                GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);
    Config::Update([](ConfigData& c) { c.font_ui.clear(); c.ui_scale = 1.0f; }, true);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    const auto click = [&](Rml::Element* element) {
        const auto center = element->GetAbsoluteOffset() +
                            Rml::Vector2f(element->GetOffsetWidth() * 0.5f, element->GetOffsetHeight() * 0.5f);
        const LPARAM position = MAKELPARAM(static_cast<int>(center.x), static_cast<int>(center.y));
        controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, 0, position);
        controller.ProcessWindowMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
        controller.ProcessWindowMessage(hwnd, WM_LBUTTONUP, 0, position);
        controller.Render();
    };
    auto* appearance = SettingsRoot(controller)->QuerySelector("[data-action='settings-page'][data-page='2']");
    ASSERT_NE(appearance, nullptr);
    click(appearance);

    auto* disclosure = SettingsRoot(controller)->QuerySelector("[data-action='toggle-advanced-appearance']");
    ASSERT_NE(disclosure, nullptr);
    disclosure->ScrollIntoView(false);
    controller.Render();
    click(disclosure);
    controller.Update(Config::Read(), false);
    controller.Update(Config::Read(), false);
    controller.Render();
    auto* page = SettingsRoot(controller)->QuerySelector(".settings-page");
    auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(
        page->QuerySelector("[data-setting='font_ui']"));
    ASSERT_NE(select, nullptr);
    ASSERT_GE(select->GetNumOptions(), 4);
    EXPECT_EQ(select->GetOption(0)->GetInnerRML(), "Default");
    EXPECT_EQ(select->GetOption(1)->GetAttribute<Rml::String>("value", ""), "Inter");
    EXPECT_FALSE(select->GetOption(1)->GetInnerRML().empty());
    EXPECT_EQ(select->GetValue(), "");

    select->ScrollIntoView(false);
    controller.Render();
    click(select);
    ASSERT_TRUE(select->IsSelectBoxVisible());
    auto* inter = select->GetOption(1);
    ASSERT_NE(inter, nullptr);
    click(inter);
    EXPECT_EQ(Config::Read().font_ui, "Inter");
    controller.Update(Config::Read(), false);
    controller.Update(Config::Read(), false);
    controller.Render();
    select = dynamic_cast<Rml::ElementFormControlSelect*>(
        SettingsRoot(controller)->QuerySelector("[data-setting='font_ui']"));
    ASSERT_NE(select, nullptr);
    EXPECT_EQ(select->GetValue(), "Inter");
    EXPECT_FALSE(select->GetOption(1)->GetInnerRML().empty());

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, SettingsDataShowsDetailedStatsRecordedSummary) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    RmlUiController controller(state, db);

    EXPECT_NE(RenderDataSettings(controller).find("Detailed stats not yet recorded"), std::string::npos);

    MatchSaveSnapshot snap;
    snap.arenaName = "DFH Stadium";
    snap.matchGuid = "settings-data-guid";
    snap.playlistId = 11;
    snap.myTeam = 0;
    snap.winnerTeam = 0;
    snap.validResult = true;
    snap.score[0] = 3;
    snap.score[1] = 1;
    snap.myPrimaryId = "Steam|settings-data";
    snap.endedAtUnixMs = 1'791'936'000'000; // 2026-10-14 00:00:00 UTC
    snap.roster["Steam|settings-data"] =
        PlayerData{.primaryId = "Steam|settings-data", .name = "Player", .team = 0, .mmr = 1200, .goals = 2};
    db->AsyncSaveMatch(snap);
    db->AsyncSetSetting("settings_data_barrier", "done");
    for (int i = 0; i < 200 && db->GetSetting("settings_data_barrier", "") != "done"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(db->GetSetting("settings_data_barrier", ""), "done");

    const std::string rendered = RenderDataSettings(controller);
    EXPECT_NE(rendered.find("Detailed stats recorded since 2026-10-14 · 1 matches"), std::string::npos) << rendered;
}

TEST_F(RmlUiControllerStateTest, DisplaySettingsPersistsReplayVisibilityWithoutEditMode) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.overlay_layout = OverlayLayout::DefaultOverlayLayout();
        c.show_lobby_ranks_overlay = true;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    auto* details = SettingsRoot(controller)->QuerySelector("[data-action='toggle-card-details']");
    ASSERT_NE(details, nullptr);
    Click(controller, details);
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* root = SettingsRoot(controller);
    ASSERT_NE(root, nullptr);

    auto* hideReplayCheckbox = root->QuerySelector("[data-setting='widget_vis_hide_replay:lobby-ranks']");
    ASSERT_NE(hideReplayCheckbox, nullptr);

    hideReplayCheckbox->Click();
    controller.Update(Config::Read(), true, Config::Revision());
    controller.Render();

    const ConfigData updated = Config::Read();
    const auto vis = OverlayLayout::GetWidgetVisibility(updated.overlay_layout, DashboardLayout::WidgetId::LobbyRanks);
    EXPECT_TRUE(vis.hideDuringReplay);
    EXPECT_FALSE(state->ui.dashboardLayoutEditMode.load());
}

TEST_F(RmlUiControllerStateTest, CardTextSizeScalesOnlyTheSelectedCardAcrossDisplayModes) {
    using Id = DashboardLayout::WidgetId;
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.ui_scale = 1.0f;
        c.card_text_percent.fill(100);
        c.show_lobby_ranks_overlay = true;
        c.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
        c.LobbyRankColumn(MmrCategory::Tourny) = true;
        c.show_session_record = true;
        OverlayLayout::ContainerConfig stack;
        stack.id = "text-size-stack";
        stack.x = 670.0f;
        stack.y = 24.0f;
        stack.widgets = {Id::LobbyRanks, Id::SessionStats};
        stack.visibility.mode = OverlayLayout::Visibility::Always;
        c.overlay_layout.containers = {stack};
        c.overlay_layout.disabledWidgets.clear();
        c.dashboard_layout.widgets = {
            {Id::LobbyRanks, DashboardLayout::Zone::Top, 0},
            {Id::SessionStats, DashboardLayout::Zone::Top, 1}};
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    state->ui.showOverlay.store(true);
    state->liveMatch->Update([](MatchState& match) {
        match.inMatch = true;
        PlayerData player;
        player.primaryId = "Steam|76561198000000001|0";
        player.name = "Tournament player";
        player.playlists = {{"2v2", 1250}, {"t", 1010}};
        player.playlistTiers = {{"2v2", "Champion II Div II"}, {"t", "Diamond II Div III"}};
        player.fetched = true;
        match.roster.emplace(player.primaryId, std::move(player));
    });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    const auto lobbyText = [](Rml::Element* root) { return root->QuerySelector(".lobby-rank-cell"); };
    const auto sessionText = [](Rml::Element* root) {
        return root->QuerySelector("[data-live-widget='session'] .metric-label");
    };
    auto* overlay = OverlayRoot(controller);
    ASSERT_NE(lobbyText(overlay), nullptr);
    ASSERT_NE(sessionText(overlay), nullptr);
    const float lobbyFont = lobbyText(overlay)->GetComputedValues().font_size();
    const float lobbyColumn = lobbyText(overlay)->GetOffsetWidth();
    const float sessionFont = sessionText(overlay)->GetComputedValues().font_size();

    const auto capture = [&](const char* name) {
        if (const char* dir = std::getenv("OMNISTATS_CAPTURE_DIR"); dir && *dir) {
            std::filesystem::create_directories(dir);
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            warp.context->ClearRenderTargetView(warp.target.Get(), clear);
            controller.Render();
            std::string error;
            EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1280, 900, 0.0f,
                                                                (std::filesystem::path(dir) / name).wstring(), error))
                << error;
        }
    };
    auto* details = SettingsRoot(controller)->QuerySelector("[data-action='toggle-card-details']");
    ASSERT_NE(details, nullptr);
    details->DispatchEvent("click", {});
    if (std::getenv("OMNISTATS_CAPTURE_DIR")) {
        auto* page = SettingsRoot(controller)->QuerySelector(".settings-page");
        details = page->QuerySelector("[data-action='toggle-card-details']");
        details->ScrollIntoView(true);
        page->SetScrollTop(std::max(0.0f, page->GetScrollTop() - 80.0f));
    }
    capture("card-options-display.png");

    auto* appearance = SettingsRoot(controller)->QuerySelector("[data-action='settings-page'][data-page='2']");
    ASSERT_NE(appearance, nullptr);
    appearance->DispatchEvent("click", {});
    const auto select = [&](const char* selector) {
        return dynamic_cast<Rml::ElementFormControl*>(SettingsRoot(controller)->QuerySelector(selector));
    };
    auto* card = select("[data-setting='card_text_widget']");
    ASSERT_NE(card, nullptr);
    card->SetValue("lobby-ranks");
    controller.Update(Config::Read(), false);
    auto* scale = select("[data-setting='widget_text_percent']");
    ASSERT_NE(scale, nullptr);
    scale->SetValue("150");
    controller.Update(Config::Read(), true, Config::Revision());
    controller.Render();

    ASSERT_NE(lobbyText(overlay), nullptr);
    ASSERT_NE(sessionText(overlay), nullptr);
    EXPECT_NEAR(lobbyText(overlay)->GetComputedValues().font_size(), lobbyFont * 1.5f, 0.01f);
    EXPECT_GT(lobbyText(overlay)->GetOffsetWidth(), lobbyColumn);
    EXPECT_FLOAT_EQ(sessionText(overlay)->GetComputedValues().font_size(), sessionFont);

    capture("card-text-size-appearance.png");
    card = select("[data-setting='card_text_widget']");
    ASSERT_NE(card, nullptr);
    card->SetValue("session");
    controller.Update(Config::Read(), false);
    scale = select("[data-setting='widget_text_percent']");
    ASSERT_NE(scale, nullptr);
    EXPECT_EQ(scale->GetValue(), "100");

    Config::Save();
    Config::Load();
    auto dashboard = Config::Read();
    dashboard.second_monitor_mode = true;
    state->ui.showMenu.store(false);
    controller.Update(dashboard);
    controller.Render();
    ASSERT_NE(lobbyText(DashboardRoot(controller)), nullptr);
    EXPECT_NEAR(lobbyText(DashboardRoot(controller))->GetComputedValues().font_size(), lobbyFont * 1.5f, 0.01f);
}
