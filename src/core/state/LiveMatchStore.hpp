#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/effects/SideEffects.hpp"
#include "core/state/LiveMatchSnapshot.hpp"
#include "core/state/MatchState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

struct ReducerContext;

// When both stores are needed, lock HistoryStore before LiveMatchStore.
class LiveMatchStore {
  public:
    LiveMatchStore();
    explicit LiveMatchStore(MatchState initialState);
    ~LiveMatchStore() = default;

    // Atomically load immutable snapshot
    std::shared_ptr<const LiveMatchSnapshot> GetSnapshot() const;

    template <class F>
    auto Update(F&& fn) {
        std::lock_guard lock(m_stateMutex);
        if constexpr (std::is_void_v<std::invoke_result_t<F, MatchState&>>) {
            std::forward<F>(fn)(m_state);
            PublishSnapshotLocked();
        } else {
            auto result = std::forward<F>(fn)(m_state);
            PublishSnapshotLocked();
            return result;
        }
    }

    SideEffects ApplyEvent(const TelemetryEvents::TelemetryEvent& event, const ReducerContext& context);
    SideEffects ConfirmDestroyedMatch(const std::string& matchGuid, bool won, const ReducerContext& context);
    void OnTelemetryDisconnected();

    // Thread-safe writers.
    void SetLocalPrimaryId(const std::string& primaryId);
    void UpdateRosterMmr(const std::string& primaryId,
                         const std::map<std::string, int>& playlists,
                         const std::map<std::string, int>& playlistMatches);
    void SetRosterFetchFailed(const std::string& primaryId);
    void UpdatePlayerLifetimeData(const std::string& account, const std::string& primaryId,
                                  int winsWith, int lossesWith,
                                  int winsAgainst, int lossesAgainst);
    void SyncSessionMmrChange(const std::map<std::string, int>& changes, float totalChange);
    void UpdateLastSessionRecapMmr(const std::map<std::string, int>& changes, float totalChange);
    bool StartNewSession(int64_t endedAtUnix, database::store::HistoryData& history);
    bool CloseSessionIfInactive(int64_t nowUnix, database::store::HistoryData& history);
    void ClearActiveMatchOnDisconnect();

    void UpdateMatchStateForTests(std::function<void(MatchState&)> fn);

  private:
    void PublishSnapshotLocked();

    std::atomic<std::shared_ptr<const LiveMatchSnapshot>> m_snapshot;

    mutable std::mutex m_stateMutex;
    MatchState m_state;
};
