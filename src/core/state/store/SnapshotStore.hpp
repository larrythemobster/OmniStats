#pragma once

#include <atomic>
#include <memory>
#include <mutex>

namespace database::store {

    template <typename Snapshot>
    class SnapshotStore {
      public:
        SnapshotStore() : m_snapshot(std::make_shared<const Snapshot>()) {}

        std::shared_ptr<const Snapshot> GetSnapshot() const {
            return m_snapshot.load(std::memory_order_acquire);
        }

      protected:
        void PublishCurrentLocked() {
            ++m_state.version;
            m_snapshot.store(std::make_shared<const Snapshot>(m_state), std::memory_order_release);
        }

        std::mutex m_mutex;
        Snapshot m_state;

      private:
        std::atomic<std::shared_ptr<const Snapshot>> m_snapshot;
    };

} // namespace database::store
