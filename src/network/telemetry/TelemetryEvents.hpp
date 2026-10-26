#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/game/MatchAnalytics.hpp" // for FieldPoint

namespace TelemetryEvents {

    // One object level (root or Game) of forfeit metadata; levels are evaluated independently.
    struct ForfeitScope {
        bool localFlag = false;
        bool forfeit = false;
        std::optional<int> forfeitTeam;
        std::optional<int> winnerTeam;
    };

    bool HasExplicitLocalForfeitSignal(const std::vector<ForfeitScope>& scopes, int localTeam);

    struct PlayerRef {
        std::string name;
        std::string primaryId;
        int team = -1;
        std::optional<int> shortcut;
    };

    struct MatchCreatedEvent {
        std::string matchGuid;
    };

    struct MatchInitializedEvent {
        std::string matchGuid;
    };

    struct CountdownBeginEvent {
        std::string matchGuid;
    };

    struct RoundStartedEvent {
        std::string matchGuid;
    };

    struct MatchPausedEvent {
        std::string matchGuid;
    };

    struct MatchUnpausedEvent {
        std::string matchGuid;
    };

    struct GoalReplayStartEvent {
        std::string matchGuid;
    };

    struct GoalReplayWillEndEvent {};

    struct GoalReplayEndEvent {
        std::string matchGuid;
    };

    struct PodiumStartEvent {
        std::string matchGuid;
    };

    struct ReplayCreatedEvent {};

    struct PlayerLeftEvent {
        std::string primaryId;
        std::string name;
        std::string matchGuid;
    };

    struct CrossbarHitEvent {
        std::string matchGuid;
        PlayerRef lastTouch;
        std::optional<float> impactForce;
        std::string eventId;
    };

    struct MatchEndedEvent {
        std::string matchGuid;
        int winnerTeam = -1;
        std::array<int, 2> teamScores{0, 0};
        bool hasTeamScores = false;
        std::vector<ForfeitScope> forfeitScopes;
    };

    struct MatchDestroyedEvent {
        std::string matchGuid;
        std::array<int, 2> teamScores{0, 0};
        bool hasTeamScores = false;
        std::vector<ForfeitScope> forfeitScopes;
        std::optional<int> winnerTeam;
    };

    struct StatfeedEvent {
        std::string eventName;
        PlayerRef mainTarget;
        std::optional<PlayerRef> secondaryTarget;
        std::string eventId;
        std::string matchGuid;
    };

    struct GoalScoredEvent {
        std::string matchGuid;
        PlayerRef scorer;
        bool hasScorer = false;
        std::optional<PlayerRef> assister;
        std::optional<float> goalSpeed;
        std::optional<float> lastTouchSpeed;
        std::optional<float> roundSeconds;
        std::optional<FieldPoint> impactLocation;
        bool isEcho = false;
        std::string eventId;
    };

    struct BallHitEvent {
        std::vector<PlayerRef> hitters;
        float postHitSpeed = 0.0f;
        std::optional<FieldPoint> location;
        std::string matchGuid;
    };

    struct BoostPickupEvent {
        PlayerRef player;
        std::string boostType;
        std::optional<FieldPoint> location;
        bool bReplay = false;
        std::string matchGuid;
    };

    struct ClockUpdatedSecondsEvent {
        float seconds = 0.0f;
    };

    struct UpdatePlayerState {
        std::string primaryId;
        std::string name;
        int team = -1;
        std::optional<int> score;
        std::optional<int> goals;
        std::optional<int> assists;
        std::optional<int> saves;
        std::optional<int> shots;
        std::optional<int> demos;
        std::optional<int> touches;
        std::optional<int> carTouches;
        std::optional<int> shortcut;

        // Car state
        bool bHasCar = false;
        // Car-state entries omit false fields, so the key's presence matters on its own.
        bool hasCarField = false;
        bool bDemolished = false;
        std::optional<bool> bSupersonic;
        bool bOnWall = false;
        bool bOnGround = false;
        bool bPowersliding = false;
        bool bBoosting = false;
        float speed = 0.0f;
        std::optional<int> boost;
    };

    struct UpdateStateEvent {
        std::string arena;
        std::string arenaAsset;
        std::string matchGuid;
        std::optional<int> playlistId;
        bool bReplay = false;
        bool bOvertime = false;
        bool bSpectator = false;
        std::optional<float> timeSeconds;
        std::array<int, 2> teamScores{0, 0};
        bool hasTeamScores = false;
        std::array<bool, 2> teamScorePresent{false, false};

        // Ball
        bool hasBall = false;
        int ballTeam = -1;
        float ballSpeed = 0.0f;
        bool hasBallSpeed = false;
        std::optional<FieldPoint> ballLocation;

        // Camera target identity signal
        std::string cameraTargetPlayerId;

        // Forfeit / early exit flags
        std::vector<ForfeitScope> forfeitScopes;

        // Legacy context fallback
        bool explicitNonRecordable = false;
        std::string fallbackNonRecordableReason;
        bool explicitCasual = false;

        std::vector<UpdatePlayerState> players;
    };

    using TelemetryEvent = std::variant<
        MatchCreatedEvent,
        MatchInitializedEvent,
        CountdownBeginEvent,
        RoundStartedEvent,
        MatchPausedEvent,
        MatchUnpausedEvent,
        GoalReplayStartEvent,
        GoalReplayWillEndEvent,
        GoalReplayEndEvent,
        PodiumStartEvent,
        ReplayCreatedEvent,
        PlayerLeftEvent,
        CrossbarHitEvent,
        MatchEndedEvent,
        MatchDestroyedEvent,
        StatfeedEvent,
        GoalScoredEvent,
        BallHitEvent,
        BoostPickupEvent,
        UpdateStateEvent,
        ClockUpdatedSecondsEvent>;

    std::optional<TelemetryEvent> ParseEventFromJson(std::string_view eventName, const std::string& jsonStr);

} // namespace TelemetryEvents

using TelemetryEvent = TelemetryEvents::TelemetryEvent;
