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

class AccountClient;

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
    struct ScheduledEvent {
        const TournamentSlot* slot = nullptr;
        const SecondChance* secondChance = nullptr;
        explicit operator bool() const {
            return slot != nullptr;
        }
        int64_t Start() const {
            return secondChance ? secondChance->startUnix : slot->startUnix;
        }
        const std::string& Title() const {
            return secondChance ? secondChance->title : slot->title;
        }
    };
    ScheduledEvent NextEvent(const std::vector<TournamentSlot>& slots, int64_t nowUnix, bool standardOnly);
    inline constexpr int kOfficialTournamentPlaylist = 34;
    enum class BracketResult { Unknown,
                               NoMatch,
                               Lost,
                               Advanced };

    class ReminderTracker {
      public:
        void Observe(int64_t nowUnix, bool connected, bool inOfficialMatch, std::string_view matchGuid);
        bool SecondChanceDue(const TournamentSlot& slot, int64_t nowUnix, int leadMinutes) const;
        bool ShouldFire(ScheduledEvent event, int64_t nowUnix, int leadMinutes, BracketResult result = BracketResult::Unknown);
        std::optional<std::string_view> ObservedMatchSince(int64_t startUnix) const;

      private:
        struct Fired {
            int64_t start;
            bool secondChance;
        };
        std::vector<Fired> m_fired;
        int64_t m_observedSince = 0;
        int64_t m_lastObservation = 0;
        int64_t m_lastOfficialMatch = 0;
        std::string m_lastMatchGuid;
    };
    class TournamentScheduleService {
      public:
        explicit TournamentScheduleService(bool enableWorker = true) : m_enableWorker(enableWorker) {}
        ~TournamentScheduleService();
        void Start();
        void Stop();
        void RequestRefresh();
        std::shared_ptr<const ScheduleSnapshot> Snapshot() const;
        void SetAccountClient(std::shared_ptr<AccountClient> accountClient) {
            m_accountClient = std::move(accountClient);
        }

        bool FetchForTests(ScheduleSnapshot& snapshot) {
            return Fetch(snapshot);
        }

      private:
        void Worker();
        bool Fetch(ScheduleSnapshot& snapshot);
        const bool m_enableWorker;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        std::jthread m_thread;
        std::atomic<bool> m_stop{false};
        bool m_refresh = false;
        std::shared_ptr<const ScheduleSnapshot> m_snapshot = std::make_shared<ScheduleSnapshot>();
        std::shared_ptr<AccountClient> m_accountClient;
    };
}
