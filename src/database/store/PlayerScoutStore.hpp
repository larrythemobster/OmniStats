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

      private:
        mutable std::mutex m_mutex;
        std::atomic<uint64_t> m_version{0};
        uint64_t m_requestId = 0;
        std::string m_playerId;
        bool m_loaded = false;
        bool m_success = false;
        PlayerScoutProfile m_profile;

        mutable std::atomic<std::shared_ptr<const PlayerScoutSnapshot>> m_snapshot;
    };

} // namespace database::store
