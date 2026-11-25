#include "InsightsStore.hpp"

#include <utility>

namespace database::store {

    void InsightsStore::PublishInsights(std::string pid,
                                        std::vector<PersonRecord> inPeople,
                                        std::vector<MatchOutcome> inOutcomes,
                                        std::vector<MatchMmrContext> inMmrContext,
                                        std::vector<SessionRecap> inSessions,
                                        std::vector<AnalyticsMatchRow> inAnalyticsMatches) {
        std::lock_guard lock(m_mutex);
        m_state.primaryId = std::move(pid);
        m_state.people = std::move(inPeople);
        m_state.outcomes = std::move(inOutcomes);
        m_state.mmrContext = std::move(inMmrContext);
        m_state.sessions = std::move(inSessions);
        m_state.analyticsMatches = std::move(inAnalyticsMatches);
        m_state.loaded = true;
        PublishCurrentLocked();
    }

    void InsightsStore::UpdateSessions(const std::string& pid, std::vector<SessionRecap> inSessions) {
        std::lock_guard lock(m_mutex);
        if (m_state.primaryId != pid) return;
        m_state.sessions = std::move(inSessions);
        PublishCurrentLocked();
    }

} // namespace database::store
