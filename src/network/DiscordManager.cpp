#include "DiscordManager.hpp"
#include "core/config/Config.hpp"
#include "network/telemetry/TelemetryManager.hpp"

#define DISCORDPP_IMPLEMENTATION
#include <discordpp.h>

#include <chrono>
#include <ctime>
#include <iostream>

static constexpr uint64_t DISCORD_APP_ID = 1501709499168194590ULL;

DiscordManager::DiscordManager(std::shared_ptr<SessionState> state, std::shared_ptr<TelemetryManager> telemetry)
    : m_state(std::move(state)), m_telemetry(std::move(telemetry)) {}

DiscordManager::~DiscordManager() {
    Shutdown();
}

void DiscordManager::Initialize() {
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::DiscordRpcEnabled);
    if (m_running.exchange(true)) return;
    m_worker = std::jthread(&DiscordManager::CallbackThread, this);
}

void DiscordManager::Shutdown() {
    m_running = false;
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
    m_activityClock.Reset();
}

void DiscordManager::CallbackThread() {
    m_client = std::make_unique<discordpp::Client>();
    // Desktop Rich Presence uses local IPC without OAuth or Client::Connect.
    m_client->SetApplicationId(DISCORD_APP_ID);
    std::cout << "[Discord] Social SDK initialized for desktop Rich Presence.\n";

    while (m_running) {
        discordpp::RunCallbacks();

        DiscordPresenceSnapshot snapshot;
        bool doUpdate = false;
        {
            std::unique_lock<std::mutex> lock(m_snapshotMutex);
            if (m_cv.wait_for(lock, std::chrono::milliseconds(50), [this]() { return !m_running || m_dirty.load(); })) {
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
    m_client->ClearRichPresence();
    discordpp::RunCallbacks();
    m_client.reset();
    std::cout << "[Discord] Social SDK stopped.\n";
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
    if (!m_client) return;

    ConfigData conf = Config::Read();
    if (!conf.discord_rpc_enabled || !snapshot.showPresence) {
        m_activityClock.Reset();
        m_client->ClearRichPresence();
        return;
    }

    discordpp::Activity presence;
    presence.SetType(discordpp::ActivityTypes::Playing);

    std::string stateStr;
    std::string detailsStr;
    const std::time_t timestamp = m_activityClock.StartFor(snapshot, std::time(nullptr));

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
    }

    presence.SetDetails(detailsStr);
    presence.SetState(stateStr);
    discordpp::ActivityTimestamps timestamps;
    timestamps.SetStart(static_cast<uint64_t>(timestamp) * 1000);
    presence.SetTimestamps(timestamps);
    discordpp::ActivityAssets assets;
    assets.SetLargeImage("https://omnistats.org/Logo.png");
    assets.SetLargeText("OmniStats - Rocket League Stats & Overlay");
    presence.SetAssets(assets);
    m_client->UpdateRichPresence(std::move(presence), [](discordpp::ClientResult result) {
        if (!result.Successful()) {
            std::cerr << "[Discord] Presence update failed: "
                      << discordpp::EnumToString(result.Type()) << '\n';
        }
    });
}
