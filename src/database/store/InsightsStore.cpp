#include "InsightsStore.hpp"

namespace database::store {

    InsightsStore::InsightsStore() {
        m_snapshot.store(std::make_shared<const InsightsSnapshot>());
    }

    std::shared_ptr<const InsightsSnapshot> InsightsStore::GetSnapshot() const {
        auto snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        std::lock_guard lock(mutex);
        snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        auto refreshed = std::make_shared<InsightsSnapshot>();
        refreshed->loaded = loaded;
        refreshed->primaryId = primaryId;
        refreshed->people = people;
        refreshed->outcomes = outcomes;
        refreshed->mmrContext = mmrContext;
        refreshed->sessions = sessions;
        refreshed->analyticsMatches = analyticsMatches;
        refreshed->version = version.load(std::memory_order_relaxed);
        m_snapshot.store(refreshed, std::memory_order_release);
        return refreshed;
    }

    void InsightsStore::PublishInsights(std::string pid,
                                        std::vector<PersonRecord> inPeople,
                                        std::vector<MatchOutcome> inOutcomes,
                                        std::vector<MatchMmrContext> inMmrContext,
                                        std::vector<SessionRecap> inSessions,
                                        std::vector<AnalyticsMatchRow> inAnalyticsMatches) {
        std::lock_guard lock(mutex);
        primaryId = pid;
        people = std::move(inPeople);
        outcomes = std::move(inOutcomes);
        mmrContext = std::move(inMmrContext);
        sessions = std::move(inSessions);
        analyticsMatches = std::move(inAnalyticsMatches);
        loaded = true;
        version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<InsightsSnapshot>();
        snap->loaded = true;
        snap->primaryId = primaryId;
        snap->people = people;
        snap->outcomes = outcomes;
        snap->mmrContext = mmrContext;
        snap->sessions = sessions;
        snap->analyticsMatches = analyticsMatches;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void InsightsStore::UpdateSessions(const std::string& pid, std::vector<SessionRecap> inSessions) {
        std::lock_guard lock(mutex);
        if (primaryId != pid) return;
        sessions = std::move(inSessions);
        version.fetch_add(1, std::memory_order_relaxed);

        auto snap = std::make_shared<InsightsSnapshot>();
        snap->loaded = loaded;
        snap->primaryId = primaryId;
        snap->people = people;
        snap->outcomes = outcomes;
        snap->mmrContext = mmrContext;
        snap->sessions = sessions;
        snap->analyticsMatches = analyticsMatches;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

} // namespace database::store
