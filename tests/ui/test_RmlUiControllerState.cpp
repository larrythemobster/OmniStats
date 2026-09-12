#include <gtest/gtest.h>
#include <RmlUi/Core/Core.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <limits>
#include <mutex>
#include <shared_mutex>

#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include "core/Storage.hpp"
#include "database/DatabaseManager.hpp"
#include "ui/rml/RmlUiController.hpp"

class RmlUiControllerStateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        original = Config::Read();
        Config::Update([](ConfigData& c) {
            c.last_primary_id.clear();
            c.graph_mmr_category = "2v2";
            c.show_running_indicator = false;
            c.second_monitor_mode = false;
        },
                       true);
    }

    void TearDown() override {
        Config::Update([this](ConfigData& c) { c = original; }, true);
    }

    const RmlRenderSnapshot& Snapshot(const RmlUiController& controller) const {
        return controller.m_snap;
    }
    float DpiScale(const RmlUiController& controller) const {
        return controller.m_dpiScale;
    }
    std::string EscapeText(std::string_view text) const {
        return RmlUiController::Escape(text);
    }
    std::string RenderGraph(RmlUiController& controller) {
        return controller.RenderMmrGraph();
    }
    std::string RenderCompactSession(RmlUiController& controller) {
        return controller.RenderSessionStats(true, false);
    }
    std::string RenderSessionWidget(RmlUiController& controller, bool dashboard) {
        return controller.RenderWidget(DashboardLayout::WidgetId::SessionStats, dashboard);
    }
    std::string RenderGamemodeWidget(RmlUiController& controller, bool dashboard) {
        return controller.RenderWidget(DashboardLayout::WidgetId::GamemodeBreakdown, dashboard);
    }
    std::string RenderFullSession(RmlUiController& controller) {
        return controller.RenderSessionStats(false, true);
    }
    std::string RenderSessionView(RmlUiController& controller) {
        return controller.RenderSessionView();
    }
    std::string RenderCardSettings(RmlUiController& controller) {
        return controller.RenderSettingsCards();
    }
    std::string RenderGeneralSettings(RmlUiController& controller) {
        return controller.RenderSettingsGeneral();
    }
    std::string RenderIntegrationSettings(RmlUiController& controller) {
        return controller.RenderSettingsIntegrations();
    }
    void BeginSaveReplayKeyCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::KeySaveReplay);
    }
    std::string SaveReplayBindTarget() const {
        return std::to_string(static_cast<int>(RmlUiController::BindCaptureTarget::KeySaveReplay));
    }
    std::string RenderRoster(RmlUiController& controller, int team = 0) {
        return controller.RenderPlayerRoster(team, team == 0 ? "BLUE" : "ORANGE");
    }
    std::string RenderLobbyRanks(RmlUiController& controller) {
        return controller.RenderLobbyRanks();
    }
    std::string RenderPreviousGames(RmlUiController& controller) {
        return controller.RenderPreviousGames();
    }
    std::string RenderOverlayContainer(RmlUiController& controller, const OverlayLayout::ContainerConfig& container, bool editMode = false) {
        return controller.RenderOverlayContainer(container, editMode);
    }
    std::string LastConfigFingerprint(const RmlUiController& controller) const {
        return controller.m_lastConfigFingerprint;
    }
    std::string LastSettingsFingerprint(const RmlUiController& controller) const {
        return controller.m_lastSettingsFingerprint;
    }
    bool ValidateStatsPath(std::string input, std::string& normalized, std::string& error) {
        return RmlUiController::ValidateStatsApiPath(std::move(input), normalized, error);
    }
    void BeginOverlayKeyCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::KeyOverlay);
    }
    void BeginCycleControllerCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::GamepadCycle);
    }
    void SetSettingsTransientState(RmlUiController& controller) {
        controller.m_showBallchasingToken = true;
        controller.m_confirmReplayUploads = true;
        controller.m_confirmDeleteHistory = true;
        controller.m_editColorKey = "theme_accent";
    }
    bool HasSettingsTransientState(const RmlUiController& controller) const {
        return controller.m_showBallchasingToken || controller.m_confirmReplayUploads ||
               controller.m_confirmDeleteHistory || !controller.m_editColorKey.empty();
    }
    void SetSettingsPage(RmlUiController& controller, int page) {
        controller.m_settingsPage = static_cast<RmlUiController::SettingsPage>(page);
    }

    ConfigData original;
};

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

TEST_F(RmlUiControllerStateTest, HeadlessUpdateHandlesEmptyAndMalformedRoster) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);

    ConfigData config = Config::Read();
    state->ui.showOverlay.store(true);
    EXPECT_NO_THROW(controller.Update(config));

    {
        std::unique_lock lock(state->game.mutex);
        state->game.inMatch.store(true);
        state->game.myPrimaryId = "steam|111";

        PlayerData local;
        local.primaryId = "steam|111";
        local.name = "\x80\x81\x82\xFE\xFF";
        local.team = 0;
        local.mmr = 1200;
        state->game.roster[local.primaryId] = local;

        PlayerData opponent;
        opponent.primaryId = "epic|222";
        opponent.name = "Player\xC0\xAF\xE0\x80";
        opponent.team = 1;
        opponent.mmr = 1100;
        state->game.roster[opponent.primaryId] = opponent;
        state->game.version.fetch_add(1);
    }

    EXPECT_NO_THROW(controller.Update(config));
}

TEST_F(RmlUiControllerStateTest, RosterKeepsLegacyPlatformLabelsAndBotsNonClickable) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        PlayerData bot;
        bot.primaryId = "Unknown|Bot_01";
        bot.name = "Merlin";
        bot.team = 0;
        bot.fetched = true;
        state->game.roster[bot.primaryId] = bot;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    EXPECT_NE(html.find(">BOT</span>"), std::string::npos);
    EXPECT_EQ(html.find(">Unknown</span>"), std::string::npos);
    EXPECT_EQ(html.find("data-action='open-player-tracker'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterMarksUnknownEncounterHistoryAsNew) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|self";

        PlayerData self;
        self.primaryId = state->game.myPrimaryId;
        self.name = "Self";
        self.team = 0;
        self.fetched = true;
        state->game.roster[self.primaryId] = self;

        PlayerData newcomer;
        newcomer.primaryId = "Epic|new";
        newcomer.name = "FirstEncounter";
        newcomer.team = 0;
        newcomer.fetched = true;
        newcomer.hasLifetimeData = false;
        state->game.roster[newcomer.primaryId] = newcomer;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderRoster(controller);

    const size_t playerPos = html.find("FirstEncounter");
    ASSERT_NE(playerPos, std::string::npos);
    EXPECT_NE(html.find("NEW", playerPos), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, RosterSortFallsBackToBestMmrWhenSelectedPlaylistIsMissing) {
    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    {
        std::unique_lock lock(state->game.mutex);
        PlayerData fallback;
        fallback.primaryId = "Unknown|Fallback";
        fallback.name = "FallbackHigh";
        fallback.team = 0;
        fallback.mmr = 1500;
        fallback.fetched = true;
        state->game.roster[fallback.primaryId] = fallback;

        PlayerData selected;
        selected.primaryId = "Steam|123";
        selected.name = "SelectedLow";
        selected.team = 0;
        selected.mmr = 1200;
        selected.playlists["2v2"] = 1200;
        selected.playlistTiers["2v2"] = "Champion I Division I";
        selected.fetched = true;
        state->game.roster[selected.primaryId] = selected;
        state->game.version.fetch_add(1);
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
    config.show_lobby_rank_1v1 = true;
    config.show_lobby_rank_2v2 = true;
    config.show_lobby_rank_3v3 = false;
    config.show_lobby_rank_casual = true;
    config.show_lobby_rank_tourny = true;
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

TEST_F(RmlUiControllerStateTest, PreviousGamesKeepsSessionRecordWhileHistoryIsLoadingOrEmpty) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        state->game.sessionTotals.wins = 3;
        state->game.sessionTotals.losses = 2;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    std::string html = RenderPreviousGames(controller);
    EXPECT_NE(html.find("Loading saved match history..."), std::string::npos);
    EXPECT_NE(html.find("Current session: <span class='win'>W:3</span> <span class='loss'>L:2</span>"), std::string::npos);

    {
        std::unique_lock lock(state->history.mutex);
        state->history.recentSavedMatchesLoaded = true;
        state->history.version.fetch_add(1);
    }
    controller.Update(Config::Read());
    html = RenderPreviousGames(controller);
    EXPECT_NE(html.find("No saved games in local history."), std::string::npos);
    EXPECT_NE(html.find("Current session: <span class='win'>W:3</span> <span class='loss'>L:2</span>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, GeneratedCheckboxesUseTheStyledNativeCheckboxClass) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderGeneralSettings(controller);
    EXPECT_NE(html.find("<input type='checkbox' class='checkbox' data-setting='require_rl_focus'"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, IdentitySelectorExcludesUnknownLobbyIdsButKeepsSavedIdentity) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        PlayerData unknown;
        unknown.primaryId = "Unknown|Bot_01";
        unknown.name = "Bot";
        state->game.roster[unknown.primaryId] = unknown;
        PlayerData valid;
        valid.primaryId = "Steam|123";
        valid.name = "Player";
        state->game.roster[valid.primaryId] = valid;
        state->game.version.fetch_add(1);
    }

    ConfigData config = Config::Read();
    config.last_primary_id.clear();
    config.known_primary_ids = {"Unknown|Old", "Steam|456"};
    RmlUiController controller(state, nullptr);
    controller.Update(config);
    std::string html = RenderGeneralSettings(controller);
    EXPECT_EQ(html.find("Unknown|Bot_01"), std::string::npos);
    EXPECT_EQ(html.find("Unknown|Old"), std::string::npos);
    EXPECT_NE(html.find("Steam|123"), std::string::npos);
    EXPECT_NE(html.find("Steam|456"), std::string::npos);

    config.last_primary_id = "Unknown|Old";
    controller.Update(config);
    html = RenderGeneralSettings(controller);
    EXPECT_NE(html.find("Unknown|Old"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, ReplaySavingKeepsInlineLegacySaveReplayBindControl) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.auto_save_replays = true;
    config.key_save_replay = 0x52;

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    std::string html = RenderIntegrationSettings(controller);
    const std::string bindTarget = SaveReplayBindTarget();
    EXPECT_NE(html.find("Save Replay Bind"), std::string::npos);
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

TEST_F(RmlUiControllerStateTest, DynamicRmlTextEscapesMarkupAndSanitizesInvalidUtf8) {
    const std::string input = std::string("<&\"'>") + "\xC0\xAF" + "ok" + "\xF0\x9F\x98\x80" + "\xED\xA0\x80";
    const std::string escaped = EscapeText(input);

    EXPECT_EQ(escaped, "&lt;&amp;&quot;&#39;&gt;\xEF\xBF\xBD\xEF\xBF\xBDok\xF0\x9F\x98\x80\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD");
}

TEST_F(RmlUiControllerStateTest, EffectivePrimaryTriggersOneDeduplicatedDbRefresh) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    DatabaseManager::s_test_async_get_lifetime_calls.store(0);
    DatabaseManager::s_test_async_refresh_calls.store(0);

    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|TEST123"; }, true);
    ConfigData config = Config::Read();
    RmlUiController controller(state, db);

    controller.Update(config);
    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 1);
    EXPECT_EQ(DatabaseManager::s_test_async_refresh_calls.load(), 1);
    {
        std::lock_guard lock(DatabaseManager::s_test_mutex);
        EXPECT_EQ(DatabaseManager::s_test_last_get_lifetime_primary, "Steam|TEST123");
        EXPECT_EQ(DatabaseManager::s_test_last_refresh_primary, "Steam|TEST123");
    }

    controller.Update(config);
    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 1);
    EXPECT_EQ(DatabaseManager::s_test_async_refresh_calls.load(), 1);
}

TEST_F(RmlUiControllerStateTest, GraphCategoryChangeRefreshesLifetimeHistoryOnce) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    {
        std::unique_lock lock(state->history.mutex);
        state->history.lifetimeMmrX = {1.0f};
        state->history.lifetimeMmrY = {1200.0f};
        state->history.version.fetch_add(1);
    }
    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|GRAPH"; }, true);
    ConfigData config = Config::Read();
    DatabaseManager::s_test_async_get_lifetime_calls.store(0);

    RmlUiController controller(state, db);
    controller.Update(config);
    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 1);

    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    controller.Update(config);
    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 2);
    {
        std::lock_guard lock(DatabaseManager::s_test_mutex);
        EXPECT_EQ(DatabaseManager::s_test_last_get_lifetime_playlist, "1v1");
    }
    {
        std::shared_lock lock(state->history.mutex);
        EXPECT_TRUE(state->history.lifetimeMmrX.empty());
        EXPECT_TRUE(state->history.lifetimeMmrY.empty());
    }

    controller.Update(config);
    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 2);
}

TEST_F(RmlUiControllerStateTest, EstimatedMmrFlagTracksHistoryVersion) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->history.mutex);
        state->history.playlistHistoryY["2v2"] = {1209.0f};
        state->history.playlistMatchPoints["2v2"] = {
            SessionMmrPoint{
                .matchGuid = "owned-match",
                .historyIndex = 0,
                .mmr = 1209,
                .trackerMatchesPlayed = 50,
                .trackerCovered = true,
                .valueEstimated = true}};
        state->history.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    ASSERT_EQ(Snapshot(controller).playlistHistoryEstimated.at("2v2").size(), 1u);
    EXPECT_TRUE(Snapshot(controller).playlistHistoryEstimated.at("2v2")[0]);

    {
        std::unique_lock lock(state->history.mutex);
        state->history.playlistMatchPoints["2v2"][0].valueEstimated = false;
        state->history.version.fetch_add(1);
    }
    controller.Update(Config::Read());
    ASSERT_EQ(Snapshot(controller).playlistHistoryEstimated.at("2v2").size(), 1u);
    EXPECT_FALSE(Snapshot(controller).playlistHistoryEstimated.at("2v2")[0]);
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

TEST_F(RmlUiControllerStateTest, StatsApiBackgroundStateParticipatesInUiInvalidation) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const ConfigData config = Config::Read();

    controller.Update(config);
    const std::string before = LastConfigFingerprint(controller);

    {
        std::lock_guard lock(state->ui.statsApiMutex);
        state->ui.statsApiResult.status = StatsApiConfig::Status::WrongPort;
        state->ui.statsApiResult.path = "C:/Games/Rocket League/TAGame/Config/DefaultStatsAPI.ini";
        state->ui.statsApiResult.message = "Port does not match OmniStats.";
        state->ui.statsApiResult.expectedPort = 49123;
        state->ui.statsApiResult.actualPort = 7777;
        state->ui.statsApiResult.packetSendRate = 30.0f;
        state->ui.statsApiResult.rlRunning = true;
    }
    state->ui.statsApiChecked.store(true);

    controller.Update(config);
    EXPECT_NE(LastConfigFingerprint(controller), before);
}

TEST_F(RmlUiControllerStateTest, ManualStatsApiPathKeepsLegacyValidationRules) {
    std::string normalized;
    std::string error;

    EXPECT_TRUE(ValidateStatsPath("  \t\r\n", normalized, error));
    EXPECT_TRUE(normalized.empty());
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(ValidateStatsPath("WrongName.ini", normalized, error));
    EXPECT_EQ(error, "Filename must be exactly DefaultStatsAPI.ini");

    const std::string missing = (std::filesystem::temp_directory_path() /
                                 "definitely-missing-omnistats-path" / "DefaultStatsAPI.ini")
                                    .string();
    EXPECT_FALSE(ValidateStatsPath(missing, normalized, error));
    EXPECT_EQ(error, "File does not exist.");
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

TEST_F(RmlUiControllerStateTest, EmptyGraphKeepsScopeControlAndSpecificGuidance) {
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    state->history.showLifetimeGraph.store(false);
    controller.Update(Config::Read());
    std::string session = RenderGraph(controller);
    EXPECT_NE(session.find("data-action='graph-mode'"), std::string::npos);
    EXPECT_NE(session.find("No MMR data for 2v2 this session."), std::string::npos);
    EXPECT_NE(session.find("Play a match to start plotting!"), std::string::npos);

    state->history.showLifetimeGraph.store(true);
    controller.Update(Config::Read());
    std::string lifetime = RenderGraph(controller);
    EXPECT_NE(lifetime.find("data-action='graph-mode'"), std::string::npos);
    EXPECT_NE(lifetime.find("No lifetime MMR history in database yet."), std::string::npos);
    EXPECT_NE(lifetime.find("Play matches to populate database records!"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, SessionGraphDoesNotInventPlaylistHistoryOrBaseline) {
    auto state = std::make_shared<SessionState>();
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    {
        std::unique_lock lock(state->history.mutex);
        // The legacy aggregate/best projection is not authoritative 2v2 history.
        state->history.initialMmr = 1400;
        state->history.mmrHistoryY = {1400.0f, 1412.0f};
        state->history.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    std::string html = RenderGraph(controller);
    EXPECT_NE(html.find("No MMR data for 2v2 this session."), std::string::npos);
    EXPECT_EQ(html.find("<mmrgraphlines"), std::string::npos);

    {
        std::unique_lock lock(state->history.mutex);
        state->history.playlistHistoryY["2v2"] = {1200.0f, 1211.0f};
        // Deliberately leave playlistInitialMmr unset. The legacy implementation
        // treated that as an unknown baseline and reported a neutral +0 delta.
        state->history.version.fetch_add(1);
    }
    controller.Update(Config::Read());
    html = RenderGraph(controller);
    EXPECT_NE(html.find("<mmrgraphlines class='graph-polyline'"), std::string::npos);
    EXPECT_EQ(html.find("class='graph-line baseline'"), std::string::npos);
    EXPECT_NE(html.find(">+0</span>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, PopulatedGraphUsesAspectCorrectCustomPolyline) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->history.mutex);
        state->history.playlistInitialMmr["2v2"] = 1200;
        state->history.playlistHistoryY["2v2"] = {1200.0f, 1211.0f, 1198.0f};
        state->history.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());
    const std::string html = RenderGraph(controller);

    EXPECT_NE(html.find("<mmrgraphlines class='graph-polyline' points='"), std::string::npos);
    EXPECT_EQ(html.find("class='graph-line' style="), std::string::npos);
    EXPECT_NE(html.find("class='graph-point"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, DashboardAndOverlaySessionCardsKeepCompactLegacySemantics) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        state->game.sessionTotals.wins = 2;
        state->game.sessionTotals.losses = 1;
        state->game.sessionTotals.shots = 4;
        state->game.sessionTotals.shotsTotal = 9;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read());

    const std::string dashboard = RenderSessionWidget(controller, true);
    const std::string overlay = RenderSessionWidget(controller, false);
    const std::string sessionView = RenderSessionView(controller);
    const std::string unusedFullBranch = RenderFullSession(controller);
    EXPECT_NE(dashboard.find("<div class='metric-label'>Record</div>"), std::string::npos);
    EXPECT_EQ(dashboard.find("Shots"), std::string::npos);
    EXPECT_EQ(overlay.find("Shots"), std::string::npos);
    EXPECT_EQ(sessionView.find("Shots"), std::string::npos);
    EXPECT_NE(unusedFullBranch.find("<div class='label'>Shots</div>"), std::string::npos);
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

TEST_F(RmlUiControllerStateTest, CompactSessionKeepsLegacySignedZeroMmrChange) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    config.show_session_mmr_change = true;
    {
        std::unique_lock lock(state->game.mutex);
        state->game.sessionTotals.totalMmrChange = 0.0f;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string html = RenderCompactSession(controller);
    EXPECT_NE(html.find("<div class='metric-label'>MMR</div><div class='metric-value mono'>+0</div>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, CompactSessionUsesFinalizedDemoTotalsOnly) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock lock(state->game.mutex);
        state->game.sessionTotals.demos = 3;
        state->game.currentMatch.demosSelf = 2;
        state->game.matchFinalized = false;
        state->game.version.fetch_add(1);
    }

    ConfigData config = Config::Read();
    config.show_session_demos = true;
    RmlUiController controller(state, nullptr);
    controller.Update(config);

    const std::string html = RenderCompactSession(controller);
    EXPECT_NE(html.find("<div class='metric-label'>Demos</div><div class='metric-value mono'>3</div>"), std::string::npos);
    EXPECT_EQ(html.find("<div class='metric-label'>Demos</div><div class='metric-value mono'>5</div>"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, LiveTelemetryDoesNotInvalidateOpenSettingsDom) {
    auto state = std::make_shared<SessionState>();
    ConfigData config = Config::Read();
    state->ui.showMenu.store(true);

    {
        std::unique_lock lock(state->game.mutex);
        PlayerData local;
        local.primaryId = "Epic|local";
        local.name = "Local Player";
        local.team = 0;
        local.fetched = true;
        local.mmr = 1200;
        local.playlists["2v2"] = 1200;
        local.playlistTiers["2v2"] = "Diamond I Division I";
        state->game.myPrimaryId = local.primaryId;
        state->game.roster[local.primaryId] = local;
        state->game.version.fetch_add(1);
    }

    RmlUiController controller(state, nullptr);
    controller.Update(config);
    const std::string before = LastSettingsFingerprint(controller);
    ASSERT_FALSE(before.empty());

    // Ordinary match telemetry changes the game version and overlay, but none
    // of the identity/rank fields rendered by the General settings page. The
    // Settings DOM should therefore remain stable instead of being recreated
    // under the mouse every telemetry tick.
    {
        std::unique_lock lock(state->game.mutex);
        state->game.currentMatch.goals += 1;
        state->game.currentMatch.shots += 2;
        state->game.sessionTotals.goals += 1;
        state->game.version.fetch_add(1);
    }
    controller.Update(config);
    EXPECT_EQ(LastSettingsFingerprint(controller), before);
}

TEST_F(RmlUiControllerStateTest, FullSettingsOpenAndRenderDoesNotCrash) {
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

    // 1. Open settings and render General page
    state->ui.showMenu.store(true);
    controller.Update(Config::Read());
    controller.Render();

    // 2. Iterate through all settings pages
    for (int page = 0; page < 8; ++page) {
        SetSettingsPage(controller, page);
        controller.Update(Config::Read());
        controller.Render();
    }

    // 3. Close settings (like pressing F5 or Escape again)
    state->ui.showMenu.store(false);
    controller.Update(Config::Read());
    controller.Render();

    DestroyWindow(hwnd);
}
