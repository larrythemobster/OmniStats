#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/game/MatchAnalytics.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

class MatchAnalyticsTracker {
  public:
    // nowSeconds is any monotonic clock in seconds; it drives sample durations.
    void OnUpdateState(const TelemetryEvents::UpdateStateEvent& ev, double nowSeconds);
    void OnCountdownBegin();
    void OnRoundStarted(double nowSeconds);
    void OnBallHit(const TelemetryEvents::BallHitEvent& ev, double nowSeconds);
    void OnBoostPickup(const TelemetryEvents::BoostPickupEvent& ev);
    void OnGoalScored(const TelemetryEvents::GoalScoredEvent& ev);
    void OnStatfeed(const TelemetryEvents::StatfeedEvent& ev);
    void OnPlayerLeft(const TelemetryEvents::PlayerLeftEvent& ev);
    void OnPaused(double nowSeconds);
    void OnUnpaused(double nowSeconds);
    void OnMatchEnded();

    MatchAnalytics Snapshot() const;

  private:
    struct SpeedSample {
        float raw = 0.0f;
        float seconds = 0.0f;
        int supersonic = -1; // -1 when the flag was absent
    };
    struct RawCarState {
        std::vector<SpeedSample> speedSamples;
        std::optional<int> lastBoost;
        bool respawning = false;
        bool wasPowersliding = false;
    };
    struct LastHit {
        double at = 0.0;
        float speed = 0.0f;
        bool counted = false;
    };

    PlayerAnalytics& PlayerFor(const std::string& primaryId);
    std::string ResolvePlayer(const TelemetryEvents::PlayerRef& ref) const;
    void AddPlayTime(float seconds);
    void CloseKickoffWindows();

    MatchAnalytics m_data;
    std::unordered_map<std::string, size_t> m_playerIndex;
    std::unordered_map<int, std::string> m_shortcutToId;
    std::unordered_map<std::string, RawCarState> m_rawCar;
    std::vector<std::pair<float, float>> m_ballSpeedSamples;
    int m_possessionTeam = -1;
    std::optional<double> m_lastSampleAt;
    std::unordered_map<std::string, LastHit> m_lastHit;
    std::optional<double> m_pausedAt;
    std::optional<float> m_lastClock;
    bool m_lastOvertime = false;
    bool m_roundActive = false;
    bool m_paused = false;
    bool m_awaitingFirstTouch = false;
    bool m_kickoffPossessionPending = false;
    std::array<int, 2> m_score{};
    std::array<int, 2> m_scoreboard{};
    PossessionWindow m_window;
};
