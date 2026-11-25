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

struct ReducerSettings {
    bool reset_session_on_close;
    bool enable_mmr_tracking;
    bool custom_api_enabled;
    bool has_account_refresh_token;
    bool show_extra_playlists;
    bool auto_switch_mmr_category;
    bool graph_follow_current_playlist;
    bool rank_progress_follow_current_playlist;
    std::string graph_mmr_category;
    std::string rank_progress_mmr_category;

    ReducerSettings() : ReducerSettings(ConfigData{}) {}
    ReducerSettings(const ConfigData& config)
        : reset_session_on_close(config.reset_session_on_close),
          enable_mmr_tracking(config.enable_mmr_tracking),
          custom_api_enabled(config.custom_api_enabled),
          has_account_refresh_token(!config.account_refresh_token.empty()),
          show_extra_playlists(config.show_extra_playlists),
          auto_switch_mmr_category(config.auto_switch_mmr_category),
          graph_follow_current_playlist(config.graph_follow_current_playlist),
          rank_progress_follow_current_playlist(config.rank_progress_follow_current_playlist),
          graph_mmr_category(config.graph_mmr_category),
          rank_progress_mmr_category(config.rank_progress_mmr_category) {}

    bool ShouldReconcileMmr() const {
        return enable_mmr_tracking || (custom_api_enabled && has_account_refresh_token);
    }
};

struct ReducerContext {
    int64_t nowMs = 0;       // Monotonic clock ms (for UI timestamps, interval checks)
    double nowSeconds = 0.0; // Monotonic clock seconds (for analytics play time)
    int64_t nowUnix = 0;     // System clock Unix epoch seconds (for match/session persistence)
    int64_t nowUnixMs = 0;   // System clock Unix epoch milliseconds
    ReducerSettings config{};
    // Categories currently shown, so following respects user overrides made between events.
    MmrCategory rosterMmrCategory = MmrCategory::Best;
    MmrCategory rankProgressMmrCategory = MmrCategory::TwoVTwo;
    MmrCategory graphMmrCategory = MmrCategory::TwoVTwo;
};

struct UIState;

ReducerContext MakeReducerContext(const UIState& ui);
ReducerContext MakeReducerContext(const UIState& ui, const ReducerSettings& settings);

struct ReducerResult {
    MatchState state;
    SideEffects effects;
};

class TelemetryReducer {
  public:
    TelemetryReducer() = default;
    explicit TelemetryReducer(MatchState initialState) : m_state(std::move(initialState)) {}

    static ReducerResult Reduce(MatchState state,
                                const TelemetryEvents::TelemetryEvent& event,
                                const ReducerContext& context);

    static SideEffects ReduceInPlace(MatchState& state,
                                     const TelemetryEvents::TelemetryEvent& event,
                                     const ReducerContext& context);

    static SideEffects ConfirmPendingDestroyedMatch(MatchState& state,
                                                    const std::string& matchGuid,
                                                    bool won,
                                                    const ReducerContext& context);

    static void OnTelemetryDisconnected(MatchState& state);

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
    static MatchEndDecision ClassifyMatchEnd(const CapturedMatch& match, int winnerTeam, const ReducerSettings& config);
    static MatchSaveSnapshot BuildMatchSaveSnapshot(const CapturedMatch& match, int winnerTeam, const MatchEndDecision& decision);
    static void FinalizeMatch(MatchState& state, int winnerTeam, MatchFinalizeSource source, const ReducerContext& context, SideEffects& effects);
    static bool IsValidEarlyCompetitiveExit(const MatchState& state, std::string& mode, std::string& voidReason);
    static int LegacyExpectedTeamSizeForMode(const std::string& mode);

  private:
    MatchState m_state;
};
