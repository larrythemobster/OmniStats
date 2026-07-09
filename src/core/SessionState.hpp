#pragma once
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
#include "core/StatsApiConfig.hpp"

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

    std::map<std::string, int> playlists; // MMR for each playlist: "1v1", "2v2", "3v3", extra modes, "best"
    std::map<std::string, std::string> playlistTiers; // Rank text for each playlist
    std::map<std::string, int> playlistMatches; // Matches played for each playlist from Tracker.gg

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

    int teamGoals = 0;              // Total goals scored by user's team this session
    int goalParticipations = 0;     // User goals + user assists, clamped per match
};

struct SessionMatchSummary {
    bool ranked = true;
    std::string mode;
    int ourScore = 0;
    int theirScore = 0;
    int mmr = 0;
    bool win = false;
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
};

// Decomposed state sections
struct UIState {
    // Visibility & UI Flags controlled by InputManager/Overlay Settings
    std::atomic<bool> showOverlay{false}; // Triggered by Tab/LB
    std::atomic<bool> showMenu{false};    // Triggered by F5
    std::atomic<bool> mmrEnabled{true};
    std::atomic<MmrCategory> rosterMmrCategory{MmrCategory::Best};
    std::atomic<MmrCategory> graphMmrCategory{MmrCategory::TwoVTwo};

    // New flags for expanded views
    std::atomic<bool> h2hExpanded{false};       // F7: Show live match stats
    std::atomic<bool> showSessionView{false};   // F8: Show session stats/graph instead of H2H
    std::atomic<bool> showGraphView{false};     // F7 while F8 active: Toggle between text and graph

    // Auto match summary popup
    std::atomic<bool> showMatchSummary{false};
    std::atomic<int64_t> matchSummaryStartMs{0};

    // Dashboard layout edit mode
    std::atomic<bool> dashboardLayoutEditMode{false};

    // Caching layer for Database Stats
    mutable std::mutex dbStatsMutex;
    CachedDbStats cachedDbStats;
    std::atomic<bool> dbStatsDirty{true};

    // Input debug/capture state (written by InputManager, read by SettingsPanel)
    std::atomic<int> lastKeyboardKeyPressed{-1};
    std::atomic<int> lastControllerButtonPressed{-1};
    std::atomic<int> lastRawControllerButtonPressed{-1};
    std::atomic<bool> inputCaptureActive{false};
    std::atomic<bool> controllerIsGameController{false};
    std::atomic<bool> controllerConnected{false};
    // Protected by a simple mutex since std::string isn't atomic
    mutable std::mutex controllerDebugMutex;
    std::string controllerDebugName;

    // Background update check status (Startup check when auto-updates are disabled)
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
};

struct GameState {
    std::shared_mutex mutex;
    std::atomic<uint64_t> version{1};

    // Match Lifecycle State
    std::atomic<bool> inMatch{false};
    std::atomic<bool> inReplay{false};
    int myTeam = -1;
    std::string myPrimaryId = ""; // PrimaryId of the local player when identified
    std::string arenaName = "";
    std::string arenaAsset = "";
    std::string matchGuid = "";
    std::array<int, 2> score{};
    int maxPlayersSeen = 0;

    bool roundEverStarted = false;
    bool localPlayerWasActive = false;
    bool localPlayerWasSpectator = false;
    bool lobbyWasEverFull = false;

    std::array<int, 2> currentTeamPlayersSeen{0, 0};
    std::array<int, 2> maxTeamPlayersSeen{0, 0};

    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    std::array<int, 2> matchSummaryScore{};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;

    // Global Collections
    MatchStats currentMatch;
    SessionTotals sessionTotals;
    std::unordered_map<std::string, PlayerData> roster;
    std::unordered_map<std::string, PlayerData> matchRoster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    bool matchFinalized = false;
};

struct HistoryState {
    std::shared_mutex mutex;
    std::atomic<uint64_t> version{1};

    // MMR history for graphing
    std::vector<float> mmrHistoryY;
    std::vector<float> mmrHistoryX;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, int> playlistInitialMmr;

    // Lifetime MMR history for graphing
    std::vector<float> lifetimeMmrY;
    std::vector<float> lifetimeMmrX;
    std::atomic<bool> showLifetimeGraph{false};

    // Recent saved match history for the previous-games dashboard card
    std::vector<SessionMatchSummary> recentSavedMatches;
    bool recentSavedMatchesLoaded = false;

    int initialMmr = -1;
};

class SessionState : public std::enable_shared_from_this<SessionState> {
public:
    UIState ui;
    GameState game;
    HistoryState history;

    void resetMatch(const std::string& newArena, const std::string& newArenaAsset = "");
};
