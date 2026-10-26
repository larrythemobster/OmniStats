#include "PlayerScoutStore.hpp"

namespace database::store {

    PlayerScoutStore::PlayerScoutStore() {
        m_snapshot.store(std::make_shared<const PlayerScoutSnapshot>());
    }

    std::shared_ptr<const PlayerScoutSnapshot> PlayerScoutStore::GetSnapshot() const {
        auto snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        std::lock_guard lock(mutex);
        snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        auto refreshed = std::make_shared<PlayerScoutSnapshot>();
        refreshed->requestId = requestId;
        refreshed->playerId = playerId;
        refreshed->loaded = loaded;
        refreshed->success = success;
        refreshed->profile = profile;
        refreshed->version = version.load(std::memory_order_relaxed);
        m_snapshot.store(refreshed, std::memory_order_release);
        return refreshed;
    }

    void PlayerScoutStore::PublishScout(uint64_t reqId, const std::string& pid, bool inSuccess, std::optional<PlayerScoutProfile> inProfile) {
        std::lock_guard lock(mutex);
        if (reqId < requestId) return;
        requestId = reqId;
        playerId = pid;
        loaded = true;
        success = inSuccess;
        profile = inProfile.value_or(PlayerScoutProfile{});
        version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<PlayerScoutSnapshot>();
        snap->requestId = requestId;
        snap->playerId = playerId;
        snap->loaded = loaded;
        snap->success = success;
        snap->profile = profile;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

} // namespace database::store
