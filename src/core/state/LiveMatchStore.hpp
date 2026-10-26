#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/effects/SideEffects.hpp"
#include "core/state/LiveMatchSnapshot.hpp"
#include "core/state/MatchState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

namespace database::store {
    class HistoryStore;
}
using HistoryState = database::store::HistoryStore;
struct ReducerContext;

class LiveMatchStore {
  public:
    LiveMatchStore();
    explicit LiveMatchStore(MatchState initialState);
    ~LiveMatchStore() = default;

    // Atomically load immutable snapshot
    std::shared_ptr<const LiveMatchSnapshot> GetSnapshot() const;

    // Telemetry thread control
    void SetTelemetryThreadActive(bool active);
    bool IsTelemetryThreadActive() const;

    // Applied exclusively on telemetry thread
    SideEffects ApplyEvent(const TelemetryEvents::TelemetryEvent& event, const ReducerContext& context);
    SideEffects DrainInboxAndApply(const ReducerContext& context);
    SideEffects ConfirmDestroyedMatch(const std::string& matchGuid, bool won, const ReducerContext& context);
    void OnTelemetryDisconnected();

    // Inbox writers (thread-safe, callable from any thread)
    void SetLocalPrimaryId(const std::string& primaryId);
    void UpdateRosterMmr(const std::string& primaryId,
                         const std::map<std::string, int>& playlists,
                         const std::map<std::string, int>& playlistMatches);
    void SetRosterFetchFailed(const std::string& primaryId);
    void UpdatePlayerLifetimeData(const std::string& primaryId,
                                  int winsWith, int lossesWith,
                                  int winsAgainst, int lossesAgainst);
    void SyncSessionMmrChange(const std::map<std::string, int>& changes, float totalChange);
    void UpdateLastSessionRecapMmr(const std::map<std::string, int>& changes, float totalChange);
    bool StartNewSession(int64_t endedAtUnix, HistoryState* history = nullptr);
    bool CloseSessionIfInactive(int64_t nowUnix, HistoryState* history = nullptr);
    void ClearActiveMatchOnDisconnect();

    void PostAction(std::function<void(MatchState&, SideEffects&)> action);
    void UpdateMatchStateForTests(std::function<void(MatchState&)> fn);

  private:
    void PublishSnapshotLocked();
    void DrainInboxLocked(SideEffects& effects);

    std::atomic<std::shared_ptr<const LiveMatchSnapshot>> m_snapshot;
    std::atomic<bool> m_telemetryThreadActive{false};

    mutable std::mutex m_stateMutex;
    MatchState m_state;

    mutable std::mutex m_inboxMutex;
    std::vector<std::function<void(MatchState&, SideEffects&)>> m_inbox;
};
