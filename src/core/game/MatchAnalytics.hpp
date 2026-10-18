#pragma once

#include <array>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Field coordinates follow the Stats API: blue (team 0) defends negative Y.
// Speeds are stored in km/h; distances in Unreal units (uu).
struct FieldPoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Car-state aggregates. During live play Rocket League streams car state only for
// players on your team; opponents' cars are visible only during goal replays.
struct CarTelemetry {
    float sampledSeconds = 0.0f;
    float distance = 0.0f;
    float maxSpeed = 0.0f;
    float slowSeconds = 0.0f;
    float boostSpeedSeconds = 0.0f;
    float supersonicSeconds = 0.0f;
    float groundSeconds = 0.0f;
    float airSeconds = 0.0f;
    float wallSeconds = 0.0f;
    float powerslideSeconds = 0.0f;
    int powerslideCount = 0;
    float boostingSeconds = 0.0f;
    float boostIntegral = 0.0f;
    float zeroBoostSeconds = 0.0f;
    float fullBoostSeconds = 0.0f;
    // [0,25), [25,50), [50,75), [75,100]
    std::array<float, 4> boostBandSeconds{};
    int boostUsed = 0;
    int boostCollected = 0;
    int bigPads = 0;
    int smallPads = 0;
    int stolenBigPads = 0;
    int stolenSmallPads = 0;
    // True when pad counts come from BoostPickup events rather than boost deltas.
    bool padsFromPickupEvents = false;
    float demolishedSeconds = 0.0f;

    float AverageSpeed() const;       // km/h
    float AverageBoost() const;       // 0-100
    float BoostPerMinute() const;     // boost used per minute of sampled play
    float Share(float seconds) const; // fraction of sampled time
};

struct PlayerAnalytics {
    std::string primaryId;
    std::string name;
    int team = -1;
    int hits = 0;
    int aerialHits = 0;
    int highAerialHits = 0;
    int hardHits = 0;
    int firstTouches = 0;
    // Defensive, neutral, offensive third relative to the player's team.
    std::array<int, 3> hitsByThird{};
    float hitSpeedSum = 0.0f;
    float maxHitSpeed = 0.0f;
    int demosInflicted = 0;
    int demosTaken = 0;
    // Every StatfeedEvent EventName the game emitted for this player.
    std::unordered_map<std::string, int> statEvents;
    std::optional<CarTelemetry> car;

    float AverageHitSpeed() const;
    int StatEvent(const std::string& eventName) const;
};

struct GoalRecord {
    float playSeconds = 0.0f;
    std::optional<float> clockSeconds;
    bool overtime = false;
    int team = -1;
    std::string scorerId;
    std::string scorerName;
    std::string assisterId;
    std::string assisterName;
    std::optional<float> goalSpeed;
    std::optional<float> lastTouchSpeed;
    std::optional<FieldPoint> location;
    // Length of the round the goal ended (Stats API GoalTime).
    std::optional<float> roundSeconds;
    std::array<int, 2> scoreAfter{};
};

struct HitRecord {
    float playSeconds = 0.0f;
    std::string playerId;
    int team = -1;
    FieldPoint location;
    float speed = 0.0f;
};

struct KickoffRecord {
    float playSeconds = 0.0f;
    std::string firstTouchId;
    int firstTouchTeam = -1;
    // Team with possession (last touch) three seconds of play after kickoff.
    int possessionTeam = -1;
    // Team that scored within ten seconds of play after kickoff, if any.
    int goalTeam = -1;
};

// Possession accumulated over consecutive windows of active play.
struct PossessionWindow {
    float endPlaySeconds = 0.0f;
    std::array<float, 2> seconds{};
};

struct PlayerDeparture {
    std::string primaryId;
    std::string name;
    int team = -1;
    float playSeconds = 0.0f;
    std::optional<float> clockSeconds;
};

struct MatchAnalytics {
    static constexpr int kFormatVersion = 1;
    static constexpr size_t kMaxHits = 2500;
    static constexpr float kPossessionWindowSeconds = 15.0f;

    int formatVersion = kFormatVersion;
    float playSeconds = 0.0f;
    std::array<float, 2> possessionSeconds{};
    float ballSpeedIntegral = 0.0f;
    float maxBallSpeed = 0.0f;
    int pauses = 0;
    float pausedSeconds = 0.0f;
    // Regulation clock when the match ended; > 0 without overtime means an early end (forfeit).
    std::optional<float> endClockSeconds;
    bool endedInOvertime = false;
    std::vector<PlayerAnalytics> players;
    std::vector<GoalRecord> goals;
    std::vector<HitRecord> hits;
    std::vector<KickoffRecord> kickoffs;
    std::vector<PossessionWindow> possessionTimeline;
    std::vector<PlayerDeparture> departures;

    bool Empty() const;
    bool EndedEarly() const;
    float AverageBallSpeed() const;
    float PossessionShare(int team) const;
    const PlayerAnalytics* FindPlayer(const std::string& primaryId) const;
};

nlohmann::json MatchAnalyticsToJson(const MatchAnalytics& analytics);
std::optional<MatchAnalytics> MatchAnalyticsFromJson(const nlohmann::json& json);

// Scoreline shape from one team's perspective, derived from GoalRecord order.
struct ScorelineSummary {
    int maxLead = 0;
    int maxDeficit = 0;
    bool comeback = false;  // won after trailing
    bool blownLead = false; // lost after leading
    bool overtime = false;
    bool lateWinner = false;      // decisive goal in the final 30 regulation seconds
    int goalsAfterKickoffFor = 0; // within 10 s of play after a kickoff
    int goalsAfterKickoffAgainst = 0;
    int lastMinuteGoalsFor = 0;
    int lastMinuteGoalsAgainst = 0;
};

ScorelineSummary AnalyzeScoreline(const MatchAnalytics& analytics, int myTeam, bool won);

class MatchAnalyticsTracker {
  public:
    void Reset();

    // nowSeconds is any monotonic clock in seconds; it drives sample durations.
    void OnUpdateState(const nlohmann::json& data, double nowSeconds);
    void OnCountdownBegin();
    void OnRoundStarted(double nowSeconds);
    void OnBallHit(const nlohmann::json& data, double nowSeconds);
    void OnBoostPickup(const nlohmann::json& data);
    void OnGoalScored(const nlohmann::json& data);
    void OnStatfeed(const nlohmann::json& data);
    void OnPlayerLeft(const nlohmann::json& data);
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

    PlayerAnalytics& PlayerFor(const std::string& primaryId);
    std::string ResolvePlayer(const nlohmann::json& ref) const;
    void AddPlayTime(float seconds);
    void CloseKickoffWindows();

    struct LastHit {
        double at = 0.0;
        float speed = 0.0f;
        bool counted = false;
    };
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
