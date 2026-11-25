#pragma once

#include "core/accounts/PlayerScout.hpp"
#include "core/state/StoreSnapshots.hpp"
#include "core/state/store/SnapshotStore.hpp"
#include <cstdint>
#include <optional>
#include <string>

namespace database::store {

    class PlayerScoutStore : public SnapshotStore<PlayerScoutSnapshot> {
      public:
        PlayerScoutStore() = default;
        ~PlayerScoutStore() = default;

        PlayerScoutStore(const PlayerScoutStore&) = delete;
        PlayerScoutStore& operator=(const PlayerScoutStore&) = delete;

        void PublishScout(uint64_t reqId, const std::string& pid, bool inSuccess, std::optional<PlayerScoutProfile> inProfile);
    };

} // namespace database::store
