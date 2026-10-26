#pragma once

#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace database::store {

    class InsightsStore {
      public:
        InsightsStore();
        ~InsightsStore() = default;

        InsightsStore(const InsightsStore&) = delete;
        InsightsStore& operator=(const InsightsStore&) = delete;

        std::shared_ptr<const InsightsSnapshot> GetSnapshot() const;

        void PublishInsights(std::string pid,
                             std::vector<PersonRecord> inPeople,
                             std::vector<MatchOutcome> inOutcomes,
                             std::vector<MatchMmrContext> inMmrContext,
                             std::vector<SessionRecap> inSessions,
                             std::vector<AnalyticsMatchRow> inAnalyticsMatches);

        void UpdateSessions(const std::string& pid, std::vector<SessionRecap> inSessions);

        // Fields maintained for backward compatibility and tests
        mutable std::mutex mutex;
        std::atomic<uint64_t> version{0};
        bool loaded = false;
        std::string primaryId;
        std::vector<PersonRecord> people;
        std::vector<MatchOutcome> outcomes;
        std::vector<MatchMmrContext> mmrContext;
        std::vector<SessionRecap> sessions;
        std::vector<AnalyticsMatchRow> analyticsMatches;

      private:
        mutable std::atomic<std::shared_ptr<const InsightsSnapshot>> m_snapshot;
    };

} // namespace database::store
