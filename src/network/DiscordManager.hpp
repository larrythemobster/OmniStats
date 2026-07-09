#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include "core/SessionState.hpp"

#include "core/DiscordPresenceSnapshot.hpp"

class DiscordManager {
public:
    DiscordManager(std::shared_ptr<SessionState> state);
    virtual ~DiscordManager();

    void Initialize();
    void Shutdown();

    // Call whenever match state changes to push an update to Discord
    virtual void PushPresenceUpdate(const DiscordPresenceSnapshot& snapshot);

private:
    void CallbackThread();
    void ApplyPresence(const DiscordPresenceSnapshot& snapshot);

    std::shared_ptr<SessionState> m_state;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_dirty{false};
    std::jthread m_worker;
    std::mutex m_snapshotMutex;
    std::condition_variable m_cv;
    DiscordPresenceSnapshot m_latestSnapshot;
};
