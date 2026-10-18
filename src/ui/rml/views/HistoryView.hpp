#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/state/SessionState.hpp"

namespace Rml {
    class Context;
}

struct HistoryFilterOption {
    Rml::String id;
    Rml::String label;
};

struct HistoryMatchBadge {
    Rml::String label;
    Rml::String badge_class;
};

struct HistoryMatchRow {
    Rml::String match_id;
    Rml::String result;
    bool win = false;
    Rml::String playlist;
    Rml::String score;
    Rml::String mmr_delta;
    int mmr_tone = 0;
    Rml::String stat_line;
    std::vector<HistoryMatchBadge> badges;
    Rml::String arena;
    Rml::String teammates;
    Rml::String time;
    bool has_replay = false;
};

struct HistoryDetailPlayerRow {
    Rml::String name;
    Rml::String platform;
    Rml::String platform_class;
    bool is_me = false;
    Rml::String mmr;
    Rml::String tier;
    Rml::String tier_color;
    // Both relationships from other saved matches, e.g. "With 3-1 · Vs 2-0".
    Rml::String encounter;
    Rml::String primary_id;
    bool can_note = false;
    bool has_note = false;
    Rml::String score;
    Rml::String goals;
    Rml::String assists;
    Rml::String saves;
    Rml::String shots;
    Rml::String demos;
    // Extended scoreboard analytics
    Rml::String touches;
    Rml::String avg_hit_speed;
    Rml::String aerial_hits;
    Rml::String first_touches;
    Rml::String demos_inflicted;
    Rml::String demos_taken;
    Rml::String hits_by_third;
};

struct HistoryEventRow {
    Rml::String clock;
    Rml::String type;
    Rml::String player;
    Rml::String detail;
    Rml::String category;
};

struct HistoryTimelineWindow {
    Rml::String our_height;
    Rml::String their_height;
};

struct HistoryTimelineGoal {
    Rml::String x_pct;
    Rml::String score_label;
    Rml::String scorer;
    bool is_our_team = false;
};

struct HistoryTeamCompareRow {
    Rml::String label;
    Rml::String our_val;
    Rml::String their_val;
    int tone = 0; // 1 = ours better, -1 = theirs better, 0 = neutral
};

struct HistoryMechanicsRow {
    Rml::String player_name;
    bool is_me = false;
    bool is_total = false;
    std::vector<Rml::String> values;
};

struct HistoryFieldDot {
    Rml::String x_pct;
    Rml::String y_pct;
    Rml::String dot_class;
};

struct HistoryFieldGoal {
    Rml::String x_pct;
    Rml::String y_pct;
    Rml::String marker_class;
    Rml::String label;
};

class HistoryView {
  public:
    static constexpr int kPageSize = 100;

    bool Create(Rml::Context* context);
    void Reset();

    void SetAccount(const std::string& account);
    const std::string& Account() const {
        return m_account;
    }

    void SetPlaylistFilter(const std::string& playlist);
    const Rml::String& PlaylistFilter() const {
        return m_playlistFilter;
    }

    void SetResultFilter(const std::string& result);
    const Rml::String& ResultFilter() const {
        return m_resultFilter;
    }

    void SetDateFilter(const std::string& dateRange);
    const Rml::String& DateFilter() const {
        return m_dateFilter;
    }

    void SetSortFilter(const std::string& sort);
    const Rml::String& SortFilter() const {
        return m_sortFilter;
    }

    void SetWithPlayer(const std::string& player);
    const Rml::String& WithPlayer() const {
        return m_withPlayer;
    }

    void SetAgainstPlayer(const std::string& player);
    const Rml::String& AgainstPlayer() const {
        return m_againstPlayer;
    }

    void SetArenaFilter(const std::string& arena);
    const Rml::String& ArenaFilter() const {
        return m_arenaFilter;
    }

    void SetNameSearch(const std::string& search);
    const Rml::String& NameSearch() const {
        return m_nameSearch;
    }

    void ClearFilters();
    MatchQuery BuildQuery(int offset = 0, int limit = kPageSize, int64_t nowUnix = 0) const;

    uint64_t BeginQuery(bool append = false);
    uint64_t ActiveRequestId() const {
        return m_activeRequestId;
    }
    bool ApplyQueryResult(uint64_t requestId, const std::vector<MatchRow>& rows, int totalCount, bool append = false);

    uint64_t BeginDetail(int64_t matchId = 0);
    uint64_t ActiveDetailRequestId() const {
        return m_activeDetailRequestId;
    }
    bool ApplyDetailResult(uint64_t requestId, const std::optional<MatchDetail>& detail);
    void CloseDetail();
    void SetDetailTab(const std::string& tab);
    const Rml::String& DetailTab() const {
        return m_detailTab;
    }
    void SetEventFilter(const std::string& filter);
    void SetEventTeamFilter(const std::string& filter);

    bool IsDetailOpen() const {
        return m_detailOpen;
    }
    bool IsLoading() const {
        return m_loading;
    }
    int LoadedRowCount() const {
        return static_cast<int>(m_sourceRows.size());
    }
    int TotalCount() const {
        return m_totalCount;
    }
    const std::vector<HistoryMatchRow>& Rows() const {
        return m_rows;
    }
    const std::optional<MatchDetail>& Detail() const {
        return m_detailSource;
    }

    std::string FormatScoreboardText() const;
    static std::string BuildScoreboardSummary(const MatchDetail& detail);
    // Identity keys (PlayerNotes::Identity::Key) that have a stored note.
    void SetNotedPlayers(const std::unordered_set<std::string>& keys, bool loaded);

  private:
    void RebuildRows();
    void RebuildDetail();
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;

    std::string m_account;
    uint64_t m_activeRequestId = 0;
    uint64_t m_activeDetailRequestId = 0;
    uint64_t m_appliedDetailRequestId = 0;
    std::unordered_set<std::string> m_notedKeys;
    bool m_notesLoaded = false;

    std::vector<MatchRow> m_sourceRows;
    int m_totalCount = 0;
    std::optional<MatchDetail> m_detailSource;

    Rml::String m_subtitle = "Your saved match history";
    Rml::String m_playlistFilter = "All";
    std::vector<HistoryFilterOption> m_playlistOptions;
    Rml::String m_resultFilter = "all";
    Rml::String m_dateFilter = "all";
    Rml::String m_sortFilter = "newest";
    Rml::String m_withPlayer;
    Rml::String m_againstPlayer;
    Rml::String m_arenaFilter;
    Rml::String m_nameSearch;

    std::vector<HistoryMatchRow> m_rows;
    bool m_loading = false;
    bool m_hasMore = false;
    Rml::String m_emptyMessage;

    bool m_detailOpen = false;
    bool m_detailLoading = false;
    Rml::String m_detailTab = "overview";
    Rml::String m_detailResult;
    bool m_detailWin = false;
    Rml::String m_detailScore;
    Rml::String m_detailPlaylist;
    Rml::String m_detailArena;
    Rml::String m_detailTime;
    Rml::String m_detailMmrChange;
    int m_detailMmrTone = 0;
    Rml::String m_detailDuration;
    Rml::String m_detailV1Note;
    Rml::String m_detailBallchasingUrl;
    std::vector<HistoryDetailPlayerRow> m_detailOurTeam;
    std::vector<HistoryDetailPlayerRow> m_detailTheirTeam;

    // Overview tab
    std::vector<Rml::String> m_detailStoryLines;
    bool m_timelineHasData = false;
    std::vector<HistoryTimelineWindow> m_timelineWindows;
    std::vector<HistoryTimelineGoal> m_timelineGoals;
    std::vector<HistoryTeamCompareRow> m_teamCompareRows;

    // Boost & movement tab
    bool m_hasCarTelemetry = false;
    bool m_hasLegacyStats = false;
    Rml::String m_telemetryNote;
    Rml::String m_carBpm;
    Rml::String m_carAvgBoost;
    Rml::String m_carZeroBoost;
    Rml::String m_carFullBoost;
    Rml::String m_carBoostCollected;
    Rml::String m_carBoostUsed;
    Rml::String m_carBigPads;
    Rml::String m_carSmallPads;
    Rml::String m_carStolenPads;
    Rml::String m_carAvgSpeed;
    Rml::String m_carMaxSpeed;
    Rml::String m_carPowerslides;
    Rml::String m_carDemoTime;
    Rml::String m_boostBand0;
    Rml::String m_boostBand1;
    Rml::String m_boostBand2;
    Rml::String m_boostBand3;
    Rml::String m_speedSlowPct;
    Rml::String m_speedBoostPct;
    Rml::String m_speedSupersonicPct;
    Rml::String m_speedSlowLabel;
    Rml::String m_speedBoostLabel;
    Rml::String m_speedSupersonicLabel;
    Rml::String m_surfaceGroundPct;
    Rml::String m_surfaceAirPct;
    Rml::String m_surfaceWallPct;
    Rml::String m_surfaceGroundLabel;
    Rml::String m_surfaceAirLabel;
    Rml::String m_surfaceWallLabel;
    Rml::String m_legacyBoostCollected;
    Rml::String m_legacyDemoed;
    Rml::String m_legacyCrossbars;
    Rml::String m_legacyHardestCrossbar;
    Rml::String m_legacyMaxBallSpeed;
    Rml::String m_legacyOwnGoals;

    // Mechanics tab
    bool m_hasMechanics = false;
    std::vector<Rml::String> m_mechanicsColumns;
    std::vector<HistoryMechanicsRow> m_mechanicsRows;

    // Field tab
    bool m_fieldHasData = false;
    std::vector<HistoryFieldDot> m_fieldDots;
    std::vector<HistoryFieldGoal> m_fieldGoals;
    Rml::String m_fieldTouchesCount;
    Rml::String m_pitchLegendSoft;
    Rml::String m_pitchLegendMed;
    Rml::String m_pitchLegendHard;

    // Events tab
    std::vector<HistoryEventRow> m_detailEvents;
    Rml::String m_eventFilter = "all";
    Rml::String m_eventTeamFilter = "all";
    bool m_eventTeamAvailable = false;
    Rml::String m_eventEmpty = "This match has no recorded events.";
};
