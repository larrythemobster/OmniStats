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

        // Fields maintained for backward compatibility and tests
        mutable std::mutex mutex;
        std::atomic<uint64_t> version{0};
        uint64_t requestId = 0;
        uint64_t detailRequestId = 0;
        bool loading = false;
        bool detailLoading = false;
        bool appendMode = false;
        MatchQuery query;
        std::vector<MatchRow> rows;
        int total = 0;
        std::optional<MatchDetail> detail;

      private:
        void PublishCurrentLocked();

        mutable std::atomic<std::shared_ptr<const HistoryViewSnapshot>> m_snapshot;
    };

} // namespace database::store
