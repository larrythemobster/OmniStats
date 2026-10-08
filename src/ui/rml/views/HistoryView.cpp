#include "ui/rml/views/HistoryView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    std::string SignedDelta(int value) {
        if (value > 0) return "+" + std::to_string(value);
        return std::to_string(value);
    }

    int DeltaTone(int value) {
        return value > 0 ? 1 : value < 0 ? -1
                                         : 0;
    }

    std::string FormatDateTime(int64_t unixSeconds) {
        if (unixSeconds <= 0) return "-";
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return "-";
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%a %d %b %Y, %H:%M", &local);
        return buffer;
    }

    std::string FormatMinutesSeconds(float seconds) {
        const int totalSec = std::max(0, static_cast<int>(std::lround(seconds)));
        const int mins = totalSec / 60;
        const int secs = totalSec % 60;
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
        return buf;
    }

    std::string FormatDurationAndOvertime(const std::optional<float>& durationSeconds,
                                          const std::optional<float>& overtimeSeconds) {
        std::string out;
        if (durationSeconds.has_value() && *durationSeconds > 0.0f) {
            out = FormatMinutesSeconds(*durationSeconds);
        }
        if (overtimeSeconds.has_value() && *overtimeSeconds > 0.0f) {
            const std::string ot = "+" + FormatMinutesSeconds(*overtimeSeconds) + " OT";
            if (out.empty()) {
                out = ot;
            } else {
                out += " (" + ot + ")";
            }
        }
        return out;
    }

    std::string FormatOptInt(const std::optional<int>& val) {
        return val.has_value() ? std::to_string(*val) : "--";
    }

    std::string ShortPlaylistBadge(const MatchRow& row) {
        if (!row.ranked) return "Casual";
        if (row.gamemode == "1v1") return "1v1";
        if (row.gamemode == "2v2") return "2v2";
        if (row.gamemode == "3v3") return "3v3";
        if (!row.playlist.empty()) return row.playlist;
        return row.gamemode.empty() ? "Unknown" : row.gamemode;
    }

    HistoryDetailPlayerRow FormatDetailPlayer(const MatchDetailPlayer& p) {
        HistoryDetailPlayerRow row;
        row.name = p.name.empty() ? p.primaryId : p.name;
        row.platform = p.platform;
        row.is_me = p.isMe;
        if (p.mmr > 0) {
            row.mmr = (p.mmrEstimated ? "~" : "") + std::to_string(p.mmr);
        } else {
            row.mmr = "--";
        }
        row.tier = p.tier.empty() ? "Unranked" : p.tier;
        if (p.isMe) {
            row.met_before = "You";
        } else {
            row.met_before = "Met before: " + std::to_string(p.metBeforeCount) + "\xC3\x97";
        }
        row.score = p.hasStats ? FormatOptInt(p.score) : "--";
        row.goals = p.hasStats ? FormatOptInt(p.goals) : "--";
        row.assists = p.hasStats ? FormatOptInt(p.assists) : "--";
        row.saves = p.hasStats ? FormatOptInt(p.saves) : "--";
        row.shots = p.hasStats ? FormatOptInt(p.shots) : "--";
        row.demos = p.hasStats ? FormatOptInt(p.demos) : "--";
        return row;
    }
}

bool HistoryView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("history");
    if (!constructor) return false;

    if (auto opt = constructor.RegisterStruct<HistoryFilterOption>()) {
        opt.RegisterMember("id", &HistoryFilterOption::id);
        opt.RegisterMember("label", &HistoryFilterOption::label);
    }
    constructor.RegisterArray<std::vector<HistoryFilterOption>>();

    if (auto row = constructor.RegisterStruct<HistoryMatchRow>()) {
        row.RegisterMember("match_id", &HistoryMatchRow::match_id);
        row.RegisterMember("result", &HistoryMatchRow::result);
        row.RegisterMember("win", &HistoryMatchRow::win);
        row.RegisterMember("playlist", &HistoryMatchRow::playlist);
        row.RegisterMember("score", &HistoryMatchRow::score);
        row.RegisterMember("mmr_delta", &HistoryMatchRow::mmr_delta);
        row.RegisterMember("mmr_tone", &HistoryMatchRow::mmr_tone);
        row.RegisterMember("arena", &HistoryMatchRow::arena);
        row.RegisterMember("teammates", &HistoryMatchRow::teammates);
        row.RegisterMember("time", &HistoryMatchRow::time);
    }
    constructor.RegisterArray<std::vector<HistoryMatchRow>>();

    if (auto player = constructor.RegisterStruct<HistoryDetailPlayerRow>()) {
        player.RegisterMember("name", &HistoryDetailPlayerRow::name);
        player.RegisterMember("platform", &HistoryDetailPlayerRow::platform);
        player.RegisterMember("is_me", &HistoryDetailPlayerRow::is_me);
        player.RegisterMember("mmr", &HistoryDetailPlayerRow::mmr);
        player.RegisterMember("tier", &HistoryDetailPlayerRow::tier);
        player.RegisterMember("met_before", &HistoryDetailPlayerRow::met_before);
        player.RegisterMember("score", &HistoryDetailPlayerRow::score);
        player.RegisterMember("goals", &HistoryDetailPlayerRow::goals);
        player.RegisterMember("assists", &HistoryDetailPlayerRow::assists);
        player.RegisterMember("saves", &HistoryDetailPlayerRow::saves);
        player.RegisterMember("shots", &HistoryDetailPlayerRow::shots);
        player.RegisterMember("demos", &HistoryDetailPlayerRow::demos);
    }
    constructor.RegisterArray<std::vector<HistoryDetailPlayerRow>>();

    constructor.Bind("subtitle", &m_subtitle);
    constructor.Bind("playlist_filter", &m_playlistFilter);
    constructor.Bind("playlist_options", &m_playlistOptions);
    constructor.Bind("result_filter", &m_resultFilter);
    constructor.Bind("date_filter", &m_dateFilter);
    constructor.Bind("sort_filter", &m_sortFilter);
    constructor.Bind("with_player", &m_withPlayer);
    constructor.Bind("against_player", &m_againstPlayer);
    constructor.Bind("arena_filter", &m_arenaFilter);
    constructor.Bind("name_search", &m_nameSearch);
    constructor.Bind("rows", &m_rows);
    constructor.Bind("loading", &m_loading);
    constructor.Bind("has_more", &m_hasMore);
    constructor.Bind("empty_message", &m_emptyMessage);
    constructor.Bind("detail_open", &m_detailOpen);
    constructor.Bind("detail_loading", &m_detailLoading);
    constructor.Bind("detail_result", &m_detailResult);
    constructor.Bind("detail_win", &m_detailWin);
    constructor.Bind("detail_score", &m_detailScore);
    constructor.Bind("detail_playlist", &m_detailPlaylist);
    constructor.Bind("detail_arena", &m_detailArena);
    constructor.Bind("detail_time", &m_detailTime);
    constructor.Bind("detail_mmr_change", &m_detailMmrChange);
    constructor.Bind("detail_mmr_tone", &m_detailMmrTone);
    constructor.Bind("detail_duration", &m_detailDuration);
    constructor.Bind("detail_v1_note", &m_detailV1Note);
    constructor.Bind("detail_our_team", &m_detailOurTeam);
    constructor.Bind("detail_their_team", &m_detailTheirTeam);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void HistoryView::Reset() {
    m_handle = {};
    m_bound = false;
    m_account.clear();
    m_activeRequestId = 0;
    m_activeDetailRequestId = 0;
    m_sourceRows.clear();
    m_totalCount = 0;
    m_detailSource.reset();
    m_subtitle = "Your saved match history";
    m_playlistFilter = "All";
    m_playlistOptions = {
        {"All", "All"},
        {"1v1", "1v1"},
        {"2v2", "2v2"},
        {"3v3", "3v3"},
        {"Extra", "Extra"},
        {"Ranked", "Ranked"},
        {"Casual", "Casual"},
    };
    m_resultFilter = "all";
    m_dateFilter = "all";
    m_sortFilter = "newest";
    m_withPlayer.clear();
    m_againstPlayer.clear();
    m_arenaFilter.clear();
    m_nameSearch.clear();
    m_rows.clear();
    m_loading = false;
    m_hasMore = false;
    m_emptyMessage = "Play a match so OmniStats can identify your account.";
    m_detailOpen = false;
    m_detailLoading = false;
    m_detailOurTeam.clear();
    m_detailTheirTeam.clear();
}

void HistoryView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void HistoryView::SetAccount(const std::string& account) {
    m_account = account;
    RebuildRows();
}

void HistoryView::SetPlaylistFilter(const std::string& playlist) {
    m_playlistFilter = playlist.empty() ? "All" : playlist;
    Dirty("playlist_filter");
}

void HistoryView::SetResultFilter(const std::string& result) {
    if (result == "win" || result == "loss") {
        m_resultFilter = result;
    } else {
        m_resultFilter = "all";
    }
    Dirty("result_filter");
}

void HistoryView::SetDateFilter(const std::string& dateRange) {
    if (dateRange == "7d" || dateRange == "30d" || dateRange == "90d") {
        m_dateFilter = dateRange;
    } else {
        m_dateFilter = "all";
    }
    Dirty("date_filter");
}

void HistoryView::SetSortFilter(const std::string& sort) {
    if (sort == "oldest" || sort == "biggest_win" || sort == "biggest_loss" || sort == "margin") {
        m_sortFilter = sort;
    } else {
        m_sortFilter = "newest";
    }
    Dirty("sort_filter");
}

void HistoryView::SetWithPlayer(const std::string& player) {
    m_withPlayer = player;
    Dirty("with_player");
}

void HistoryView::SetAgainstPlayer(const std::string& player) {
    m_againstPlayer = player;
    Dirty("against_player");
}

void HistoryView::SetArenaFilter(const std::string& arena) {
    m_arenaFilter = arena;
    Dirty("arena_filter");
}

void HistoryView::SetNameSearch(const std::string& search) {
    m_nameSearch = search;
    Dirty("name_search");
}

void HistoryView::ClearFilters() {
    m_playlistFilter = "All";
    m_resultFilter = "all";
    m_dateFilter = "all";
    m_sortFilter = "newest";
    m_withPlayer.clear();
    m_againstPlayer.clear();
    m_arenaFilter.clear();
    m_nameSearch.clear();
    for (const char* name : {"playlist_filter", "result_filter", "date_filter", "sort_filter",
                             "with_player", "against_player", "arena_filter", "name_search"}) {
        Dirty(name);
    }
}

MatchQuery HistoryView::BuildQuery(int offset, int limit, int64_t nowUnix) const {
    MatchQuery q;
    q.account = m_account;
    if (!m_playlistFilter.empty() && m_playlistFilter != "All") {
        q.playlistLabel = m_playlistFilter;
        if (m_playlistFilter == "Ranked") q.ranked = true;
        if (m_playlistFilter == "Casual") q.ranked = false;
    }
    if (m_resultFilter == "win") {
        q.win = true;
    } else if (m_resultFilter == "loss") {
        q.win = false;
    }

    const int64_t effectiveNow = nowUnix > 0 ? nowUnix : static_cast<int64_t>(std::time(nullptr));
    if (m_dateFilter == "7d") {
        q.fromUnix = effectiveNow - 7 * 86400;
    } else if (m_dateFilter == "30d") {
        q.fromUnix = effectiveNow - 30 * 86400;
    } else if (m_dateFilter == "90d") {
        q.fromUnix = effectiveNow - 90 * 86400;
    }

    q.withPlayer = m_withPlayer;
    q.againstPlayer = m_againstPlayer;
    q.arena = m_arenaFilter;
    q.nameSearch = m_nameSearch;
    q.offset = std::max(0, offset);
    q.limit = limit > 0 ? limit : kPageSize;

    if (m_sortFilter == "oldest") {
        q.sort = MatchQuery::Sort::Oldest;
    } else if (m_sortFilter == "biggest_win") {
        q.sort = MatchQuery::Sort::BiggestWin;
    } else if (m_sortFilter == "biggest_loss") {
        q.sort = MatchQuery::Sort::BiggestLoss;
    } else if (m_sortFilter == "margin") {
        q.sort = MatchQuery::Sort::ScoreMargin;
    } else {
        q.sort = MatchQuery::Sort::Newest;
    }
    return q;
}

uint64_t HistoryView::BeginQuery(bool /*append*/) {
    ++m_activeRequestId;
    m_loading = true;
    Dirty("loading");
    RebuildRows();
    return m_activeRequestId;
}

bool HistoryView::ApplyQueryResult(uint64_t requestId, const std::vector<MatchRow>& rows, int totalCount, bool append) {
    if (requestId < m_activeRequestId) {
        return false;
    }
    m_activeRequestId = requestId;
    m_loading = false;
    m_totalCount = std::max(0, totalCount);
    if (append && !m_sourceRows.empty() && rows.size() <= static_cast<size_t>(m_totalCount)) {
        for (const auto& r : rows) {
            const bool exists = std::any_of(m_sourceRows.begin(), m_sourceRows.end(), [&](const MatchRow& existing) {
                return existing.matchId == r.matchId;
            });
            if (!exists) {
                m_sourceRows.push_back(r);
            }
        }
    } else {
        m_sourceRows = rows;
    }
    RebuildRows();
    return true;
}

uint64_t HistoryView::BeginDetail(int64_t /*matchId*/) {
    ++m_activeDetailRequestId;
    m_detailOpen = true;
    m_detailLoading = true;
    Dirty("detail_open");
    Dirty("detail_loading");
    return m_activeDetailRequestId;
}

bool HistoryView::ApplyDetailResult(uint64_t requestId, const std::optional<MatchDetail>& detail) {
    if (requestId != 0 && requestId < m_activeDetailRequestId) {
        return false;
    }
    if (requestId != 0) {
        m_activeDetailRequestId = requestId;
    }
    m_detailLoading = false;
    m_detailSource = detail;
    m_detailOpen = detail.has_value() && detail->found;
    RebuildDetail();
    return true;
}

void HistoryView::CloseDetail() {
    m_detailOpen = false;
    m_detailLoading = false;
    Dirty("detail_open");
    Dirty("detail_loading");
}

void HistoryView::RebuildRows() {
    m_rows.clear();
    m_rows.reserve(m_sourceRows.size());
    for (const auto& src : m_sourceRows) {
        HistoryMatchRow row;
        row.match_id = std::to_string(src.matchId);
        row.result = src.win ? "W" : "L";
        row.win = src.win;
        row.playlist = ShortPlaylistBadge(src);
        row.score = std::to_string(src.ourScore) + "-" + std::to_string(src.theirScore);
        if (src.mmrDelta.has_value()) {
            row.mmr_delta = SignedDelta(*src.mmrDelta);
            row.mmr_tone = DeltaTone(*src.mmrDelta);
        } else {
            row.mmr_delta = "--";
            row.mmr_tone = 0;
        }
        row.arena = src.arena.empty() ? "Unknown Arena" : src.arena;
        if (!src.teammates.empty()) {
            std::string joined = "with ";
            for (size_t i = 0; i < src.teammates.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += src.teammates[i];
            }
            row.teammates = std::move(joined);
        } else {
            row.teammates = "Solo";
        }
        row.time = FormatClock(src.timestampUnix);
        m_rows.push_back(std::move(row));
    }

    m_hasMore = static_cast<int>(m_sourceRows.size()) < m_totalCount;
    m_subtitle = m_totalCount > 0 ? Rml::String(std::to_string(m_totalCount) + (m_totalCount == 1 ? " match" : " matches"))
                                  : Rml::String("Your saved match history");

    if (m_account.empty()) {
        m_emptyMessage = "Play a match so OmniStats can identify your account.";
    } else if (m_loading) {
        m_emptyMessage = "Loading match history...";
    } else {
        m_emptyMessage = "No saved matches match the current filters.";
    }

    for (const char* name : {"rows", "loading", "has_more", "subtitle", "empty_message"}) {
        Dirty(name);
    }
}

void HistoryView::RebuildDetail() {
    m_detailOurTeam.clear();
    m_detailTheirTeam.clear();
    if (!m_detailSource.has_value() || !m_detailSource->found) {
        m_detailResult.clear();
        m_detailWin = false;
        m_detailScore.clear();
        m_detailPlaylist.clear();
        m_detailArena.clear();
        m_detailTime.clear();
        m_detailMmrChange.clear();
        m_detailMmrTone = 0;
        m_detailDuration.clear();
        m_detailV1Note.clear();
    } else {
        const MatchDetail& d = *m_detailSource;
        m_detailResult = d.win ? "WIN" : "LOSS";
        m_detailWin = d.win;
        m_detailScore = std::to_string(d.ourScore) + " - " + std::to_string(d.theirScore);
        m_detailPlaylist = (d.ranked ? "Ranked " : "") + (d.playlist.empty() ? std::string("Match") : d.playlist);
        if (!d.ranked && d.playlist == "Casual") m_detailPlaylist = "Casual";
        m_detailArena = d.arena.empty() ? "Unknown Arena" : d.arena;
        m_detailTime = FormatDateTime(d.timestampUnix);

        if (d.mmrBefore.has_value() && d.mmrAfter.has_value()) {
            const int delta = *d.mmrAfter - *d.mmrBefore;
            m_detailMmrChange = std::to_string(*d.mmrBefore) + " -> " + std::to_string(*d.mmrAfter) +
                                " (" + SignedDelta(delta) + ")";
            m_detailMmrTone = DeltaTone(delta);
        } else if (d.mmrAfter.has_value()) {
            m_detailMmrChange = (d.mmrEstimated ? "~" : "") + std::to_string(*d.mmrAfter) + " MMR";
            m_detailMmrTone = 0;
        } else {
            m_detailMmrChange.clear();
            m_detailMmrTone = 0;
        }

        m_detailDuration = FormatDurationAndOvertime(d.durationSeconds, d.overtimeSeconds);
        m_detailV1Note = d.hasPlayerStats ? "" : "Per-player scoreboard stats were not recorded for this match.";

        for (const auto& p : d.ourTeam) {
            m_detailOurTeam.push_back(FormatDetailPlayer(p));
        }
        for (const auto& p : d.theirTeam) {
            m_detailTheirTeam.push_back(FormatDetailPlayer(p));
        }
    }

    for (const char* name : {"detail_open", "detail_loading", "detail_result", "detail_win", "detail_score",
                             "detail_playlist", "detail_arena", "detail_time", "detail_mmr_change",
                             "detail_mmr_tone", "detail_duration", "detail_v1_note",
                             "detail_our_team", "detail_their_team"}) {
        Dirty(name);
    }
}

std::string HistoryView::BuildScoreboardSummary(const MatchDetail& detail) {
    if (!detail.found) return {};
    std::ostringstream out;
    out << (detail.win ? "WIN" : "LOSS") << " " << detail.ourScore << "-" << detail.theirScore
        << " | " << (detail.playlist.empty() ? "Match" : detail.playlist)
        << " | " << (detail.arena.empty() ? "Unknown Arena" : detail.arena)
        << " | " << FormatDateTime(detail.timestampUnix) << "\n";

    if (detail.mmrBefore.has_value() && detail.mmrAfter.has_value()) {
        const int delta = *detail.mmrAfter - *detail.mmrBefore;
        out << "MMR: " << *detail.mmrBefore << " -> " << *detail.mmrAfter << " (" << SignedDelta(delta) << ")\n";
    } else if (detail.mmrAfter.has_value()) {
        out << "MMR: " << *detail.mmrAfter << "\n";
    }

    const std::string dur = FormatDurationAndOvertime(detail.durationSeconds, detail.overtimeSeconds);
    if (!dur.empty()) {
        out << "Duration: " << dur << "\n";
    }

    auto writeTeam = [&](const char* heading, const std::vector<MatchDetailPlayer>& team) {
        out << "\n"
            << heading << " (Name | Platform | Tier | MMR | Score | G | A | S | Sh | D | Met)\n";
        for (const auto& p : team) {
            const HistoryDetailPlayerRow r = FormatDetailPlayer(p);
            out << "- " << r.name;
            if (!r.platform.empty()) out << " (" << r.platform << ")";
            out << " | " << r.tier << " | " << r.mmr
                << " | " << r.score << " | " << r.goals << " | " << r.assists
                << " | " << r.saves << " | " << r.shots << " | " << r.demos
                << " | " << r.met_before << "\n";
        }
    };

    writeTeam("OUR TEAM", detail.ourTeam);
    writeTeam("OPPONENTS", detail.theirTeam);
    return out.str();
}

std::string HistoryView::FormatScoreboardText() const {
    if (!m_detailSource.has_value() || !m_detailSource->found) return {};
    return BuildScoreboardSummary(*m_detailSource);
}
