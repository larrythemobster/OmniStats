#include "app/App.hpp"
#include "core/accounts/AccountManager.hpp"
#include "core/config/Config.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "core/state/SessionState.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/api/AccountClient.hpp"
#include "network/api/RemoteConfig.hpp"
#include "network/api/ReplayUploader.hpp"
#include "network/api/StatsClient.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/telemetry/TelemetryManager.hpp"
#include "platform/InputManager.hpp"
#include "ui/Overlay.hpp"
#include "utils/Storage.hpp"

#include <iostream>
#include <mutex>
#include <windows.h>

App::App() = default;

App::~App() {
    Stop();
}

int App::Run() {
    std::cout << "OmniStats Starting...\n";
    Config::Load();
    m_telemetryManager = std::make_shared<TelemetryManager>();
    ExternalUpdaterLauncher::SetTelemetryManager(m_telemetryManager);

    // Network services wait for the Privacy Policy and Terms, which the first-run window collects.
    const bool consentedAtStartup = Config::HasCurrentConsent(Config::Read());
    if (consentedAtStartup) {
        AccountClient::Instance().SyncFromConfig();
    }

    ConfigData startupUpdateConf = Config::Read();
    if (consentedAtStartup && startupUpdateConf.enable_auto_updates && ExternalUpdaterLauncher::RunStartupUpdateCheck()) {
        std::cout << "[Main] External updater launched for startup update.\n";
        return 0;
    }

    Config::InitSaver();
    m_saverStarted = true;

    m_state = std::make_shared<SessionState>();

    // Remember the selected MMR category from previous session
    ConfigData startupConf = Config::Read();
    MmrCategory liveCategory = StringToMmrCategory(startupConf.mmr_category);
    if (!startupConf.show_extra_playlists && IsExtraMmrCategory(liveCategory)) {
        liveCategory = MmrCategory::Best;
    }
    m_state->ui.rosterMmrCategory.store(liveCategory);

    MmrCategory graphCategory = StringToMmrCategory(startupConf.graph_mmr_category);
    if (graphCategory == MmrCategory::Best ||
        (!startupConf.show_extra_playlists && IsExtraMmrCategory(graphCategory))) {
        graphCategory = MmrCategory::TwoVTwo;
    }
    m_state->ui.graphMmrCategory.store(graphCategory);

    MmrCategory rankProgressCategory = StringToMmrCategory(startupConf.rank_progress_mmr_category);
    if (!startupConf.show_extra_playlists && IsExtraMmrCategory(rankProgressCategory)) {
        rankProgressCategory = MmrCategory::TwoVTwo;
    }
    m_state->ui.rankProgressMmrCategory.store(rankProgressCategory);

    // Stats API config check
    std::string apiPath = startupConf.rocket_league_stats_api_config_path;
    if (apiPath.empty()) {
        apiPath = StatsApiConfig::DetectConfigPath();
    }
    StatsApiConfig::CheckResult checkRes = StatsApiConfig::VerifyConfig(apiPath, startupConf.port);
    {
        std::lock_guard<std::mutex> lock(m_state->ui.statsApiMutex);
        m_state->ui.statsApiResult = checkRes;
        m_state->ui.statsApiChecked.store(true);
    }
    if (checkRes.status == StatsApiConfig::Status::Valid) {
        std::cout << "[StatsApiConfig] Rocket League Stats API config is valid.\n";
    } else {
        std::cout << "[StatsApiConfig] Rocket League Stats API config check failed: " << StatsApiConfig::GetStatusMessage(checkRes.status) << "\n";
    }

    m_dbManager = std::make_shared<DatabaseManager>(m_state);
    if (!m_dbManager->Initialize(Storage::GetDataDirectory() + Storage::APP_NAME + ".db")) {
        std::cerr << "[Database] Initialization failed. Match recording is unavailable.\n";
        MessageBoxW(nullptr,
                    L"OmniStats could not open or upgrade your local database and cannot save matches.\n\n"
                    L"Check disk space and access to your OmniStats data folder, then restart the app.",
                    L"OmniStats database error", MB_OK | MB_ICONERROR);
        return 1;
    }

    m_remoteConfig = std::make_shared<RemoteConfig>();
    m_tournamentService = std::make_shared<Tournaments::TournamentScheduleService>();

    m_dbManager->SetTelemetryManager(m_telemetryManager);
    AccountClient::Instance().SetTelemetryManager(m_telemetryManager);

    const auto startConsentedServices = [this](bool syncAccount) {
        if (syncAccount) AccountClient::Instance().SyncFromConfig();
        if (!Config::Read().enable_auto_updates) {
            ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
                m_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Initial);
        }
        m_telemetryManager->Initialize(m_dbManager);
        m_remoteConfig->Start();
        m_tournamentService->Start();
    };
    if (consentedAtStartup) {
        startConsentedServices(false);
    }

    // Start Backend Threads
    m_mmrFetcher = std::make_shared<MMRFetcher>(m_state, m_dbManager);
    m_mmrFetcher->Start();

    m_accountManager = std::make_shared<AccountManagerService>(m_dbManager, m_mmrFetcher);
    m_accountManager->Start();
    m_accountManager->QueueStartupRefresh();

    m_statsClient = std::make_unique<StatsClient>(m_state, m_mmrFetcher, m_dbManager);
    m_statsClient->SetAccountManager(m_accountManager);
    m_statsClient->SetTelemetryManager(m_telemetryManager);

    if (Config::Read().discord_rpc_enabled) {
        m_discordManager = std::make_shared<DiscordManager>(m_state, m_telemetryManager);
        m_discordManager->Initialize();
        m_statsClient->SetDiscordManager(m_discordManager);
    }
    m_statsClient->Start();

    m_inputManager = std::make_unique<InputManager>(m_state, m_telemetryManager);
    m_inputManager->Start();

    ConfigData mainConf = Config::Read();
    if (mainConf.auto_upload_replays || mainConf.auto_save_replays) {
        m_uploader = std::make_unique<ReplayUploader>(m_state, m_dbManager, m_telemetryManager);
        m_uploader->Start();
    }

    m_overlay = std::make_unique<Overlay>(m_state, m_dbManager, m_accountManager, m_remoteConfig, m_tournamentService, m_telemetryManager);
    if (!m_overlay->Initialize()) {
        return 1;
    }

    if (!consentedAtStartup) {
        m_overlay->SetConsentHandler([this, startConsentedServices] { startConsentedServices(true); });
    }

    m_overlay->RunLoop();

    Stop();
    return 0;
}

void App::Stop() {
    if (m_stopped) return;
    m_stopped = true;

    std::cout << "[Main] Shutting down components...\n";
    if (m_inputManager) {
        m_inputManager->Stop();
        std::cout << "[Main] InputManager stopped.\n";
    }
    if (m_statsClient) {
        m_statsClient->Stop();
        std::cout << "[Main] StatsClient stopped.\n";
    }
    if (m_mmrFetcher) {
        m_mmrFetcher->Stop();
        std::cout << "[Main] MMRFetcher stopped.\n";
    }
    if (m_accountManager) {
        m_accountManager->Stop();
        std::cout << "[Main] AccountManager stopped.\n";
    }
    if (m_discordManager) {
        m_discordManager->Shutdown();
        std::cout << "[Main] DiscordManager stopped.\n";
    }
    if (m_uploader) {
        m_uploader->Stop();
        std::cout << "[Main] ReplayUploader stopped.\n";
    }
    if (m_tournamentService) {
        m_tournamentService->Stop();
    }
    AccountClient::Instance().Shutdown();
    if (m_remoteConfig) {
        m_remoteConfig->Shutdown();
    }
    if (m_telemetryManager) {
        m_telemetryManager->Shutdown();
        std::cout << "[Main] TelemetryManager stopped.\n";
    }
    if (m_saverStarted) {
        Config::ShutdownSaver();
        m_saverStarted = false;
        std::cout << "[Main] Config Saver stopped.\n";
    }
    ExternalUpdaterLauncher::ShutdownBackgroundTasks();
    std::cout << "[Main] Cleanup complete. Exiting.\n";
}
