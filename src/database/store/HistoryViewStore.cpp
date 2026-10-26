#include "HistoryViewStore.hpp"

namespace database::store {

    HistoryViewStore::HistoryViewStore() {
        m_snapshot.store(std::make_shared<const HistoryViewSnapshot>());
    }

    std::shared_ptr<const HistoryViewSnapshot> HistoryViewStore::GetSnapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    void HistoryViewStore::PublishCurrentLocked() {
        auto snap = std::make_shared<HistoryViewSnapshot>();
        snap->requestId = m_requestId;
        snap->detailRequestId = m_detailRequestId;
        snap->loading = m_loading;
        snap->detailLoading = m_detailLoading;
        snap->appendMode = m_appendMode;
        snap->rows = m_rows;
        snap->total = m_total;
        snap->detail = m_detail;
        snap->version = m_version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void HistoryViewStore::RequestQuery(uint64_t reqId, bool append, const MatchQuery& q) {
        std::lock_guard lock(m_mutex);
        if (reqId >= m_requestId) {
            m_requestId = reqId;
            m_loading = true;
            m_appendMode = append;
            m_query = q;
            m_version.fetch_add(1, std::memory_order_relaxed);
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishQueryResult(uint64_t reqId, const MatchQuery& q, std::vector<MatchRow> inRows, int totalCount, bool append) {
        std::lock_guard lock(m_mutex);
        m_requestId = reqId;
        m_query = q;
        m_loading = false;
        m_appendMode = append;
        m_total = totalCount;
        if (append) {
            m_rows.insert(m_rows.end(), std::make_move_iterator(inRows.begin()), std::make_move_iterator(inRows.end()));
        } else {
            m_rows = std::move(inRows);
        }
        m_version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void HistoryViewStore::RequestDetail(uint64_t detailReqId) {
        std::lock_guard lock(m_mutex);
        if (detailReqId >= m_detailRequestId) {
            m_detailRequestId = detailReqId;
            m_detailLoading = true;
            m_version.fetch_add(1, std::memory_order_relaxed);
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishDetailResult(uint64_t detailReqId, std::optional<MatchDetail> detailResult) {
        std::lock_guard lock(m_mutex);
        m_detailRequestId = detailReqId;
        m_detailLoading = false;
        m_detail = std::move(detailResult);
        m_version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

} // namespace database::store
