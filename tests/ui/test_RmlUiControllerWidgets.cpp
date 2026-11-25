#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, PreviousGamesKeepsSessionRecordWhileHistoryIsLoadingOrEmpty) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.wins = 3;
        ms.sessionTotals.losses = 2;
    });

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    std::string html = RenderPreviousGames(controller);
    EXPECT_NE(html.find("Loading saved match history..."), std::string::npos);
    EXPECT_NE(html.find("Current session: <span class='win'>W:3</span> <span class='loss'>L:2</span>"), std::string::npos);

    state->history.Mutate([](auto& history) {
        history.recentSavedMatchesLoaded = true;
    });
    controller.Update(Config::Read());
    html = RenderPreviousGames(controller);
    EXPECT_NE(html.find("No saved games in local history."), std::string::npos);
    EXPECT_NE(html.find("Current session: <span class='win'>W:3</span> <span class='loss'>L:2</span>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, CompactSessionKeepsLegacySignedZeroMmrChange) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.show_session_mmr_change = true;
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.totalMmrChange = 0.0f;
    });

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const LiveValues live = ComputeLiveValues(Snapshot(controller), config);
    EXPECT_EQ(live.session_mmr, "+0");
    EXPECT_EQ(live.session_mmr_tone, 0);
    EXPECT_NE(RenderCompactSession(controller).find("{{session_mmr}}"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, CompactSessionUsesFinalizedDemoTotalsOnly) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.demos = 3;
        ms.currentMatch.demosSelf = 2;
        ms.matchFinalized = false;
    });

    ConfigData config = Config::Read();
    config.show_session_demos = true;
    RmlUiController controller(state, nullptr);
    controller.Update(config);

    const LiveValues live = ComputeLiveValues(Snapshot(controller), config);
    EXPECT_EQ(live.session_demos, "3");
    EXPECT_EQ(live.demo_session_count, "5-0");
}

TEST_F(RmlUiControllerStateTest, TelemetryUpdatesBoundTextWithoutRebuildingWidgets) {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, &featureLevel, &context))) {
        GTEST_SKIP() << "WARP not available.";
    }
    Config::Update([](ConfigData& c) {
        c.second_monitor_mode = true;
        c.show_demo_tracker_overlay = true;
        c.dashboard_layout = DashboardLayout::DefaultLayout();
        for (auto& widget : c.dashboard_layout.widgets)
            if (widget.id == DashboardLayout::WidgetId::DemoTracker) widget.zone = DashboardLayout::Zone::Right;
    },
                   true);
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.currentMatch.demosSelf = 2;
        ms.currentMatch.demoedSelf = 1;
    });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, device.Get(), context.Get(), 1200, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    const auto kdElement = [&]() -> Rml::Element* {
        Rml::ElementList elements;
        Document(controller)->QuerySelectorAll(elements, "[data-widget='demos'] .demo-kd");
        return elements.empty() ? nullptr : elements.front();
    };
    Rml::Element* before = kdElement();
    ASSERT_NE(before, nullptr);
    EXPECT_EQ(before->GetInnerRML(), "2.00");
    EXPECT_TRUE(before->IsClassSet("win"));

    state->liveMatch->Update([](MatchState& ms) {
        ms.currentMatch.demoedSelf = 4;
    });
    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_EQ(kdElement(), before);
    EXPECT_EQ(before->GetInnerRML(), "0.50");
    EXPECT_TRUE(before->IsClassSet("loss"));
    EXPECT_FALSE(before->IsClassSet("win"));
}

TEST_F(RmlUiControllerStateTest, DemoTrackerFormatsKdClassesAndSpacing) {
    // 1. Check DemoKdClass logic
    EXPECT_STREQ(RmlUiController::DemoKdClass(0, 0), "loss");
    EXPECT_STREQ(RmlUiController::DemoKdClass(0, 1), "loss");
    EXPECT_STREQ(RmlUiController::DemoKdClass(1, 4), "loss");
    EXPECT_STREQ(RmlUiController::DemoKdClass(4, 13), "loss");
    EXPECT_STREQ(RmlUiController::DemoKdClass(13, 4), "win");
    EXPECT_STREQ(RmlUiController::DemoKdClass(2, 0), "win");
    EXPECT_STREQ(RmlUiController::DemoKdClass(5, 2), "win");
    EXPECT_STREQ(RmlUiController::DemoKdClass(1, 1), "muted");
    EXPECT_STREQ(RmlUiController::DemoKdClass(4, 4), "muted");
    EXPECT_STREQ(RmlUiController::DemoKdClass(1, 0), "muted");

    // 2. A 0-0 game reads as a losing K/D.
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const ConfigData config = Config::Read();
    controller.Update(config);
    LiveValues live = ComputeLiveValues(Snapshot(controller), config);
    EXPECT_EQ(live.demo_game_count, "0-0");
    EXPECT_EQ(live.demo_game_tone, -1);
    EXPECT_NE(RenderDemoTracker(controller).find("style='margin-left:6dp'"), std::string::npos);

    // 3. 13-4 formats with two decimals and reads as winning.
    state->liveMatch->Update([](MatchState& ms) {
        ms.currentMatch.demosSelf = 13;
        ms.currentMatch.demoedSelf = 4;
    });
    controller.Update(config);
    live = ComputeLiveValues(Snapshot(controller), config);
    EXPECT_EQ(live.demo_game_count, "13-4");
    EXPECT_EQ(live.demo_game_kd, "3.25");
    EXPECT_EQ(live.demo_game_tone, 1);
}

TEST_F(RmlUiControllerStateTest, FastestKickoffLiveValueImprovesAndResetsForNewMatch) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) {
        c.second_monitor_mode = true;
        c.dashboard_layout = DashboardLayout::DefaultLayout();
        for (auto& widget : c.dashboard_layout.widgets)
            if (widget.id == DashboardLayout::WidgetId::LiveMatchStats) widget.zone = DashboardLayout::Zone::Left;
    },
                   true);
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    const auto valueElement = [&]() -> Rml::Element* {
        Rml::ElementList rows;
        Document(controller)->QuerySelectorAll(rows, "[data-widget='live-match'] .metric-row");
        for (auto* row : rows) {
            auto* label = row->QuerySelector(".metric-label");
            if (label && label->GetInnerRML() == "Fastest kickoff hit") return row->QuerySelector(".metric-value");
        }
        return nullptr;
    };
    auto* value = valueElement();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->GetInnerRML(), "--");
    state->liveMatch->Update([](MatchState& ms) { ms.currentMatch.kickoffFirstTouchSeconds = 1.95f; });
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(valueElement(), value);
    EXPECT_EQ(value->GetInnerRML(), "1.95 s");
    CaptureKickoffView(warp, controller, "kickoff-live.png");
    state->liveMatch->Update([](MatchState& ms) { ms.currentMatch.kickoffFirstTouchSeconds = 1.2f; });
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(value->GetInnerRML(), "1.20 s");
    state->liveMatch->Update([](MatchState& ms) { ms.ResetMatch("Stadium_P"); });
    controller.Update(Config::Read(), false);
    controller.Render();
    value = valueElement();
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->GetInnerRML(), "--");
}

TEST_F(RmlUiControllerStateTest, HistoryKickoffFirstHitAveragesMeasuredTeams) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.OpenHistory();
    MatchDetail detail;
    detail.found = true;
    detail.hasPlayerStats = true;
    MatchDetailPlayer local;
    local.team = 1;
    detail.ourTeam.push_back(local);
    detail.analytics.emplace();
    KickoffRecord ours, oursAgain, theirs, missing;
    ours.firstTouchTeam = 1;
    ours.firstTouchSeconds = 1.5f;
    oursAgain.firstTouchTeam = 1;
    oursAgain.firstTouchSeconds = 2.4f;
    theirs.firstTouchTeam = 0;
    theirs.firstTouchSeconds = 2.5f;
    missing.firstTouchTeam = 1;
    detail.analytics->kickoffs = {ours, oursAgain, theirs, missing};
    auto& view = HistoryModel(controller);
    ASSERT_TRUE(view.ApplyDetailResult(view.BeginDetail(), detail));
    controller.Render();
    const auto compareRow = [&]() -> Rml::Element* {
        Rml::ElementList rows;
        HistoryDocument(controller)->QuerySelectorAll(rows, ".history-compare-row");
        for (auto* row : rows) {
            auto* label = row->QuerySelector(".history-cmp-label");
            if (label && label->GetInnerRML() == "Avg kickoff first hit") return row;
        }
        return nullptr;
    };
    auto* row = compareRow();
    ASSERT_NE(row, nullptr);
    auto* ourValue = row->QuerySelector(".history-cmp-our");
    auto* theirValue = row->QuerySelector(".history-cmp-their");
    ASSERT_NE(ourValue, nullptr);
    ASSERT_NE(theirValue, nullptr);
    EXPECT_EQ(ourValue->GetInnerRML(), "1.95 s");
    EXPECT_EQ(theirValue->GetInnerRML(), "2.50 s");
    EXPECT_TRUE(ourValue->IsClassSet("win"));
    EXPECT_TRUE(theirValue->IsClassSet("loss"));
    CaptureKickoffView(warp, controller, "kickoff-history.png");
    detail.analytics->kickoffs = {theirs, missing};
    ASSERT_TRUE(view.ApplyDetailResult(view.ActiveDetailRequestId(), detail));
    controller.Render();
    row = compareRow();
    ASSERT_NE(row, nullptr);
    ourValue = row->QuerySelector(".history-cmp-our");
    theirValue = row->QuerySelector(".history-cmp-their");
    ASSERT_NE(ourValue, nullptr);
    ASSERT_NE(theirValue, nullptr);
    EXPECT_EQ(ourValue->GetInnerRML(), "--");
    EXPECT_EQ(theirValue->GetInnerRML(), "2.50 s");
    EXPECT_FALSE(ourValue->IsClassSet("loss"));
    EXPECT_FALSE(theirValue->IsClassSet("win"));
}

TEST_F(RmlUiControllerStateTest, OldLayoutPreservesDemoTrackerInMatchWithoutTabAndHonorsReplayHide) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.inMatch = true;
    });
    state->ui.showOverlay.store(false);

    ConfigData config = Config::Read();
    config.show_demo_tracker_overlay = true;
    config.show_streaks_stats = true;

    OverlayLayout::ContainerConfig demoContainer;
    demoContainer.id = "demo_tracker";
    demoContainer.widgets = {
        DashboardLayout::WidgetId::DemoTracker,
        DashboardLayout::WidgetId::StreaksStats,
        DashboardLayout::WidgetId::SessionStats};
    demoContainer.visibility.mode = OverlayLayout::Visibility::KeyHeld;

    RmlUiController controller(state, nullptr);
    controller.Update(config, true);

    // Without Tab held in a match, only DemoTracker renders in the docked container.
    std::string html = RenderOverlayContainer(controller, demoContainer, false);
    EXPECT_NE(html.find("data-live-widget='demos'"), std::string::npos);
    EXPECT_EQ(html.find("data-live-widget='streaks'"), std::string::npos);
    EXPECT_EQ(html.find("data-live-widget='session'"), std::string::npos);

    // With hideDuringReplay enabled and inGoalReplay active, DemoTracker also hides.
    demoContainer.visibility.hideDuringReplay = true;
    state->ui.inGoalReplay.store(true);
    html = RenderOverlayContainer(controller, demoContainer, false);
    EXPECT_TRUE(html.empty());
}

TEST_F(RmlUiControllerStateTest, MatchSummaryPrefersPodiumStartTimestampForCurrentMatch) {
    auto state = std::make_shared<SessionState>();
    const int64_t nowMs = RmlUiDetail::SteadyNowMs();
    state->ui.showMatchSummary.store(true);
    state->ui.lastMatchStartMs.store(nowMs - 40000);
    // MatchEnded was 25s ago (would be expired for a 20s window), but PodiumStart was 5s ago.
    state->ui.matchSummaryStartMs.store(nowMs - 25000);
    state->ui.lastPodiumMs.store(nowMs - 5000);

    ConfigData config = Config::Read();
    config.show_match_summary = true;
    config.match_summary_seconds = 20;

    RmlUiController controller(state, nullptr);
    controller.Update(config, true);
    EXPECT_TRUE(state->ui.showMatchSummary.load());
    EXPECT_EQ(controller.NextVisibilityWakeMs(), std::optional<int64_t>(nowMs - 5000 + 20000));
}

TEST_F(RmlUiControllerStateTest, LastGoalSpeedSitsBelowMaxAndFollowsUnitSetting) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ConfigData config = Config::Read();
    config.imperial_units = false;
    controller.Update(config);

    const std::string card = RenderLiveMatch(controller);
    const size_t maxRow = card.find("Max goal speed");
    const size_t lastRow = card.find("Last goal speed");
    ASSERT_NE(maxRow, std::string::npos);
    ASSERT_NE(lastRow, std::string::npos);
    EXPECT_LT(maxRow, lastRow);
    EXPECT_NE(card.find("{{last_goal_speed}}"), std::string::npos);

    RmlRenderSnapshot snap;
    EXPECT_EQ(ComputeLiveValues(snap, config).last_goal_speed, "-");
    snap.lastGoalSpeed = 160.0f;
    snap.lastGoalSpeedSelf = 143.0f;
    EXPECT_EQ(ComputeLiveValues(snap, config).last_goal_speed, "160 kph | 143 kph");
    config.imperial_units = true;
    EXPECT_EQ(ComputeLiveValues(snap, config).last_goal_speed, "99 mph | 89 mph");
}

TEST_F(RmlUiControllerStateTest, CompactTournamentCountdownFitsBothSurfaces) {
    WarpDevice warp;
    if (!warp.Create(1000, 500)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1000, 500, 1.0f));
    Tournaments::ScheduleSnapshot snapshot;
    snapshot.status = Tournaments::ScheduleStatus::Ready;
    snapshot.group = "US";
    snapshot.slots = {{1000 + 22860, false, "2v2 Pentathlon", 2, Tournaments::SecondChance{"2v2 Pentathlon (2nd Chance)", 1000 + 23760}}};
    const std::string countdown = TournamentUi::RenderWidget(snapshot, 1000);
    const std::string second = TournamentUi::RenderWidget(snapshot, 1000 + 22860);
    EXPECT_NE(TournamentUi::RenderTopbar(snapshot, 1000, false, 10).find("Second Chance"), std::string::npos);
    OverlayRoot(controller)->SetInnerRML("<div id='compact-overlay' class='card' style='position:absolute;left:24dp;top:24dp;width:320dp'>" + countdown + "</div>"
                                                                                                                                                          "<div id='compact-dashboard' class='card tournament-dashboard' style='position:absolute;left:400dp;top:24dp'>" +
                                         second + "</div>");
    RmlContext(controller)->Update();
    controller.Render();
    if (const char* dir = std::getenv("OMNISTATS_CAPTURE_DIR"); dir && *dir) {
        const auto path = std::filesystem::path(dir) / "tournament-second-chance.png";
        std::string error;
        EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 760, 110, 0.0f,
                                                            path.wstring(), error))
            << error;
    }
    for (const char* id : {"compact-overlay", "compact-dashboard"}) {
        auto* card = Document(controller)->GetElementById(id);
        ASSERT_NE(card, nullptr);
        Rml::ElementList rows;
        card->QuerySelectorAll(rows, ".row");
        ASSERT_EQ(rows.size(), 1u);
        const bool secondChance = std::string_view(id) == "compact-dashboard";
        EXPECT_EQ(card->GetInnerRML().find("Second Chance") != std::string::npos, secondChance);
        auto* name = card->QuerySelector(".tournament-name");
        auto* time = card->QuerySelector(".tournament-remaining");
        ASSERT_NE(name, nullptr);
        ASSERT_NE(time, nullptr);
        EXPECT_EQ(time->GetInnerRML(), secondChance ? "15m" : "6h 21m");
        EXPECT_LE(name->GetAbsoluteOffset().x + name->GetOffsetWidth(), time->GetAbsoluteOffset().x);
        EXPECT_LE(time->GetAbsoluteOffset().x + time->GetOffsetWidth(), card->GetAbsoluteOffset().x + card->GetOffsetWidth());
    }
}
