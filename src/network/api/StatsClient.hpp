#pragma once
#include <memory>
#include <atomic>
#include <string>
#include <mutex>
#include <optional>
#include "network/telemetry/TelemetryRecorder.hpp"
#include "core/state/SessionState.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/effects/SideEffectExecutor.hpp"
#include "network/telemetry/TelemetryParser.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"

class AccountManagerService;

class StatsClient {
  public:
    StatsClient(std::shared_ptr<SessionState> state, std::shared_ptr<MMRFetcher> mmrFetcher, std::shared_ptr<DatabaseManager> dbManager);
    ~StatsClient();

    void Start();
    void Stop();
    void SetDiscordManager(std::shared_ptr<DiscordManager> dm) {
        m_discordManager = dm;
    }
    void SetAccountManager(std::shared_ptr<AccountManagerService> accountManager) {
        m_accountManager = std::move(accountManager);
    }
    void HandleLine(const std::string& line);

  private:
    void RunLoop();
    void OnJsonLine(const std::string& jsonStr);
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

    TelemetryParser m_parser;
    TelemetryReducer m_reducer;
    SideEffectExecutor m_executor;
    std::mutex m_reducerMutex;
    std::shared_ptr<ConfirmationCallbackGuard> m_confirmationCallbackGuard;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
    int m_disconnectCount{0};
    bool m_resetAfterDisconnect{false};
    std::optional<TelemetryRecorder> m_recorder;
};
