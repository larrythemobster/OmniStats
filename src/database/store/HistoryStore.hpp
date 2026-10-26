#pragma once

#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

namespace database::store {

    class HistoryStore {
      public:
        HistoryStore();
        ~HistoryStore() = default;

        HistoryStore(const HistoryStore&) = delete;
        HistoryStore& operator=(const HistoryStore&) = delete;

        std::shared_ptr<const HistorySnapshot> GetSnapshot() const;

        // Single-writer store mutation API:
        void SelectMmrOwner(const std::string& primaryId);
        void ResetSessionMmr();
        void SetLifetimeMmr(std::vector<int64_t> x, std::vector<float> y);
        void SetRecentSavedMatches(std::vector<SessionMatchSummary> matches);
        void AddPendingRecentMatch(SessionMatchSummary match);
        void ResolvePendingRecentMatch(const std::string& matchGuid);
        void SetShowLifetimeGraph(bool show);
        void SetPlaylistInitialMmr(const std::string& playlist, int mmr);
        void UpdatePlaylistMmrPoint(const std::string& playlist, size_t index, int mmr, bool estimated);
        void AppendPlaylistMmrPoint(const std::string& playlist, const SessionMmrPoint& point, float mmr);

        // Publishes current fields into an immutable snapshot
        void PublishSnapshot();

        // Fields maintained for MMR code and backward compatibility
        mutable std::shared_mutex mutex;
        std::atomic<uint64_t> version{1};
        std::vector<float> mmrHistoryY;
        std::vector<float> mmrHistoryX;
        std::map<std::string, std::vector<float>> playlistHistoryY;
        std::map<std::string, std::vector<SessionMmrPoint>> playlistMatchPoints;
        std::map<std::string, int> playlistInitialMmr;
        std::string mmrOwnerPrimaryId;
        std::map<std::string, AccountMmrHistory> inactiveMmrHistories;
        std::vector<float> lifetimeMmrY;
        std::vector<int64_t> lifetimeMmrX;
        std::atomic<bool> showLifetimeGraph{false};
        std::vector<SessionMatchSummary> recentSavedMatches;
        std::vector<SessionMatchSummary> pendingRecentMatches;
        bool recentSavedMatchesLoaded = false;
        int initialMmr = -1;

      private:
        mutable std::atomic<std::shared_ptr<const HistorySnapshot>> m_snapshot;
    };

} // namespace database::store
