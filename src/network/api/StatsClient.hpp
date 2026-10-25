#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/effects/EffectRouter.hpp"
#include "core/effects/SideEffects.hpp"
#include "core/state/SessionState.hpp"
#include "network/telemetry/TelemetryRecorder.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/telemetry/TelemetryEvents.hpp"
#include "network/telemetry/TelemetryParser.hpp"

class TelemetryManager;

class AccountManagerService;

class StatsClient {
  public:
    StatsClient(std::shared_ptr<SessionState> state,
                std::shared_ptr<MMRFetcher> mmrFetcher,
                std::shared_ptr<DatabaseManager> dbManager);
    ~StatsClient();

    void Start();
    void Stop();
    void SetDiscordManager(std::shared_ptr<DiscordManager> dm) {
        m_discordManager = dm;
    }
    void SetAccountManager(std::shared_ptr<AccountManagerService> accountManager) {
        m_accountManager = std::move(accountManager);
    }
    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm) {
        m_telemetry = tm;
        m_parser.SetTelemetryManager(std::move(tm));
    }
    void HandleLine(const std::string& line);
    void HandleEvent(const TelemetryEvents::TelemetryEvent& event);

  private:
    void RunLoop();
    void OnEvent(const TelemetryEvents::TelemetryEvent& event);
    void HandleDestroyedMatchConfirmation(const std::string& matchGuid, bool won);

    struct ConfirmationCallbackGuard {
        std::mutex mutex;
        StatsClient* owner = nullptr;
    };

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<MMRFetcher> m_mmrFetcher;
    std::shared_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<DiscordManager> m_discordManager;
    std::shared_ptr<AccountManagerService> m_accountManager;

    std::shared_ptr<TelemetryManager> m_telemetry;
    TelemetryParser m_parser;
    std::shared_ptr<ConfirmationCallbackGuard> m_confirmationCallbackGuard;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
    int m_disconnectCount{0};
    bool m_resetAfterDisconnect{false};
    std::optional<TelemetryRecorder> m_recorder;
    uint64_t m_configRevision{0};
};
