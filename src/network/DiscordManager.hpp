#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include "core/state/SessionState.hpp"

#include "core/state/DiscordPresenceSnapshot.hpp"

namespace discordpp {
    class Client;
}

class TelemetryManager;

class DiscordManager {
  public:
    DiscordManager(std::shared_ptr<SessionState> state, std::shared_ptr<TelemetryManager> telemetry = nullptr);
    virtual ~DiscordManager();

    void Initialize();
    void Shutdown();

    // Call whenever match state changes to push an update to Discord
    virtual void PushPresenceUpdate(const DiscordPresenceSnapshot& snapshot);

  private:
    void CallbackThread();
    void ApplyPresence(const DiscordPresenceSnapshot& snapshot);

    std::shared_ptr<SessionState> m_state;
    std::unique_ptr<discordpp::Client> m_client;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_dirty{false};
    std::jthread m_worker;
    std::mutex m_snapshotMutex;
    std::condition_variable m_cv;
    DiscordPresenceSnapshot m_latestSnapshot;
    DiscordActivityClock m_activityClock;
    std::shared_ptr<TelemetryManager> m_telemetry;
};
