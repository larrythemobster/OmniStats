#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace Tournaments {
    struct SecondChance {
        std::string title;
        int64_t startUnix = 0;
    };
    struct TournamentSlot {
        int64_t startUnix = 0;
        bool standard = false;
        std::string title;
        int teamSize = 0;
        std::optional<SecondChance> secondChance;
    };
    enum class ScheduleStatus { SignedOut,
                                Disabled,
                                NoRegion,
                                Loading,
                                Ready,
                                Unavailable };
    struct ScheduleSnapshot {
        ScheduleStatus status = ScheduleStatus::Disabled;
        std::string region, regionSource, detectedServer, group;
        int detectedPingMs = 0;
        std::vector<TournamentSlot> slots;
        int64_t fetchedAtUnix = 0;
        uint64_t version = 0;
    };
    bool ParseSchedule(std::string_view body, ScheduleSnapshot& snapshot);
    const TournamentSlot* NextSlot(const std::vector<TournamentSlot>& slots, int64_t nowUnix, bool standardOnly);
    class ReminderTracker {
      public:
        bool ShouldFire(const TournamentSlot& slot, int64_t nowUnix, int leadMinutes, bool standardOnly);

      private:
        std::vector<int64_t> m_fired;
    };
    class TournamentScheduleService {
      public:
        ~TournamentScheduleService();
        void Start();
        void Stop();
        void RequestRefresh();
        std::shared_ptr<const ScheduleSnapshot> Snapshot() const;

#ifdef OMNISTATS_TEST_ENVIRONMENT
        bool FetchForTests(ScheduleSnapshot& snapshot) {
            return Fetch(snapshot);
        }
#endif
      private:
        void Worker();
        bool Fetch(ScheduleSnapshot& snapshot);
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::jthread m_thread;
        std::atomic<bool> m_stop{false};
        bool m_refresh = false;
        std::shared_ptr<const ScheduleSnapshot> m_snapshot = std::make_shared<ScheduleSnapshot>();
    };
}
