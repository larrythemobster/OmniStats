#include "DiscordManager.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include <discord_rpc.h>
#include <iostream>
#include <cstring>
#include <ctime>
#include <map>

static const char* DISCORD_APP_ID = "1501709499168194590";

static void handleReady(const DiscordUser* user) {
    std::string userTag = std::string(user->username) + "#" + user->discriminator;
    std::cout << "[Discord] Connected as: " << PrivacyLog::Sensitive(userTag, "Discord user") << "\n";
}

static void handleDisconnected(int errorCode, const char* message) {
    std::cout << "[Discord] Disconnected (" << errorCode << "): " << message << "\n";
}

static void handleError(int errorCode, const char* message) {
    std::cout << "[Discord] Error (" << errorCode << "): " << message << "\n";
}

DiscordManager::DiscordManager(std::shared_ptr<SessionState> state)
    : m_state(state) {}

DiscordManager::~DiscordManager() {
    Shutdown();
}

void DiscordManager::Initialize() {
    DiscordEventHandlers handlers;
    std::memset(&handlers, 0, sizeof(handlers));
    handlers.ready = handleReady;
    handlers.disconnected = handleDisconnected;
    handlers.errored = handleError;

    Discord_Initialize(DISCORD_APP_ID, &handlers, 1, nullptr);
    m_initialized = true;

    // Start a background thread to pump Discord callbacks
    m_running = true;
    m_worker = std::jthread(&DiscordManager::CallbackThread, this);

    std::cout << "[Discord] RPC Initialized.\n";

    // Clear presence until Rocket League telemetry is active.
    DiscordPresenceSnapshot initial;
    PushPresenceUpdate(initial);
}

void DiscordManager::Shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    if (m_initialized) {
        Discord_ClearPresence();
        Discord_Shutdown();
        m_initialized = false;
        std::cout << "[Discord] RPC Shutdown.\n";
    }
}

void DiscordManager::CallbackThread() {
    while (m_running) {
#ifdef DISCORD_DISABLE_IO_THREAD
        Discord_UpdateConnection();
#endif
        Discord_RunCallbacks();

        DiscordPresenceSnapshot snapshot;
        bool doUpdate = false;
        {
            std::unique_lock<std::mutex> lock(m_snapshotMutex);
            if (m_cv.wait_for(lock, std::chrono::seconds(2), [this]() { return !m_running || m_dirty.load(); })) {
                if (!m_running) break;
                if (m_dirty) {
                    m_dirty = false;
                    doUpdate = true;
                    snapshot = m_latestSnapshot;
                }
            }
        }

        if (doUpdate) {
            ApplyPresence(snapshot);
        }
    }
}

void DiscordManager::PushPresenceUpdate(const DiscordPresenceSnapshot& snapshot) {
    {
        std::lock_guard<std::mutex> lock(m_snapshotMutex);
        m_latestSnapshot = snapshot;
        m_dirty = true;
    }
    m_cv.notify_one();
}

void DiscordManager::ApplyPresence(const DiscordPresenceSnapshot& snapshot) {
    if (!m_initialized) return;

    ConfigData conf = Config::Read();
    if (!conf.discord_rpc_enabled || !snapshot.showPresence) {
        Discord_ClearPresence();
        return;
    }

    DiscordRichPresence presence;
    std::memset(&presence, 0, sizeof(presence));

    std::string stateStr;
    std::string detailsStr;
    std::time_t timestamp = std::time(nullptr);
    
    if (snapshot.inMatch) {
        // In a match — show arena and score
        detailsStr = snapshot.arenaName.empty() ? "Unknown Arena" : snapshot.arenaName;
        
        // Show score in details
        int team0 = snapshot.score[0];
        int team1 = snapshot.score[1];
        
        if (snapshot.myTeam == 0) {
            stateStr = "Score: " + std::to_string(team0) + " - " + std::to_string(team1);
        } else if (snapshot.myTeam == 1) {
            stateStr = "Score: " + std::to_string(team1) + " - " + std::to_string(team0);
        } else {
            stateStr = "Score: " + std::to_string(team0) + " - " + std::to_string(team1);
        }
        
        // Add session record to state
        int wins = snapshot.sessionWins;
        int losses = snapshot.sessionLosses;
        if (wins > 0 || losses > 0) {
            stateStr += " | Session: " + std::to_string(wins) + "W - " + std::to_string(losses) + "L";
        }
        
        presence.startTimestamp = timestamp;
    } else {
        // In menus
        detailsStr = "In Menus";

        int wins = snapshot.sessionWins;
        int losses = snapshot.sessionLosses;
        
        if (wins > 0 || losses > 0) {
            float winRate = wins > 0 ? (float)wins / (wins + losses) * 100.0f : 0.0f;
            stateStr = "Session: " + std::to_string(wins) + "W - " + std::to_string(losses) + "L";
            stateStr += " (" + std::to_string((int)winRate) + "%)";
        } else {
            stateStr = "Warming up...";
        }
        
        presence.startTimestamp = timestamp;
    }

    presence.details = detailsStr.c_str();
    presence.state = stateStr.c_str();
    presence.largeImageKey = "omnistats_logo";
    presence.largeImageText = "OmniStats - Rocket League Stats & Overlay";

    Discord_UpdatePresence(&presence);
}
