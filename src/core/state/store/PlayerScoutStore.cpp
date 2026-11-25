#include "PlayerScoutStore.hpp"

#include <utility>

namespace stores {

    void PlayerScoutStore::PublishScout(uint64_t reqId, const std::string& pid, bool inSuccess, std::optional<PlayerScoutProfile> inProfile) {
        std::lock_guard lock(m_mutex);
        if (reqId < m_state.requestId) return;
        m_state.requestId = reqId;
        m_state.playerId = pid;
        m_state.loaded = true;
        m_state.success = inSuccess;
        m_state.profile = inProfile ? std::move(*inProfile) : PlayerScoutProfile{};
        PublishCurrentLocked();
    }

} // namespace stores
