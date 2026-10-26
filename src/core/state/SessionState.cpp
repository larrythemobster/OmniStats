#include "core/state/LiveMatchStore.hpp"
#include "core/state/SessionState.hpp"
#include "core/config/Config.hpp"
#include <cmath>
#include <algorithm>

std::string MmrCategoryToString(MmrCategory cat) {
    switch (cat) {
    case MmrCategory::OneVOne:
        return "1v1";
    case MmrCategory::TwoVTwo:
        return "2v2";
    case MmrCategory::ThreeVThree:
        return "3v3";
    case MmrCategory::Casual:
        return "casual";
    case MmrCategory::Tourny:
        return "t";
    case MmrCategory::Hoops:
        return "hoops";
    case MmrCategory::Rumble:
        return "rumble";
    case MmrCategory::Dropshot:
        return "dropshot";
    case MmrCategory::SnowDay:
        return "snowday";
    case MmrCategory::Heatseeker:
        return "heatseeker";
    default:
        return "best";
    }
}

MmrCategory StringToMmrCategory(const std::string& str) {
    if (str == "1v1") return MmrCategory::OneVOne;
    if (str == "2v2") return MmrCategory::TwoVTwo;
    if (str == "3v3") return MmrCategory::ThreeVThree;
    if (str == "casual") return MmrCategory::Casual;
    if (str == "t") return MmrCategory::Tourny;
    if (str == "hoops") return MmrCategory::Hoops;
    if (str == "rumble") return MmrCategory::Rumble;
    if (str == "dropshot") return MmrCategory::Dropshot;
    if (str == "snowday") return MmrCategory::SnowDay;
    if (str == "heatseeker") return MmrCategory::Heatseeker;
    return MmrCategory::Best;
}

bool IsExtraMmrCategory(MmrCategory cat) {
    return cat == MmrCategory::Hoops ||
           cat == MmrCategory::Rumble ||
           cat == MmrCategory::Dropshot ||
           cat == MmrCategory::SnowDay ||
           cat == MmrCategory::Heatseeker;
}

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

bool SessionState::startNewSessionLocked(int64_t endedAtUnix, database::store::HistoryData& data) {
    const bool capture = liveMatch ? liveMatch->StartNewSession(endedAtUnix, &data) : false;
    ui.graphOffset.store(0);
    return capture;
}

bool SessionState::closeSessionIfInactiveLocked(int64_t nowUnix, database::store::HistoryData& data) {
    return liveMatch ? liveMatch->CloseSessionIfInactive(nowUnix, &data) : false;
}

void SessionState::syncSessionMmrChangeLocked(database::store::HistoryData& data) {
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

void SessionState::selectMmrOwnerLocked(const std::string& primaryId, database::store::HistoryData& data) {
    data.SelectMmrOwner(primaryId);
    if (liveMatch) {
        liveMatch->SetLocalPrimaryId(primaryId);
    }
    syncSessionMmrChangeLocked(data);
}
