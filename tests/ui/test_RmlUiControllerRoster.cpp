#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, HeadlessUpdateHandlesEmptyAndMalformedRoster) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);

    ConfigData config = Config::Read();
    state->ui.showOverlay.store(true);
    EXPECT_NO_THROW(controller.Update(config));

    state->liveMatch->Update([](MatchState& ms) {
        ms.inMatch = true;
        ms.myPrimaryId = "steam|111";

        PlayerData local;
        local.primaryId = "steam|111";
        local.name = "\x80\x81\x82\xFE\xFF";
        local.team = 0;
        local.mmr = 1200;
        ms.roster[local.primaryId] = local;

        PlayerData opponent;
        opponent.primaryId = "epic|222";
        opponent.name = "Player\xC0\xAF\xE0\x80";
        opponent.team = 1;
        opponent.mmr = 1100;
        ms.roster[opponent.primaryId] = opponent;
    });

    EXPECT_NO_THROW(controller.Update(config));
}

TEST_F(RmlUiControllerStateTest, RosterKeepsLegacyPlatformLabelsAndBotsNonClickable) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        PlayerData bot;
        bot.primaryId = "Unknown|Bot_01";
        bot.name = "Merlin";
        bot.team = 0;
        bot.fetched = true;
        ms.roster[bot.primaryId] = bot;
    });

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    EXPECT_NE(html.find(">BOT</span>"), std::string::npos);
    EXPECT_EQ(html.find(">Unknown</span>"), std::string::npos);
    EXPECT_EQ(html.find("data-action='open-player-tracker'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterSeparatesUnloadedFirstMeetingAndBothEncounterRecords) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        ms.myPrimaryId = "Steam|self";

        const auto add = [&](const std::string& id, const std::string& name, bool loaded, int ww, int lw, int wa, int la) {
            PlayerData p;
            p.primaryId = id;
            p.name = name;
            p.team = 0;
            p.fetched = true;
            p.hasLifetimeData = loaded;
            p.lifetimeWinsWith = ww;
            p.lifetimeLossesWith = lw;
            p.lifetimeWinsAgainst = wa;
            p.lifetimeLossesAgainst = la;
            ms.roster[id] = p;
        };
        add("Steam|self", "Self", false, 0, 0, 0, 0);
        add("Epic|pending", "StillLoading", false, 0, 0, 0, 0);
        add("Epic|new", "FirstEncounter", true, 0, 0, 0, 0);
        add("Epic|both", "BothSides", true, 5, 3, 4, 2);
    });

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);
    const auto rowFor = [&](const std::string& name) {
        const size_t start = html.find(name);
        if (start == std::string::npos) return std::string{};
        const size_t end = html.find("player-row", start);
        return html.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    const std::string pending = rowFor("StillLoading");
    ASSERT_FALSE(pending.empty());
    EXPECT_EQ(pending.find("NEW"), std::string::npos);

    const std::string first = rowFor("FirstEncounter");
    ASSERT_FALSE(first.empty());
    EXPECT_NE(first.find(">NEW<"), std::string::npos);

    const std::string both = rowFor("BothSides");
    ASSERT_FALSE(both.empty());
    EXPECT_NE(both.find("with 5-3"), std::string::npos);
    EXPECT_NE(both.find("vs 4-2"), std::string::npos);
    EXPECT_NE(both.find("8 matches · 5W 3L · 63%"), std::string::npos);
    EXPECT_NE(both.find("6 matches · 4W 2L · 67%"), std::string::npos);
    EXPECT_EQ(both.find(">NEW<"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterSortFallsBackToBestMmrWhenSelectedPlaylistIsMissing) {
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    {
        state->liveMatch->Update([](MatchState& ms) {
            PlayerData fallback;
            fallback.primaryId = "Unknown|Fallback";
            fallback.name = "FallbackHigh";
            fallback.team = 0;
            fallback.mmr = 1500;
            fallback.fetched = true;
            ms.roster[fallback.primaryId] = fallback;

            PlayerData selected;
            selected.primaryId = "Steam|123";
            selected.name = "SelectedLow";
            selected.team = 0;
            selected.mmr = 1200;
            selected.playlists["2v2"] = 1200;
            selected.playlistTiers["2v2"] = "Champion I Division I";
            selected.fetched = true;
            ms.roster[selected.primaryId] = selected;
        });
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    const size_t fallbackPos = html.find("FallbackHigh");
    const size_t selectedPos = html.find("SelectedLow");
    ASSERT_NE(fallbackPos, std::string::npos);
    ASSERT_NE(selectedPos, std::string::npos);
    EXPECT_LT(fallbackPos, selectedPos);
}

TEST_F(RmlUiControllerStateTest, LobbyRanksKeepsEnabledPlaylistColumnHeaders) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.LobbyRankColumn(MmrCategory::OneVOne) = true;
    config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
    config.LobbyRankColumn(MmrCategory::ThreeVThree) = false;
    config.LobbyRankColumn(MmrCategory::Casual) = true;
    config.LobbyRankColumn(MmrCategory::Tourny) = true;
    config.show_extra_playlists = false;

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string html = RenderLobbyRanks(controller);

    EXPECT_NE(html.find("lobby-rank-header"), std::string::npos);
    EXPECT_NE(html.find(">1v1</div>"), std::string::npos);
    EXPECT_NE(html.find(">2v2</div>"), std::string::npos);
    EXPECT_NE(html.find(">Casual</div>"), std::string::npos);
    EXPECT_NE(html.find(">Tourney</div>"), std::string::npos);
    EXPECT_EQ(html.find(">3v3</div>"), std::string::npos);
    EXPECT_EQ(html.find(">Hoops</div>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RankWidgetsExplainMissingRankSourceUntilOneIsEnabled) {
    auto state = std::make_shared<SessionState>();
    {
        state->liveMatch->Update([](MatchState& ms) {
            PlayerData player;
            player.primaryId = "Epic|unranked";
            player.name = "NoSource";
            player.team = 0;
            ms.roster[player.primaryId] = player;
        });
    }
    ConfigData config = Config::Read();
    config.enable_mmr_tracking = false;
    config.custom_api_enabled = true;

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string dashboardRoster = RenderWidgetHtml(controller, DashboardLayout::WidgetId::LiveRoster, true);
    EXPECT_NE(dashboardRoster.find("rank-source-hint"), std::string::npos);
    EXPECT_NE(dashboardRoster.find("data-action='open-rank-settings'"), std::string::npos);
    EXPECT_EQ(dashboardRoster.find("Fetching"), std::string::npos);
    EXPECT_EQ(RenderWidgetHtml(controller, DashboardLayout::WidgetId::LiveRoster, false).find("open-rank-settings"),
              std::string::npos);
    const std::string lobby = RenderWidgetHtml(controller, DashboardLayout::WidgetId::LobbyRanks, true);
    EXPECT_NE(lobby.find("rank-source-hint"), std::string::npos);
    EXPECT_EQ(lobby.find("Fetching rank..."), std::string::npos);

    config.enable_mmr_tracking = true;
    controller.Update(config);
    EXPECT_EQ(RenderWidgetHtml(controller, DashboardLayout::WidgetId::LiveRoster, true).find("rank-source-hint"),
              std::string::npos);
    EXPECT_NE(RenderLobbyRanks(controller).find("Fetching rank..."), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterBestUsesUnderlyingPlaylistMatchCountWhenSyntheticBestIsMissing) {
    auto state = std::make_shared<SessionState>();
    {
        state->liveMatch->Update([](MatchState& ms) {
            PlayerData player;
            player.primaryId = "Epic|test1";
            player.name = "TestPlayer";
            player.team = 0;
            player.mmr = 1250;
            player.rankTier = "Diamond II Div III";
            player.playlists["2v2"] = 1250;
            player.playlistTiers["2v2"] = "Diamond II Div III";
            player.playlistMatches["2v2"] = 45;
            player.fetched = true;
            ms.roster[player.primaryId] = player;
        });
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    const std::string rosterHtml = RenderRoster(controller);
    EXPECT_NE(rosterHtml.find("<span class='chip'>45 matches</span>"), std::string::npos);

    const std::string lobbyHtml = RenderLobbyRanks(controller);
    EXPECT_NE(lobbyHtml.find("<span class='lobby-rank-matches'>(45)</span>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, LobbyRanksCalculatesRanksPerPlaylistForUnrankedPlacementPlayers) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        PlayerData player;
        player.primaryId = "Epic|test1";
        player.name = "TestPlayer";
        player.playlists["casual"] = 698;
        player.playlistTiers["casual"] = "Unranked";
        player.playlists["1v1"] = 586;
        player.playlistTiers["1v1"] = "Unranked";
        player.playlists["2v2"] = 1114;
        player.playlistTiers["2v2"] = "Champion I Div II";
        player.playlists["3v3"] = 699;
        player.playlistTiers["3v3"] = "Unranked";
        player.playlists["t"] = 1122;
        player.playlistTiers["t"] = "Unranked";
        player.fetched = true;
        ms.roster[player.primaryId] = player;
    });

    ConfigData config = Config::Read();
    config.LobbyRankColumn(MmrCategory::OneVOne) = true;
    config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
    config.LobbyRankColumn(MmrCategory::ThreeVThree) = true;
    config.LobbyRankColumn(MmrCategory::Casual) = true;
    config.LobbyRankColumn(MmrCategory::Tourny) = true;
    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string html = RenderLobbyRanks(controller);

    // Rank sits on its own line above the rating, which carries the games played.
    const auto stacked = [](const char* tier, const char* mmr) {
        return std::string("<div class='mono'>") + tier + "</div><div><span class='mono'>" + mmr + "</span>";
    };
    // 2v2 has an actual rank from Tracker Network.
    EXPECT_NE(html.find(stacked("C1.D2", "1114")), std::string::npos);
    // Tourney maps tournament MMR to a tournament tier.
    EXPECT_NE(html.find(stacked("C1.D2", "1122")), std::string::npos);
    // Unranked 1v1 maps MMR to 1v1 specific thresholds (586 is G3.D2 in 1v1).
    EXPECT_NE(html.find(stacked("G3.D2", "586")), std::string::npos);
    // Unranked 3v3 maps MMR to 3v3 specific thresholds (699 is P1.D4 in 3v3).
    EXPECT_NE(html.find(stacked("P1.D4", "699")), std::string::npos);
    // Casual is not a ranked playlist: raw MMR only, no rank line.
    EXPECT_NE(html.find("<span class='mono'>698</span>"), std::string::npos);
    EXPECT_EQ(html.find(stacked("P1.D4", "698")), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, IdentitySelectorExcludesUnknownLobbyIdsButKeepsSavedIdentity) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& ms) {
        PlayerData unknown;
        unknown.primaryId = "Unknown|Bot_01";
        unknown.name = "Bot";
        ms.roster[unknown.primaryId] = unknown;
        PlayerData valid;
        valid.primaryId = "Steam|123";
        valid.name = "Player";
        ms.roster[valid.primaryId] = valid;
    });

    ConfigData config = Config::Read();
    config.last_primary_id.clear();
    config.known_primary_ids = {"Unknown|Old", "Steam|456"};
    RmlUiController controller(state, nullptr);
    controller.Update(config);
    auto values = IdentityValues(controller);
    EXPECT_EQ(std::find(values.begin(), values.end(), "Unknown|Bot_01"), values.end());
    EXPECT_EQ(std::find(values.begin(), values.end(), "Unknown|Old"), values.end());
    EXPECT_NE(std::find(values.begin(), values.end(), "Steam|123"), values.end());
    EXPECT_NE(std::find(values.begin(), values.end(), "Steam|456"), values.end());

    config.last_primary_id = "Unknown|Old";
    controller.Update(config);
    values = IdentityValues(controller);
    EXPECT_NE(std::find(values.begin(), values.end(), "Unknown|Old"), values.end());
}

TEST_F(RmlUiControllerStateTest, LobbyRanksContainerWidthGrowsWithEnabledColumns) {
    auto state = std::make_shared<SessionState>();
    state->ui.showOverlay.store(true);
    RmlUiController controller(state, nullptr);

    OverlayLayout::ContainerConfig container;
    container.id = "lobby_ranks";
    container.widgets = {DashboardLayout::WidgetId::LobbyRanks};

    const auto widthOf = [&](const std::string& html) {
        const auto pos = html.find("width:");
        if (pos == std::string::npos) return -1.0f;
        return std::strtof(html.c_str() + pos + 6, nullptr);
    };

    ConfigData config = Config::Read();
    config.show_lobby_ranks_overlay = true;
    config.show_extra_playlists = false;
    config.LobbyRankColumn(MmrCategory::OneVOne) = true;
    config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
    config.LobbyRankColumn(MmrCategory::ThreeVThree) = true;
    config.LobbyRankColumn(MmrCategory::Casual) = true;
    config.LobbyRankColumn(MmrCategory::Tourny) = true;
    controller.Update(config);
    const float fiveColumns = widthOf(RenderOverlayContainer(controller, container));
    ASSERT_GT(fiveColumns, 0.0f);
    EXPECT_LE(fiveColumns, 520.0f);

    config.LobbyRankColumn(MmrCategory::OneVOne) = false;
    config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
    config.LobbyRankColumn(MmrCategory::ThreeVThree) = false;
    config.LobbyRankColumn(MmrCategory::Casual) = false;
    config.LobbyRankColumn(MmrCategory::Tourny) = false;
    controller.Update(config);
    const float oneColumn = widthOf(RenderOverlayContainer(controller, container));
    EXPECT_LT(oneColumn, fiveColumns);
    EXPECT_LE(oneColumn, 240.0f);

    config.LobbyRankColumn(MmrCategory::OneVOne) = true;
    config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
    config.LobbyRankColumn(MmrCategory::ThreeVThree) = true;
    config.LobbyRankColumn(MmrCategory::Casual) = true;
    config.LobbyRankColumn(MmrCategory::Tourny) = true;

    config.show_extra_playlists = true;
    config.LobbyRankColumn(MmrCategory::Hoops) = true;
    config.LobbyRankColumn(MmrCategory::Rumble) = true;
    config.LobbyRankColumn(MmrCategory::Dropshot) = true;
    config.LobbyRankColumn(MmrCategory::SnowDay) = true;
    config.LobbyRankColumn(MmrCategory::Heatseeker) = true;
    controller.Update(config);
    const float tenColumns = widthOf(RenderOverlayContainer(controller, container));
    EXPECT_GT(tenColumns, fiveColumns) << "ten rank columns must reserve more width than five";

    // Docking other widgets into the lobby container must not shrink the rank
    // table below the width its columns need.
    OverlayLayout::ContainerConfig merged = container;
    merged.widgets = {DashboardLayout::WidgetId::LobbyRanks, DashboardLayout::WidgetId::DemoTracker,
                      DashboardLayout::WidgetId::SessionStats};
    const float mergedWidth = widthOf(RenderOverlayContainer(controller, merged));
    EXPECT_GE(mergedWidth, fiveColumns) << "merged container clipped the lobby rank columns";
}

TEST_F(RmlUiControllerStateTest, RosterMmrChipShowsOnlyMmrWhenRankIconsEnabled) {
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    state->liveMatch->Update([](MatchState& ms) {
        PlayerData player;
        player.primaryId = "Epic|test1";
        player.name = "TestPlayer";
        player.team = 0;
        player.mmr = 1114;
        player.playlists["2v2"] = 1114;
        player.playlistTiers["2v2"] = "Champion I Division II";
        player.fetched = true;
        ms.roster[player.primaryId] = player;
    });

    ConfigData config = Config::Read();
    config.use_rank_icons = true;
    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string html = RenderRoster(controller);

    EXPECT_NE(html.find("chip-mmr"), std::string::npos);
    EXPECT_NE(html.find(">1114</span>"), std::string::npos);
    EXPECT_EQ(html.find("C1.D2"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterShowsEscapedNoteBadgeOnlyForStableIdentities) {
    const std::string otherId = "Epic|fedcba9876543210fedcba9876543210|0";
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = kSelfId;
        for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
                 {kSelfId, "Me"}, {kRivalId, "Rival"}, {otherId, "Stranger"}, {"Unknown|Bot", "Bot"}}) {
            PlayerData p;
            p.primaryId = id;
            p.name = name;
            p.team = 0;
            p.fetched = true;
            ms.roster[id] = p;
        }
    });
    {
        PlayerNotes::Note note;
        note.identity = *PlayerNotes::IdentityFromPrimaryId(kRivalId);
        note.text = "Toxic <b>{{match_saves}}</b>\nreports";
        std::unordered_map<std::string, PlayerNotes::Note> notesMap;
        notesMap[note.identity.Key()] = note;
        state->notes.PublishLoaded(false, std::move(notesMap), {});
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    const size_t rival = html.find(">Rival<");
    const size_t stranger = html.find(">Stranger<");
    const size_t bot = html.find(">Bot<");
    ASSERT_NE(rival, std::string::npos);
    ASSERT_NE(stranger, std::string::npos);
    ASSERT_NE(bot, std::string::npos);
    EXPECT_NE(html.find("note-badge", rival), std::string::npos);
    EXPECT_NE(html.find("Toxic &lt;b&gt;&#123;&#123;match_saves&#125;&#125;&lt;/b&gt; reports"), std::string::npos);
    EXPECT_EQ(html.find("{{match_saves}}"), std::string::npos);
    EXPECT_EQ(html.find("Toxic <b>"), std::string::npos);

    const auto rowOf = [&](size_t start) { return html.substr(start, html.find("player-row", start) - start); };
    EXPECT_NE(rowOf(stranger).find("note-add"), std::string::npos);
    EXPECT_EQ(rowOf(stranger).find("note-badge"), std::string::npos);
    EXPECT_EQ(rowOf(bot).find("player-note-open"), std::string::npos);
    EXPECT_EQ(rowOf(html.find(">Me<")).find("player-note-open"), std::string::npos);

    const std::string lobby = RenderLobbyRanks(controller);
    EXPECT_NE(lobby.find(">Rival<"), std::string::npos);
    EXPECT_EQ(lobby.find("player-note-open"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterShowsEscapedTagChipsWithOverflow) {
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = kSelfId;
        for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{{kSelfId, "Me"}, {kRivalId, "Rival"}}) {
            PlayerData p;
            p.primaryId = id;
            p.name = name;
            p.team = 0;
            p.fetched = true;
            ms.roster[id] = p;
        }
    });
    {
        std::unordered_map<std::string, std::vector<std::string>> tagsMap;
        tagsMap[PlayerNotes::IdentityFromPrimaryId(kRivalId)->Key()] = {"Fast <b>{{x}}</b>", "Ball chaser", "Tilts"};
        tagsMap[PlayerNotes::IdentityFromPrimaryId(kSelfId)->Key()] = {"Me tag"};
        state->notes.PublishLoaded(false, {}, std::move(tagsMap));
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    const size_t rival = html.find(">Rival<");
    ASSERT_NE(rival, std::string::npos);
    const std::string row = html.substr(rival, html.find("player-row", rival) - rival);
    EXPECT_NE(row.find("note-badge"), std::string::npos);
    EXPECT_NE(row.find("chip-tag'>Fast &lt;b&gt;&#123;&#123;x&#125;&#125;&lt;/b&gt;<"), std::string::npos);
    EXPECT_NE(row.find("chip-tag'>Ball chaser<"), std::string::npos);
    EXPECT_EQ(row.find(">Tilts<"), std::string::npos);
    EXPECT_NE(row.find("chip-tag'>+1<"), std::string::npos);
    EXPECT_NE(row.find("Tags: Fast"), std::string::npos);
    EXPECT_EQ(html.find("{{x}}"), std::string::npos);
    EXPECT_EQ(html.find("Me tag"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, OverlayNoteBadgeOpensTheEditorInsteadOfMovingTheCard) {
    WarpDevice warp;
    if (!warp.Create(1920, 1080)) GTEST_SKIP() << "WARP not available.";
    HWND hwnd = CreateWindowExA(0, "STATIC", "test", WS_POPUP, 0, 0, 1920, 1080, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    ASSERT_NE(hwnd, nullptr);
    Config::Update(
        [](ConfigData& c) {
            c.second_monitor_mode = false;
            c.overlay_layout = OverlayLayout::DefaultOverlayLayout();
        },
        true);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = kSelfId;
        PlayerData rival;
        rival.primaryId = kRivalId;
        rival.name = "Rival";
        rival.team = 1;
        rival.fetched = true;
        ms.roster[kRivalId] = rival;
    });
    {
        PlayerNotes::Note note;
        note.identity = *PlayerNotes::IdentityFromPrimaryId(kRivalId);
        note.text = "Rotates well";
        std::unordered_map<std::string, PlayerNotes::Note> notesMap;
        notesMap[note.identity.Key()] = note;
        state->notes.PublishLoaded(false, std::move(notesMap), {});
    }
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(hwnd, warp.device.Get(), warp.context.Get(), 1920, 1080, 1.0f));
    controller.Update(Config::Read());
    controller.Render();

    auto* badge = OverlayRoot(controller)->QuerySelector("[data-container='main_stack'] .note-badge");
    ASSERT_NE(badge, nullptr);
    const Rml::Vector2f center = badge->GetAbsoluteOffset() + Rml::Vector2f(badge->GetOffsetWidth() * 0.5f, badge->GetOffsetHeight() * 0.5f);
    const LPARAM position = MAKELPARAM(static_cast<int>(center.x), static_cast<int>(center.y));
    const auto before = Config::Read().overlay_layout.containers;

    controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, 0, position);
    controller.Render();
    controller.ProcessWindowMessage(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, position);
    controller.ProcessWindowMessage(hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(static_cast<int>(center.x) + 2, static_cast<int>(center.y) + 2));
    controller.ProcessWindowMessage(hwnd, WM_LBUTTONUP, 0, position);
    controller.Render();

    EXPECT_TRUE(IsNoteVisible(controller));
    const auto after = Config::Read().overlay_layout.containers;
    ASSERT_EQ(after.size(), before.size());
    for (size_t i = 0; i < after.size(); ++i) {
        EXPECT_FLOAT_EQ(after[i].x, before[i].x) << after[i].id;
        EXPECT_FLOAT_EQ(after[i].y, before[i].y) << after[i].id;
    }
    DestroyWindow(hwnd);
}

TEST_F(RmlUiControllerStateTest, PlayerNoteEditorSavesEditsDeletesAndGuardsUnsavedChanges) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = kSelfId;
        PlayerData rival;
        rival.primaryId = kRivalId;
        rival.name = "Rival";
        rival.team = 1;
        rival.fetched = true;
        ms.roster[kRivalId] = rival;
    });
    int barrier = 0;
    const auto drain = [&]() {
        const std::string key = "note_ui_barrier_" + std::to_string(++barrier);
        db->AsyncSetSetting(key, "done");
        for (int i = 0; i < 400 && db->GetSetting(key, "") != "done"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };

    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    drain();
    controller.Update(Config::Read(), false);

    OpenNote(controller, kRivalId, "Rival");
    ASSERT_TRUE(IsNoteVisible(controller));
    EXPECT_TRUE(controller.WantsAttention());
    EXPECT_TRUE(controller.WantsInteraction());
    controller.Render();
    ASSERT_NE(NoteDocument(controller), nullptr);
    EXPECT_NE(NoteDocument(controller)->QuerySelector(".player-note-input"), nullptr);
    EXPECT_FALSE(NoteModel(controller).IsDirty());

    // Escape with unsaved text asks first instead of discarding.
    NoteModel(controller).SetDraft("Strong teammate\nrotates well");
    EXPECT_TRUE(NoteModel(controller).IsDirty());
    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
    EXPECT_TRUE(IsNoteVisible(controller));
    EXPECT_TRUE(NoteModel(controller).ConfirmingDiscard());
    EXPECT_TRUE(ViewAction(controller, "player-note-keep-editing"));

    EXPECT_TRUE(ViewAction(controller, "player-note-save"));
    EXPECT_TRUE(NoteModel(controller).IsSaving());
    drain();
    controller.Update(Config::Read(), false);
    EXPECT_FALSE(NoteModel(controller).IsSaving());
    EXPECT_FALSE(NoteModel(controller).IsDirty());
    const auto identity = *PlayerNotes::IdentityFromPrimaryId(kRivalId);
    ASSERT_TRUE(db->GetPlayerNote(identity));
    EXPECT_EQ(db->GetPlayerNote(identity)->text, "Strong teammate\nrotates well");
    EXPECT_NE(RenderRoster(controller, 1).find("note-badge"), std::string::npos);

    EXPECT_TRUE(controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0));
    EXPECT_FALSE(IsNoteVisible(controller));
    OpenNote(controller, kRivalId, "Rival");
    EXPECT_EQ(NoteModel(controller).Draft(), "Strong teammate\nrotates well");

    EXPECT_TRUE(ViewAction(controller, "player-note-delete"));
    EXPECT_TRUE(NoteModel(controller).ConfirmingDelete());
    EXPECT_TRUE(ViewAction(controller, "player-note-confirm-delete"));
    drain();
    controller.Update(Config::Read(), false);
    EXPECT_FALSE(db->GetPlayerNote(identity));
    EXPECT_EQ(RenderRoster(controller, 1).find("note-badge"), std::string::npos);
    EXPECT_TRUE(ViewAction(controller, "player-note-close"));
    EXPECT_FALSE(IsNoteVisible(controller));

    // Bots have no stable identity, so the editor explains instead of saving.
    OpenNote(controller, "Unknown|Bot", "Bot");
    EXPECT_TRUE(IsNoteVisible(controller));
    EXPECT_FALSE(NoteModel(controller).Identity().has_value());
    NoteModel(controller).SetDraft("ignored");
    EXPECT_FALSE(NoteModel(controller).IsDirty());
    EXPECT_FALSE(NoteModel(controller).BeginSave(1000));
}

TEST_F(RmlUiControllerStateTest, DashboardLobbyRanksFitANarrowColumnWithoutScrolling) {
    WarpDevice warp;
    if (!warp.Create(800, 1000)) GTEST_SKIP() << "WARP not available.";
    Config::Update(
        [](ConfigData& c) {
            c.second_monitor_mode = true;
            c.show_lobby_ranks_overlay = true;
            c.LobbyRankColumn(MmrCategory::OneVOne) = c.LobbyRankColumn(MmrCategory::TwoVTwo) = c.LobbyRankColumn(MmrCategory::ThreeVThree) = true;
            c.LobbyRankColumn(MmrCategory::Casual) = c.LobbyRankColumn(MmrCategory::Tourny) = true;
            c.dashboard_layout = DashboardLayout::DefaultLayout();
        },
        true);
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([&](MatchState& ms) {
        ms.myPrimaryId = kSelfId;
        for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{{kSelfId, "SparePrinter948"}, {kRivalId, "AVeryLongOpponentName"}}) {
            PlayerData p;
            p.primaryId = id;
            p.name = name;
            p.fetched = true;
            for (const char* key : {"1v1", "2v2", "3v3", "casual", "t"}) {
                p.playlists[key] = 1749;
                p.playlistMatches[key] = 114;
            }
            ms.roster[id] = p;
        }
    });
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 800, 1000, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    Rml::Element* table = Document(controller)->QuerySelector("[data-widget='lobby-ranks'] .lobby-rank-table");
    ASSERT_NE(table, nullptr);
    EXPECT_LE(table->GetScrollWidth(), table->GetClientWidth() + 1.0f);
    const float tableRight = table->GetAbsoluteLeft() + table->GetClientWidth();
    Rml::ElementList cells;
    table->QuerySelectorAll(cells, ".lobby-rank-cell");
    ASSERT_EQ(cells.size(), 15u);
    for (Rml::Element* cell : cells) {
        EXPECT_GE(cell->GetOffsetWidth(), 44.0f);
        EXPECT_LE(cell->GetAbsoluteLeft() + cell->GetOffsetWidth(), tableRight + 1.0f);
    }
}
