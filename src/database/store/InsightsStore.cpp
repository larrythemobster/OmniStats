#include "InsightsStore.hpp"

namespace database::store {

    InsightsStore::InsightsStore() {
        m_snapshot.store(std::make_shared<const InsightsSnapshot>());
    }

    std::shared_ptr<const InsightsSnapshot> InsightsStore::GetSnapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    void InsightsStore::PublishInsights(std::string pid,
                                        std::vector<PersonRecord> inPeople,
                                        std::vector<MatchOutcome> inOutcomes,
                                        std::vector<MatchMmrContext> inMmrContext,
                                        std::vector<SessionRecap> inSessions,
                                        std::vector<AnalyticsMatchRow> inAnalyticsMatches) {
        std::lock_guard lock(m_mutex);
        m_primaryId = pid;
        m_people = std::move(inPeople);
        m_outcomes = std::move(inOutcomes);
        m_mmrContext = std::move(inMmrContext);
        m_sessions = std::move(inSessions);
        m_analyticsMatches = std::move(inAnalyticsMatches);
        m_loaded = true;
        m_version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<InsightsSnapshot>();
        snap->loaded = true;
        snap->primaryId = m_primaryId;
        snap->people = m_people;
        snap->outcomes = m_outcomes;
        snap->mmrContext = m_mmrContext;
        snap->sessions = m_sessions;
        snap->analyticsMatches = m_analyticsMatches;
        snap->version = m_version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void InsightsStore::UpdateSessions(const std::string& pid, std::vector<SessionRecap> inSessions) {
        std::lock_guard lock(m_mutex);
        if (m_primaryId != pid) return;
        m_sessions = std::move(inSessions);
        m_version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<InsightsSnapshot>();
        snap->loaded = m_loaded;
        snap->primaryId = m_primaryId;
        snap->people = m_people;
        snap->outcomes = m_outcomes;
        snap->mmrContext = m_mmrContext;
        snap->sessions = m_sessions;
        snap->analyticsMatches = m_analyticsMatches;
        snap->version = m_version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

} // namespace database::store
