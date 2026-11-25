#pragma once

#include "core/state/StoreSnapshots.hpp"
#include "core/state/store/SnapshotStore.hpp"
#include <string>
#include <vector>

namespace database::store {

    class InsightsStore : public SnapshotStore<InsightsSnapshot> {
      public:
        InsightsStore() = default;
        ~InsightsStore() = default;

        InsightsStore(const InsightsStore&) = delete;
        InsightsStore& operator=(const InsightsStore&) = delete;

        void PublishInsights(std::string pid,
                             std::vector<PersonRecord> inPeople,
                             std::vector<MatchOutcome> inOutcomes,
                             std::vector<MatchMmrContext> inMmrContext,
                             std::vector<SessionRecap> inSessions,
                             std::vector<AnalyticsMatchRow> inAnalyticsMatches);

        void UpdateSessions(const std::string& pid, std::vector<SessionRecap> inSessions);
    };

} // namespace database::store
