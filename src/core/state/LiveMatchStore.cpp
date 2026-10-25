#include "core/state/LiveMatchStore.hpp"

#include <algorithm>
#include <utility>

#include "core/game/Insights.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/SessionState.hpp"

LiveMatchStore::LiveMatchStore()
    : m_state() {
    m_snapshot.store(std::make_shared<LiveMatchSnapshot>(m_state), std::memory_order_relaxed);
}

LiveMatchStore::LiveMatchStore(MatchState initialState)
    : m_state(std::move(initialState)) {
    m_snapshot.store(std::make_shared<LiveMatchSnapshot>(m_state), std::memory_order_relaxed);
}

std::shared_ptr<const LiveMatchSnapshot> LiveMatchStore::GetSnapshot() const {
    return m_snapshot.load(std::memory_order_acquire);
}

void LiveMatchStore::SetTelemetryThreadActive(bool active) {
    m_telemetryThreadActive.store(active, std::memory_order_release);
}

bool LiveMatchStore::IsTelemetryThreadActive() const {
    return m_telemetryThreadActive.load(std::memory_order_acquire);
}

void LiveMatchStore::PublishSnapshotLocked() {
    m_state.version++;
    m_snapshot.store(std::make_shared<LiveMatchSnapshot>(m_state), std::memory_order_release);
}

void LiveMatchStore::DrainInboxLocked(SideEffects& effects) {
    std::vector<std::function<void(MatchState&, SideEffects&)>> pending;
    {
        std::lock_guard lock(m_inboxMutex);
        if (m_inbox.empty()) return;
        pending = std::move(m_inbox);
        m_inbox.clear();
    }
    for (auto& action : pending) {
        if (action) action(m_state, effects);
    }
}

SideEffects LiveMatchStore::DrainInboxAndApply(const ReducerContext& context) {
    (void)context;
    std::lock_guard lock(m_stateMutex);
    SideEffects effects;
    DrainInboxLocked(effects);
    PublishSnapshotLocked();
    return effects;
}

SideEffects LiveMatchStore::ApplyEvent(const TelemetryEvents::TelemetryEvent& event, const ReducerContext& context) {
    std::lock_guard lock(m_stateMutex);
    SideEffects effects;
    DrainInboxLocked(effects);
    effects = TelemetryReducer::ReduceInPlace(m_state, event, context);
    PublishSnapshotLocked();
    return effects;
}

SideEffects LiveMatchStore::ConfirmDestroyedMatch(const std::string& matchGuid, bool won, const ReducerContext& context) {
    std::lock_guard lock(m_stateMutex);
    SideEffects effects = TelemetryReducer::ConfirmPendingDestroyedMatch(m_state, matchGuid, won, context);
    PublishSnapshotLocked();
    return effects;
}

void LiveMatchStore::OnTelemetryDisconnected() {
    std::lock_guard lock(m_stateMutex);
    TelemetryReducer::OnTelemetryDisconnected(m_state);
    PublishSnapshotLocked();
}

void LiveMatchStore::PostAction(std::function<void(MatchState&, SideEffects&)> action) {
    {
        std::lock_guard lock(m_inboxMutex);
        m_inbox.push_back(std::move(action));
    }
    if (!m_telemetryThreadActive.load(std::memory_order_relaxed)) {
        std::lock_guard lock(m_stateMutex);
        SideEffects effects;
        DrainInboxLocked(effects);
        PublishSnapshotLocked();
    }
}

void LiveMatchStore::SetLocalPrimaryId(const std::string& primaryId) {
    PostAction([primaryId](MatchState& state, SideEffects& effects) {
        state.myPrimaryId = primaryId;
        state.myTeam = -1;
        if (!primaryId.empty()) {
            auto it = state.roster.find(primaryId);
            if (it != state.roster.end()) {
                state.myTeam = it->second.team;
            }
        }
        (void)effects;
    });
}

void LiveMatchStore::UpdateRosterMmr(const std::string& primaryId,
                                     const std::map<std::string, int>& playlists,
                                     const std::map<std::string, int>& playlistMatches) {
    PostAction([primaryId, playlists, playlistMatches](MatchState& state, SideEffects& effects) {
        auto it = state.roster.find(primaryId);
        if (it != state.roster.end()) {
            it->second.playlists = playlists;
            it->second.playlistMatches = playlistMatches;
            it->second.fetched = true;
            it->second.fetchFailed = false;
        }
        if (state.inMatch && !state.matchGuid.empty() &&
            state.preMatchMmrByGuid.count(state.matchGuid) == 0 &&
            state.myPrimaryId == primaryId) {
            bool hasPlaylistMmr = false;
            for (const auto& [pl, mmr] : playlists) {
                if (mmr > 0) {
                    hasPlaylistMmr = true;
                    break;
                }
            }
            if (hasPlaylistMmr) {
                state.preMatchMmrByGuid.emplace(
                    state.matchGuid,
                    LocalPreMatchMmrSnapshot{.playlistMmrs = playlists,
                                             .playlistMatches = playlistMatches});
            }
        }
        (void)effects;
    });
}

void LiveMatchStore::SetRosterFetchFailed(const std::string& primaryId) {
    PostAction([primaryId](MatchState& state, SideEffects& effects) {
        auto it = state.roster.find(primaryId);
        if (it != state.roster.end()) {
            it->second.fetched = true;
            it->second.fetchFailed = true;
        }
        (void)effects;
    });
}

void LiveMatchStore::UpdatePlayerLifetimeData(const std::string& primaryId,
                                              int winsWith, int lossesWith,
                                              int winsAgainst, int lossesAgainst) {
    PostAction([primaryId, winsWith, lossesWith, winsAgainst, lossesAgainst](MatchState& state, SideEffects& effects) {
        auto it = state.roster.find(primaryId);
        if (it != state.roster.end()) {
            auto& player = it->second;
            player.lifetimeWinsWith = winsWith;
            player.lifetimeLossesWith = lossesWith;
            player.lifetimeWinsAgainst = winsAgainst;
            player.lifetimeLossesAgainst = lossesAgainst;
            player.hasLifetimeData = true;
        }
        (void)effects;
    });
}

void LiveMatchStore::SyncSessionMmrChange(const std::map<std::string, int>& changes, float totalChange) {
    PostAction([changes, totalChange](MatchState& state, SideEffects& effects) {
        state.sessionTotals.mmrChangeByPlaylist = changes;
        state.sessionTotals.totalMmrChange = totalChange;
        (void)effects;
    });
}

void LiveMatchStore::UpdateLastSessionRecapMmr(const std::map<std::string, int>& changes, float totalChange) {
    PostAction([changes, totalChange](MatchState& state, SideEffects& effects) {
        if (state.lastSessionRecap.valid) {
            state.lastSessionRecap.totals.mmrChangeByPlaylist = changes;
            state.lastSessionRecap.totals.totalMmrChange = totalChange;
        }
        (void)effects;
    });
}

bool LiveMatchStore::StartNewSession(int64_t endedAtUnix, HistoryState* history) {
    std::lock_guard lock(m_stateMutex);
    SideEffects effects;
    DrainInboxLocked(effects);

    const bool captureRecap = m_state.StartNewSession(endedAtUnix, history);
    PublishSnapshotLocked();
    return captureRecap;
}

bool LiveMatchStore::CloseSessionIfInactive(int64_t nowUnix, HistoryState* history) {
    int64_t endedAt = 0;
    {
        std::lock_guard lock(m_stateMutex);
        if (m_state.inMatch) return false;
        if (m_state.sessionTotals.wins + m_state.sessionTotals.losses <= 0) return false;
        if (m_state.lastMatchEndedAtUnix <= 0) return false;
        if (!Insights::StartsNewSession(m_state.lastMatchEndedAtUnix, nowUnix, true)) return false;
        endedAt = m_state.lastMatchEndedAtUnix;
    }
    return StartNewSession(endedAt, history);
}

void LiveMatchStore::ClearActiveMatchOnDisconnect() {
    std::lock_guard lock(m_stateMutex);
    m_state.ClearActiveMatchOnDisconnect();
    PublishSnapshotLocked();
}

void LiveMatchStore::UpdateMatchStateForTests(std::function<void(MatchState&)> fn) {
    std::lock_guard lock(m_stateMutex);
    if (fn) fn(m_state);
    PublishSnapshotLocked();
}
