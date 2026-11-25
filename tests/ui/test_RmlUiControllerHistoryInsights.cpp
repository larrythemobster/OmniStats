#include "RmlUiControllerStateFixture.hpp"

// Closing Rocket League hides the overlay, and the host stops calling Update()
// while hidden. The pending recap must keep the window drawn, or the recap
// is never opened.
TEST_F(RmlUiControllerStateTest, PendingSessionRecapKeepsOverlayDrawn) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    EXPECT_FALSE(controller.WantsAttention());
    state->ui.showSessionRecap.store(true);
    EXPECT_TRUE(controller.WantsAttention());
}

TEST_F(RmlUiControllerStateTest, AutomaticSessionRecapExpiresWithoutDiscardingSavedRecap) {
    for (bool dashboard : {false, true}) {
        SCOPED_TRACE(dashboard);
        Config::Update([&](ConfigData& c) { c.second_monitor_mode = dashboard; }, true);
        WarpDevice warp;
        if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
        auto state = std::make_shared<SessionState>();
        state->liveMatch->Update([](MatchState& ms) {
            ms.lastSessionRecap.valid = true;
            ms.lastSessionRecap.totals.wins = 4;
            ms.lastSessionRecap.totals.losses = 2;
        });
        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
        state->ui.showSessionRecap.store(true);
        const int64_t openedAt = RmlUiDetail::SteadyNowMs();
        controller.Update(Config::Read(), false);
        controller.Render();
        ASSERT_TRUE(InsightsDocument(controller)->IsVisible());
        ASSERT_TRUE(RecapDismissAt(controller).has_value());
        EXPECT_GE(*RecapDismissAt(controller), openedAt + 300'000);

        const int64_t beforeExpiry = RmlUiDetail::SteadyNowMs() + 60'000;
        SetRecapDismissAt(controller, beforeExpiry);
        state->liveMatch->Update([](MatchState& ms) { ms.currentMatch.goals += 1; });
        controller.Update(Config::Read(), false);
        controller.Render();
        EXPECT_TRUE(InsightsDocument(controller)->IsVisible());
        EXPECT_EQ(RecapDismissAt(controller), beforeExpiry);

        SetRecapDismissAt(controller, RmlUiDetail::SteadyNowMs() - 1);
        controller.Update(Config::Read(), false);
        controller.Render();
        EXPECT_FALSE(InsightsDocument(controller)->IsVisible());
        EXPECT_FALSE(controller.WantsAttention());
        EXPECT_FALSE(RecapDismissAt(controller).has_value());
        EXPECT_TRUE(state->liveMatch->GetSnapshot()->lastSessionRecap.valid);
        EXPECT_EQ(state->liveMatch->GetSnapshot()->lastSessionRecap.totals.wins, 4);

        controller.OpenInsights();
        controller.Update(Config::Read(), false);
        controller.Render();
        EXPECT_TRUE(InsightsDocument(controller)->IsVisible());
        EXPECT_TRUE(InsightsShowEndedSession(controller));
        EXPECT_FALSE(RecapDismissAt(controller).has_value());
    }
}

TEST_F(RmlUiControllerStateTest, LayoutClicksRestartAutomaticRecapTimeoutButHoverDoesNot) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    state->ui.showSessionRecap.store(true);
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* card = InsightsDocument(controller)->GetElementById("recap-card");
    ASSERT_NE(card, nullptr);
    const auto offset = card->GetAbsoluteOffset(Rml::BoxArea::Border);
    const int64_t originalDeadline = RmlUiDetail::SteadyNowMs() + 60'000;
    SetRecapDismissAt(controller, originalDeadline);
    RmlContext(controller)->ProcessMouseMove(static_cast<int>(offset.x + 20), static_cast<int>(offset.y + 20), 0);
    EXPECT_EQ(RecapDismissAt(controller), originalDeadline);

    const int64_t pressedAt = RmlUiDetail::SteadyNowMs();
    RmlContext(controller)->ProcessMouseButtonDown(0, 0);
    ASSERT_TRUE(RecapDismissAt(controller).has_value());
    EXPECT_GE(*RecapDismissAt(controller), pressedAt + 300'000);
    SetRecapDismissAt(controller, originalDeadline);
    const int64_t clickedAt = RmlUiDetail::SteadyNowMs();
    RmlContext(controller)->ProcessMouseButtonUp(0, 0);
    ASSERT_TRUE(RecapDismissAt(controller).has_value());
    EXPECT_GE(*RecapDismissAt(controller), clickedAt + 300'000);
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_TRUE(InsightsDocument(controller)->IsVisible());

    auto* backdrop = InsightsDocument(controller)->QuerySelector(".view-backdrop");
    ASSERT_NE(backdrop, nullptr);
    SetRecapDismissAt(controller, originalDeadline);
    const int64_t layoutClickedAt = RmlUiDetail::SteadyNowMs();
    backdrop->DispatchEvent("click", Rml::Dictionary{});
    EXPECT_GE(*RecapDismissAt(controller), layoutClickedAt + 300'000);
}

TEST_F(RmlUiControllerStateTest, ClosingAutomaticRecapClearsTimeoutBeforeManualReopen) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    state->ui.showSessionRecap.store(true);
    controller.Update(Config::Read(), false);
    controller.Render();
    auto* close = InsightsDocument(controller)->QuerySelector("[data-action='insights-close']");
    ASSERT_NE(close, nullptr);
    close->DispatchEvent("click", Rml::Dictionary{});
    EXPECT_FALSE(InsightsDocument(controller)->IsVisible());
    EXPECT_FALSE(RecapDismissAt(controller).has_value());

    controller.OpenInsights();
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_TRUE(InsightsDocument(controller)->IsVisible());
    EXPECT_FALSE(RecapDismissAt(controller).has_value());

    state->ui.showSessionRecap.store(true);
    const int64_t reopenedAt = RmlUiDetail::SteadyNowMs();
    controller.Update(Config::Read(), false);
    ASSERT_TRUE(RecapDismissAt(controller).has_value());
    EXPECT_GE(*RecapDismissAt(controller), reopenedAt + 300'000);
}

TEST_F(RmlUiControllerStateTest, InsightsButtonShowsEndedSessionAfterReset) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.lastSessionRecap.valid = true;
        ms.lastSessionRecap.totals.wins = 4;
        ms.lastSessionRecap.totals.losses = 2;
    });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.OpenInsights();
    EXPECT_TRUE(InsightsShowEndedSession(controller));

    // With games in the live session, the button shows the live session.
    controller.Shutdown();
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.wins = 1;
    });
    RmlUiController live(state, nullptr);
    ASSERT_TRUE(live.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    live.Update(Config::Read(), false);
    live.OpenInsights();
    EXPECT_FALSE(InsightsShowEndedSession(live));
}

TEST_F(RmlUiControllerStateTest, RecapCardRendersAndCapturesToPng) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.sessionTotals.wins = 3;
        ms.sessionTotals.losses = 1;
        ms.sessionTotals.goals = 5;
        ms.sessionGamemodes["2v2"] = {3, 1, 4};
    });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    ShowRecap(controller);
    controller.Render();

    Rml::Element* card = InsightsDocument(controller)->GetElementById("recap-card");
    ASSERT_NE(card, nullptr);
    const Rml::Vector2f offset = card->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = card->GetBox().GetSize(Rml::BoxArea::Border);
    ASSERT_GT(size.x, 300.0f);
    ASSERT_GT(size.y, 100.0f);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "omnistats_recap_test.png";
    std::filesystem::remove(path);
    std::string error;
    ASSERT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), static_cast<int>(offset.x), static_cast<int>(offset.y),
                                                        static_cast<int>(size.x), static_cast<int>(size.y),
                                                        card->GetComputedValues().border_top_left_radius(), path.wstring(), error))
        << error;
    EXPECT_GT(std::filesystem::file_size(path), 1024u);
    UINT width = 0, height = 0;
    const std::vector<unsigned char> png = DecodePngBgra(path, width, height);
    ASSERT_FALSE(png.empty());
    const auto alphaAt = [&](UINT x, UINT y) { return png[(static_cast<size_t>(y) * width + x) * 4 + 3]; };
    EXPECT_EQ(alphaAt(0, 0), 0);
    EXPECT_EQ(alphaAt(width - 1, 0), 0);
    EXPECT_EQ(alphaAt(0, height - 1), 0);
    EXPECT_EQ(alphaAt(width - 1, height - 1), 0);
    EXPECT_EQ(alphaAt(width / 2, height / 2), 255);
    std::filesystem::remove(path);
}

TEST_F(RmlUiControllerStateTest, BuildTrendSectionsIncludesLobbyStrengthSection) {
    std::vector<MatchOutcome> outcomes;
    std::vector<MatchMmrContext> mmrContext;
    for (int i = 0; i < 12; ++i) {
        const int64_t ts = 1'800'000'000 + i * 600;
        const bool win = (i % 2 == 0);
        outcomes.push_back({ts, 20, "Doubles", win});
        mmrContext.push_back({ts, "Doubles", win, 1000 + i * 5, false, 1000.0, 1080.0, 2, 2});
    }

    const TrendsReport trends = Insights::ComputeTrends(outcomes);
    const GapReport gap = Insights::ComputeGapTrends(mmrContext, "2v2");
    const std::vector<TrendSection> sections = InsightsView::BuildTrendSections(trends, gap);

    ASSERT_EQ(sections.size(), 5u);
    EXPECT_EQ(sections[0].title, "BY PLAYLIST");
    EXPECT_EQ(sections[1].title, "TIME OF DAY");
    EXPECT_EQ(sections[2].title, "GAMES INTO A SITTING");
    EXPECT_EQ(sections[3].title, "AFTER THE PREVIOUS GAME");
    EXPECT_EQ(sections[4].title, "LOBBY STRENGTH");
    EXPECT_EQ(sections[4].subtitle, "Opponents vs your team, MMR recorded at match time");
    ASSERT_GE(sections[4].rows.size(), 5u);
    EXPECT_EQ(sections[4].rows[0].label, "Much stronger (+75 or more)");
    EXPECT_EQ(sections[4].rows[0].rate, "50%");
    EXPECT_EQ(sections[4].rows[0].games, "12 games");
    EXPECT_EQ(sections[4].rows[1].label, "Stronger (+25 to +75)");
    EXPECT_EQ(sections[4].rows[1].rate, "Not enough games");
    EXPECT_EQ(sections[4].rows[2].label, "Even (±25)");
    EXPECT_EQ(sections[4].rows[3].label, "Weaker (-25 to -75)");
    EXPECT_EQ(sections[4].rows[4].label, "Much weaker (-75 or less)");

    const GapReport emptyGap = Insights::ComputeGapTrends(mmrContext, "1v1");
    const std::vector<TrendSection> emptySections = InsightsView::BuildTrendSections(trends, emptyGap);
    ASSERT_EQ(emptySections.size(), 5u);
    ASSERT_EQ(emptySections[4].rows.size(), 1u);
    EXPECT_EQ(emptySections[4].rows[0].label, "Not enough ranked data");
}

TEST_F(RmlUiControllerStateTest, SeasonTabRendersSummaryRewardAndPreviousSeason) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    SeasonSummary current;
    current.season = {20, 1'700'000'000, 1'710'000'000, "rocket_league"};
    current.wins = 12;
    current.losses = 8;
    current.winRate = 0.6;
    current.totalMmrChange = 45;
    current.peakMmr = 1250;
    current.peakRank = "Champion II";
    current.playlists["2v2"] = {9, 6, 35};
    current.playlists["1v1"] = {3, 2, 10};
    SeasonSummary previous;
    previous.season = {19, 1'690'000'000, 1'700'000'000, "rocket_league"};
    previous.wins = 4;
    previous.losses = 6;
    previous.winRate = 0.4;
    previous.peakMmr = 1180;
    previous.peakRank = "Champion I";
    const std::vector<SeasonSummary> summaries{current, previous};
    const TrackedAccounts::SeasonReward reward{20, 4, 7, 1'705'000'000};

    ShowSeason(controller, summaries, reward, 1'705'000'000);
    controller.Render();
    RmlContext(controller)->Update();
    auto* doc = InsightsDocument(controller);
    ASSERT_NE(doc, nullptr);
    ASSERT_TRUE(doc->IsVisible());
    auto* card = doc->GetElementById("season-card");
    ASSERT_NE(card, nullptr);
    EXPECT_NE(card->GetInnerRML().find("SEASON 20"), std::string::npos);
    EXPECT_NE(card->GetInnerRML().find("20 ranked games"), std::string::npos);
    EXPECT_NE(card->GetInnerRML().find("60%"), std::string::npos);
    EXPECT_NE(card->GetInnerRML().find("1250"), std::string::npos);
    ASSERT_NE(card->QuerySelector(".reward-row"), nullptr);
    auto* rewardCount = card->QuerySelector(".reward-count");
    auto* rewardTier = card->QuerySelector(".reward-tier");
    ASSERT_NE(rewardCount, nullptr);
    ASSERT_NE(rewardTier, nullptr);
    EXPECT_NE(rewardCount->GetInnerRML().find("7 / 10"), std::string::npos);
    EXPECT_NE(rewardTier->GetInnerRML().find("Diamond"), std::string::npos);
    Rml::ElementList rows;
    card->QuerySelectorAll(rows, ".recap-mode-row");
    EXPECT_EQ(std::count_if(rows.begin(), rows.end(), [](Rml::Element* row) { return row->IsVisible(); }), 2);
    EXPECT_NE(card->GetInnerRML().find("Season 19"), std::string::npos);

    ShowSeason(controller, summaries, reward, 1'705'000'000, 19);
    controller.Render();
    RmlContext(controller)->Update();
    card = doc->GetElementById("season-card");
    ASSERT_NE(card, nullptr);
    EXPECT_NE(card->GetInnerRML().find("SEASON 19"), std::string::npos);
    auto* previousReward = card->QuerySelector(".reward-row");
    EXPECT_TRUE(!previousReward || !previousReward->IsVisible());
}

TEST_F(RmlUiControllerStateTest, SessionsTabRendersRowsOpensArchivedRecapAndComparesWithPrevious) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    const std::string pid = "Steam|sessions-ui";
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    // Seed 6 archived sessions (enough for top/bottom 10% best/worst badges) plus 1 backfilled session without MMR.
    const int mmrDeltas[] = {35, 18, 9, -4, -12, -28};
    for (int i = 0; i < 6; ++i) {
        SessionRecap s;
        s.valid = true;
        s.account = pid;
        s.startedAtUnix = 1'770'000'000 + i * 10'000;
        s.endedAtUnix = s.startedAtUnix + 3600;
        s.sessionGeneration = static_cast<uint64_t>(i + 1);
        s.totals.wins = 3 + (i % 2);
        s.totals.losses = 2;
        s.totals.goals = 4 + i;
        s.totals.saves = 3;
        s.totals.mmrChangeByPlaylist["2v2"] = mmrDeltas[i];
        s.gamemodes["2v2"] = {s.totals.wins, s.totals.losses, s.totals.wins + s.totals.losses};
        ASSERT_GT(db->SaveSession(s), 0);
    }
    {
        SessionRecap backfilled;
        backfilled.valid = true;
        backfilled.account = pid;
        backfilled.source = "backfill";
        backfilled.startedAtUnix = 1'769'900'000;
        backfilled.endedAtUnix = 1'769'901'800;
        backfilled.totals.wins = 2;
        backfilled.totals.losses = 1;
        ASSERT_GT(db->SaveSession(backfilled), 0);
    }

    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = pid;
        ms.sessionTotals.wins = 1;
        ms.sessionTotals.losses = 0;
        ms.sessionTotals.mmrChangeByPlaylist["2v2"] = 10;
    });
    state->insights.PublishInsights(pid, {}, {}, {}, db->ListSessions(pid, 0, 50), {});

    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.OpenInsights();
    controller.Render();

    Rml::ElementDocument* doc = InsightsDocument(controller);
    ASSERT_NE(doc, nullptr);

    // Switch to Sessions tab.
    Rml::ElementList tabs;
    doc->QuerySelectorAll(tabs, "[data-action='insights-tab'][data-tab='sessions']");
    ASSERT_EQ(tabs.size(), 1u);
    Click(controller, tabs.front());
    controller.Render();

    Rml::ElementList sessionRows;
    doc->QuerySelectorAll(sessionRows, ".session-row.item");
    // 1 Live session + 7 archived sessions = 8 rows.
    ASSERT_EQ(sessionRows.size(), 8u);
    EXPECT_NE(sessionRows[0]->GetInnerRML().find("Live"), std::string::npos);

    // Verify Best (+35) and Worst (-28) badges and "--" for unknown MMR exist in the rendered table.
    Rml::Element* table = doc->QuerySelector(".sessions-table");
    ASSERT_NE(table, nullptr);
    const std::string tableRml = table->GetInnerRML();
    EXPECT_NE(tableRml.find("Best"), std::string::npos);
    EXPECT_NE(tableRml.find("Worst"), std::string::npos);
    EXPECT_NE(tableRml.find("--"), std::string::npos);

    // Clicking an archived session opens it in the Recap tab and populates the recap card.
    Click(controller, sessionRows[1]);
    controller.Render();

    Rml::Element* card = doc->GetElementById("recap-card");
    ASSERT_NE(card, nullptr);
    EXPECT_NE(card->GetInnerRML().find("-28"), std::string::npos);

    // Toggle "Compare with previous" and verify comparison rows render.
    Rml::ElementList compareBtns;
    doc->QuerySelectorAll(compareBtns, "[data-action='recap-compare']");
    ASSERT_EQ(compareBtns.size(), 1u);
    Click(controller, compareBtns.front());
    controller.Render();
    EXPECT_NE(card->GetInnerRML().find("Compared with"), std::string::npos);

    // Step to the next older session via › button.
    Rml::ElementList nextBtns;
    doc->QuerySelectorAll(nextBtns, "[data-action='recap-next-session']");
    ASSERT_EQ(nextBtns.size(), 1u);
    Click(controller, nextBtns.front());
    controller.Render();
    EXPECT_NE(card->GetInnerRML().find("-12"), std::string::npos);
    Rml::Element* stepLabel = doc->QuerySelector(".recap-stepper .label.mono");
    ASSERT_NE(stepLabel, nullptr);
    EXPECT_EQ(stepLabel->GetInnerRML(), "3 / 8");

    Rml::ElementList prevBtns;
    doc->QuerySelectorAll(prevBtns, "[data-action='recap-prev-session']");
    ASSERT_EQ(prevBtns.size(), 1u);
    EXPECT_FALSE(prevBtns.front()->HasAttribute("disabled"));
    Click(controller, prevBtns.front());
    controller.Render();
    EXPECT_EQ(stepLabel->GetInnerRML(), "2 / 8");

    for (int i = 0; i < 10; ++i) {
        Click(controller, nextBtns.front());
        controller.Render();
    }
    EXPECT_EQ(stepLabel->GetInnerRML(), "8 / 8");
    EXPECT_TRUE(nextBtns.front()->HasAttribute("disabled"));
    EXPECT_FALSE(prevBtns.front()->HasAttribute("disabled"));
}

TEST_F(RmlUiControllerStateTest, HistoryViewOpensClosesIgnoresStaleResponsesAndOpensDetail) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    const std::string me = "Steam|hist_ui_user";
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = me;
    });

    for (int i = 0; i < 2; ++i) {
        MatchSaveSnapshot snap;
        snap.arenaName = i == 0 ? "DFH Stadium" : "Mannfield";
        snap.matchGuid = "ui-hist-guid-" + std::to_string(i + 1);
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = i == 0 ? 0 : 1;
        snap.validResult = true;
        snap.score[0] = i == 0 ? 3 : 1;
        snap.score[1] = i == 0 ? 1 : 2;
        snap.endedAtUnixMs = 1'700'100'000'000LL + i * 600'000LL;
        snap.durationSeconds = 300.0f;
        snap.myPrimaryId = me;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Me", .team = 0, .mmr = 1200 + i * 9, .goals = 2, .saves = 1, .shots = 4};
        snap.roster["Epic|zylo"] = PlayerData{.primaryId = "Epic|zylo", .name = "Zylo", .team = 0, .mmr = 1190, .goals = 1, .assists = 1};
        snap.roster["PS4|opp1"] = PlayerData{.primaryId = "PS4|opp1", .name = "Opp1", .team = 1, .mmr = 1205, .goals = 1};
        snap.roster["XboxOne|opp2"] = PlayerData{.primaryId = "XboxOne|opp2", .name = "Opp2", .team = 1, .mmr = 1195, .saves = 2};
        db->SaveMatch(snap);
    }

    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    EXPECT_FALSE(controller.WantsAttention());
    EXPECT_FALSE(IsHistoryVisible(controller));

    // 1. Open History view; overlay stays drawn and interactive while open
    controller.OpenHistory();
    EXPECT_TRUE(IsHistoryVisible(controller));
    EXPECT_TRUE(controller.WantsAttention());
    EXPECT_TRUE(controller.WantsInteraction());

    db->AsyncSetSetting("ui_hist_barrier_1", "done");
    for (int i = 0; i < 200 && db->GetSetting("ui_hist_barrier_1", "") != "done"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_EQ(HistoryModel(controller).TotalCount(), 2);
    ASSERT_EQ(HistoryModel(controller).Rows().size(), 2u);
    EXPECT_EQ(HistoryModel(controller).Rows()[0].arena, "Mannfield");
    EXPECT_EQ(HistoryModel(controller).Rows()[0].mmr_delta, "+9");
    EXPECT_EQ(HistoryDocument(controller)->QuerySelector(".history-count"), nullptr);
    auto* headerSubtitle = HistoryDocument(controller)->QuerySelector(".view-header .label");
    ASSERT_NE(headerSubtitle, nullptr);
    EXPECT_EQ(headerSubtitle->GetInnerRML(), "2 matches");

    // 2. Stale response with older requestId must be ignored
    const uint64_t activeReq = HistoryModel(controller).ActiveRequestId();
    ASSERT_GT(activeReq, 0u);
    MatchRow fakeStaleRow;
    fakeStaleRow.matchId = 999;
    fakeStaleRow.arena = "StaleArena";
    EXPECT_FALSE(HistoryModel(controller).ApplyQueryResult(activeReq - 1, {fakeStaleRow}, 1));
    EXPECT_EQ(HistoryModel(controller).TotalCount(), 2);
    EXPECT_EQ(HistoryModel(controller).Rows()[0].arena, "Mannfield");

    state->historyView.PublishQueryResult(activeReq - 1, {fakeStaleRow}, 1, false);
    controller.Update(Config::Read(), false);
    EXPECT_EQ(HistoryModel(controller).TotalCount(), 2);
    EXPECT_EQ(HistoryModel(controller).Rows()[0].arena, "Mannfield");

    // 3. Click a row in HistoryDocument to open Match Detail modal
    Rml::ElementList rowElements;
    ASSERT_NE(HistoryDocument(controller), nullptr);
    HistoryDocument(controller)->QuerySelectorAll(rowElements, ".history-row.item");
    ASSERT_FALSE(rowElements.empty());
    Click(controller, rowElements.front());

    db->AsyncSetSetting("ui_hist_barrier_2", "done");
    for (int i = 0; i < 200 && db->GetSetting("ui_hist_barrier_2", "") != "done"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_TRUE(HistoryModel(controller).IsDetailOpen());
    ASSERT_TRUE(HistoryModel(controller).Detail().has_value());
    EXPECT_EQ(HistoryModel(controller).Detail()->matchId, 2);
    EXPECT_NE(HistoryDocument(controller)->GetElementById("history-detail-modal"), nullptr);

    const std::string summaryText = HistoryModel(controller).FormatScoreboardText();
    EXPECT_NE(summaryText.find("Mannfield"), std::string::npos);
    EXPECT_NE(summaryText.find("1200 -> 1209 (+9)"), std::string::npos);
    EXPECT_NE(summaryText.find("Zylo"), std::string::npos);
    EXPECT_NE(summaryText.find("With 1-0"), std::string::npos);
    EXPECT_NE(summaryText.find("Vs 1-0"), std::string::npos);

    // 4. First Escape closes detail modal; a later filter query must not reopen it; second Escape closes History view
    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
    EXPECT_FALSE(HistoryModel(controller).IsDetailOpen());
    EXPECT_TRUE(IsHistoryVisible(controller));

    auto* lossFilter = HistoryDocument(controller)->QuerySelector("[data-action='history-filter'][data-result='loss']");
    ASSERT_NE(lossFilter, nullptr);
    Click(controller, lossFilter);
    db->AsyncSetSetting("ui_hist_barrier_3", "done");
    for (int i = 0; i < 200 && db->GetSetting("ui_hist_barrier_3", "") != "done"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(HistoryModel(controller).TotalCount(), 1);
    EXPECT_FALSE(HistoryModel(controller).IsDetailOpen());

    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
    EXPECT_FALSE(IsHistoryVisible(controller));
    EXPECT_FALSE(controller.WantsAttention());

    // 5. Previous Games widget has "View all" and row click action, and Match Summary has "Details"
    state->history.Mutate([](auto& history) {
        SessionMatchSummary recent;
        recent.matchId = 2;
        recent.matchGuid = "ui-hist-guid-2";
        recent.mode = "Doubles";
        recent.ranked = true;
        recent.ourScore = 1;
        recent.theirScore = 2;
        recent.mmr = 1209;
        recent.win = false;
        recent.endedAtUnix = 1'700'100'600LL;
        history.recentSavedMatches = {recent};
        history.recentSavedMatchesLoaded = true;
    });
    controller.Update(Config::Read(), false);
    const std::string prevGamesHtml = RenderPreviousGames(controller);
    EXPECT_NE(prevGamesHtml.find("previous-games-view-all"), std::string::npos);
    EXPECT_NE(prevGamesHtml.find("data-action='history-open' data-match-id='2'"), std::string::npos);

    state->liveMatch->Update([](MatchState& ms) {
        ms.matchGuid = "ui-hist-guid-2";
        ms.lastMatchWasVoid = false;
        ms.matchSummaryMyTeam = 0;
        ms.matchSummaryWinnerTeam = 1;
        ms.matchSummaryScore = {1, 2};
    });
    state->ui.showMatchSummary.store(true);
    state->ui.matchSummaryStartMs.store(RmlUiDetail::SteadyNowMs());
    controller.Update(Config::Read(), true);
    controller.Render();
    ASSERT_NE(OverlayRoot(controller), nullptr);
    auto* detailsBtn = OverlayRoot(controller)->QuerySelector("[data-action='history-open'][data-source='match-summary']");
    ASSERT_NE(detailsBtn, nullptr);
    Click(controller, detailsBtn);

    db->AsyncSetSetting("ui_hist_barrier_4", "done");
    for (int i = 0; i < 200 && db->GetSetting("ui_hist_barrier_4", "") != "done"; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    controller.Update(Config::Read(), false);
    EXPECT_TRUE(IsHistoryVisible(controller));
    EXPECT_TRUE(HistoryModel(controller).IsDetailOpen());
    ASSERT_TRUE(HistoryModel(controller).Detail().has_value());
    EXPECT_EQ(HistoryModel(controller).Detail()->matchId, 2);
    controller.OpenHistory();
    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
}

TEST_F(RmlUiControllerStateTest, HistoryLoadMoreAppendsNextPageOnce) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    const std::string me = "Steam|hist_more_user";
    state->liveMatch->Update([&](MatchState& ms) { ms.myPrimaryId = me; });
    const int matchCount = HistoryView::kPageSize + 1;
    for (int i = 0; i < matchCount; ++i) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "hist-more-" + std::to_string(i);
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 2;
        snap.score[1] = 1;
        snap.endedAtUnixMs = 1'700'100'000'000LL + i * 600'000LL;
        snap.myPrimaryId = me;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Me", .team = 0, .mmr = 1200};
        db->SaveMatch(snap);
    }

    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    const auto settle = [&](const char* barrier) {
        db->AsyncSetSetting(barrier, "done");
        for (int i = 0; i < 200 && db->GetSetting(barrier, "") != "done"; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        controller.Update(Config::Read(), false);
        controller.Render();
    };

    controller.OpenHistory();
    settle("hist_more_barrier_1");
    ASSERT_EQ(HistoryModel(controller).Rows().size(), static_cast<size_t>(HistoryView::kPageSize));

    auto* more = HistoryDocument(controller)->QuerySelector("[data-action='history-more']");
    ASSERT_NE(more, nullptr);
    Click(controller, more);
    Click(controller, more);
    settle("hist_more_barrier_2");
    EXPECT_EQ(HistoryModel(controller).Rows().size(), static_cast<size_t>(matchCount));

    Rml::ElementList rowElements;
    HistoryDocument(controller)->QuerySelectorAll(rowElements, ".history-row.item");
    ASSERT_FALSE(rowElements.empty());
    Click(controller, rowElements.front());
    settle("hist_more_barrier_3");
    EXPECT_TRUE(HistoryModel(controller).IsDetailOpen());
    EXPECT_EQ(HistoryModel(controller).Rows().size(), static_cast<size_t>(matchCount));
}

TEST_F(RmlUiControllerStateTest, HistoryFieldHoverConnectsOnlyScoringRoundAndRotatesForOrange) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.imperial_units = false; }, true);
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.OpenHistory();

    MatchDetail detail;
    detail.found = true;
    detail.accountPrimaryId = "Steam|map";
    detail.arena = "DFH Stadium";
    detail.playlist = "2v2";
    detail.ourScore = 3;
    detail.theirScore = 1;
    detail.hasPlayerStats = true;
    detail.durationSeconds = 100.0f;
    MatchDetailPlayer local;
    local.primaryId = detail.accountPrimaryId;
    local.name = "Map player";
    local.team = 1;
    local.isMe = true;
    detail.ourTeam.push_back(local);
    detail.analytics.emplace();
    auto& analytics = *detail.analytics;
    analytics.playSeconds = 100.0f;
    PlayerAnalytics player;
    player.primaryId = local.primaryId;
    player.name = local.name;
    player.team = 1;
    analytics.players.push_back(player);
    analytics.hits = {
        {10.0f, local.primaryId, 1, {2048.0f, 2560.0f, 200.0f}, 60.0f},
        {25.0f, local.primaryId, 1, {-2048.0f, -2560.0f, 400.0f}, 120.0f},
        {55.0f, local.primaryId, 1, {1000.0f, -1000.0f, 100.0f}, 80.0f},
        {80.0f, "Steam|opponent", 0, {2000.0f, 3000.0f, 100.0f}, 90.0f}};
    GoalRecord first;
    first.playSeconds = 30.0f;
    first.clockSeconds = 270.0f;
    first.team = 1;
    first.scorerId = local.primaryId;
    first.scorerName = local.name;
    first.assisterName = "Teammate";
    first.goalSpeed = 110.0f;
    first.location = FieldPoint{100.0f, -5300.0f, 200.0f};
    first.scoreAfter = {0, 1};
    analytics.goals.push_back(first);
    auto missingImpact = first;
    missingImpact.playSeconds = 60.0f;
    missingImpact.location.reset();
    analytics.goals.push_back(missingImpact);
    auto opponentGoal = first;
    opponentGoal.playSeconds = 90.0f;
    opponentGoal.team = 0;
    opponentGoal.scorerId = "Steam|opponent";
    opponentGoal.scorerName = "Opponent";
    opponentGoal.location = FieldPoint{-200.0f, 5300.0f, 200.0f};
    opponentGoal.scoreAfter = {1, 2};
    analytics.goals.push_back(opponentGoal);
    auto noRoundTouch = first;
    noRoundTouch.playSeconds = 100.0f;
    noRoundTouch.location = FieldPoint{-600.0f, -5300.0f, 200.0f};
    analytics.goals.push_back(noRoundTouch);

    auto& view = HistoryModel(controller);
    ASSERT_TRUE(view.ApplyDetailResult(view.BeginDetail(), detail));
    view.SetDetailTab("field");
    controller.Render();
    auto* doc = HistoryDocument(controller);
    Rml::ElementList goals;
    doc->QuerySelectorAll(goals, ".pitch-goal-event");
    ASSERT_EQ(goals.size(), 3u);
    auto* path = goals[0]->QuerySelector(".pitch-goal-path");
    auto* goal = goals[0]->QuerySelector(".pitch-goal");
    auto* tooltip = goals[0]->QuerySelector(".pitch-tooltip");
    ASSERT_NE(path, nullptr);
    ASSERT_NE(goal, nullptr);
    ASSERT_NE(tooltip, nullptr);
    float tx = 0, ty = 0, gx = 0, gy = 0;
    ASSERT_EQ(std::sscanf(path->GetAttribute<Rml::String>("points", "").c_str(), "%f,%f;%f,%f", &tx, &ty, &gx, &gy), 4);
    EXPECT_FLOAT_EQ(tx, 0.75f);
    EXPECT_FLOAT_EQ(ty, 0.25f);
    EXPECT_NEAR(gx, 0.4878f, 0.0001f);
    EXPECT_FLOAT_EQ(gy, 0.02f);
    EXPECT_NE(goals[1]->QuerySelector(".pitch-goal-path"), nullptr);
    auto* absentPath = goals[2]->QuerySelector(".pitch-goal-path");
    ASSERT_NE(absentPath, nullptr);
    EXPECT_EQ(absentPath->GetComputedValues().display(), Rml::Style::Display::None);
    EXPECT_EQ(path->GetComputedValues().visibility(), Rml::Style::Visibility::Hidden);
    EXPECT_EQ(tooltip->GetComputedValues().display(), Rml::Style::Display::None);
    const auto hover = [&](Rml::Element* element) {
        const auto at = element->GetAbsoluteOffset() + Rml::Vector2f(8.0f, 8.0f);
        RmlContext(controller)->ProcessMouseMove(static_cast<int>(at.x), static_cast<int>(at.y), 0);
        controller.Render();
    };
    const auto capture = [&](const char* filename) {
        if (const char* dir = std::getenv("OMNISTATS_CAPTURE_DIR"); dir && *dir) {
            std::filesystem::create_directories(dir);
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            warp.context->ClearRenderTargetView(warp.target.Get(), clear);
            controller.Render();
            std::string error;
            EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1280, 900, 0.0f,
                                                                (std::filesystem::path(dir) / filename).wstring(), error))
                << error;
        }
    };
    hover(goal);
    EXPECT_EQ(path->GetComputedValues().visibility(), Rml::Style::Visibility::Visible);
    EXPECT_NE(tooltip->GetComputedValues().display(), Rml::Style::Display::None);
    capture("history-goal-hover.png");
    Rml::ElementList touches;
    doc->QuerySelectorAll(touches, ".pitch-dot");
    ASSERT_EQ(touches.size(), 3u);
    hover(touches[0]);
    EXPECT_EQ(path->GetComputedValues().visibility(), Rml::Style::Visibility::Hidden);
    auto* touchTooltip = touches[0]->QuerySelector(".pitch-tooltip");
    ASSERT_NE(touchTooltip, nullptr);
    EXPECT_NE(touchTooltip->GetComputedValues().display(), Rml::Style::Display::None);
    const auto displayedSpeed = [](Rml::Element* element) {
        int value = -1;
        char unit[16]{};
        std::sscanf(element->GetInnerRML().c_str(), "%d %15s", &value, unit);
        return std::pair{value, std::string(unit)};
    };
    EXPECT_EQ(displayedSpeed(touches[0]->QuerySelector(".pitch-tooltip-stat")), (std::pair{60, std::string("km/h")}));
    capture("history-touch-hover.png");
    Config::Update([](ConfigData& c) { c.imperial_units = true; }, true);
    ASSERT_TRUE(view.ApplyDetailResult(view.ActiveDetailRequestId(), detail));
    controller.Render();
    auto* speed = doc->QuerySelector(".pitch-dot .pitch-tooltip-stat");
    ASSERT_NE(speed, nullptr);
    EXPECT_EQ(displayedSpeed(speed), (std::pair{37, std::string("mph")}));
}

TEST_F(RmlUiControllerStateTest, HistoryViewSetSeasonOptionsAndBuildQueryBoundaries) {
    HistoryView view;

    const std::vector<SeasonRecord> seasons = {
        {22, 1'000'000LL, 1'900'000LL, "rocket_league"},
        {23, 2'000'000LL, 2'800'000LL, "rocket_league"},
        {24, 3'000'000LL, 4'000'000LL, "rocket_league"},
    };

    EXPECT_FALSE(view.SetSeasonOptions(true, seasons));

    const int64_t nowUnix = 3'500'000LL; // inside season 24

    view.SetDateFilter("this_season");
    EXPECT_EQ(view.DateFilter(), "this_season");
    const auto qThis = view.BuildQuery(0, 50, nowUnix);
    EXPECT_EQ(qThis.fromUnix, 3'000'000LL);
    EXPECT_EQ(qThis.toUnix, 4'000'000LL);

    view.SetDateFilter("last_season");
    EXPECT_EQ(view.DateFilter(), "last_season");
    const auto qLast = view.BuildQuery(0, 50, nowUnix);
    EXPECT_EQ(qLast.fromUnix, 2'000'000LL);
    EXPECT_EQ(qLast.toUnix, 2'999'999LL);

    const auto qUnknown = view.BuildQuery(0, 50, 500'000LL);
    EXPECT_EQ(qUnknown.fromUnix, 1);
    EXPECT_EQ(qUnknown.toUnix, 1);

    view.SetDateFilter("this_season");
    EXPECT_TRUE(view.SetSeasonOptions(false, {}));
    EXPECT_EQ(view.DateFilter(), "all");

    EXPECT_FALSE(view.SetSeasonOptions(true, seasons));
    view.SetDateFilter("last_season");
    EXPECT_TRUE(view.SetSeasonOptions(false, {}));
    EXPECT_EQ(view.DateFilter(), "all");

    EXPECT_FALSE(view.SetSeasonOptions(true, seasons));
    view.SetDateFilter("7d");
    EXPECT_FALSE(view.SetSeasonOptions(false, {}));
    EXPECT_EQ(view.DateFilter(), "7d");
}

TEST_F(RmlUiControllerStateTest, HistoryBadgesStayInsideTheirColumnWithoutOverlapping) {
    for (const auto [width, height] : {std::pair{960, 1032}, std::pair{800, 600}}) {
        WarpDevice warp;
        if (!warp.Create(width, height)) GTEST_SKIP() << "WARP not available.";
        auto state = std::make_shared<SessionState>();
        state->liveMatch->Update([](MatchState& ms) { ms.myPrimaryId = "Steam|badges"; });
        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), width, height, 1.0f));
        controller.Update(Config::Read(), false);
        controller.OpenHistory();
        std::vector<MatchRow> rows;
        const auto row = [&](bool mvp, bool overtime, bool comeback, bool blownLead, bool lateWinner, bool endedEarly) {
            MatchRow match;
            match.matchId = static_cast<int64_t>(rows.size() + 1);
            match.playlist = "2v2";
            match.arena = "Utopia Coliseum (Dusk)";
            match.teammates = {"OverallHawk1081"};
            match.mvp = mvp;
            match.overtime = overtime;
            match.endedEarly = endedEarly;
            ScorelineSummary scoreline;
            scoreline.comeback = comeback;
            scoreline.blownLead = blownLead;
            scoreline.lateWinner = lateWinner;
            match.scoreline = scoreline;
            rows.push_back(match);
        };
        row(true, false, true, false, false, false);
        row(false, true, false, true, false, false);
        row(false, false, true, false, false, true);
        row(false, false, true, false, true, false);
        row(true, true, true, false, true, true);
        auto& history = HistoryModel(controller);
        ASSERT_TRUE(history.ApplyQueryResult(history.ActiveRequestId(), rows, static_cast<int>(rows.size())));
        controller.Render();
        controller.Render();

        Rml::ElementList cells;
        HistoryDocument(controller)->QuerySelectorAll(cells, ".history-row.item .history-cell-badges");
        ASSERT_EQ(cells.size(), rows.size());
        for (auto* cell : cells) {
            const auto cellLeft = cell->GetAbsoluteOffset(Rml::BoxArea::Border).x;
            const auto cellRight = cellLeft + cell->GetOffsetWidth();
            Rml::ElementList badges;
            cell->QuerySelectorAll(badges, ".badge");
            badges.erase(std::remove_if(badges.begin(), badges.end(), [](Rml::Element* badge) { return !badge->IsVisible(); }), badges.end());
            ASSERT_FALSE(badges.empty());
            for (size_t i = 0; i < badges.size(); ++i) {
                const auto a = badges[i]->GetAbsoluteOffset(Rml::BoxArea::Border);
                EXPECT_GE(a.x + 0.5f, cellLeft) << width;
                EXPECT_LE(a.x + badges[i]->GetOffsetWidth(), cellRight + 0.5f) << width;
                for (size_t j = i + 1; j < badges.size(); ++j) {
                    const auto b = badges[j]->GetAbsoluteOffset(Rml::BoxArea::Border);
                    const bool separate = a.x + badges[i]->GetOffsetWidth() <= b.x + 0.5f || b.x + badges[j]->GetOffsetWidth() <= a.x + 0.5f ||
                                          a.y + badges[i]->GetOffsetHeight() <= b.y + 0.5f || b.y + badges[j]->GetOffsetHeight() <= a.y + 0.5f;
                    EXPECT_TRUE(separate) << width << ": " << badges[i]->GetInnerRML() << " overlaps " << badges[j]->GetInnerRML();
                }
            }
        }
    }
}
