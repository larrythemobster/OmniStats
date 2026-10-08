#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <mutex>
#include <shared_mutex>

class ConfigTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Ensure the data directory exists (required on CI runners)
        Storage::InitializeEnvironment();
        // Backup current config
        original_config = Config::Read();
    }

    void TearDown() override {
        // Restore config
        Config::Update([this](ConfigData& c) { c = original_config; });
    }

    ConfigData original_config;
};

TEST_F(ConfigTest, ReadAndWriteState) {
    Config::Update([](ConfigData& c) {
        c.port = 9999;
        c.host = "127.0.0.99";
    });

    ConfigData readData = Config::Read();
    EXPECT_EQ(readData.port, 9999);
    EXPECT_EQ(readData.host, "127.0.0.99");
}

TEST_F(ConfigTest, SaveAndLoad) {
    Config::Update([](ConfigData& c) {
        c.port = 12345;
        c.host = "test_host";
    });
    Config::Save();

    // Modify memory to ensure load overwrites
    Config::Update([](ConfigData& c) {
        c.port = 0;
        c.host = "";
    },
                   false);

    Config::Load();
    ConfigData loadedData = Config::Read();
    EXPECT_EQ(loadedData.port, 12345);
    EXPECT_EQ(loadedData.host, "test_host");
}

TEST_F(ConfigTest, PersistsSettingsPanelThemeColor) {
    const ColorRGBA expected = {0.12f, 0.34f, 0.56f, 0.78f};
    Config::Update([expected](ConfigData& c) { c.themeSettingsPanel = expected; });
    Config::Save();

    Config::Update([](ConfigData& c) { c.themeSettingsPanel = {0.0f, 0.0f, 0.0f, 0.0f}; }, false);
    Config::Load();

    const ColorRGBA actual = Config::Read().themeSettingsPanel;
    EXPECT_FLOAT_EQ(actual.r, expected.r);
    EXPECT_FLOAT_EQ(actual.g, expected.g);
    EXPECT_FLOAT_EQ(actual.b, expected.b);
    EXPECT_FLOAT_EQ(actual.a, expected.a);
}

TEST_F(ConfigTest, PersistsDisabledStatsApiStartupCheck) {
    Config::Update([](ConfigData& c) { c.check_stats_api_config_on_startup = false; });
    Config::Save();
    Config::Update([](ConfigData& c) { c.check_stats_api_config_on_startup = true; }, false);
    Config::Load();
    EXPECT_FALSE(Config::Read().check_stats_api_config_on_startup);
}

TEST_F(ConfigTest, ConcurrencyReadUpdate) {
    std::atomic<bool> start{false};
    std::atomic<int> completed{0};

    auto reader = [&]() {
        while (!start) {
        }
        for (int i = 0; i < 1000; ++i) {
            ConfigData c = Config::Read();
            EXPECT_GE(c.port, 0);
        }
        completed++;
    };

    auto updater = [&]() {
        while (!start) {
        }
        for (int i = 0; i < 1000; ++i) {
            Config::Update([i](ConfigData& c) {
                c.port = i;
            },
                           false);
        }
        completed++;
    };

    std::thread t1(reader);
    std::thread t2(updater);
    std::thread t3(reader);

    start = true;
    t1.join();
    t2.join();
    t3.join();
    EXPECT_EQ(completed.load(), 3);
}

TEST_F(ConfigTest, LobbyRanksConfigVisibility) {
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = false;
        c.show_lobby_rank_2v2 = true;
        c.show_lobby_rank_3v3 = false;
        c.show_lobby_rank_casual = true;
        c.show_lobby_rank_tourny = false;
        c.show_lobby_rank_hoops = true;
        c.show_lobby_rank_rumble = false;
        c.show_lobby_rank_dropshot = true;
        c.show_lobby_rank_snowday = false;
        c.show_lobby_rank_heatseeker = true;
    });
    Config::Save();

    // Reset values in memory
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = true;
        c.show_lobby_rank_2v2 = false;
        c.show_lobby_rank_3v3 = true;
        c.show_lobby_rank_casual = false;
        c.show_lobby_rank_tourny = true;
        c.show_lobby_rank_hoops = false;
        c.show_lobby_rank_rumble = true;
        c.show_lobby_rank_dropshot = false;
        c.show_lobby_rank_snowday = true;
        c.show_lobby_rank_heatseeker = false;
    },
                   false);

    Config::Load();
    ConfigData loaded = Config::Read();
    EXPECT_FALSE(loaded.show_lobby_rank_1v1);
    EXPECT_TRUE(loaded.show_lobby_rank_2v2);
    EXPECT_FALSE(loaded.show_lobby_rank_3v3);
    EXPECT_TRUE(loaded.show_lobby_rank_casual);
    EXPECT_FALSE(loaded.show_lobby_rank_tourny);
    EXPECT_TRUE(loaded.show_lobby_rank_hoops);
    EXPECT_FALSE(loaded.show_lobby_rank_rumble);
    EXPECT_TRUE(loaded.show_lobby_rank_dropshot);
    EXPECT_FALSE(loaded.show_lobby_rank_snowday);
    EXPECT_TRUE(loaded.show_lobby_rank_heatseeker);
}

TEST_F(ConfigTest, LobbyRanksConfigFallback) {
    // Attempting to set all to false should trigger fallback to enable "2v2"
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = false;
        c.show_lobby_rank_2v2 = false;
        c.show_lobby_rank_3v3 = false;
        c.show_lobby_rank_casual = false;
        c.show_lobby_rank_tourny = false;
        c.show_lobby_rank_hoops = false;
        c.show_lobby_rank_rumble = false;
        c.show_lobby_rank_dropshot = false;
        c.show_lobby_rank_snowday = false;
        c.show_lobby_rank_heatseeker = false;
    },
                   false);

    ConfigData loaded = Config::Read();
    EXPECT_TRUE(loaded.show_lobby_rank_2v2);
}

TEST_F(ConfigTest, MigratesLegacySharedMmrCategory) {
    const std::string configPath =
        Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream file(configPath);
        file << nlohmann::json{
            {"mmr_category", "3v3"},
            {"auto_switch_mmr_category", false},
            {"port", 49200}}
                    .dump(2);
    }

    Config::Load();
    const ConfigData migrated = Config::Read();
    EXPECT_EQ(migrated.mmr_category, "3v3");
    EXPECT_FALSE(migrated.auto_switch_mmr_category);
    EXPECT_EQ(migrated.graph_mmr_category, "3v3");
    EXPECT_FALSE(migrated.graph_follow_current_playlist);
    EXPECT_EQ(migrated.port, 49200);

    Config::Save();
    nlohmann::json saved;
    {
        std::ifstream file(configPath);
        file >> saved;
    }
    EXPECT_EQ(saved["mmr_category"], "3v3");
    EXPECT_EQ(saved["auto_switch_mmr_category"], false);
    EXPECT_EQ(saved["graph_mmr_category"], "3v3");
    EXPECT_EQ(saved["graph_follow_current_playlist"], false);
}

TEST_F(ConfigTest, PersistsIndependentMmrSettings) {
    Config::Update([](ConfigData& c) {
        c.mmr_category = "best";
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "2v2";
        c.graph_follow_current_playlist = true;
    });
    Config::Save();

    Config::Update([](ConfigData& c) {
        c.mmr_category = "1v1";
        c.auto_switch_mmr_category = true;
        c.graph_mmr_category = "3v3";
        c.graph_follow_current_playlist = false;
    },
                   false);

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.mmr_category, "best");
    EXPECT_FALSE(loaded.auto_switch_mmr_category);
    EXPECT_EQ(loaded.graph_mmr_category, "2v2");
    EXPECT_TRUE(loaded.graph_follow_current_playlist);
}

TEST_F(ConfigTest, InvalidMmrCategoriesUseSafeDefaults) {
    const std::string configPath =
        Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream file(configPath);
        file << nlohmann::json{
            {"mmr_category", "not-a-playlist"},
            {"graph_mmr_category", "also-invalid"},
            {"graph_follow_current_playlist", true}}
                    .dump(2);
    }

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.mmr_category, "best");
    EXPECT_EQ(loaded.graph_mmr_category, "2v2");
    EXPECT_TRUE(loaded.graph_follow_current_playlist);
}

TEST_F(ConfigTest, ResetThemeColorsRestoresDefaultColors) {
    ConfigData conf;
    const ConfigData defaults;

    conf.themeBg = {1.0f, 0.0f, 0.0f, 0.5f};
    conf.themeSettingsPanel = {0.0f, 1.0f, 0.0f, 0.5f};
    conf.themeTopbar = {0.0f, 0.0f, 1.0f, 0.5f};
    conf.themeGraphPanel = {0.5f, 0.5f, 0.5f, 0.5f};
    conf.themeText = {0.1f, 0.1f, 0.1f, 1.0f};
    conf.themeAccent = {0.2f, 0.2f, 0.2f, 1.0f};
    conf.themeWin = {0.3f, 0.3f, 0.3f, 1.0f};
    conf.themeLoss = {0.4f, 0.4f, 0.4f, 1.0f};
    conf.themeDim = {0.5f, 0.5f, 0.5f, 1.0f};
    conf.themeMuted = {0.6f, 0.6f, 0.6f, 1.0f};
    conf.themeGraphLine = {0.7f, 0.7f, 0.7f, 1.0f};
    conf.themeGraphBaseline = {0.8f, 0.8f, 0.8f, 1.0f};
    conf.themeRosterCard = {0.85f, 0.85f, 0.85f, 1.0f};
    conf.themeRosterCardSelf = {0.86f, 0.86f, 0.86f, 1.0f};
    conf.themeStatBox = {0.87f, 0.87f, 0.87f, 1.0f};
    conf.themeMatchRow = {0.88f, 0.88f, 0.88f, 1.0f};
    conf.themeMatchRowAlt = {0.89f, 0.89f, 0.89f, 1.0f};

    conf.ResetThemeColors();

    EXPECT_EQ(conf.themeBg.r, defaults.themeBg.r);
    EXPECT_EQ(conf.themeBg.g, defaults.themeBg.g);
    EXPECT_EQ(conf.themeBg.b, defaults.themeBg.b);
    EXPECT_EQ(conf.themeBg.a, defaults.themeBg.a);
    EXPECT_EQ(conf.themeSettingsPanel.r, defaults.themeSettingsPanel.r);
    EXPECT_EQ(conf.themeTopbar.r, defaults.themeTopbar.r);
    EXPECT_EQ(conf.themeGraphPanel.r, defaults.themeGraphPanel.r);
    EXPECT_EQ(conf.themeText.r, defaults.themeText.r);
    EXPECT_EQ(conf.themeAccent.r, defaults.themeAccent.r);
    EXPECT_EQ(conf.themeWin.r, defaults.themeWin.r);
    EXPECT_EQ(conf.themeLoss.r, defaults.themeLoss.r);
    EXPECT_EQ(conf.themeDim.r, defaults.themeDim.r);
    EXPECT_EQ(conf.themeMuted.r, defaults.themeMuted.r);
    EXPECT_EQ(conf.themeGraphLine.r, defaults.themeGraphLine.r);
    EXPECT_EQ(conf.themeGraphBaseline.r, defaults.themeGraphBaseline.r);
    EXPECT_EQ(conf.themeRosterCard.r, defaults.themeRosterCard.r);
    EXPECT_EQ(conf.themeRosterCardSelf.r, defaults.themeRosterCardSelf.r);
    EXPECT_EQ(conf.themeStatBox.r, defaults.themeStatBox.r);
    EXPECT_EQ(conf.themeMatchRow.r, defaults.themeMatchRow.r);
    EXPECT_EQ(conf.themeMatchRowAlt.r, defaults.themeMatchRowAlt.r);
}

TEST_F(ConfigTest, ResetLayoutsRestoresDefaultLayoutAndPlacement) {
    ConfigData conf;
    const ConfigData defaults;

    conf.session_view_x = 999.0f;
    conf.session_view_y = 888.0f;
    conf.match_summary_x = 777.0f;
    conf.match_summary_y = 666.0f;
    conf.dashboard_layout.leftColumnWeight = 0.99f;
    conf.overlay_layout.containers.clear();

    conf.ResetLayouts();

    EXPECT_EQ(conf.session_view_x, defaults.session_view_x);
    EXPECT_EQ(conf.session_view_y, defaults.session_view_y);
    EXPECT_EQ(conf.match_summary_x, defaults.match_summary_x);
    EXPECT_EQ(conf.match_summary_y, defaults.match_summary_y);
    EXPECT_EQ(conf.dashboard_layout.leftColumnWeight, defaults.dashboard_layout.leftColumnWeight);
    EXPECT_EQ(conf.overlay_layout.containers.size(), defaults.overlay_layout.containers.size());
}

TEST_F(ConfigTest, ResetThemeAndLayoutRestoresBoth) {
    ConfigData conf;
    const ConfigData defaults;

    conf.themeBg = {1.0f, 0.0f, 0.0f, 0.5f};
    conf.session_view_x = 999.0f;

    conf.ResetThemeAndLayout();

    EXPECT_EQ(conf.themeBg.r, defaults.themeBg.r);
    EXPECT_EQ(conf.session_view_x, defaults.session_view_x);
}

TEST_F(ConfigTest, AccountDpapiFieldsRoundTripWithoutPlaintextOnDisk) {
    const std::string secretRefresh = "plaintext_refresh_secret_value_998877";
    const std::string secretDeviceKey = "plaintext_device_seed_value_112233";

    Config::Update([&](ConfigData& c) {
        c.account_signed_in_name = "PilotOne";
        c.account_device_public_id = "dev_pub_id_123";
        c.account_refresh_token = secretRefresh;
        c.account_device_key = secretDeviceKey;
    });
    Config::Save();

    const std::string configPath = Storage::GetDataDirectory() + "config.json";
    std::string rawDiskContent;
    {
        std::ifstream file(configPath);
        rawDiskContent.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    EXPECT_EQ(rawDiskContent.find(secretRefresh), std::string::npos);
    EXPECT_EQ(rawDiskContent.find(secretDeviceKey), std::string::npos);

    nlohmann::json savedJson = nlohmann::json::parse(rawDiskContent);
    EXPECT_FALSE(savedJson.at("account_refresh_token").get<std::string>().empty());
    EXPECT_FALSE(savedJson.at("account_device_key").get<std::string>().empty());
    EXPECT_NE(savedJson.at("account_refresh_token").get<std::string>(), secretRefresh);
    EXPECT_NE(savedJson.at("account_device_key").get<std::string>(), secretDeviceKey);

    Config::Update([](ConfigData& c) {
        c.account_signed_in_name.clear();
        c.account_device_public_id.clear();
        c.account_refresh_token.clear();
        c.account_device_key.clear();
    },
                   false);

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.account_signed_in_name, "PilotOne");
    EXPECT_EQ(loaded.account_device_public_id, "dev_pub_id_123");
    EXPECT_EQ(loaded.account_refresh_token, secretRefresh);
    EXPECT_EQ(loaded.account_device_key, secretDeviceKey);
}

TEST_F(ConfigTest, OverlayVisibilityRoundTripAndSanitization) {
    Config::Update([](ConfigData& c) {
        c.match_summary_seconds = 25;
        c.overlay_layout.containers.clear();

        OverlayLayout::ContainerConfig afterGoal;
        afterGoal.id = "goal_box";
        afterGoal.x = 100.0f;
        afterGoal.y = 200.0f;
        afterGoal.widgets = {DashboardLayout::WidgetId::LiveMatchStats};
        afterGoal.visibility = {
            .mode = OverlayLayout::Visibility::AfterEvent,
            .event = OverlayLayout::Visibility::Goal,
            .seconds = 12,
            .alsoWhileKeyHeld = false,
            .hideDuringReplay = true};
        c.overlay_layout.containers.push_back(afterGoal);

        OverlayLayout::ContainerConfig menusCard;
        menusCard.id = "menus_box";
        menusCard.widgets = {DashboardLayout::WidgetId::PreviousGames};
        menusCard.visibility = {
            .mode = OverlayLayout::Visibility::MenusOnly,
            .event = OverlayLayout::Visibility::Podium,
            .seconds = 20,
            .alsoWhileKeyHeld = true,
            .hideDuringReplay = false};
        c.overlay_layout.containers.push_back(menusCard);
    });
    Config::Save();

    Config::Update([](ConfigData& c) {
        c.match_summary_seconds = 10;
        c.overlay_layout.containers.clear();
    },
                   false);

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.match_summary_seconds, 25);
    EXPECT_EQ(loaded.overlay_layout.version, OverlayLayout::kCurrentLayoutVersion);
    ASSERT_EQ(loaded.overlay_layout.containers.size(), 2u);

    const auto& c0 = loaded.overlay_layout.containers[0];
    EXPECT_EQ(c0.id, "goal_box");
    EXPECT_EQ(c0.visibility.mode, OverlayLayout::Visibility::AfterEvent);
    EXPECT_EQ(c0.visibility.event, OverlayLayout::Visibility::Goal);
    EXPECT_EQ(c0.visibility.seconds, 12);
    EXPECT_FALSE(c0.visibility.alsoWhileKeyHeld);
    EXPECT_TRUE(c0.visibility.hideDuringReplay);

    const auto& c1 = loaded.overlay_layout.containers[1];
    EXPECT_EQ(c1.id, "menus_box");
    EXPECT_EQ(c1.visibility.mode, OverlayLayout::Visibility::MenusOnly);
    EXPECT_EQ(c1.visibility.event, OverlayLayout::Visibility::Podium);
    EXPECT_EQ(c1.visibility.seconds, 20);
    EXPECT_TRUE(c1.visibility.alsoWhileKeyHeld);
    EXPECT_FALSE(c1.visibility.hideDuringReplay);
}

TEST_F(ConfigTest, OldLayoutMigratesToKeyHeldEverywhereAndInvalidValuesSanitize) {
    const std::string configPath = Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream file(configPath);
        file << nlohmann::json{
            {"onboarding_completed", true},
            {"overlay_layout",
             {{"version", 3},
              {"toolbox_open", false},
              {"containers",
               nlohmann::json::array({
                   {{"id", "lobby_ranks"},
                    {"x", 743.5},
                    {"y", 774.0},
                    {"w", 0.0},
                    {"h", 0.0},
                    {"widgets", nlohmann::json::array({"lobby_ranks"})}},
                   {{"id", "main_stack"},
                    {"x", 1490.0},
                    {"y", 0.0},
                    {"w", 430.0},
                    {"h", 759.0},
                    {"widgets", nlohmann::json::array({"live_roster"})},
                    {"visibility",
                     {{"mode", "not_a_valid_mode"},
                      {"event", "not_a_valid_event"},
                      {"seconds", 999},
                      {"also_while_key_held", false},
                      {"hide_during_replay", true}}}},
                   {{"id", "demo_tracker"},
                    {"x", 1270.0},
                    {"y", 0.0},
                    {"w", 220.0},
                    {"h", 489.0},
                    {"widgets", nlohmann::json::array({"demo_tracker"})},
                    {"visibility",
                     {{"mode", 99},
                      {"event", -5},
                      {"seconds", 0}}}},
               })}}}}
                    .dump(2);
    }

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.overlay_layout.version, OverlayLayout::kCurrentLayoutVersion);
    EXPECT_EQ(loaded.match_summary_seconds, 30);
    ASSERT_EQ(loaded.overlay_layout.containers.size(), 3u);

    // Missing visibility in old v3 layout => KeyHeld everywhere (including lobby_ranks).
    EXPECT_EQ(loaded.overlay_layout.containers[0].id, "lobby_ranks");
    EXPECT_EQ(loaded.overlay_layout.containers[0].visibility.mode, OverlayLayout::Visibility::KeyHeld);

    // Invalid strings/numbers sanitized and seconds clamped to [1, 30].
    EXPECT_EQ(loaded.overlay_layout.containers[1].visibility.mode, OverlayLayout::Visibility::KeyHeld);
    EXPECT_EQ(loaded.overlay_layout.containers[1].visibility.event, OverlayLayout::Visibility::FirstCountdown);
    EXPECT_EQ(loaded.overlay_layout.containers[1].visibility.seconds, 30);
    EXPECT_FALSE(loaded.overlay_layout.containers[1].visibility.alsoWhileKeyHeld);
    EXPECT_TRUE(loaded.overlay_layout.containers[1].visibility.hideDuringReplay);

    EXPECT_EQ(loaded.overlay_layout.containers[2].visibility.mode, OverlayLayout::Visibility::KeyHeld);
    EXPECT_EQ(loaded.overlay_layout.containers[2].visibility.event, OverlayLayout::Visibility::FirstCountdown);
    EXPECT_EQ(loaded.overlay_layout.containers[2].visibility.seconds, 1);
}

TEST_F(ConfigTest, NewInstallDefaultsLobbyRanksToFirstCountdown8sAndMatchSummaryTo20s) {
    const std::string configPath = Storage::GetDataDirectory() + "config.json";
    std::error_code ec;
    std::filesystem::remove(configPath, ec);

    Config::Load();
    const ConfigData fresh = Config::Read();
    EXPECT_EQ(fresh.match_summary_seconds, 20);
    EXPECT_EQ(fresh.overlay_layout.version, OverlayLayout::kCurrentLayoutVersion);

    bool foundLobby = false;
    for (const auto& c : fresh.overlay_layout.containers) {
        if (c.id == "lobby_ranks") {
            foundLobby = true;
            EXPECT_EQ(c.visibility.mode, OverlayLayout::Visibility::AfterEvent);
            EXPECT_EQ(c.visibility.event, OverlayLayout::Visibility::FirstCountdown);
            EXPECT_EQ(c.visibility.seconds, 8);
            EXPECT_TRUE(c.visibility.alsoWhileKeyHeld);
            EXPECT_FALSE(c.visibility.hideDuringReplay);
        } else {
            EXPECT_EQ(c.visibility.mode, OverlayLayout::Visibility::KeyHeld) << c.id;
        }
    }
    EXPECT_TRUE(foundLobby);
}
