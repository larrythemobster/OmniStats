#include "RmlUiControllerStateFixture.hpp"
#include "core/game/RankTiers.hpp"

TEST_F(RmlUiControllerStateTest, GraphCategoryChangeRefreshesLifetimeHistoryOnce) {
    auto state = std::make_shared<SessionState>();
    int lifetimeCalls = 0;
    std::string lifetimePlaylist;
    DatabaseHooks hooks;
    hooks.onAsyncGetLifetimeMmr = [&](const std::string&, const std::string& playlist) {
        ++lifetimeCalls;
        lifetimePlaylist = playlist;
    };
    auto db = std::make_shared<DatabaseManager>(state, std::move(hooks));
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    state->history.Mutate([](auto& history) {
        history.lifetimeMmrX = {1};
        history.lifetimeMmrY = {1200.0f};
    });
    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|GRAPH"; }, true);
    ConfigData config = Config::Read();

    RmlUiController controller(state, db);
    controller.Update(config);
    EXPECT_EQ(lifetimeCalls, 1);

    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    controller.Update(config);
    EXPECT_EQ(lifetimeCalls, 2);
    EXPECT_EQ(lifetimePlaylist, "1v1");
    {
        const auto history = state->history.GetSnapshot();
        EXPECT_TRUE(history->lifetimeMmrX.empty());
        EXPECT_TRUE(history->lifetimeMmrY.empty());
    }

    controller.Update(config);
    EXPECT_EQ(lifetimeCalls, 2);
}

TEST_F(RmlUiControllerStateTest, EmptyGraphShowsAppropriateGuidanceWithoutNavigationControls) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);

    // Session Graph empty state
    state->history.Mutate([](auto& history) { history.showLifetimeGraph = false; });
    controller.Update(Config::Read());
    const std::string sessionHtml = RenderGraph(controller);
    EXPECT_EQ(sessionHtml.find("data-action='graph-pan"), std::string::npos);
    EXPECT_EQ(sessionHtml.find("data-action='graph-zoom"), std::string::npos);
    EXPECT_EQ(sessionHtml.find("data-action='graph-mode"), std::string::npos);
    EXPECT_NE(sessionHtml.find("No MMR data for 2v2 this session."), std::string::npos);
    EXPECT_NE(sessionHtml.find("Play a match to start plotting!"), std::string::npos);

    // Lifetime Graph empty state
    state->history.Mutate([](auto& history) { history.showLifetimeGraph = true; });
    controller.Update(Config::Read());
    const std::string lifetimeHtml = RenderGraph(controller);
    EXPECT_EQ(lifetimeHtml.find("data-action='graph-pan"), std::string::npos);
    EXPECT_EQ(lifetimeHtml.find("data-action='graph-zoom"), std::string::npos);
    EXPECT_EQ(lifetimeHtml.find("data-action='graph-mode"), std::string::npos);
    EXPECT_NE(lifetimeHtml.find("No lifetime MMR history in database yet."), std::string::npos);
    EXPECT_NE(lifetimeHtml.find("Play matches to populate database records!"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, LifetimeGraphKeepsLatestTwentyFiveDottedMatchesWithPanButtonsOnly) {
    auto state = std::make_shared<SessionState>();
    state->history.Mutate([](auto& history) {
        history.showLifetimeGraph = true;
        for (int mmr = 1200; mmr < 1250; ++mmr) {
            history.lifetimeMmrY.push_back(static_cast<float>(mmr));
            history.lifetimeMmrX.push_back(mmr);
        }
    });

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderGraph(controller);

    EXPECT_NE(html.find("26-50 of 50"), std::string::npos);
    EXPECT_NE(html.find("data-action='graph-pan-older'"), std::string::npos);
    EXPECT_NE(html.find("data-action='graph-pan-newer'"), std::string::npos);
    EXPECT_EQ(html.find("data-action='graph-zoom-in'"), std::string::npos);
    EXPECT_EQ(html.find("data-action='graph-zoom-out'"), std::string::npos);
    EXPECT_EQ(html.find("data-action='graph-mode'"), std::string::npos);
    EXPECT_NE(html.find("<mmrgraphlines class='graph-polyline'"), std::string::npos);
    EXPECT_NE(html.find("class='graph-point"), std::string::npos);
    const std::string baseline = "<mmrgraphlines class='graph-baseline' dashed='1'";
    const size_t baselinePos = html.find(baseline);
    ASSERT_NE(baselinePos, std::string::npos);
    EXPECT_EQ(html.find(baseline, baselinePos + baseline.size()), std::string::npos);
    EXPECT_EQ(html.find("class='graph-line baseline'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, MmrGraphShowsLabeledDivisionBoundariesPerPlaylist) {
    auto state = std::make_shared<SessionState>();
    SeedGraph(*state, "2v2", {1180.0f, 1190.0f, 1200.0f}, 1200, "Champion II Div I");
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    const auto doubles = RankProgress::Compute(RankTiers::RankThresholdsForPlaylist("2v2"), 1200, "Champion II Div I");
    ASSERT_TRUE(doubles.next);
    std::string html = RenderGraph(controller);
    EXPECT_EQ(html.find("rank-progress"), std::string::npos);
    EXPECT_NE(html.find("graph-rank-line"), std::string::npos);
    EXPECT_NE(html.find("graph-band-label"), std::string::npos);
    EXPECT_NE(html.find("'>" + std::to_string(doubles.next->mmr) + "</div>"), std::string::npos);
    EXPECT_NE(html.find("mmrgraphlines"), std::string::npos);

    // Switching playlists swaps the table.
    SeedGraph(*state, "3v3", {1180.0f, 1190.0f, 1200.0f}, 0, "");
    state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);
    controller.Update(Config::Read());
    const auto standard = RankProgress::Compute(RankTiers::RankThresholdsForPlaylist("3v3"), 1200, "");
    ASSERT_TRUE(standard.next);
    html = RenderGraph(controller);
    EXPECT_NE(html.find(" starts at " + std::to_string(standard.next->mmr) + " MMR"), std::string::npos);
    EXPECT_EQ(html.find("estimate"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, SessionGraphStartsFromThePreSessionRating) {
    auto state = std::make_shared<SessionState>();
    SeedGraph(*state, "2v2", {1009.0f}, 1009, "");
    state->history.Mutate([](auto& history) { history.playlistInitialMmr["2v2"] = 1000; });
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    std::string html = RenderGraph(controller);

    // One game still draws a segment: session start, then the game.
    const size_t polyline = html.find("points='");
    ASSERT_NE(polyline, std::string::npos);
    EXPECT_NE(html.find(';', polyline), std::string::npos);
    EXPECT_NE(html.find("graph-point start"), std::string::npos);
    EXPECT_NE(html.find("Session start \xC2\xB7 1000 MMR"), std::string::npos);
    EXPECT_NE(html.find("Game 1 \xC2\xB7 1009 MMR (+9)"), std::string::npos);

    SeedGraph(*state, "2v2", {1009.0f, 1000.0f}, 1000, "");
    controller.Update(Config::Read());
    html = RenderGraph(controller);
    EXPECT_NE(html.find("Game 2 \xC2\xB7 1000 MMR (-9)"), std::string::npos);

    // Without a rank table the generic value gridlines come back and no bands are drawn.
    SeedGraph(*state, "casual", {900.0f, 910.0f}, 910, "");
    state->ui.graphMmrCategory.store(MmrCategory::Casual);
    controller.Update(Config::Read());
    html = RenderGraph(controller);
    EXPECT_EQ(html.find("graph-band"), std::string::npos);
    EXPECT_NE(html.find("graph-gridline"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, MmrGraphOmitsRankLinesWithoutATableAndExplainsEmptyHistory) {
    auto state = std::make_shared<SessionState>();
    SeedGraph(*state, "casual", {900.0f, 910.0f}, 910, "");
    state->ui.graphMmrCategory.store(MmrCategory::Casual);
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    std::string html = RenderGraph(controller);
    EXPECT_EQ(html.find("graph-rank-line"), std::string::npos);

    SeedGraph(*state, "2v2", {}, 0, "");
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    controller.Update(Config::Read());
    html = RenderGraph(controller);
    EXPECT_NE(html.find("No MMR data"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, PannedGraphUsesVisibleRatingsAndDisablesBoundaryActions) {
    WarpDevice warp;
    ASSERT_TRUE(warp.Create(1200, 1000));
    Config::Update([](ConfigData& config) {
        config.second_monitor_mode = true;
        config.dashboard_layout = DashboardLayout::DefaultLayout();
    },
                   true);
    auto state = std::make_shared<SessionState>();
    std::vector<float> ratings;
    for (int i = 0; i < 40; ++i)
        ratings.push_back(800.0f + i * 2.0f);
    SeedGraph(*state, "casual", std::move(ratings), 878, "");
    state->ui.graphMmrCategory.store(MmrCategory::Casual);
    state->history.Mutate([](auto& history) { history.playlistInitialMmr["casual"] = 100; });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1200, 1000, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    auto* graph = DashboardRoot(controller)->QuerySelector("[data-widget='mmr-graph']");
    ASSERT_NE(graph, nullptr);
    auto* plot = graph->QuerySelector(".graph-wrap");
    ASSERT_NE(plot, nullptr);
    Rml::ElementList points;
    plot->QuerySelectorAll(points, ".graph-point");
    ASSERT_EQ(points.size(), 25u);
    const float firstY = points.front()->GetAbsoluteTop();
    const float lastY = points.back()->GetAbsoluteTop();
    EXPECT_GT(firstY - lastY, plot->GetOffsetHeight() * 0.5f);

    auto* newer = graph->QuerySelector("[data-action='graph-pan-newer']");
    ASSERT_NE(newer, nullptr);
    EXPECT_TRUE(newer->HasAttribute("disabled"));
    Click(controller, newer);
    EXPECT_EQ(state->ui.graphOffset.load(), 0);

    for (int offset : {5, 10, 15}) {
        auto* older = DashboardRoot(controller)->QuerySelector("[data-action='graph-pan-older']");
        ASSERT_NE(older, nullptr);
        EXPECT_FALSE(older->HasAttribute("disabled"));
        Click(controller, older);
        controller.Update(Config::Read());
        controller.Render();
        EXPECT_EQ(state->ui.graphOffset.load(), offset);
    }
    auto* older = DashboardRoot(controller)->QuerySelector("[data-action='graph-pan-older']");
    ASSERT_NE(older, nullptr);
    EXPECT_TRUE(older->HasAttribute("disabled"));
    Click(controller, older);
    EXPECT_EQ(state->ui.graphOffset.load(), 15);
}

TEST_F(RmlUiControllerStateTest, LifetimeGraphDefaultSwitchesBothSurfacesAndAllowsTemporaryOverride) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";

    for (const bool dashboard : {false, true}) {
        SCOPED_TRACE(dashboard);
        Config::Update([dashboard](ConfigData& c) {
            c.second_monitor_mode = dashboard;
            c.graph_default_lifetime = false;
            WidgetSettings::SetEnabled(c, DashboardLayout::WidgetId::MmrGraph, dashboard, true, 1280, 900);
        },
                       false);
        auto state = std::make_shared<SessionState>();
        state->ui.showMenu.store(true);
        state->history.Mutate([](auto& history) {
            history.playlistHistoryY["2v2"] = {1200.0f, 1210.0f};
            history.lifetimeMmrY = {1500.0f, 1510.0f};
            history.lifetimeMmrX = {1700000000, 1700000060};
        });
        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
        SetSettingsPage(controller, 4);
        controller.Update(Config::Read(), true);
        auto* details = SettingsRoot(controller)->QuerySelector("[data-action='toggle-rank-details']");
        ASSERT_NE(details, nullptr);
        Click(controller, details);
        const auto graphRoot = [&]() { return dashboard ? DashboardRoot(controller) : OverlayRoot(controller); };
        ASSERT_NE(graphRoot()->QuerySelector(".graph-wrap"), nullptr);
        EXPECT_NE(graphRoot()->GetInnerRML().find("1210 MMR"), std::string::npos);
        EXPECT_EQ(graphRoot()->GetInnerRML().find("1510 MMR"), std::string::npos);

        auto* toggle = SettingsRoot(controller)->QuerySelector("[data-setting='graph_default_lifetime']");
        ASSERT_NE(toggle, nullptr);
        state->ui.graphOffset.store(10);
        Rml::Dictionary parameters;
        parameters["checked"] = true;
        toggle->DispatchEvent("change", parameters);
        controller.Update(Config::Read());
        controller.Render();
        EXPECT_TRUE(Config::Read().graph_default_lifetime);
        EXPECT_EQ(state->ui.graphOffset.load(), 0);
        EXPECT_NE(graphRoot()->GetInnerRML().find("1510 MMR"), std::string::npos);
        EXPECT_EQ(graphRoot()->GetInnerRML().find("1210 MMR"), std::string::npos);

        if (const char* dir = std::getenv("OMNISTATS_CAPTURE_DIR"); dir && *dir) {
            std::filesystem::create_directories(dir);
            toggle = SettingsRoot(controller)->QuerySelector("[data-setting='graph_default_lifetime']");
            ASSERT_NE(toggle, nullptr);
            toggle->ScrollIntoView();
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            warp.context->ClearRenderTargetView(warp.target.Get(), clear);
            controller.Render();
            std::string error;
            const auto path = std::filesystem::path(dir) / (dashboard ? "lifetime-default-dashboard.png" : "lifetime-default-overlay.png");
            EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1280, 900, 0.0f, path.wstring(), error)) << error;
        }

        InputManager::HandleHotKeyAction(*state, InputManager::HotKeyLifetimeGraph, true, true, nullptr);
        Config::Update([](ConfigData& c) { c.show_session_goals = !c.show_session_goals; }, false);
        controller.Update(Config::Read(), true);
        EXPECT_TRUE(Config::Read().graph_default_lifetime);
        EXPECT_NE(graphRoot()->GetInnerRML().find("1210 MMR"), std::string::npos);
        EXPECT_EQ(graphRoot()->GetInnerRML().find("1510 MMR"), std::string::npos);

        toggle = SettingsRoot(controller)->QuerySelector("[data-setting='graph_default_lifetime']");
        ASSERT_NE(toggle, nullptr);
        parameters["checked"] = false;
        toggle->DispatchEvent("change", parameters);
        controller.Update(Config::Read());
        EXPECT_FALSE(Config::Read().graph_default_lifetime);
        EXPECT_NE(graphRoot()->GetInnerRML().find("1210 MMR"), std::string::npos);
        EXPECT_EQ(graphRoot()->GetInnerRML().find("1510 MMR"), std::string::npos);
    }
}
