#pragma once

#include <memory>

class SessionState;
class DatabaseManager;
class MMRFetcher;
class AccountManagerService;
class StatsClient;
class DiscordManager;
class InputManager;
class ReplayUploader;
class Overlay;
class RemoteConfig;
class TelemetryManager;
namespace Tournaments {
    class TournamentScheduleService;
}

class App {
  public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();
    void Stop();

  private:
    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<RemoteConfig> m_remoteConfig;
    std::shared_ptr<Tournaments::TournamentScheduleService> m_tournamentService;
    std::shared_ptr<TelemetryManager> m_telemetryManager;
    std::shared_ptr<MMRFetcher> m_mmrFetcher;
    std::shared_ptr<AccountManagerService> m_accountManager;
    std::unique_ptr<StatsClient> m_statsClient;
    std::shared_ptr<DiscordManager> m_discordManager;
    std::unique_ptr<InputManager> m_inputManager;
    std::unique_ptr<ReplayUploader> m_uploader;
    std::unique_ptr<Overlay> m_overlay;

    bool m_saverStarted = false;
    bool m_stopped = false;
};
