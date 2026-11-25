#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, StatsApiBackgroundStateParticipatesInUiInvalidation) {
    WarpDevice warp;
    if (!warp.Create(1200, 900)) GTEST_SKIP() << "WARP not available.";
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    const ConfigData config = Config::Read();

    state->ui.showMenu.store(true);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1200, 900, 1.0f));
    controller.Update(config);
    controller.Render();
    auto* dataPage = SettingsRoot(controller)->QuerySelector("[data-action='settings-page'][data-page='5']");
    ASSERT_NE(dataPage, nullptr);
    dataPage->DispatchEvent("click", {});
    controller.Render();
    auto* troubleshooting = SettingsRoot(controller)->QuerySelector("[data-action='toggle-troubleshooting']");
    ASSERT_NE(troubleshooting, nullptr);
    troubleshooting->DispatchEvent("click", {});
    controller.Render();
    ASSERT_EQ(SettingsRoot(controller)->GetInnerRML().find("Port does not match OmniStats."), std::string::npos);

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
    controller.Render();
    EXPECT_NE(SettingsRoot(controller)->GetInnerRML().find("Port does not match OmniStats."), std::string::npos);
    EXPECT_NE(SettingsRoot(controller)->GetInnerRML().find("DefaultStatsAPI.ini"), std::string::npos);
}

TEST_F(RmlUiControllerStateTest, ManualStatsApiPathAcceptsOnlyStatsApiIniFiles) {
    std::string normalized;
    std::string error;

    EXPECT_TRUE(ValidateStatsPath("  \t\r\n", normalized, error));
    EXPECT_TRUE(normalized.empty());
    EXPECT_TRUE(error.empty());

    EXPECT_FALSE(ValidateStatsPath("WrongName.ini", normalized, error));
    EXPECT_FALSE(error.empty());

    const auto missingDir = std::filesystem::temp_directory_path() / "definitely-missing-omnistats-path";
    for (const char* name : {"DefaultStatsAPI.ini", "TAStatsAPI.ini"}) {
        EXPECT_FALSE(ValidateStatsPath((missingDir / name).string(), normalized, error));
        EXPECT_EQ(error, "File does not exist.") << name;
    }
}

TEST_F(RmlUiControllerStateTest, StatsApiRepairRunsOffRenderThreadAndPublishesResult) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "OmniStatsRepairUiTest";
    std::filesystem::create_directories(dir);
    const std::filesystem::path ini = dir / "DefaultStatsAPI.ini";
    {
        std::ofstream file(ini, std::ios::binary);
        file << "[StatsAPI]\r\nPacketSendRate=0\r\nPort=12345\r\n";
    }
    Config::Update([&](ConfigData& c) {
        c.rocket_league_stats_api_config_path = ini.string();
        c.port = 49123;
    },
                   true);
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    controller.Update(Config::Read(), true);

    RunStatsApiCheck(controller, true);
    {
        std::lock_guard lock(state->ui.statsApiMutex);
        EXPECT_EQ(state->ui.statsApiResult.message.rfind("Repairing the Stats API config", 0), 0u);
    }
    for (int i = 0; i < 200 && StatsApiRepairRunning(controller); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(StatsApiRepairRunning(controller));
    controller.Update(Config::Read(), false);
    {
        std::lock_guard lock(state->ui.statsApiMutex);
        EXPECT_EQ(state->ui.statsApiResult.status, StatsApiConfig::Status::Valid);
    }
    EXPECT_TRUE(state->ui.statsApiChecked.load());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(RmlUiControllerStateTest, StartupStatsApiPromptAsksBeforeRepairingAndReportsResult) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "OmniStatsStartupPromptTest";
    std::filesystem::create_directories(dir);
    const std::filesystem::path ini = dir / "DefaultStatsAPI.ini";
    {
        std::ofstream file(ini, std::ios::binary);
        file << "[StatsAPI]\r\nPacketSendRate=0\r\nPort=12345\r\n";
    }
    Config::Update([&](ConfigData& c) {
        c.rocket_league_stats_api_config_path = ini.string();
        c.port = 49123;
        c.check_stats_api_config_on_startup = true;
    },
                   true);
    auto state = std::make_shared<SessionState>();
    {
        std::lock_guard lock(state->ui.statsApiMutex);
        state->ui.statsApiResult = StatsApiConfig::VerifyConfig(ini.string(), 49123);
    }
    state->ui.statsApiChecked.store(true);

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_FALSE(state->ui.showMenu.load());
    EXPECT_TRUE(controller.WantsAttention());

    auto* fix = PromptRoot(controller)->QuerySelector("[data-action='statsapi-prompt-fix']");
    ASSERT_NE(fix, nullptr);
    {
        std::ifstream file(ini, std::ios::binary);
        const std::string before((std::istreambuf_iterator<char>(file)), {});
        EXPECT_NE(before.find("PacketSendRate=0"), std::string::npos);
    }
    Click(controller, fix);
    EXPECT_EQ(PromptRoot(controller)->QuerySelector("[data-action='statsapi-prompt-fix']"), nullptr);
    for (int i = 0; i < 200 && StatsApiRepairRunning(controller); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_FALSE(StatsApiRepairRunning(controller));
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* done = PromptRoot(controller)->QuerySelector("[data-action='statsapi-prompt-close']");
    ASSERT_NE(done, nullptr);
    Click(controller, done);
    EXPECT_FALSE(PromptRoot(controller)->HasChildNodes());
    EXPECT_FALSE(controller.WantsAttention());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_F(RmlUiControllerStateTest, StartupStatsApiPromptCanBeTurnedOff) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.check_stats_api_config_on_startup = true; }, true);
    auto state = std::make_shared<SessionState>();
    {
        std::lock_guard lock(state->ui.statsApiMutex);
        state->ui.statsApiResult.status = StatsApiConfig::Status::DisabledPacketSendRate;
        state->ui.statsApiResult.path = "C:\\Rocket League\\TAGame\\Config\\DefaultStatsAPI.ini";
    }
    state->ui.statsApiChecked.store(true);

    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    auto* never = PromptRoot(controller)->QuerySelector("[data-action='statsapi-prompt-never']");
    ASSERT_NE(never, nullptr);
    Click(controller, never);
    EXPECT_FALSE(Config::Read().check_stats_api_config_on_startup);
    EXPECT_FALSE(controller.WantsAttention());
}
