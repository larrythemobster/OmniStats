#pragma once

#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

namespace database::store {

    struct HistoryData : HistorySnapshot {
        void SelectMmrOwner(const std::string& primaryId);
        void ResetSessionMmr();
    };

    class HistoryStore {
      public:
        HistoryStore();
        ~HistoryStore() = default;

        HistoryStore(const HistoryStore&) = delete;
        HistoryStore& operator=(const HistoryStore&) = delete;

        std::shared_ptr<const HistorySnapshot> GetSnapshot() const;

        template <class F>
        auto Mutate(F&& f) -> std::invoke_result_t<F, HistoryData&> {
            using Result = std::invoke_result_t<F, HistoryData&>;
            static_assert(!std::is_reference_v<Result>);
            std::lock_guard lock(m_mutex);
            if constexpr (std::is_void_v<Result>) {
                std::forward<F>(f)(m_data);
                ++m_data.version;
                m_snapshot.store(std::make_shared<const HistorySnapshot>(m_data), std::memory_order_release);
            } else {
                Result result = std::forward<F>(f)(m_data);
                ++m_data.version;
                m_snapshot.store(std::make_shared<const HistorySnapshot>(m_data), std::memory_order_release);
                return result;
            }
        }

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

      private:
        std::mutex m_mutex;
        HistoryData m_data;
        std::atomic<std::shared_ptr<const HistorySnapshot>> m_snapshot;
    };

} // namespace database::store
