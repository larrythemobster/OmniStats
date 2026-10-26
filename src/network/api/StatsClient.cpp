#include "network/api/StatsClient.hpp"

#include <chrono>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>

#include "core/config/Config.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/telemetry/TelemetryRecorder.hpp"

static void ConfigureFromSavedIdentity(std::shared_ptr<SessionState> state,
                                       std::shared_ptr<DatabaseManager> dbManager) {
    ConfigData conf = Config::Read();
    if (state && !conf.last_primary_id.empty()) {
        if (state->liveMatch) {
            state->liveMatch->SetLocalPrimaryId(conf.last_primary_id);
        }
        if (dbManager) {
            dbManager->AsyncGetLifetimeMmrHistory(conf.last_primary_id,
                                                  MmrCategoryToString(state->ui.graphMmrCategory.load()));
            dbManager->AsyncRefreshDbStats(conf.last_primary_id);
        }
    }
    if (dbManager) {
        dbManager->AsyncGetRecentMatchHistory(conf.last_primary_id, conf.previous_games_limit);
    }
}

StatsClient::StatsClient(std::shared_ptr<SessionState> state,
                         std::shared_ptr<MMRFetcher> mmrFetcher,
                         std::shared_ptr<DatabaseManager> dbManager)
    : m_state(state), m_mmrFetcher(mmrFetcher), m_dbManager(dbManager) {
    ConfigureFromSavedIdentity(state, dbManager);
    if (m_state) {
        m_configRevision = Config::Revision();
        m_state->ui.ApplyRankProgressConfig(Config::Read(), true);
    }
    m_confirmationCallbackGuard = std::make_shared<ConfirmationCallbackGuard>();
    m_confirmationCallbackGuard->owner = this;
    if (m_mmrFetcher) {
        std::weak_ptr<ConfirmationCallbackGuard> weakGuard = m_confirmationCallbackGuard;
        m_mmrFetcher->SetDestroyedMatchConfirmationCallback(
            [weakGuard](const std::string& matchGuid, bool won) {
                const auto guard = weakGuard.lock();
                if (!guard) return;
                std::lock_guard<std::mutex> lock(guard->mutex);
                if (guard->owner) {
                    guard->owner->HandleDestroyedMatchConfirmation(matchGuid, won);
                }
            });
    }
    if (auto recPath = TelemetryRecorder::PathFromEnvironment()) {
        m_recorder.emplace(std::move(*recPath));
    }
}

StatsClient::~StatsClient() {
    Stop();
}

void StatsClient::Start() {
    if (m_isRunning) return;
    m_isRunning = true;
    m_workerThread = std::jthread(&StatsClient::RunLoop, this);
    std::cout << "[StatsClient] Background thread started.\n";
}

void StatsClient::Stop() {
    m_isRunning = false;
    m_parser.Stop();
    if (m_mmrFetcher) {
        m_mmrFetcher->SetDestroyedMatchConfirmationCallback({});
    }
    if (m_confirmationCallbackGuard) {
        std::lock_guard<std::mutex> lock(m_confirmationCallbackGuard->mutex);
        m_confirmationCallbackGuard->owner = nullptr;
    }
    if (m_state && m_state->liveMatch) {
        m_state->liveMatch->SetTelemetryThreadActive(false);
    }
    if (m_workerThread.joinable()) m_workerThread.join();
}

void StatsClient::OnEvent(const TelemetryEvents::TelemetryEvent& event) {
    m_disconnectCount = 0;
    m_resetAfterDisconnect = false;
    if (!m_state || !m_state->liveMatch) return;

    if (const uint64_t revision = Config::Revision(); revision != m_configRevision) {
        m_configRevision = revision;
        m_state->ui.ApplyRankProgressConfig(Config::Read(), false);
    }
    ReducerContext ctx = MakeReducerContext(m_state->ui);
    SideEffects effects = m_state->liveMatch->ApplyEvent(event, ctx);
    EffectRouter::Route(std::move(effects),
                        m_dbManager,
                        m_discordManager,
                        m_mmrFetcher,
                        m_accountManager,
                        m_state);
}

void StatsClient::HandleEvent(const TelemetryEvents::TelemetryEvent& event) {
    OnEvent(event);
}

void StatsClient::HandleLine(const std::string& line) {
    if (m_recorder) m_recorder->Record(line);
    try {
        nlohmann::json msg = nlohmann::json::parse(line);
        if (msg.contains("Event") && msg["Event"].is_string() && msg.contains("Data")) {
            std::string eventName = msg["Event"].get<std::string>();
            std::string dataStr;
            if (msg["Data"].is_string()) {
                dataStr = msg["Data"].get<std::string>();
            } else {
                dataStr = msg["Data"].dump();
            }
            auto ev = TelemetryEvents::ParseEventFromJson(eventName, dataStr);
            if (ev) {
                OnEvent(*ev);
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[StatsClient] Error processing line: " << e.what() << "\n";
    }
}

void StatsClient::HandleDestroyedMatchConfirmation(const std::string& matchGuid, bool won) {
    if (!m_state || !m_state->liveMatch) return;
    ReducerContext ctx = MakeReducerContext(m_state->ui);
    SideEffects effects = m_state->liveMatch->ConfirmDestroyedMatch(matchGuid, won, ctx);
    EffectRouter::Route(std::move(effects),
                        m_dbManager,
                        m_discordManager,
                        m_mmrFetcher,
                        m_accountManager,
                        m_state);
}

void StatsClient::RunLoop() {
    if (m_state && m_state->liveMatch) {
        m_state->liveMatch->SetTelemetryThreadActive(true);
    }
    bool errorLogged = false;
    while (m_isRunning) {
        try {
            m_parser.ConnectAndRead([this](const std::string& message) { HandleLine(message); });
            errorLogged = false;
        } catch (const std::exception& e) {
            if (!errorLogged) {
                std::cout << "[StatsClient] Connection error: " << e.what() << " (Suppressing further errors until connected)\n";
                errorLogged = true;
            }

            m_disconnectCount++;

            ConfigData conf = Config::Read();
            bool resetSessionAfterDisconnect = false;
            if (conf.reset_session_on_close && !m_resetAfterDisconnect &&
                (m_disconnectCount >= 5 || !StatsApiConfig::IsRocketLeagueRunning())) {
                m_resetAfterDisconnect = true;
                resetSessionAfterDisconnect = true;
                std::cout << "[StatsClient] Game closed. Resetting session stats.\n";
            }

            if (m_state && m_state->liveMatch) {
                m_state->liveMatch->OnTelemetryDisconnected();
            }

            SideEffects disconnectEffects;
            if (m_state && m_state->liveMatch) {
                const int64_t nowUnix = static_cast<int64_t>(std::time(nullptr));
                if (resetSessionAfterDisconnect) {
                    std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
                    if (m_state->liveMatch->StartNewSession(nowUnix, &m_state->history)) {
                        if (conf.show_session_recap_on_close) {
                            m_state->ui.showSessionRecap.store(true);
                        }
                        disconnectEffects.persistSession = m_state->liveMatch->GetSnapshot()->lastSessionRecap;
                    }
                } else if (!conf.reset_session_on_close) {
                    std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
                    if (m_state->liveMatch->CloseSessionIfInactive(nowUnix, &m_state->history)) {
                        disconnectEffects.persistSession = m_state->liveMatch->GetSnapshot()->lastSessionRecap;
                    }
                }
                m_state->clearActiveMatchOnDisconnect();
            }

            if (disconnectEffects.persistSession) {
                EffectRouter::Route(std::move(disconnectEffects),
                                    m_dbManager,
                                    m_discordManager,
                                    m_mmrFetcher,
                                    m_accountManager,
                                    m_state);
            }

            if (m_discordManager) {
                m_discordManager->PushPresenceUpdate(DiscordPresenceSnapshot{});
            }
        }

        if (m_isRunning) {
            for (int i = 0; i < 30 && m_isRunning; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (m_state && m_state->liveMatch) {
                    m_state->liveMatch->DrainInboxAndApply(MakeReducerContext(m_state->ui));
                }
            }
        }
    }
}
