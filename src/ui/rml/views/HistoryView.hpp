#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/SessionState.hpp"

namespace Rml {
    class Context;
}

struct HistoryFilterOption {
    Rml::String id;
    Rml::String label;
};

struct HistoryMatchRow {
    Rml::String match_id;
    Rml::String result;
    bool win = false;
    Rml::String playlist;
    Rml::String score;
    Rml::String mmr_delta;
    int mmr_tone = 0;
    Rml::String arena;
    Rml::String teammates;
    Rml::String time;
};

struct HistoryDetailPlayerRow {
    Rml::String name;
    Rml::String platform;
    Rml::String platform_class;
    bool is_me = false;
    Rml::String mmr;
    Rml::String tier;
    Rml::String tier_color;
    Rml::String met_before;
    Rml::String score;
    Rml::String goals;
    Rml::String assists;
    Rml::String saves;
    Rml::String shots;
    Rml::String demos;
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
    std::vector<HistoryDetailPlayerRow> m_detailOurTeam;
    std::vector<HistoryDetailPlayerRow> m_detailTheirTeam;
};
