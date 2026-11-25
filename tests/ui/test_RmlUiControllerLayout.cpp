#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, OverlayRenderSelfRepairsPersistedUndersizedContainer) {
    Config::Update([](ConfigData& c) {
        for (auto& container : c.overlay_layout.containers) {
            if (container.id == "main_stack") {
                container.w = 50.0f;
                container.h = 50.0f;
                break;
            }
        }
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    RmlUiController controller(state, nullptr);
    const ConfigData config = Config::Read();
    controller.Update(config);

    const auto it = std::find_if(config.overlay_layout.containers.begin(), config.overlay_layout.containers.end(), [](const auto& container) {
        return container.id == "main_stack";
    });
    ASSERT_NE(it, config.overlay_layout.containers.end());
    EXPECT_FALSE(RenderOverlayContainer(controller, *it).empty());

    const ConfigData repaired = Config::Read();
    const auto repairedIt = std::find_if(repaired.overlay_layout.containers.begin(), repaired.overlay_layout.containers.end(), [](const auto& container) {
        return container.id == "main_stack";
    });
    ASSERT_NE(repairedIt, repaired.overlay_layout.containers.end());
    EXPECT_GE(repairedIt->w, 410.0f);
    EXPECT_GE(repairedIt->h, 120.0f);
}

TEST_F(RmlUiControllerStateTest, DashboardAndOverlaySessionCardsKeepCompactLegacySemantics) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.wins = 2;
        ms.sessionTotals.losses = 1;
        ms.sessionTotals.shots = 4;
        ms.sessionTotals.shotsTotal = 9;
    });

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    const std::string dashboard = RenderSessionWidget(controller, true);
    const std::string overlay = RenderSessionWidget(controller, false);
    const std::string sessionView = RenderSessionView(controller);
    const std::string unusedFullBranch = RenderFullSession(controller);
    EXPECT_EQ(dashboard.find("Shots"), std::string::npos);
    EXPECT_EQ(overlay.find("Shots"), std::string::npos);
    EXPECT_EQ(sessionView.find("Shots"), std::string::npos);
    EXPECT_NE(unusedFullBranch.find("<div class='label'>Shots</div>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, DashboardWidgetsSpanFullWidthWithoutCollapsing) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP not available.";
    }
    auto state = std::make_shared<SessionState>();
    Config::Update([](ConfigData& c) {
        c.second_monitor_mode = true;
    },
                   true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, device.Get(), context.Get(), 1223, 1032, 1.0f));
    controller.Update(Config::Read());
    ASSERT_NE(Document(controller), nullptr);
    controller.Render();

    Rml::ElementList widgets;
    Document(controller)->QuerySelectorAll(widgets, ".dashboard-widget");
    EXPECT_GT(widgets.size(), 0u);
    for (auto* widget : widgets) {
        EXPECT_GT(widget->GetOffsetWidth(), 200.0f);
    }
}

TEST_F(RmlUiControllerStateTest, DashboardGamemodeBreakdownKeepsLegacyScopeSelector) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.gamemode_breakdown_scope = "all_time";

    RmlUiController controller(state, nullptr);
    controller.Update(config);

    const std::string dashboard = RenderGamemodeWidget(controller, true);
    const std::string overlay = RenderGamemodeWidget(controller, false);
    EXPECT_NE(dashboard.find("data-setting='gamemode_breakdown_scope'"), std::string::npos);
    EXPECT_NE(dashboard.find("value='all_time' selected='selected'"), std::string::npos);
    EXPECT_EQ(overlay.find("data-setting='gamemode_breakdown_scope'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, ActiveOverlayDragPreservesCoordinatesAndPreventsRebuilds) {
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ConfigData config = Config::Read();
    ASSERT_FALSE(config.overlay_layout.containers.empty());
    controller.Update(config);

    const std::string containerId = config.overlay_layout.containers.front().id;
    SimulateOverlayDragStart(controller, containerId, 550.0f, 350.0f);

    EXPECT_TRUE(controller.WantsInteraction());

    // Polling replaces persisted config without overwriting the gesture draft.
    config.overlay_layout.containers.front().x = 25.0f;
    config.overlay_layout.containers.front().y = 35.0f;
    controller.Update(config);

    const auto [curX, curY] = GetContainerPos(controller, containerId);
    EXPECT_EQ(curX, 25.0f);
    EXPECT_EQ(curY, 35.0f);
    const auto [dragX, dragY] = GetDragPos(controller);
    EXPECT_EQ(dragX, 550.0f);
    EXPECT_EQ(dragY, 350.0f);
}

TEST_F(RmlUiControllerStateTest, OverlayContainerResizeHandleVisibleInEditMode) {
    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    state->ui.dashboardLayoutEditMode.store(true);
    RmlUiController controller(state, nullptr);

    OverlayLayout::ContainerConfig container;
    container.id = "lobby_ranks";
    container.widgets = {DashboardLayout::WidgetId::LobbyRanks};

    ConfigData config = Config::Read();
    config.show_lobby_ranks_overlay = true;
    controller.Update(config);

    const std::string html = RenderOverlayContainer(controller, container, true);
    EXPECT_NE(html.find("data-action='overlay-resize'"), std::string::npos);
}

// A position saved at one text size must not park a card off screen at another.
TEST_F(RmlUiControllerStateTest, OverlayCardStaysOnScreenAfterScaleIncrease) {
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
        container.id = "edge";
        container.x = 1300.0f;
        container.y = 900.0f;
        container.w = 600.0f;
        container.h = 160.0f;
        container.widgets = {DashboardLayout::WidgetId::SessionStats};
        c.overlay_layout.containers.push_back(container);
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));

    ConfigData config = Config::Read();
    config.ui_scale = 1.5f;
    controller.Update(config);
    controller.Render();

    auto* card = OverlayRoot(controller)->QuerySelector("[data-container='edge']");
    ASSERT_NE(card, nullptr);
    const Rml::Vector2f offset = card->GetAbsoluteOffset();
    EXPECT_LE(offset.x + card->GetOffsetWidth(), 1920.0f);
    EXPECT_LT(offset.y, 1080.0f);

    DestroyWindow(hwnd);
}

// Live telemetry must not replace the overlay DOM between mousedown and
// mouseup: RmlUi only emits `click` when both land on the same element, so a
// rebuild in between silently swallowed every overlay control click.
TEST_F(RmlUiControllerStateTest, OverlayControlsStayClickableWhileTelemetryUpdates) {
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

    Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    state->ui.dashboardLayoutEditMode.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    auto* toggle = OverlayRoot(controller)->QuerySelector("[data-action='overlay-toggle-toolbox']");
    ASSERT_NE(toggle, nullptr);
    const Rml::Vector2f center = toggle->GetAbsoluteOffset() +
                                 Rml::Vector2f(toggle->GetOffsetWidth() * 0.5f, toggle->GetOffsetHeight() * 0.5f);
    const LPARAM position = MAKELPARAM(static_cast<int>(center.x), static_cast<int>(center.y));

    controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, 0, position);
    controller.Render();
    controller.ProcessWindowMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);

    // A telemetry tick between press and release must not invalidate the target.
    state->liveMatch->Update([](MatchState& ms) {
        ms.currentMatch.goals += 1;
    });
    controller.Update(Config::Read());
    controller.Render();
    EXPECT_EQ(OverlayRoot(controller)->QuerySelector("[data-action='overlay-toggle-toolbox']"), toggle);

    controller.ProcessWindowMessage(hwnd, WM_LBUTTONUP, 0, position);
    controller.Render();
    EXPECT_TRUE(Config::Read().overlay_layout.toolboxOpen) << "overlay toolbox click was swallowed";
    EXPECT_TRUE(controller.HasPendingInteractionUpdate()) << "click should bypass the data update timer";
    controller.Update(Config::Read(), false);
    EXPECT_FALSE(controller.HasPendingInteractionUpdate());

    DestroyWindow(hwnd);
}

// Switching window modes changes the client size. Settings must be re-centered
// for the new size instead of staying positioned for the old one, which clipped
// every control off the right edge of the dashboard window.
// Outside edit mode a container auto-fits its visible widgets. Dragging must be
// clamped to that rendered height, not the taller saved/default geometry, or
// short cards cannot be moved into the lower part of the screen.
TEST_F(RmlUiControllerStateTest, ShortOverlayCardCanBeDraggedToTheBottomOfTheScreen) {
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
        c.show_lobby_ranks_overlay = true;
        c.overlay_layout.containers.clear();
        OverlayLayout::ContainerConfig lobby;
        lobby.id = "lobby_ranks";
        lobby.x = 20.0f;
        lobby.y = 60.0f;
        lobby.w = 0.0f;
        lobby.h = 0.0f;
        lobby.widgets = {DashboardLayout::WidgetId::LobbyRanks};
        c.overlay_layout.containers.push_back(lobby);
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    state->liveMatch->Update([](MatchState& ms) {
        PlayerData player;
        player.primaryId = "Epic|drag";
        player.name = "Dragged Player";
        player.fetched = true;
        ms.roster[player.primaryId] = player;
    });

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    auto* card = OverlayRoot(controller)->QuerySelector("[data-container='lobby_ranks']");
    ASSERT_NE(card, nullptr);
    const float renderedHeight = card->GetOffsetHeight();
    ASSERT_GT(renderedHeight, 1.0f);
    // The saved geometry resolves much taller than the auto-fitted card.
    ASSERT_LT(renderedHeight, 300.0f);

    // Press near the card's top-left so the press cannot land on the Settings window.
    const Rml::Vector2f grip = card->GetAbsoluteOffset() + Rml::Vector2f(20.0f, 10.0f);
    controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(static_cast<int>(grip.x), static_cast<int>(grip.y)));
    controller.Render();
    controller.ProcessWindowMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(static_cast<int>(grip.x), static_cast<int>(grip.y)));
    controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(grip.x), 1000));
    EXPECT_EQ(GetContainerPos(controller, "lobby_ranks").second, 60.0f);
    EXPECT_GT(GetDragPos(controller).second, 850.0f);
    controller.Update(Config::Read(), true);
    EXPECT_GT(GetDragPos(controller).second, 850.0f);
    controller.ProcessWindowMessage(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(grip.x), 1000));
    controller.Render();

    const auto [movedX, movedY] = GetContainerPos(controller, "lobby_ranks");
    // Without the fix the drag stops at 1080 - 360 (the resolved default height).
    EXPECT_GT(movedY, 850.0f) << "drag was clamped against phantom container height";
    EXPECT_LE(movedY + renderedHeight, 1080.0f);
    EXPECT_GE(movedX, 0.0f);

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, DockedOverlayWidgetCanBeDraggedOutInEditMode) {
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
        OverlayLayout::ContainerConfig stack;
        stack.id = "stack";
        stack.x = 40.0f;
        stack.y = 60.0f;
        stack.widgets = {DashboardLayout::WidgetId::LiveRoster, DashboardLayout::WidgetId::SessionStats};
        c.overlay_layout.containers.push_back(stack);
    },
                   true);

    auto state = std::make_shared<SessionState>();
    state->ui.dashboardLayoutEditMode.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, device.Get(), context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    const auto press = [&](Rml::Element* handle, int toX, int toY) {
        const Rml::Vector2f grip = handle->GetAbsoluteOffset() + Rml::Vector2f(10.0f, 5.0f);
        const LPARAM at = MAKELPARAM(static_cast<int>(grip.x), static_cast<int>(grip.y));
        controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, 0, at);
        controller.Render();
        controller.ProcessWindowMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, at);
        controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(toX, toY));
        controller.ProcessWindowMessage(hwnd, WM_LBUTTONUP, 0, MAKELPARAM(toX, toY));
        controller.Render();
    };
    const auto handleFor = [&] {
        return OverlayRoot(controller)->QuerySelector("[data-action='overlay-widget-drag'][data-widget='session']");
    };

    auto* handle = handleFor();
    ASSERT_NE(handle, nullptr);
    const Rml::Vector2f grip = handle->GetAbsoluteOffset() + Rml::Vector2f(10.0f, 5.0f);
    press(handle, static_cast<int>(grip.x), static_cast<int>(grip.y));
    ASSERT_EQ(Config::Read().overlay_layout.containers.size(), 1u) << "a click on the handle must not undock";

    press(handleFor(), 1400, 700);
    const auto& containers = Config::Read().overlay_layout.containers;
    ASSERT_EQ(containers.size(), 2u);
    const auto source = std::find_if(containers.begin(), containers.end(), [](const auto& c) { return c.id == "stack"; });
    ASSERT_NE(source, containers.end());
    EXPECT_EQ(source->widgets, (std::vector<DashboardLayout::WidgetId>{DashboardLayout::WidgetId::LiveRoster}));
    const auto detached = std::find_if(containers.begin(), containers.end(), [](const auto& c) { return c.id != "stack"; });
    ASSERT_NE(detached, containers.end());
    EXPECT_EQ(detached->widgets, (std::vector<DashboardLayout::WidgetId>{DashboardLayout::WidgetId::SessionStats}));
    EXPECT_GT(detached->x, 1300.0f);

    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, IdleOverlayLayoutRequestsNoWakeDeadlineAndDoesNotRenderExtraFrames) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    ConfigData config = Config::Read();
    config.second_monitor_mode = false;
    config.show_lobby_ranks_overlay = true;
    controller.Update(config, true);
    controller.Render();

    // With no timed windows active, no wake deadline is scheduled and ShouldRender is false.
    EXPECT_EQ(controller.NextVisibilityWakeMs(), std::nullopt);
    EXPECT_FALSE(controller.ShouldRender());

    // Trigger FirstCountdown in match: lobby_ranks (AfterEvent FirstCountdown 8s) becomes visible
    // and schedules a wake deadline at firstCountdownMs + 8000 without rendering extra frames in between.
    const int64_t nowMs = RmlUiDetail::SteadyNowMs();
    state->liveMatch->Update([](MatchState& ms) {
        ms.inMatch = true;
    });
    state->ui.lastMatchStartMs.store(nowMs - 500);
    state->ui.firstCountdownOfMatchMs.store(nowMs);

    controller.Update(config, false);
    EXPECT_EQ(controller.NextVisibilityWakeMs(), std::optional<int64_t>(nowMs + 8000));
    EXPECT_NE(OverlayRoot(controller)->QuerySelector("[data-container='lobby_ranks']"), nullptr);
    controller.Render();
    EXPECT_FALSE(controller.ShouldRender());

    // Once the 8s window expires, the wake deadline clears and lobby_ranks hides.
    state->ui.lastMatchStartMs.store(nowMs - 10000);
    state->ui.firstCountdownOfMatchMs.store(nowMs - 8000);
    controller.Update(config, false);
    EXPECT_EQ(controller.NextVisibilityWakeMs(), std::nullopt);
    EXPECT_EQ(OverlayRoot(controller)->QuerySelector("[data-container='lobby_ranks']"), nullptr);
    controller.Render();
    EXPECT_FALSE(controller.ShouldRender());
}

TEST_F(RmlUiControllerStateTest, FloatingCardsSavedForLargerDisplayStayOnScreen) {
    WarpDevice warp;
    if (!warp.Create(1280, 720)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    const int64_t nowMs = RmlUiDetail::SteadyNowMs();
    state->ui.showMatchSummary.store(true);
    state->ui.matchSummaryStartMs.store(nowMs);
    state->ui.showSessionView.store(true);

    ConfigData config = Config::Read();
    config.second_monitor_mode = false;
    config.show_match_summary = true;
    config.match_summary_seconds = 60;
    config.match_summary_x = 1496.0f;
    config.match_summary_y = 1000.0f;
    config.session_view_x = 2400.0f;
    config.session_view_y = 2.0f;

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 720, 1.0f));
    controller.Update(config, true);
    controller.Render();

    for (const char* card : {"match-summary", "session-view"}) {
        SCOPED_TRACE(card);
        Rml::Element* element = OverlayRoot(controller)->QuerySelector(std::string("[data-card='") + card + "']");
        ASSERT_NE(element, nullptr);
        EXPECT_GE(element->GetAbsoluteLeft(), 0.0f);
        EXPECT_LE(element->GetAbsoluteLeft() + element->GetOffsetWidth(), 1280.5f);
        EXPECT_LT(element->GetAbsoluteTop(), 720.0f - 100.0f);
    }
}

TEST_F(RmlUiControllerStateTest, DragHoldClearsExpiredVisibilityDeadlineWithoutPinningShouldRender) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    const int64_t nowMs = RmlUiDetail::SteadyNowMs();
    state->liveMatch->Update([](MatchState& ms) {
        ms.inMatch = true;
    });
    state->ui.lastMatchStartMs.store(nowMs - 500);
    state->ui.firstCountdownOfMatchMs.store(nowMs);

    ConfigData config = Config::Read();
    config.second_monitor_mode = false;
    config.show_lobby_ranks_overlay = true;
    controller.Update(config, true);
    controller.Render();
    ASSERT_EQ(controller.NextVisibilityWakeMs(), std::optional<int64_t>(nowMs + 8000));
    EXPECT_FALSE(controller.ShouldRender());

    // Start an active drag while the deadline expires in the past.
    SimulateOverlayDragStart(controller, "main_stack", 1490.0f, 0.0f);
    state->ui.lastMatchStartMs.store(nowMs - 10000);
    state->ui.firstCountdownOfMatchMs.store(nowMs - 9000);
    SetVisibilityWakeMs(controller, nowMs - 1000);
    ASSERT_TRUE(controller.ShouldRender());

    // Render() runs RebuildVisibleUi, which hits the drag early-return and clears
    // the expired deadline so ShouldRender() does not stay pinned true every frame.
    controller.Render();
    EXPECT_EQ(controller.NextVisibilityWakeMs(), std::nullopt);
    EXPECT_FALSE(controller.ShouldRender());

    // Ending the drag reconciles the deferred visibility change on the next Update().
    ClearOverlayDrag(controller);
    controller.Update(config, false);
    EXPECT_EQ(OverlayRoot(controller)->QuerySelector("[data-container='lobby_ranks']"), nullptr);
}

TEST_F(RmlUiControllerStateTest, DisplayCardToggleWorksWithoutLayoutEditingAndKeepsPlacement) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.overlay_layout.containers.clear(); }, false);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read());
    controller.Render();
    auto* toggle = SettingsRoot(controller)->QuerySelector("[data-setting='widget_enabled:mmr-graph']");
    ASSERT_NE(toggle, nullptr);
    Rml::Dictionary parameters;
    parameters["checked"] = true;
    toggle->DispatchEvent("change", parameters);
    controller.Update(Config::Read());
    EXPECT_FALSE(state->ui.dashboardLayoutEditMode.load());
    ASSERT_TRUE(WidgetSettings::Enabled(Config::Read(), DashboardLayout::WidgetId::MmrGraph, false));
    const auto saved = Config::Read().overlay_layout.containers.front();
    auto* position = SettingsRoot(controller)->QuerySelector("[data-action='overlay-edit-toggle']");
    ASSERT_NE(position, nullptr);
    Click(controller, position);
    EXPECT_TRUE(state->ui.dashboardLayoutEditMode.load());
    auto* done = OverlayRoot(controller)->QuerySelector("[data-action='finish-position']");
    ASSERT_NE(done, nullptr);
    Click(controller, done);
    EXPECT_FALSE(state->ui.dashboardLayoutEditMode.load());
    state->ui.showMenu.store(true);
    controller.Update(Config::Read());
    toggle = SettingsRoot(controller)->QuerySelector("[data-setting='widget_enabled:mmr-graph']");
    ASSERT_NE(toggle, nullptr);
    parameters["checked"] = false;
    toggle->DispatchEvent("change", parameters);
    controller.Update(Config::Read());
    EXPECT_FALSE(WidgetSettings::Enabled(Config::Read(), DashboardLayout::WidgetId::MmrGraph, false));
    EXPECT_EQ(Config::Read().overlay_layout.containers.front().id, saved.id);
    EXPECT_FLOAT_EQ(Config::Read().overlay_layout.containers.front().x, saved.x);
    EXPECT_FALSE(state->ui.dashboardLayoutEditMode.load());
}
