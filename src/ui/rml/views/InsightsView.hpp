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
    Rml::String win_width;
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
    Rml::String subtitle;
    Rml::String tooltip;
    std::vector<TrendRow> rows;
};

struct TrendPlaylistOption {
    Rml::String id;
    Rml::String label;
};
struct SessionPlaylistMmr {
    Rml::String playlist;
    Rml::String mmr;
    Rml::String label;
    int tone = 0;
};

struct SessionRow {
    int index = 0;
    Rml::String session_id;
    Rml::String date;
    Rml::String time_range;
    Rml::String games;
    Rml::String record;
    Rml::String rate;
    Rml::String win_width;
    Rml::String mmr;
    int mmr_tone = 0;
    Rml::String badge;
    Rml::String badge_kind;
    bool is_live = false;
    bool selected = false;
    std::vector<SessionPlaylistMmr> playlist_mmrs;
};

struct RecapCompareRow {
    Rml::String label;
    Rml::String current_val;
    Rml::String previous_val;
    Rml::String delta;
    int tone = 0;
};

class InsightsView {
  public:
    enum class Tab { Recap,
                     Sessions,
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
    void SetTrendPlaylistFilter(const std::string& playlist);
    const std::string& TrendPlaylistFilter() const {
        return m_trendPlaylistFilter;
    }

    // `currentSession` is false for a recap captured when a session ended.
    void SetRecap(const SessionRecap& recap, bool currentSession);
    void SetLiveAndEndedSession(const SessionRecap& liveRecap, const SessionRecap& endedRecap, bool preferEnded);
    // Full match history for `primaryId`. An empty id means the local account
    // is not known yet.
    void SetHistory(const std::string& primaryId, bool loaded, const std::vector<PersonRecord>& people,
                    const std::vector<MatchOutcome>& outcomes, const std::vector<MatchMmrContext>& mmrContext = {},
                    const std::vector<SessionRecap>& sessions = {});
    void SetSessions(const std::vector<SessionRecap>& sessions);
    bool OpenSession(size_t index);
    bool OpenSessionById(int64_t id);
    bool StepSession(int delta);
    void ToggleCompareWithPrevious();
    bool IsComparingWithPrevious() const {
        return m_recapComparing;
    }
    void LoadMoreSessions();
    bool HasMoreSessions() const {
        return m_sessionsHasMore;
    }
    void ClearArchivedSelection();
    bool ViewingArchivedSession() const {
        return m_viewingArchivedSession;
    }

    bool RecapHasGames() const {
        return m_recapHasGames;
    }
    const std::vector<SessionRow>& SessionRows() const {
        return m_sessions;
    }
    const std::vector<RecapCompareRow>& CompareRows() const {
        return m_recapCompareRows;
    }
    const Rml::String& RecapTitle() const {
        return m_recapTitle;
    }
    const Rml::String& RecapDate() const {
        return m_recapDate;
    }
    const Rml::String& RecapMmr() const {
        return m_recapMmr;
    }
    size_t SelectedSessionIndex() const {
        return m_selectedSessionIndex;
    }
    static std::vector<TrendSection> BuildTrendSections(const TrendsReport& report, const GapReport& gap = {});
    static std::vector<RecapModeRow> BuildRecapModes(const SessionRecap& recap);

  private:
    void RebuildPeople();
    void RebuildTrends();
    void RebuildSessions();
    void RebuildComparison();
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;
    Tab m_tab = Tab::Recap;
    Insights::PeopleFilter m_peopleFilter = Insights::PeopleFilter::Teammates;
    std::vector<PersonRecord> m_peopleSource;
    std::vector<MatchOutcome> m_outcomesSource;
    std::vector<MatchMmrContext> m_mmrContextSource;
    std::vector<SessionRecap> m_sessionsSource;
    std::vector<SessionRecap> m_allSessions;
    SessionRecap m_liveRecap;
    SessionRecap m_endedRecap;
    SessionRecap m_displayedRecap;
    bool m_displayedIsLive = true;
    bool m_viewingArchivedSession = false;
    size_t m_selectedSessionIndex = 0;
    size_t m_sessionsVisibleLimit = 25;
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
    Rml::String m_recapGames;
    Rml::String m_recapWinWidth;
    int m_recapMmrTone = 0;
    bool m_recapHasGames = false;
    std::vector<RecapStatRow> m_recapStats;
    std::vector<RecapModeRow> m_recapModes;
    bool m_recapCanStep = false;
    bool m_recapCanPrev = false;
    bool m_recapCanNext = false;
    Rml::String m_recapStepLabel;
    bool m_recapCanCompare = false;
    bool m_recapComparing = false;
    Rml::String m_recapCompareTitle;
    std::vector<RecapCompareRow> m_recapCompareRows;

    std::vector<SessionRow> m_sessions;
    Rml::String m_sessionsEmpty;
    bool m_sessionsHasMore = false;

    Rml::String m_peopleFilterName = "teammates";
    std::vector<PersonRow> m_people;
    Rml::String m_peopleEmpty;

    Rml::String m_tiltMessage;
    Rml::String m_gapCallout;
    Rml::String m_trendPlaylistFilter = "All";
    std::vector<TrendPlaylistOption> m_trendPlaylists;
    Rml::String m_trendsSummary;
    std::vector<TrendSection> m_trendSections;
    Rml::String m_trendsEmpty;
};
