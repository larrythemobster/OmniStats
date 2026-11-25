#include "HistoryStore.hpp"
#include <algorithm>

namespace database::store {

    HistoryStore::HistoryStore() {
        m_data.version = 1;
        m_snapshot.store(std::make_shared<const HistorySnapshot>(m_data), std::memory_order_release);
    }

    std::shared_ptr<const HistorySnapshot> HistoryStore::GetSnapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    void HistoryData::SelectMmrOwner(const std::string& primaryId) {
        if (mmrOwnerPrimaryId == primaryId) return;

        if (!mmrOwnerPrimaryId.empty()) {
            auto& saved = inactiveMmrHistories[mmrOwnerPrimaryId];
            saved.mmrHistoryY = std::move(mmrHistoryY);
            saved.mmrHistoryX = std::move(mmrHistoryX);
            saved.playlistHistoryY = std::move(playlistHistoryY);
            saved.playlistMatchPoints = std::move(playlistMatchPoints);
            saved.playlistInitialMmr = std::move(playlistInitialMmr);
            saved.initialMmr = initialMmr;
        }

        mmrOwnerPrimaryId = primaryId;
        const auto previous = inactiveMmrHistories.find(primaryId);
        if (previous != inactiveMmrHistories.end()) {
            mmrHistoryY = std::move(previous->second.mmrHistoryY);
            mmrHistoryX = std::move(previous->second.mmrHistoryX);
            playlistHistoryY = std::move(previous->second.playlistHistoryY);
            playlistMatchPoints = std::move(previous->second.playlistMatchPoints);
            playlistInitialMmr = std::move(previous->second.playlistInitialMmr);
            initialMmr = previous->second.initialMmr;
            inactiveMmrHistories.erase(previous);
        } else {
            mmrHistoryY.clear();
            mmrHistoryX.clear();
            playlistHistoryY.clear();
            playlistMatchPoints.clear();
            playlistInitialMmr.clear();
            initialMmr = -1;
        }
    }

    void HistoryData::ResetSessionMmr() {
        mmrHistoryY.clear();
        mmrHistoryX.clear();
        playlistHistoryY.clear();
        playlistMatchPoints.clear();
        playlistInitialMmr.clear();
        initialMmr = -1;
        inactiveMmrHistories.clear();
    }

    void HistoryStore::SelectMmrOwner(const std::string& primaryId) {
        Mutate([&](HistoryData& data) { data.SelectMmrOwner(primaryId); });
    }

    void HistoryStore::ResetSessionMmr() {
        Mutate([](HistoryData& data) { data.ResetSessionMmr(); });
    }

    void HistoryStore::SetLifetimeMmr(std::vector<int64_t> x, std::vector<float> y) {
        Mutate([&](HistoryData& data) {
            data.lifetimeMmrX = std::move(x);
            data.lifetimeMmrY = std::move(y);
        });
    }

    void HistoryStore::SetRecentSavedMatches(std::vector<SessionMatchSummary> matches) {
        Mutate([&](HistoryData& data) {
            auto& pendingMatches = data.pendingRecentMatches;
            pendingMatches.erase(
                std::remove_if(pendingMatches.begin(), pendingMatches.end(),
                               [&](const SessionMatchSummary& pending) {
                                   return !pending.matchGuid.empty() &&
                                          std::any_of(matches.begin(), matches.end(),
                                                      [&](const SessionMatchSummary& saved) {
                                                          return saved.matchGuid == pending.matchGuid;
                                                      });
                               }),
                pendingMatches.end());
            data.recentSavedMatches = std::move(matches);
            data.recentSavedMatchesLoaded = true;
        });
    }

    void HistoryStore::AddPendingRecentMatch(SessionMatchSummary match) {
        Mutate([&](HistoryData& data) { data.pendingRecentMatches.push_back(std::move(match)); });
    }

    void HistoryStore::ResolvePendingRecentMatch(const std::string& matchGuid) {
        Mutate([&](HistoryData& data) {
            auto& pendingMatches = data.pendingRecentMatches;
            pendingMatches.erase(
                std::remove_if(pendingMatches.begin(), pendingMatches.end(),
                               [&](const SessionMatchSummary& m) { return m.matchGuid == matchGuid; }),
                pendingMatches.end());
        });
    }

    void HistoryStore::SetShowLifetimeGraph(bool show) {
        Mutate([&](HistoryData& data) { data.showLifetimeGraph = show; });
    }

    void HistoryStore::SetPlaylistInitialMmr(const std::string& playlist, int mmr) {
        Mutate([&](HistoryData& data) { data.playlistInitialMmr[playlist] = mmr; });
    }

    void HistoryStore::UpdatePlaylistMmrPoint(const std::string& playlist, size_t index, int mmr, bool estimated) {
        Mutate([&](HistoryData& data) {
            auto& points = data.playlistMatchPoints[playlist];
            if (index < points.size()) {
                points[index].mmr = mmr;
                points[index].valueEstimated = estimated;
            }
            auto& proj = data.playlistHistoryY[playlist];
            if (index < proj.size()) {
                proj[index] = static_cast<float>(mmr);
            }
        });
    }

    void HistoryStore::AppendPlaylistMmrPoint(const std::string& playlist, const SessionMmrPoint& point, float mmr) {
        Mutate([&](HistoryData& data) {
            data.playlistMatchPoints[playlist].push_back(point);
            data.playlistHistoryY[playlist].push_back(mmr);
        });
    }

} // namespace database::store
