#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <nlohmann/json.hpp>
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/SideEffects.hpp"

class TelemetryReducer {
  public:
    explicit TelemetryReducer(std::shared_ptr<SessionState> state);

    SideEffects Reduce(const std::string& eventName, const nlohmann::json& data);
    void OnConfigChanged();
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
        uint64_t matchGeneration = 0;
        std::string myPrimaryId;
        int myTeam = -1;
        std::array<int, 2> score{};
        int maxPlayersSeen = 0;
        std::array<int, 2> maxTeamPlayersSeen{};
        bool roundEverStarted = false;
        bool localPlayerWasActive = false;
        bool localPlayerWasSpectator = false;
        bool lobbyWasEverFull = false;
        bool localPlayerDisappeared = false;
        bool explicitLocalForfeit = false;
        bool nonLiveReplay = false;
        MatchStats stats;
        std::unordered_map<std::string, PlayerData> roster;
        MmrCategory rosterMmrCategory = MmrCategory::Best;
        MmrCategory graphMmrCategory = MmrCategory::Best;
        std::string mode;
        LocalPreMatchMmrSnapshot preMatchMmr;
        bool hasPreMatchMmr = false;
        int64_t endedAtUnixMs = 0;
    };

    void FinalizeMatchLocked(int winnerTeam, MatchFinalizeSource source, SideEffects& effects);
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

    struct MatchEndDecision {
        bool shouldCount = false;
        bool shouldPersist = false;
        bool iWon = false;
        std::string resultText = "Void";
        std::string voidReason;
    };

    MatchEndDecision ClassifyMatchEndLocked(const CapturedMatch& match, int winnerTeam) const;
    static int ExpectedTeamSizeForMode(const std::string& mode);

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
    std::unordered_set<std::string> m_identityCandidates;
    int m_missedMyIdCount = 0;
    MmrCategory m_autoSwitchedPlaylistCategory = MmrCategory::Best;
};
