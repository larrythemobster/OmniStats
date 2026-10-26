#pragma once

#include "core/accounts/PlayerScout.hpp"
#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace database::store {

    class PlayerScoutStore {
      public:
        PlayerScoutStore();
        ~PlayerScoutStore() = default;

        PlayerScoutStore(const PlayerScoutStore&) = delete;
        PlayerScoutStore& operator=(const PlayerScoutStore&) = delete;

        std::shared_ptr<const PlayerScoutSnapshot> GetSnapshot() const;

        void PublishScout(uint64_t reqId, const std::string& pid, bool inSuccess, std::optional<PlayerScoutProfile> inProfile);

        // Fields maintained for backward compatibility and tests
        mutable std::mutex mutex;
        std::atomic<uint64_t> version{0};
        uint64_t requestId = 0;
        std::string playerId;
        bool loaded = false;
        bool success = false;
        PlayerScoutProfile profile;

      private:
        mutable std::atomic<std::shared_ptr<const PlayerScoutSnapshot>> m_snapshot;
    };

} // namespace database::store
