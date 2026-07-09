#include "StatsClient.hpp"
#include "core/Config.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/MMRFetcher.hpp"
#include <iostream>

static void ConfigureFromSavedIdentity(std::shared_ptr<SessionState> state,
                                        std::shared_ptr<DatabaseManager> dbManager) {
    ConfigData conf = Config::Read();
    if (state && !conf.last_primary_id.empty()) {
        {
            std::unique_lock<std::shared_mutex> lock(state->game.mutex);
            state->game.myPrimaryId = conf.last_primary_id;
            state->game.version++;
        }
        if (dbManager) {
            dbManager->AsyncGetLifetimeMmrHistory(state->game.myPrimaryId,
                MmrCategoryToString(state->ui.graphMmrCategory.load()));
            dbManager->AsyncRefreshDbStats(state->game.myPrimaryId);
        }
    }
    if (dbManager) {
        dbManager->AsyncGetRecentMatchHistory(conf.last_primary_id, conf.previous_games_limit);
    }
}

StatsClient::StatsClient(std::shared_ptr<SessionState> state,
                         std::shared_ptr<MMRFetcher> mmrFetcher,
                         std::shared_ptr<DatabaseManager> dbManager)
    : m_state(state), m_mmrFetcher(mmrFetcher), m_dbManager(dbManager),
      m_reducer(state) {
    ConfigureFromSavedIdentity(state, dbManager);
}

StatsClient::~StatsClient() { Stop(); }

void StatsClient::Start() {
    if (m_isRunning) return;
    m_isRunning = true;
    m_workerThread = std::jthread(&StatsClient::RunLoop, this);
    std::cout << "[StatsClient] Background thread started.\n";
}

void StatsClient::Stop() {
    m_isRunning = false;
    m_parser.Stop();
    m_executor.Stop();
    if (m_workerThread.joinable()) m_workerThread.join();
}

void StatsClient::RunLoop() {
    bool errorLogged = false;
    while (m_isRunning) {
        try {
            m_parser.ConnectAndRead([this](const std::string& jsonStr) {
                OnJsonLine(jsonStr);
            });
            errorLogged = false; // Reset on success
        } catch (const std::exception& e) {
            if (!errorLogged) {
                std::cout << "[StatsClient] Connection error: " << e.what() << " (Suppressing further errors until connected)\n";
                errorLogged = true;
            }
            
            m_disconnectCount++;
            
            ConfigData conf = Config::Read();
            bool resetSessionAfterDisconnect = false;
            // Reset at exactly 5 (approx 15 seconds) to avoid doing it continuously
            if (conf.reset_session_on_close && !m_resetAfterDisconnect && m_disconnectCount >= 5) {
                m_resetAfterDisconnect = true;
                resetSessionAfterDisconnect = true;
                std::cout << "[StatsClient] Game closed (multiple disconnects). Resetting session stats.\n";
            }

            {
                std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
                if (resetSessionAfterDisconnect) {
                    m_state->game.sessionTotals = SessionTotals();
                    m_state->game.sessionGamemodes.clear();
                }
                m_state->game.inMatch = false;
                m_state->game.inReplay = false;
                m_state->game.arenaName = "";
                m_state->game.arenaAsset = "";
                m_state->game.myTeam = -1;
                m_state->game.roster.clear();
                m_state->game.version++;
            }

            if (m_discordManager) {
                // Clear Discord presence when the telemetry socket drops.
                m_discordManager->PushPresenceUpdate(DiscordPresenceSnapshot{});
            }
        }

        if (m_isRunning) {
            for (int i = 0; i < 30 && m_isRunning; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void StatsClient::OnJsonLine(const std::string& jsonStr) {
    m_disconnectCount = 0;
    m_resetAfterDisconnect = false;
    try {
        nlohmann::json msg = nlohmann::json::parse(jsonStr);
        if (msg.contains("Event") && msg["Event"].is_string() && msg.contains("Data")) {
            std::string eventName = msg["Event"].get<std::string>();
            nlohmann::json dataObj;
            if (msg["Data"].is_string())
                dataObj = nlohmann::json::parse(msg["Data"].get<std::string>());
            else
                dataObj = msg["Data"];

            SideEffects effects = m_reducer.Reduce(eventName, dataObj);
            m_executor.Execute(std::move(effects), m_dbManager, m_discordManager, m_mmrFetcher);
        }
    } catch (const std::exception& e) {
        std::cout << "[StatsClient] Error processing line: " << e.what() << "\n";
    }
}

void StatsClient::HandleLine(const std::string& line) {
    OnJsonLine(line);
}
