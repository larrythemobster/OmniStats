#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <functional>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/SideEffects.hpp"

class TelemetryReducer {
  public:
    explicit TelemetryReducer(std::shared_ptr<SessionState> state);

    void SetClockForTests(std::function<std::chrono::steady_clock::time_point()> clockFn);

    SideEffects Reduce(const std::string& eventName, const nlohmann::json& data);
    void OnConfigChanged();
    void OnTelemetryDisconnected();
    SideEffects ConfirmPendingDestroyedMatch(const std::string& matchGuid, bool won);

  private:
    void HandleUpdateState(const nlohmann::json& data, SideEffects& effects);
    void CapturePreMatchMmrLocked();
    void HandleStatFeed(const nlohmann::json& data);
    void HandleGoalScored(const nlohmann::json& data, SideEffects& effects);
    void HandleBallHit(const nlohmann::json& data);
    void HandleCrossbarHit(const nlohmann::json& data);
    void HandleMatchEnded(const nlohmann::json& data, SideEffects& effects);
    void HandleMatchDestroyed(const nlohmann::json& data, SideEffects& effects);
    bool IsSelf(const std::string& name) const;
    bool IsSelfById(const std::string& pid) const;
    std::string ResolveRosterPlayerIdLocked(const nlohmann::json& target) const;
    DiscordPresenceSnapshot BuildDiscordSnapshotLocked() const;

    enum class MatchFinalizeSource {
        MatchEnded,
        MatchDestroyed,
        LocalForfeit,
        TrackerConfirmedDestroyed
    };

    struct CapturedMatch {
        std::string arenaName;
        std::string arenaAsset;
        std::string matchGuid;
        int playlistId = -1;
        uint64_t matchGeneration = 0;
        uint64_t sessionGeneration = 0;
        std::string myPrimaryId;
        int myTeam = -1;
        std::array<int, 2> score{};
        int legacyMaxPlayersSeen = 0;
        std::array<int, 2> legacyMaxTeamPlayersSeen{};
        bool roundEverStarted = false;
        bool localPlayerWasActive = false;
        bool localPlayerWasSpectator = false;
        bool legacyLobbyWasEverFull = false;
        bool localPlayerDisappeared = false;
        bool explicitLocalForfeit = false;
        bool fallbackCasualContext = false;
        bool fallbackNonRecordableContext = false;
        std::string fallbackNonRecordableReason;
        bool nonLiveReplay = false;
        MatchStats stats;
        std::unordered_map<std::string, PlayerData> roster;
        MmrCategory rosterMmrCategory = MmrCategory::Best;
        std::string mode;
        LocalPreMatchMmrSnapshot preMatchMmr;
        bool hasPreMatchMmr = false;
        int64_t endedAtUnixMs = 0;
        float durationSeconds = 0.0f;
        float overtimeSeconds = 0.0f;
    };

    void FinalizeMatchLocked(int winnerTeam, MatchFinalizeSource source, SideEffects& effects);
    void AddMatchToSessionTotalsLocked(const CapturedMatch& match,
                                       bool iWon,
                                       SessionTotals& sessionTotals,
                                       std::map<std::string, GamemodeStat>& sessionGamemodes);
    void FinalizeCapturedMatchLocked(CapturedMatch match,
                                     int winnerTeam,
                                     MatchFinalizeSource source,
                                     bool enqueueMmrRefresh,
                                     SideEffects& effects);
    CapturedMatch CaptureMatchLocked() const;
    bool BuildPostMatchMmrRefreshLocked(const CapturedMatch& match,
                                        bool won,
                                        PostMatchMmrRefresh& refresh) const;
    void RecordTerminalMatchGuidLocked(const std::string& matchGuid);
    void MarkDestroyedMatchVoidLocked(const std::string& reason, SideEffects& effects);
    bool IsValidEarlyCompetitiveExitLocked(std::string& mode, std::string& voidReason) const;
    void UpdateLifecycleSignalsLocked(const nlohmann::json& data);
    static bool HasExplicitLocalForfeitSignal(const nlohmann::json& data, int localTeam);
    bool AttachTerminalGuidToCurrentLocked(const std::string& eventMatchGuid);
    bool AcceptsUiMatchEventLocked(const nlohmann::json& data, int64_t nowMs);
    bool TargetsCurrentActiveMatchUiLocked(const nlohmann::json& data);

    struct MatchEndDecision {
        bool shouldCount = false;
        bool shouldPersist = false;
        bool iWon = false;
        std::string resultText = "Void";
        std::string voidReason;
    };

    MatchEndDecision ClassifyMatchEndLocked(const CapturedMatch& match, int winnerTeam) const;
    // Roster is left empty; callers move or copy it in.
    static MatchSaveSnapshot BuildMatchSaveSnapshot(const CapturedMatch& match,
                                                    int winnerTeam,
                                                    const MatchEndDecision& decision);
    static int LegacyExpectedTeamSizeForMode(const std::string& mode);

    std::shared_ptr<SessionState> m_state;
    ConfigData m_cachedConf;
    std::chrono::steady_clock::time_point m_lastConfigReadTime;

    std::string m_lastQueuedReplayGuid;
    std::string m_lastSavedMatchGuid;
    std::unordered_map<std::string, int> m_lastPlayerBoost;
    std::unordered_map<std::string, CapturedMatch> m_pendingDestroyedMatches;
    std::unordered_set<std::string> m_finalizedMatchGuids;
    uint64_t m_nextMatchGeneration = 0;
    bool m_missingGuidAssociationBlockedByReconnect = false;
    bool m_nonLiveReplayActive = false;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastPlayerSeen;

    bool m_roundActive = true;
    std::string m_uiMatchGuid;
    bool m_countdownSeenThisRound = false;
    std::unordered_set<std::string> m_identityCandidates;
    int m_missedMyIdCount = 0;
    MmrCategory m_autoSwitchedPlaylistCategory = MmrCategory::Best;
    MmrCategory m_followedGraphPlaylistCategory = MmrCategory::Best;
    struct PauseInterval {
        std::chrono::steady_clock::time_point start;
        std::chrono::steady_clock::time_point end;
    };
    // Regulation time comes from the game clock so goal replays and kickoff countdowns are excluded.
    std::optional<float> m_regulationClockStart;
    float m_regulationClockLatest = 0.0f;
    std::optional<std::chrono::steady_clock::time_point> m_overtimeStartedAt;
    std::optional<std::chrono::steady_clock::time_point> m_currentPauseStart;
    std::vector<PauseInterval> m_pauseIntervals;

    std::function<std::chrono::steady_clock::time_point()> m_clockFn;
    std::chrono::steady_clock::time_point Now() const {
        if (m_clockFn) return m_clockFn();
        return std::chrono::steady_clock::now();
    }
    float PausedSecondsWithin(std::chrono::steady_clock::time_point windowStart,
                              std::chrono::steady_clock::time_point windowEnd) const;
    void ResetMatchTimingState();
};
