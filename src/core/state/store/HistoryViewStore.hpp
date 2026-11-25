#pragma once

#include "core/state/StoreSnapshots.hpp"
#include "core/state/store/SnapshotStore.hpp"
#include <cstdint>
#include <optional>
#include <vector>

namespace database::store {

    class HistoryViewStore : public SnapshotStore<HistoryViewSnapshot> {
      public:
        HistoryViewStore() = default;
        ~HistoryViewStore() = default;

        HistoryViewStore(const HistoryViewStore&) = delete;
        HistoryViewStore& operator=(const HistoryViewStore&) = delete;

        void RequestQuery(uint64_t reqId);
        // Results older than the latest request are dropped; `append` adds to the loaded rows.
        void PublishQueryResult(uint64_t reqId, std::vector<MatchRow> inRows, int totalCount, bool append);
        void RequestDetail(uint64_t detailReqId);
        void PublishDetailResult(uint64_t detailReqId, std::optional<MatchDetail> detailResult);
    };

} // namespace database::store
