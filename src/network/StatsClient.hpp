#pragma once
#include <memory>
#include <atomic>
#include <string>
#include "core/SessionState.hpp"
#include "core/TelemetryReducer.hpp"
#include "core/SideEffectExecutor.hpp"
#include "network/TelemetryParser.hpp"
#include "network/MMRFetcher.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"

class StatsClient {
public:
    StatsClient(std::shared_ptr<SessionState> state, std::shared_ptr<MMRFetcher> mmrFetcher, std::shared_ptr<DatabaseManager> dbManager);
    ~StatsClient();

    void Start();
    void Stop();
    void SetDiscordManager(std::shared_ptr<DiscordManager> dm) { m_discordManager = dm; }
    void HandleLine(const std::string& line);

private:
    void RunLoop();
    void OnJsonLine(const std::string& jsonStr);

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<MMRFetcher> m_mmrFetcher;
    std::shared_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<DiscordManager> m_discordManager;

    TelemetryParser m_parser;
    TelemetryReducer m_reducer;
    SideEffectExecutor m_executor;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
    int m_disconnectCount{0};
    bool m_resetAfterDisconnect{false};
};
