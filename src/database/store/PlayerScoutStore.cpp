#include "PlayerScoutStore.hpp"

namespace database::store {

    PlayerScoutStore::PlayerScoutStore() {
        m_snapshot.store(std::make_shared<const PlayerScoutSnapshot>());
    }

    std::shared_ptr<const PlayerScoutSnapshot> PlayerScoutStore::GetSnapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    void PlayerScoutStore::PublishScout(uint64_t reqId, const std::string& pid, bool inSuccess, std::optional<PlayerScoutProfile> inProfile) {
        std::lock_guard lock(m_mutex);
        if (reqId < m_requestId) return;
        m_requestId = reqId;
        m_playerId = pid;
        m_loaded = true;
        m_success = inSuccess;
        m_profile = inProfile.value_or(PlayerScoutProfile{});
        m_version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<PlayerScoutSnapshot>();
        snap->requestId = m_requestId;
        snap->playerId = m_playerId;
        snap->loaded = m_loaded;
        snap->success = m_success;
        snap->profile = m_profile;
        snap->version = m_version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

} // namespace database::store
