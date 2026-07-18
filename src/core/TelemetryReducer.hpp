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

  private:
    void HandleUpdateState(const nlohmann::json& data, SideEffects& effects);
    void HandleStatFeed(const nlohmann::json& data);
    void HandleGoalScored(const nlohmann::json& data, SideEffects& effects);
    void HandleBallHit(const nlohmann::json& data);
    void HandleCrossbarHit(const nlohmann::json& data);
    void HandleMatchEnded(const nlohmann::json& data, SideEffects& effects);
    bool IsSelf(const std::string& name) const;
    bool IsSelfById(const std::string& pid) const;
    DiscordPresenceSnapshot BuildDiscordSnapshotLocked() const;

    enum class MatchFinalizeSource {
        MatchEnded,
        MatchDestroyed
    };

    void FinalizeMatchLocked(int winnerTeam, MatchFinalizeSource source, SideEffects& effects);

    struct MatchEndDecision {
        bool shouldCount = false;
        bool shouldPersist = false;
        bool iWon = false;
        std::string resultText = "Void";
        std::string voidReason;
    };

    MatchEndDecision ClassifyMatchEndLocked(int winnerTeam) const;
    static int ExpectedTeamSizeForMode(const std::string& mode);

    std::shared_ptr<SessionState> m_state;
    ConfigData m_cachedConf;
    std::chrono::steady_clock::time_point m_lastConfigReadTime;

    std::string m_lastQueuedReplayGuid;
    std::string m_lastSavedMatchGuid;
    std::unordered_map<std::string, int> m_lastPlayerBoost;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> m_lastPlayerSeen;

    bool m_roundActive = true;
    std::unordered_set<std::string> m_identityCandidates;
    int m_missedMyIdCount = 0;
    MmrCategory m_autoSwitchedPlaylistCategory = MmrCategory::Best;
};
