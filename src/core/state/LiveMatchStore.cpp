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

void LiveMatchStore::PublishSnapshotLocked() {
    m_state.version++;
    m_snapshot.store(std::make_shared<LiveMatchSnapshot>(m_state), std::memory_order_release);
}

SideEffects LiveMatchStore::ApplyEvent(const TelemetryEvents::TelemetryEvent& event, const ReducerContext& context) {
    std::lock_guard lock(m_stateMutex);
    SideEffects effects = TelemetryReducer::ReduceInPlace(m_state, event, context);
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

void LiveMatchStore::SetLocalPrimaryId(const std::string& primaryId) {
    Update([&](MatchState& state) {
        state.myPrimaryId = primaryId;
        state.myTeam = -1;
        if (!primaryId.empty()) {
            auto it = state.roster.find(primaryId);
            if (it != state.roster.end()) {
                state.myTeam = it->second.team;
            }
        }
    });
}

void LiveMatchStore::UpdateRosterMmr(const std::string& primaryId,
                                     const std::map<std::string, int>& playlists,
                                     const std::map<std::string, int>& playlistMatches) {
    Update([&](MatchState& state) {
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
                if (pl != "best" && mmr > 0) {
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
    });
}

void LiveMatchStore::SetRosterFetchFailed(const std::string& primaryId) {
    Update([&](MatchState& state) {
        auto it = state.roster.find(primaryId);
        if (it != state.roster.end()) {
            it->second.fetched = true;
            it->second.fetchFailed = true;
        }
    });
}

void LiveMatchStore::UpdatePlayerLifetimeData(const std::string& account, const std::string& primaryId,
                                              int winsWith, int lossesWith,
                                              int winsAgainst, int lossesAgainst) {
    Update([&](MatchState& state) {
        if (state.myPrimaryId != account) return;
        auto it = state.roster.find(primaryId);
        if (it != state.roster.end()) {
            auto& player = it->second;
            player.lifetimeWinsWith = winsWith;
            player.lifetimeLossesWith = lossesWith;
            player.lifetimeWinsAgainst = winsAgainst;
            player.lifetimeLossesAgainst = lossesAgainst;
            player.hasLifetimeData = true;
        }
    });
}

void LiveMatchStore::SyncSessionMmrChange(const std::map<std::string, int>& changes, float totalChange) {
    Update([&](MatchState& state) {
        state.sessionTotals.mmrChangeByPlaylist = changes;
        state.sessionTotals.totalMmrChange = totalChange;
    });
}

void LiveMatchStore::UpdateLastSessionRecapMmr(const std::map<std::string, int>& changes, float totalChange) {
    Update([&](MatchState& state) {
        if (state.lastSessionRecap.valid) {
            state.lastSessionRecap.totals.mmrChangeByPlaylist = changes;
            state.lastSessionRecap.totals.totalMmrChange = totalChange;
        }
    });
}

bool LiveMatchStore::StartNewSession(int64_t endedAtUnix, database::store::HistoryData& history) {
    return Update([&](MatchState& state) { return state.StartNewSession(endedAtUnix, &history); });
}

bool LiveMatchStore::CloseSessionIfInactive(int64_t nowUnix, database::store::HistoryData& history) {
    return Update([&](MatchState& state) {
        if (state.inMatch) return false;
        if (state.sessionTotals.wins + state.sessionTotals.losses <= 0) return false;
        if (state.lastMatchEndedAtUnix <= 0) return false;
        if (!Insights::StartsNewSession(state.lastMatchEndedAtUnix, nowUnix, true)) return false;
        return state.StartNewSession(state.lastMatchEndedAtUnix, &history);
    });
}

void LiveMatchStore::ClearActiveMatchOnDisconnect() {
    std::lock_guard lock(m_stateMutex);
    m_state.ClearActiveMatchOnDisconnect();
    PublishSnapshotLocked();
}
