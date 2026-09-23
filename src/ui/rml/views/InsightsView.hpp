#pragma once

// Data model behind resources/rml/insights.rml: the session recap card, the
// People list, and the Trends breakdown. The controller feeds it raw session
// and database data; this class formats rows and marks bound variables dirty.

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <vector>

#include "core/Insights.hpp"
#include "core/SessionState.hpp"

namespace Rml {
    class Context;
}

struct RecapStatRow {
    Rml::String label;
    Rml::String value;
};

struct RecapModeRow {
    Rml::String name;
    Rml::String record;
    Rml::String mmr;
    int tone = 0;
};

struct PersonRow {
    Rml::String name;
    Rml::String platform;
    Rml::String games;
    Rml::String record;
    Rml::String rate;
    Rml::String last_seen;
    int tone = 0;
};

struct TrendRow {
    Rml::String label;
    Rml::String rate;
    Rml::String games;
    Rml::String width;
    int tone = 0;
};

struct TrendSection {
    Rml::String title;
    std::vector<TrendRow> rows;
};

class InsightsView {
  public:
    enum class Tab { Recap,
                     People,
                     Trends };

    // Must run before insights.rml is loaded.
    bool Create(Rml::Context* context);
    void Reset();

    void SetTab(Tab tab);
    Tab CurrentTab() const {
        return m_tab;
    }
    void SetPeopleFilter(Insights::PeopleFilter filter);

    // `currentSession` is false for a recap captured when a session ended.
    void SetRecap(const SessionRecap& recap, bool currentSession);
    // Full match history for `primaryId`. An empty id means the local account
    // is not known yet.
    void SetHistory(const std::string& primaryId, bool loaded, const std::vector<PersonRecord>& people,
                    const std::vector<MatchOutcome>& outcomes);

    bool RecapHasGames() const {
        return m_recapHasGames;
    }

    static std::vector<TrendSection> BuildTrendSections(const TrendsReport& report);
    static std::vector<RecapModeRow> BuildRecapModes(const SessionRecap& recap);

  private:
    void RebuildPeople();
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;
    Tab m_tab = Tab::Recap;
    Insights::PeopleFilter m_peopleFilter = Insights::PeopleFilter::Teammates;
    std::vector<PersonRecord> m_peopleSource;
    bool m_historyLoaded = false;
    bool m_hasAccount = false;

    Rml::String m_tabName = "recap";
    Rml::String m_subtitle;

    Rml::String m_recapTitle;
    Rml::String m_recapDate;
    Rml::String m_recapWins;
    Rml::String m_recapLosses;
    Rml::String m_recapRate;
    Rml::String m_recapMmr;
    int m_recapMmrTone = 0;
    bool m_recapHasGames = false;
    std::vector<RecapStatRow> m_recapStats;
    std::vector<RecapModeRow> m_recapModes;

    Rml::String m_peopleFilterName = "teammates";
    std::vector<PersonRow> m_people;
    Rml::String m_peopleEmpty;

    Rml::String m_tiltMessage;
    Rml::String m_trendsSummary;
    std::vector<TrendSection> m_trendSections;
    Rml::String m_trendsEmpty;
};
