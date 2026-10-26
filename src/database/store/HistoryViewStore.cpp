#include "HistoryViewStore.hpp"

namespace database::store {

    HistoryViewStore::HistoryViewStore() {
        m_snapshot.store(std::make_shared<const HistoryViewSnapshot>());
    }

    std::shared_ptr<const HistoryViewSnapshot> HistoryViewStore::GetSnapshot() const {
        auto snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        std::lock_guard lock(mutex);
        snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        auto refreshed = std::make_shared<HistoryViewSnapshot>();
        refreshed->requestId = requestId;
        refreshed->detailRequestId = detailRequestId;
        refreshed->loading = loading;
        refreshed->detailLoading = detailLoading;
        refreshed->appendMode = appendMode;
        refreshed->rows = rows;
        refreshed->total = total;
        refreshed->detail = detail;
        refreshed->version = version.load(std::memory_order_relaxed);
        m_snapshot.store(refreshed, std::memory_order_release);
        return refreshed;
    }

    void HistoryViewStore::PublishCurrentLocked() {
        auto snap = std::make_shared<HistoryViewSnapshot>();
        snap->requestId = requestId;
        snap->detailRequestId = detailRequestId;
        snap->loading = loading;
        snap->detailLoading = detailLoading;
        snap->appendMode = appendMode;
        snap->rows = rows;
        snap->total = total;
        snap->detail = detail;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void HistoryViewStore::RequestQuery(uint64_t reqId, bool append, const MatchQuery& q) {
        std::lock_guard lock(mutex);
        if (reqId >= requestId) {
            requestId = reqId;
            loading = true;
            appendMode = append;
            query = q;
            version.fetch_add(1, std::memory_order_relaxed);
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishQueryResult(uint64_t reqId, const MatchQuery& q, std::vector<MatchRow> inRows, int totalCount, bool append) {
        std::lock_guard lock(mutex);
        if (reqId < requestId) return;
        requestId = reqId;
        query = q;
        loading = false;
        appendMode = append;
        total = totalCount;
        if (append) {
            rows.insert(rows.end(), std::make_move_iterator(inRows.begin()), std::make_move_iterator(inRows.end()));
        } else {
            rows = std::move(inRows);
        }
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void HistoryViewStore::RequestDetail(uint64_t detailReqId) {
        std::lock_guard lock(mutex);
        if (detailReqId >= detailRequestId) {
            detailRequestId = detailReqId;
            detailLoading = true;
            version.fetch_add(1, std::memory_order_relaxed);
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishDetailResult(uint64_t detailReqId, std::optional<MatchDetail> detailResult) {
        std::lock_guard lock(mutex);
        if (detailReqId < detailRequestId) return;
        detailRequestId = detailReqId;
        detailLoading = false;
        detail = std::move(detailResult);
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

} // namespace database::store
