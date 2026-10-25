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

struct SessionTotals {
    int wins = 0;
    int losses = 0;
    int goals = 0;
    int saves = 0;
    int savesTotal = 0;
    int shots = 0;
    int shotsTotal = 0;
    int demos = 0;
    int demosTotal = 0;
    int demoed = 0;
    int assists = 0;
    int assistsTotal = 0;
    int crossbars = 0;
    int crossbarsTotal = 0;

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
    float totalMmrChange = 0.0f;
    std::map<std::string, int> mmrChangeByPlaylist;

    int teamGoals = 0;          // Total goals scored by user's team this session
    int goalParticipations = 0; // User goals + user assists, clamped per match
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

inline int CalculateTrackedSessionMmrChange(const std::map<std::string, int>& changes) {
    int total = 0;
    for (const auto& [playlist, change] : changes) {
        if (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" ||
            playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" ||
            playlist == "snowday" || playlist == "heatseeker") {
            total += change;
        }
    }
    return total;
}

struct SessionMmrPoint {
    std::string matchGuid;
    size_t historyIndex = 0;
    int mmr = 0;
    int trackerMatchesPlayed = -1;
    bool trackerCovered = false;
    bool valueEstimated = true;
};

struct LocalPreMatchMmrSnapshot {
    std::map<std::string, int> playlistMmrs;
    std::map<std::string, int> playlistMatches;
};

struct SessionMatchSummary {
    int64_t matchId = 0;
    bool ranked = true;
    std::string mode;
    std::string matchGuid;
    int ourScore = 0;
    int theirScore = 0;
    int mmr = 0;
    bool mmrEstimated = false;
    bool win = false;
    bool pendingTrackerConfirmation = false;
    int64_t endedAtUnix = 0;
};

inline constexpr int kPreviousGamesDefaultLimit = 20;
inline constexpr int kPreviousGamesMaxLimit = 50;

enum class MmrCategory : int {
    Best = 0,
    OneVOne = 1,
    TwoVTwo = 2,
    ThreeVThree = 3,
    Casual = 4,
    Tourny = 5,
    Hoops = 6,
    Rumble = 7,
    Dropshot = 8,
    SnowDay = 9,
    Heatseeker = 10
};

std::string MmrCategoryToString(MmrCategory cat);
MmrCategory StringToMmrCategory(const std::string& str);
bool IsExtraMmrCategory(MmrCategory cat);

struct GamemodeStat {
    int wins = 0;
    int losses = 0;
    int total = 0;
};

struct CachedDbStats {
    int currentWins = 0;
    int currentLosses = 0;
    int longestWins = 0;
    int longestLosses = 0;
    std::map<std::string, GamemodeStat> gamemodes;
    int detailedStatsMatchCount = 0;
    std::string detailedStatsSinceDate;
};

// Session totals captured when a session ends (Rocket League closed with
// reset_session_on_close) so the recap can still be shown after the reset.
struct SessionRecap {
    bool valid = false;
    int64_t endedAtUnix = 0;
    SessionTotals totals;
    std::map<std::string, GamemodeStat> gamemodes;
    uint64_t sessionGeneration = 0;
    std::string mmrOwnerPrimaryId;
    std::map<std::string, int> playlistInitialMmr;
    int64_t id = 0;
    std::string account;
    int64_t startedAtUnix = 0;
    std::string source = "live";
    std::vector<std::string> matchGuids;

    int Games() const {
        return totals.wins + totals.losses;
    }
    bool HasKnownMmrChange() const {
        return !totals.mmrChangeByPlaylist.empty();
    }
    int NetMmrChange() const {
        return CalculateTrackedSessionMmrChange(totals.mmrChangeByPlaylist);
    }
};

struct MatchQuery {
    enum Sort {
        Newest = 0,
        Oldest = 1,
        BiggestWin = 2,
        BiggestLoss = 3,
        ScoreMargin = 4,
    };

    std::string account;
    std::optional<std::string> playlistLabel;
    std::optional<bool> ranked;
    std::optional<bool> win;
    int64_t fromUnix = 0;
    int64_t toUnix = 0;
    std::string arena;
    std::string withPlayer;
    std::string againstPlayer;
    std::string nameSearch;
    int offset = 0;
    int limit = 100;
    Sort sort = Sort::Newest;
};

struct MatchRow {
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string playlist;
    std::string gamemode;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int myMmr = 0;
    bool mmrEstimated = false;
    std::optional<int> mmrDelta;
    std::string arena;
    std::vector<std::string> teammates;
    bool hasBallchasing = false;
    std::string ballchasingUrl;
    std::optional<int> myScore;
    std::optional<int> myGoals;
    std::optional<int> myAssists;
    std::optional<int> mySaves;
    std::optional<int> myShots;
    // Highest scoreboard score in the lobby and on the winning team.
    bool mvp = false;
    bool overtime = false;
    bool endedEarly = false;
    std::optional<ScorelineSummary> scoreline;
};

struct MatchDetailPlayer {
    std::string primaryId;
    std::string name;
    std::string platform;
    int team = 0;
    bool isOpponent = false;
    bool isMe = false;
    int mmr = 0;
    bool mmrEstimated = false;
    std::string tier = "Unranked";
    bool hasStats = false;
    std::optional<int> score;
    std::optional<int> goals;
    std::optional<int> assists;
    std::optional<int> saves;
    std::optional<int> shots;
    std::optional<int> demos;
    std::optional<int> touches;
    std::optional<int> carTouches;
    std::optional<float> maxGoalSpeed;
    std::optional<float> fastestGoalTime;
    // Record with this player in other saved matches; nullopt when unavailable.
    std::optional<PersonRecord> encounter;
};

struct MatchLocalStatsRow {
    int boostCollected = 0;
    int demoed = 0;
    int crossbars = 0;
    std::optional<float> hardestCrossbar;
    std::optional<float> maxBallSpeed;
    int ownGoals = 0;
};

struct MatchDetail {
    bool found = false;
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string arena;
    std::string playlist;
    std::string gamemode;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    std::string accountPrimaryId;
    std::optional<int> mmrBefore;
    std::optional<int> mmrAfter;
    std::optional<int> mmrDelta;
    bool mmrEstimated = false;
    bool hasPlayerStats = false;
    bool hasLocalStats = false;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    std::string ballchasingId;
    std::string ballchasingUrl;
    std::vector<MatchDetailPlayer> ourTeam;
    std::vector<MatchDetailPlayer> theirTeam;
    std::vector<MatchJournalEvent> events;
    std::optional<MatchLocalStatsRow> localStats;
    // Absent for matches saved before analytics capture.
    std::optional<MatchAnalytics> analytics;
};

// One saved match from the account's perspective, for insight aggregation.
struct AnalyticsMatchRow {
    int64_t matchId = 0;
    int64_t timestampUnix = 0;
    std::string playlist;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int myTeam = -1;
    std::string arena;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    std::optional<int> score;
    std::optional<int> goals;
    std::optional<int> assists;
    std::optional<int> saves;
    std::optional<int> shots;
    std::optional<int> demos;
    std::optional<int> touches;
    std::optional<MatchLocalStatsRow> localStats;
    std::optional<MatchAnalytics> analytics;
};

// Database aggregates behind the Insights window, loaded on demand.
struct InsightsState {
    mutable std::mutex mutex;
    std::atomic<uint64_t> version{0};
    bool loaded = false;
    std::string primaryId;
    std::vector<PersonRecord> people;
    std::vector<MatchOutcome> outcomes;
    std::vector<MatchMmrContext> mmrContext;
    std::vector<SessionRecap> sessions;
    std::vector<AnalyticsMatchRow> analyticsMatches;
};

struct HistoryViewState {
    mutable std::mutex mutex;
    std::atomic<uint64_t> version{0};
    uint64_t requestId = 0;
    uint64_t detailRequestId = 0;
    bool loading = false;
    bool detailLoading = false;
    bool appendMode = false;
    MatchQuery query;
    std::vector<MatchRow> rows;
    int total = 0;
    std::optional<MatchDetail> detail;
};

struct ReplayUploadStatus {
    int uploadedToday = 0;
    int retrying = 0;
    int failed = 0;
    int total = 0;

    bool operator==(const ReplayUploadStatus&) const = default;
};

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

struct AccountMmrHistory {
    std::vector<float> mmrHistoryY;
    std::vector<float> mmrHistoryX;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, std::vector<SessionMmrPoint>> playlistMatchPoints;
    std::map<std::string, int> playlistInitialMmr;
    int initialMmr = -1;
};

struct HistoryState {
    std::shared_mutex mutex;
    std::atomic<uint64_t> version{1};

    // MMR history for graphing
    std::vector<float> mmrHistoryY;
    std::vector<float> mmrHistoryX;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    // Match ownership metadata is authoritative. playlistHistoryY remains the
    // float projection consumed by the existing graph widget.
    std::map<std::string, std::vector<SessionMmrPoint>> playlistMatchPoints;
    // Absent when no pre-match rating was observed; never backfilled later.
    std::map<std::string, int> playlistInitialMmr;
    std::string mmrOwnerPrimaryId;
    std::map<std::string, AccountMmrHistory> inactiveMmrHistories;
    void SelectMmrOwner(const std::string& primaryId);
    void ResetSessionMmr();

    // Lifetime MMR history for graphing
    std::vector<float> lifetimeMmrY;
    std::vector<int64_t> lifetimeMmrX;
    std::atomic<bool> showLifetimeGraph{false};

    // Recent saved match history for the previous-games dashboard card.
    // Destroyed competitive matches can also appear here provisionally while
    // Tracker confirmation is pending; those are kept separate so normal DB
    // refreshes cannot make the pending row disappear.
    std::vector<SessionMatchSummary> recentSavedMatches;
    std::vector<SessionMatchSummary> pendingRecentMatches;
    bool recentSavedMatchesLoaded = false;

    int initialMmr = -1;
};

// Local player notes mirrored from SQLite by the database worker. The UI reads
// this cache; it never queries the database on the render path.
struct PlayerNotesState {
    mutable std::mutex mutex;
    std::atomic<uint64_t> version{0};
    bool loaded = false;
    bool loadFailed = false;
    std::unordered_map<std::string, PlayerNotes::Note> byKey; // Identity::Key()
    std::unordered_map<std::string, std::vector<std::string>> tagsByKey;
    // Outcome of the most recent save/delete, matched to the editor by request id.
    uint64_t completedRequestId = 0;
    bool lastRequestOk = true;
    std::string lastRequestError;
    uint64_t completedTagRequestId = 0;
    bool lastTagRequestOk = true;
    std::string lastTagRequestError;
};

struct PlayerScoutState {
    mutable std::mutex mutex;
    std::atomic<uint64_t> version{0};
    uint64_t requestId = 0;
    std::string playerId;
    bool loaded = false;
    bool success = false;
    PlayerScoutProfile profile;
};

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
    bool startNewSessionLocked(int64_t endedAtUnix);
    bool closeSessionIfInactiveLocked(int64_t nowUnix);
    void syncSessionMmrChangeLocked();
    void selectMmrOwnerLocked(const std::string& primaryId);
};
