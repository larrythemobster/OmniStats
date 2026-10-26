#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

class DatabaseManager;

namespace TelemetryDetail {
    inline constexpr uint64_t k100nsPerMinute = 600'000'000ULL;
    inline constexpr int kMaxOpenMinutes = 1440;

    struct HeartbeatAccounting {
        int open_minutes = 0;
        uint64_t remainder_100ns = 0;
    };

    inline constexpr HeartbeatAccounting CalculateHeartbeatMinutes(uint64_t elapsed_100ns, uint64_t carry_100ns = 0) {
        const uint64_t total = (UINT64_MAX - elapsed_100ns < carry_100ns)
                                   ? UINT64_MAX
                                   : (elapsed_100ns + carry_100ns);
        const uint64_t minutes = total / k100nsPerMinute;
        const uint64_t maxOpen = static_cast<uint64_t>(kMaxOpenMinutes);
        const int clamped = static_cast<int>(minutes < maxOpen ? minutes : maxOpen);
        const uint64_t remainder = total % k100nsPerMinute;
        return {clamped, remainder};
    }
}

class TelemetryManager {
  public:
    enum class Milestone : uint8_t {
        FirstLaunch,
        OnboardingCompleted,
        StatsApiConnected,
        FirstMatchTracked,
        AccountLinked,
        OverlayToggle,
        MmrGraphView,
        SessionInsights,
        MatchHistoryDetail,
        PlayerLookup,
        ReplayUploadBallchasing,
        DiscordRpcEnabled,
        TournamentsView,
        SettingsChanged,
        MatchCardShared,
        Count
    };

    struct MilestoneInfo {
        std::string_view kind;
        std::string_view name;
    };

    static constexpr MilestoneInfo MilestoneMetadata(Milestone m) {
        switch (m) {
        case Milestone::FirstLaunch:
            return {"onboarding", "first_launch"};
        case Milestone::OnboardingCompleted:
            return {"onboarding", "onboarding_completed"};
        case Milestone::StatsApiConnected:
            return {"onboarding", "stats_api_connected"};
        case Milestone::FirstMatchTracked:
            return {"onboarding", "first_match_tracked"};
        case Milestone::AccountLinked:
            return {"onboarding", "account_linked"};
        case Milestone::OverlayToggle:
            return {"feature", "overlay_toggle"};
        case Milestone::MmrGraphView:
            return {"feature", "mmr_graph_view"};
        case Milestone::SessionInsights:
            return {"feature", "session_insights"};
        case Milestone::MatchHistoryDetail:
            return {"feature", "match_history_detail"};
        case Milestone::PlayerLookup:
            return {"feature", "player_lookup"};
        case Milestone::ReplayUploadBallchasing:
            return {"feature", "replay_upload_ballchasing"};
        case Milestone::DiscordRpcEnabled:
            return {"feature", "discord_rpc_enabled"};
        case Milestone::TournamentsView:
            return {"feature", "tournaments_view"};
        case Milestone::SettingsChanged:
            return {"feature", "settings_changed"};
        case Milestone::MatchCardShared:
            return {"feature", "match_card_shared"};
        default:
            return {"", ""};
        }
    }

    static std::string GenerateUUIDv4();

    TelemetryManager();
    explicit TelemetryManager(std::shared_ptr<DatabaseManager> db);
    ~TelemetryManager();

    TelemetryManager(const TelemetryManager&) = delete;
    TelemetryManager& operator=(const TelemetryManager&) = delete;

    void Initialize(std::shared_ptr<DatabaseManager> db);
    void Shutdown();

    void RecordMilestone(Milestone m);
    void RecordUpdaterEvent(const std::string& event,
                            const std::string& targetVersion,
                            const std::string& errorCategory,
                            const std::string& version);

    void FlushPendingMilestones();
    void FlushPendingUpdaterEvents();
    void SendHeartbeat();
    void CheckAndReportCrashes();
    void SendTelemetryAsync();
    void SendCrashAsync(const std::string& filePath);

  private:
    void SendHeartbeatInternal(bool isShutdown);
    void SendShutdownHeartbeat();

    bool m_isTestMode = false;
    std::unique_ptr<std::jthread> m_workerThread;
    std::atomic<bool> m_isRunning{false};
    uint64_t m_lastAccountingUnbiasedTime = 0;
    std::mutex m_heartbeatMutex;
    std::atomic<bool> m_shutdownHeartbeatSent{false};
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;

    std::mutex m_milestonesMutex;
    std::atomic<uint32_t> m_knownMask{0};
    uint32_t m_reportedMask = 0;
    uint32_t m_pendingMask = 0;
    std::shared_ptr<DatabaseManager> m_db;
};
