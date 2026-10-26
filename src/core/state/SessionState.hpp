#pragma once
#include "core/state/MatchEvent.hpp"
#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <vector>
#include <map>
#include <array>
#include <memory>
#include <cstdint>
#include <cstddef>
#include <optional>
#include "core/game/Insights.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/accounts/PlayerNotes.hpp"
#include "core/accounts/PlayerScout.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "core/state/SessionTypes.hpp"

// Data for individual players in the current lobby
struct PlayerData {
    std::string primaryId;
    std::string name;
    int team = -1;
    int mmr = 0;
    std::string rankTier = "Unranked";
    int lifetimeWinsWith = 0;      // Matches won with this player as teammate all-time
    int lifetimeLossesWith = 0;    // Matches lost with this player as teammate all-time
    int lifetimeWinsAgainst = 0;   // Matches won against this player as opponent all-time
    int lifetimeLossesAgainst = 0; // Matches lost against this player as opponent all-time
    bool hasLifetimeData = false;
    bool fetched = false;
    bool fetchFailed = false;
    bool enqueued = false;

    std::string rankVerificationSource = "";
    std::map<std::string, int> playlists;             // MMR for each playlist: "1v1", "2v2", "3v3", extra modes, "best"
    std::map<std::string, std::string> playlistTiers; // Rank text for each playlist
    std::map<std::string, int> playlistMatches;       // Matches played for each playlist from Tracker.gg

    // Per-match stats for this player (reset when roster is cleared on match start)
    int goals = 0;
    int saves = 0;
    int shots = 0;
    int demos = 0;
    int assists = 0;
    int totalWins = -1;
    // Per-player goal metrics
    float maxGoalSpeed = 0.0f;
    float fastestGoalTime = 0.0f;
    int score = -1; // Scoreboard points from UpdateState; -1 until observed.
    std::optional<int> touches;
    std::optional<int> carTouches;
};

// Real-time telemetry for the active match
struct MatchStats {
    int goals = 0;
    int goalsSelf = 0;
    int saves = 0;
    int savesSelf = 0;
    int shots = 0;
    int shotsSelf = 0;
    int demos = 0;
    int demosSelf = 0;
    int assists = 0;
    int assistsSelf = 0;
    int demoedSelf = 0;
    int crossbars = 0;
    int crossbarsSelf = 0;

    float maxGoalSpeed = 0.0f;
    float maxGoalSpeedSelf = 0.0f;
    float maxBallSpeed = 0.0f;
    float maxBallSpeedSelf = 0.0f;
    float maxImpactForce = 0.0f;
    float maxImpactForceSelf = 0.0f;

    float fastestGoalTime = 0.0f;
    float fastestGoalTimeSelf = 0.0f;
    int ownGoals = 0;
    int ownGoalsSelf = 0;
    int boostPickedUp = 0;
    int boostPickedUpSelf = 0;
};

struct DemolitionCounts {
    int demos = 0;
    int demoed = 0;
};

inline DemolitionCounts CalculateSessionDemolitionCounts(
    const SessionTotals& sessionTotals,
    const MatchStats& currentMatch,
    bool matchFinalized) {
    const int liveDemos = matchFinalized ? 0 : currentMatch.demosSelf;
    const int liveDemoed = matchFinalized ? 0 : currentMatch.demoedSelf;
    return {
        sessionTotals.demos + liveDemos,
        sessionTotals.demoed + liveDemoed};
}

inline int CalculateSessionBoostPickedUp(
    const SessionTotals& sessionTotals,
    const MatchStats& currentMatch,
    bool matchFinalized) {
    return sessionTotals.boostPickedUp +
           (matchFinalized ? 0 : currentMatch.boostPickedUpSelf);
}

#include "core/state/StoreSnapshots.hpp"
#include "database/store/HistoryStore.hpp"
#include "database/store/InsightsStore.hpp"
#include "database/store/HistoryViewStore.hpp"
#include "database/store/PlayerNotesStore.hpp"
#include "database/store/PlayerScoutStore.hpp"

using InsightsState = database::store::InsightsStore;
using HistoryViewState = database::store::HistoryViewStore;

struct ConfigData;

// Decomposed state sections
struct UIState {
    // Visibility & UI Flags controlled by InputManager/Overlay Settings
    std::atomic<bool> showOverlay{false}; // Triggered by Tab/LB
    std::atomic<bool> showMenu{false};    // Triggered by F5
    std::atomic<bool> mmrEnabled{true};
    std::atomic<MmrCategory> rosterMmrCategory{MmrCategory::Best};
    std::atomic<MmrCategory> graphMmrCategory{MmrCategory::TwoVTwo};
    std::atomic<MmrCategory> rankProgressMmrCategory{MmrCategory::TwoVTwo};

    // F7 while F8 is active toggles the lifetime MMR graph. It always shows the
    // latest 25 matches, preserving per-match dots at the overlay's card width.
    std::atomic<bool> h2hExpanded{false};     // F7: Show live match stats
    std::atomic<bool> showSessionView{false}; // F8: Show session stats/graph instead of H2H
    std::atomic<bool> showGraphView{false};
    std::atomic<int> graphOffset{0};

    // Auto match summary popup
    std::atomic<bool> showMatchSummary{false};
    std::atomic<int64_t> matchSummaryStartMs{0};
    // Set when a finished session was captured into lastSessionRecap.
    std::atomic<bool> showSessionRecap{false};

    // Steady-clock millisecond timestamps for event-driven overlay visibility.
    std::atomic<int64_t> lastMatchStartMs{0};
    std::atomic<int64_t> firstCountdownOfMatchMs{0};
    std::atomic<int64_t> lastCountdownMs{0};
    std::atomic<int64_t> lastGoalMs{0};
    std::atomic<int64_t> lastMatchEndMs{0};
    std::atomic<int64_t> lastPodiumMs{0};
    std::atomic<bool> inGoalReplay{false};

    void ResetMatchTimestamps(int64_t matchStartMs = 0) noexcept {
        lastMatchStartMs.store(matchStartMs, std::memory_order_relaxed);
        firstCountdownOfMatchMs.store(0, std::memory_order_relaxed);
        lastCountdownMs.store(0, std::memory_order_relaxed);
        lastGoalMs.store(0, std::memory_order_relaxed);
        lastMatchEndMs.store(0, std::memory_order_relaxed);
        lastPodiumMs.store(0, std::memory_order_relaxed);
        inGoalReplay.store(false, std::memory_order_relaxed);
    }

    // Initial applies the pinned rank-progress playlist; later revisions only drop hidden extra playlists.
    void ApplyRankProgressConfig(const ConfigData& config, bool initial);

    // Dashboard layout edit mode
    std::atomic<bool> dashboardLayoutEditMode{false};

    // Caching layer for Database Stats
    mutable std::mutex dbStatsMutex;
    CachedDbStats cachedDbStats;
    std::atomic<bool> dbStatsDirty{true};
    std::atomic<uint64_t> dbStatsVersion{1};

    // Input debug/capture state (written by InputManager, read by the UI controller)
    std::atomic<int> lastKeyboardKeyPressed{-1};
    std::atomic<int> lastControllerButtonPressed{-1};
    std::atomic<int> lastRawControllerButtonPressed{-1};
    std::atomic<bool> inputCaptureActive{false};
    std::atomic<bool> controllerIsGameController{false};
    std::atomic<bool> controllerConnected{false};
    // Protected by a simple mutex since std::string isn't atomic
    mutable std::mutex controllerDebugMutex;
    std::string controllerDebugName;

    // Background update discovery status. Checks run at startup, periodically,
    // and when Settings is opened; installing an update remains user-controlled
    // while the app is already running.
    std::atomic<bool> updateChecked{false};
    std::atomic<bool> updateAvailable{false};
    std::atomic<bool> updateDownloading{false};
    std::atomic<bool> updateDownloadFailed{false};
    std::atomic<bool> updatePromptShown{false};
    std::atomic<bool> appExitRequested{false};
    mutable std::mutex updateMutex;
    std::string updateAvailableVersion;
    std::string updateServerUrl;

    // Stats API config verification state (set on startup or settings check)
    mutable std::mutex statsApiMutex;
    StatsApiConfig::CheckResult statsApiResult;
    std::atomic<bool> statsApiChecked{false};
    std::atomic<bool> ballchasingTokenRejected{false};
    mutable std::mutex replayUploadMutex;
    ReplayUploadStatus replayUploadStatus;
    std::string lastBallchasingMatchGuid;
    std::string lastBallchasingUrl;
    std::atomic<uint64_t> replayUploadVersion{0};
};

using HistoryState = database::store::HistoryStore;
using PlayerNotesState = database::store::PlayerNotesStore;
using PlayerScoutState = database::store::PlayerScoutStore;

class LiveMatchStore;

class SessionState : public std::enable_shared_from_this<SessionState> {
  public:
    UIState ui;
    SessionState();
    ~SessionState();

    std::shared_ptr<LiveMatchStore> liveMatch;
    HistoryState history;
    InsightsState insights;
    HistoryViewState historyView;
    PlayerNotesState notes;
    PlayerScoutState scout;

    void clearActiveMatchOnDisconnect();

    // Session management methods.
    // Returns whether a recap was captured.
    bool startNewSessionLocked(int64_t endedAtUnix, database::store::HistoryData& data);
    bool closeSessionIfInactiveLocked(int64_t nowUnix, database::store::HistoryData& data);
    void syncSessionMmrChangeLocked(database::store::HistoryData& data);
    void selectMmrOwnerLocked(const std::string& primaryId, database::store::HistoryData& data);
};
