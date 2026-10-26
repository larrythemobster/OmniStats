#pragma once

#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace database::store {

    class HistoryViewStore {
      public:
        HistoryViewStore();
        ~HistoryViewStore() = default;

        HistoryViewStore(const HistoryViewStore&) = delete;
        HistoryViewStore& operator=(const HistoryViewStore&) = delete;

        std::shared_ptr<const HistoryViewSnapshot> GetSnapshot() const;

        void RequestQuery(uint64_t reqId, bool append, const MatchQuery& q);
        void PublishQueryResult(uint64_t reqId, const MatchQuery& q, std::vector<MatchRow> inRows, int totalCount, bool append);
        void RequestDetail(uint64_t detailReqId);
        void PublishDetailResult(uint64_t detailReqId, std::optional<MatchDetail> detailResult);

      private:
        void PublishCurrentLocked();

        mutable std::mutex m_mutex;
        std::atomic<uint64_t> m_version{0};
        uint64_t m_requestId = 0;
        uint64_t m_detailRequestId = 0;
        bool m_loading = false;
        bool m_detailLoading = false;
        bool m_appendMode = false;
        MatchQuery m_query;
        std::vector<MatchRow> m_rows;
        int m_total = 0;
        std::optional<MatchDetail> m_detail;

        mutable std::atomic<std::shared_ptr<const HistoryViewSnapshot>> m_snapshot;
    };

} // namespace database::store
