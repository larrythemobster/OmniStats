#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/config/Config.hpp"
#include "core/effects/SideEffects.hpp"
#include "core/state/DiscordPresenceSnapshot.hpp"
#include "core/state/MatchState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

struct ReducerContext {
    int64_t nowMs = 0;       // Monotonic clock ms (for UI timestamps, interval checks)
    double nowSeconds = 0.0; // Monotonic clock seconds (for analytics play time)
    int64_t nowUnix = 0;     // System clock Unix epoch seconds (for match/session persistence)
    ConfigData config{};
    // Categories currently shown, so following respects user overrides made between events.
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    MmrCategory rankProgressMmrCategory = MmrCategory::TwoVTwo;
};

struct UIState;

ReducerContext MakeReducerContext(const UIState& ui);

struct ReducerResult {
    MatchState state;
    SideEffects effects;
};

class TelemetryReducer {
  public:
    TelemetryReducer() = default;
    explicit TelemetryReducer(MatchState initialState) : m_state(std::move(initialState)) {}

    // Pure functional API: (MatchState, Event, ReducerContext) -> (MatchState, SideEffects)
    static ReducerResult Reduce(MatchState state,
                                const TelemetryEvents::TelemetryEvent& event,
                                const ReducerContext& context);

    // In-place reduction for zero copy
    static SideEffects ReduceInPlace(MatchState& state,
                                     const TelemetryEvents::TelemetryEvent& event,
                                     const ReducerContext& context);

    static SideEffects ConfirmPendingDestroyedMatch(MatchState& state,
                                                    const std::string& matchGuid,
                                                    bool won,
                                                    const ReducerContext& context);

    static void OnTelemetryDisconnected(MatchState& state);

    // Instance convenience methods
    SideEffects Reduce(const TelemetryEvents::TelemetryEvent& event, const ReducerContext& context) {
        return ReduceInPlace(m_state, event, context);
    }
    const MatchState& State() const {
        return m_state;
    }
    MatchState& State() {
        return m_state;
    }

    static DiscordPresenceSnapshot BuildDiscordSnapshot(const MatchState& state);

    enum class MatchFinalizeSource {
        MatchEnded,
        MatchDestroyed,
        LocalForfeit,
        TrackerConfirmedDestroyed
    };

    struct MatchEndDecision {
        bool shouldCount = false;
        bool shouldPersist = false;
        bool iWon = false;
        std::string resultText = "Void";
        std::string voidReason;
    };

    static CapturedMatch CaptureMatch(const MatchState& state);
    static MatchEndDecision ClassifyMatchEnd(const CapturedMatch& match, int winnerTeam, const ConfigData& config);
    static MatchSaveSnapshot BuildMatchSaveSnapshot(const CapturedMatch& match, int winnerTeam, const MatchEndDecision& decision);
    static void FinalizeMatch(MatchState& state, int winnerTeam, MatchFinalizeSource source, const ReducerContext& context, SideEffects& effects);
    static bool IsValidEarlyCompetitiveExit(const MatchState& state, std::string& mode, std::string& voidReason);
    static int LegacyExpectedTeamSizeForMode(const std::string& mode);

  private:
    MatchState m_state;
};
