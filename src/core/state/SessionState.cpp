#include "core/state/LiveMatchStore.hpp"
#include "core/state/SessionState.hpp"
#include "core/config/Config.hpp"
#include <cmath>
#include <algorithm>

void UIState::ApplyRankProgressConfig(const ConfigData& config, bool initial) {
    if (!config.show_extra_playlists && IsExtraMmrCategory(rankProgressMmrCategory.load())) {
        rankProgressMmrCategory.store(MmrCategory::TwoVTwo);
    }
    if (initial && !config.rank_progress_follow_current_playlist) {
        MmrCategory fallback = StringToMmrCategory(config.rank_progress_mmr_category);
        if (!config.show_extra_playlists && IsExtraMmrCategory(fallback)) {
            fallback = MmrCategory::TwoVTwo;
        }
        rankProgressMmrCategory.store(fallback);
    }
}

SessionState::SessionState()
    : liveMatch(std::make_shared<LiveMatchStore>()) {}

SessionState::~SessionState() = default;

void SessionState::clearActiveMatchOnDisconnect() {
    if (liveMatch) {
        liveMatch->ClearActiveMatchOnDisconnect();
    }
    ui.inGoalReplay.store(false, std::memory_order_relaxed);
    ui.firstCountdownOfMatchMs.store(0, std::memory_order_relaxed);
    ui.lastCountdownMs.store(0, std::memory_order_relaxed);
    ui.lastGoalMs.store(0, std::memory_order_relaxed);
    ui.lastMatchStartMs.store(0, std::memory_order_relaxed);
}

bool SessionState::startNewSessionLocked(int64_t endedAtUnix, stores::HistoryData& data) {
    const bool capture = liveMatch ? liveMatch->StartNewSession(endedAtUnix, data) : false;
    ui.graphOffset.store(0);
    return capture;
}

void SessionState::syncSessionMmrChangeLocked(stores::HistoryData& data) {
    if (!liveMatch) return;
    std::map<std::string, int> changes;
    for (const auto& [playlist, projection] : data.playlistHistoryY) {
        const auto initialIt = data.playlistInitialMmr.find(playlist);
        if (projection.empty() || initialIt == data.playlistInitialMmr.end() || initialIt->second <= 0) continue;
        changes[playlist] = static_cast<int>(std::lround(projection.back())) - initialIt->second;
    }
    const float total = static_cast<float>(CalculateTrackedSessionMmrChange(changes));
    liveMatch->SyncSessionMmrChange(changes, total);
}

void SessionState::selectMmrOwnerLocked(const std::string& primaryId, stores::HistoryData& data) {
    if (!liveMatch) {
        data.SelectMmrOwner(primaryId);
        return;
    }
    liveMatch->Update([&](MatchState& match) {
        data.SelectMmrOwner(primaryId);
        match.myPrimaryId = primaryId;
        match.myTeam = -1;
        const auto it = match.roster.find(primaryId);
        if (!primaryId.empty() && it != match.roster.end()) match.myTeam = it->second.team;
        std::map<std::string, int> changes;
        for (const auto& [playlist, projection] : data.playlistHistoryY) {
            const auto initialIt = data.playlistInitialMmr.find(playlist);
            if (projection.empty() || initialIt == data.playlistInitialMmr.end() || initialIt->second <= 0) continue;
            changes[playlist] = static_cast<int>(std::lround(projection.back())) - initialIt->second;
        }
        match.sessionTotals.mmrChangeByPlaylist = std::move(changes);
        match.sessionTotals.totalMmrChange =
            static_cast<float>(CalculateTrackedSessionMmrChange(match.sessionTotals.mmrChangeByPlaylist));
    });
}
