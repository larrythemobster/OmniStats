#include "HistoryStore.hpp"
#include <algorithm>

namespace database::store {

    HistoryStore::HistoryStore() {
        m_snapshot.store(std::make_shared<const HistorySnapshot>());
    }

    std::shared_ptr<const HistorySnapshot> HistoryStore::GetSnapshot() const {
        const bool lifetimeGraph = showLifetimeGraph.load(std::memory_order_relaxed);
        auto snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed) &&
            snapshot->showLifetimeGraph == lifetimeGraph) {
            return snapshot;
        }

        std::shared_lock lock(mutex);
        snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed) &&
            snapshot->showLifetimeGraph == showLifetimeGraph.load(std::memory_order_relaxed)) {
            return snapshot;
        }

        auto refreshed = std::make_shared<HistorySnapshot>();
        refreshed->mmrHistoryY = mmrHistoryY;
        refreshed->mmrHistoryX = mmrHistoryX;
        refreshed->playlistHistoryY = playlistHistoryY;
        refreshed->playlistMatchPoints = playlistMatchPoints;
        refreshed->playlistInitialMmr = playlistInitialMmr;
        refreshed->mmrOwnerPrimaryId = mmrOwnerPrimaryId;
        refreshed->inactiveMmrHistories = inactiveMmrHistories;
        refreshed->lifetimeMmrY = lifetimeMmrY;
        refreshed->lifetimeMmrX = lifetimeMmrX;
        refreshed->showLifetimeGraph = showLifetimeGraph.load(std::memory_order_relaxed);
        refreshed->recentSavedMatches = recentSavedMatches;
        refreshed->pendingRecentMatches = pendingRecentMatches;
        refreshed->recentSavedMatchesLoaded = recentSavedMatchesLoaded;
        refreshed->initialMmr = initialMmr;
        refreshed->version = version.load(std::memory_order_relaxed);
        m_snapshot.store(refreshed, std::memory_order_release);
        return refreshed;
    }

    void HistoryStore::PublishSnapshot() {
        auto snap = std::make_shared<HistorySnapshot>();
        snap->mmrHistoryY = mmrHistoryY;
        snap->mmrHistoryX = mmrHistoryX;
        snap->playlistHistoryY = playlistHistoryY;
        snap->playlistMatchPoints = playlistMatchPoints;
        snap->playlistInitialMmr = playlistInitialMmr;
        snap->mmrOwnerPrimaryId = mmrOwnerPrimaryId;
        snap->inactiveMmrHistories = inactiveMmrHistories;
        snap->lifetimeMmrY = lifetimeMmrY;
        snap->lifetimeMmrX = lifetimeMmrX;
        snap->showLifetimeGraph = showLifetimeGraph.load(std::memory_order_relaxed);
        snap->recentSavedMatches = recentSavedMatches;
        snap->pendingRecentMatches = pendingRecentMatches;
        snap->recentSavedMatchesLoaded = recentSavedMatchesLoaded;
        snap->initialMmr = initialMmr;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void HistoryStore::SelectMmrOwner(const std::string& primaryId) {
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
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::ResetSessionMmr() {
        mmrHistoryY.clear();
        mmrHistoryX.clear();
        playlistHistoryY.clear();
        playlistMatchPoints.clear();
        playlistInitialMmr.clear();
        initialMmr = -1;
        inactiveMmrHistories.clear();
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::SetLifetimeMmr(std::vector<int64_t> x, std::vector<float> y) {
        std::unique_lock lock(mutex);
        lifetimeMmrX = std::move(x);
        lifetimeMmrY = std::move(y);
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::SetRecentSavedMatches(std::vector<SessionMatchSummary> matches) {
        std::unique_lock lock(mutex);
        pendingRecentMatches.erase(
            std::remove_if(pendingRecentMatches.begin(), pendingRecentMatches.end(),
                           [&](const SessionMatchSummary& pending) {
                               return !pending.matchGuid.empty() &&
                                      std::any_of(matches.begin(), matches.end(),
                                                  [&](const SessionMatchSummary& saved) {
                                                      return saved.matchGuid == pending.matchGuid;
                                                  });
                           }),
            pendingRecentMatches.end());
        recentSavedMatches = std::move(matches);
        recentSavedMatchesLoaded = true;
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::AddPendingRecentMatch(SessionMatchSummary match) {
        std::unique_lock lock(mutex);
        pendingRecentMatches.push_back(std::move(match));
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::ResolvePendingRecentMatch(const std::string& matchGuid) {
        std::unique_lock lock(mutex);
        pendingRecentMatches.erase(
            std::remove_if(pendingRecentMatches.begin(), pendingRecentMatches.end(),
                           [&](const SessionMatchSummary& m) { return m.matchGuid == matchGuid; }),
            pendingRecentMatches.end());
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::SetShowLifetimeGraph(bool show) {
        showLifetimeGraph.store(show, std::memory_order_relaxed);
        std::unique_lock lock(mutex);
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::SetPlaylistInitialMmr(const std::string& playlist, int mmr) {
        std::unique_lock lock(mutex);
        playlistInitialMmr[playlist] = mmr;
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::UpdatePlaylistMmrPoint(const std::string& playlist, size_t index, int mmr, bool estimated) {
        std::unique_lock lock(mutex);
        auto& points = playlistMatchPoints[playlist];
        if (index < points.size()) {
            points[index].mmr = mmr;
            points[index].valueEstimated = estimated;
        }
        auto& proj = playlistHistoryY[playlist];
        if (index < proj.size()) {
            proj[index] = static_cast<float>(mmr);
        }
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

    void HistoryStore::AppendPlaylistMmrPoint(const std::string& playlist, const SessionMmrPoint& point, float mmr) {
        std::unique_lock lock(mutex);
        playlistMatchPoints[playlist].push_back(point);
        playlistHistoryY[playlist].push_back(mmr);
        version.fetch_add(1, std::memory_order_relaxed);
        PublishSnapshot();
    }

} // namespace database::store
