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

      private:
        mutable std::mutex m_mutex;
        std::atomic<uint64_t> m_version{0};
        bool m_loaded = false;
        std::string m_primaryId;
        std::vector<PersonRecord> m_people;
        std::vector<MatchOutcome> m_outcomes;
        std::vector<MatchMmrContext> m_mmrContext;
        std::vector<SessionRecap> m_sessions;
        std::vector<AnalyticsMatchRow> m_analyticsMatches;

        mutable std::atomic<std::shared_ptr<const InsightsSnapshot>> m_snapshot;
    };

} // namespace database::store
