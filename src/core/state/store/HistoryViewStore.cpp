#include "HistoryViewStore.hpp"

#include <iterator>
#include <utility>

namespace stores {

    void HistoryViewStore::RequestQuery(uint64_t reqId) {
        std::lock_guard lock(m_mutex);
        if (reqId >= m_state.requestId) {
            m_state.requestId = reqId;
            m_state.loading = true;
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishQueryResult(uint64_t reqId, std::vector<MatchRow> inRows, int totalCount, bool append) {
        std::lock_guard lock(m_mutex);
        if (reqId < m_state.requestId) return;
        m_state.requestId = reqId;
        m_state.loading = false;
        m_state.total = totalCount;
        if (append) {
            m_state.rows.insert(m_state.rows.end(), std::make_move_iterator(inRows.begin()), std::make_move_iterator(inRows.end()));
        } else {
            m_state.rows = std::move(inRows);
        }
        PublishCurrentLocked();
    }

    void HistoryViewStore::RequestDetail(uint64_t detailReqId) {
        std::lock_guard lock(m_mutex);
        if (detailReqId >= m_state.detailRequestId) {
            m_state.detailRequestId = detailReqId;
            m_state.detailLoading = true;
            PublishCurrentLocked();
        }
    }

    void HistoryViewStore::PublishDetailResult(uint64_t detailReqId, std::optional<MatchDetail> detailResult) {
        std::lock_guard lock(m_mutex);
        if (detailReqId < m_state.detailRequestId) return;
        m_state.detailRequestId = detailReqId;
        m_state.detailLoading = false;
        m_state.detail = std::move(detailResult);
        PublishCurrentLocked();
    }

} // namespace stores
