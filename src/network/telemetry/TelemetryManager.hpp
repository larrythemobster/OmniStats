#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <memory>
#include "database/DatabaseManager.hpp"

namespace TelemetryManager {
    namespace detail {
        inline constexpr uint64_t k100nsPerMinute = 600'000'000ULL;
        inline constexpr int kMaxOpenMinutes = 1440;

        struct HeartbeatAccounting {
            int open_minutes = 0;
            uint64_t remainder_100ns = 0;
        };

        inline HeartbeatAccounting CalculateHeartbeatMinutes(uint64_t elapsed_100ns, uint64_t carry_100ns = 0) {
            const uint64_t total = (UINT64_MAX - elapsed_100ns < carry_100ns)
                                       ? UINT64_MAX
                                       : (elapsed_100ns + carry_100ns);
            const uint64_t minutes = total / k100nsPerMinute;
            const int clamped = static_cast<int>(std::min<uint64_t>(minutes, static_cast<uint64_t>(kMaxOpenMinutes)));
            const uint64_t remainder = total % k100nsPerMinute;
            return {clamped, remainder};
        }
    }

    // Generates a random standard UUIDv4 string
    std::string GenerateUUIDv4();

    // Initializes telemetry: loads/creates client_uuid and enables telemetry.
    void Initialize(std::shared_ptr<DatabaseManager> db);

    // Checks and reports any pending crashes asynchronously
    void CheckAndReportCrashes();

    // Asynchronously sends a telemetry ping
    void SendTelemetryAsync();

    // Asynchronously sends a crash report ping with error details
    void SendCrashAsync(const std::string& filePath);

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

    constexpr MilestoneInfo MilestoneMetadata(Milestone m) {
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

    // Records a milestone via lockless fast path.
    void RecordMilestone(Milestone m);
    // Appends an updater outcome to <appdata>/updater_events.jsonl capped at 20 lines.
    void RecordUpdaterEvent(const std::string& event,
                            const std::string& targetVersion,
                            const std::string& errorCategory,
                            const std::string& version);

    // Flushes pending milestones to the server (max 32 per batch).
    void FlushPendingMilestones();

    // Reads and sends pending updater events from <appdata>/updater_events.jsonl.
    void FlushPendingUpdaterEvents();

    // Sends hourly heartbeat ping.
    void SendHeartbeat();

    // Gracefully stops telemetry background worker
    void Shutdown();
}
