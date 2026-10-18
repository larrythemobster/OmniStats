#include "ui/rml/views/InsightsView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    constexpr size_t kPeopleLimit = 40;
    constexpr size_t kSessionsPageSize = 25;
    constexpr size_t kMinSessionsForMmrBadge = 5;
    // Buckets within this many points of the overall rate read as neutral.
    constexpr float kTrendToneMargin = 0.03f;

    std::string Percent(float rate) {
        return std::to_string(static_cast<int>(std::lround(rate * 100.0f))) + "%";
    }

    std::string SignedNumber(int value) {
        return (value > 0 ? "+" : "") + std::to_string(value);
    }

    int Tone(int value) {
        return value > 0 ? 1 : value < 0 ? -1
                                         : 0;
    }

    std::string LocalDate(int64_t unixSeconds) {
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return {};
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%a %d %b %Y, %H:%M", &local);
        return buffer;
    }

    std::string ShortDate(int64_t unixSeconds) {
        if (unixSeconds <= 0) return {};
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return {};
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%a %d %b %Y", &local);
        return buffer;
    }

    std::string LocalTimeHm(int64_t unixSeconds) {
        if (unixSeconds <= 0) return {};
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return {};
        char buffer[32]{};
        std::strftime(buffer, sizeof(buffer), "%H:%M", &local);
        return buffer;
    }

    std::string FormatTimeRange(int64_t startedAtUnix, int64_t endedAtUnix) {
        if (startedAtUnix <= 0 && endedAtUnix <= 0) return "-";
        if (startedAtUnix <= 0 || startedAtUnix >= endedAtUnix) {
            return LocalTimeHm(endedAtUnix > 0 ? endedAtUnix : startedAtUnix);
        }
        return LocalTimeHm(startedAtUnix) + " - " + LocalTimeHm(endedAtUnix);
    }

    std::string PlatformFor(const std::string& primaryId) {
        const size_t delimiter = primaryId.find('|');
        if (delimiter == std::string::npos) return {};
        const std::string platform = primaryId.substr(0, delimiter);
        return PlatformDisplayName(PlatformKindFor(platform), platform);
    }

    TrendRow MakeTrendRow(const TrendBucket& bucket, float overallRate) {
        TrendRow row;
        row.label = bucket.label;
        row.games = std::to_string(bucket.games) + (bucket.games == 1 ? " game" : " games");
        if (bucket.games == 0) {
            row.rate = "-";
            row.width = "0%";
            return row;
        }
        const float rate = bucket.WinRate();
        row.rate = Percent(rate);
        row.width = Percent(rate);
        row.tone = rate > overallRate + kTrendToneMargin ? 1 : rate < overallRate - kTrendToneMargin ? -1
                                                                                                     : 0;
        return row;
    }

    TrendRow MakeGapWinRow(const GapBucket& bucket, float overallRate) {
        TrendRow row;
        row.label = bucket.label;
        row.games = std::to_string(bucket.games) + (bucket.games == 1 ? " game" : " games");
        if (bucket.games < Insights::kGapMinimumBucketGames) {
            row.rate = "Not enough games";
            row.width = "0%";
            return row;
        }
        const float rate = bucket.WinRate();
        row.rate = Percent(rate);
        row.width = Percent(rate);
        row.tone = rate > overallRate + kTrendToneMargin ? 1 : rate < overallRate - kTrendToneMargin ? -1
                                                                                                     : 0;
        return row;
    }

    TrendRow MakeCarryWinRow(const TrendBucket& bucket, float overallRate) {
        TrendRow row;
        row.label = bucket.label;
        row.games = std::to_string(bucket.games) + (bucket.games == 1 ? " game" : " games");
        if (bucket.games < Insights::kGapMinimumBucketGames) {
            row.rate = "Not enough games";
            row.width = "0%";
            return row;
        }
        const float rate = bucket.WinRate();
        row.rate = Percent(rate);
        row.width = Percent(rate);
        row.tone = rate > overallRate + kTrendToneMargin ? 1 : rate < overallRate - kTrendToneMargin ? -1
                                                                                                     : 0;
        return row;
    }

    TrendSection MakeSection(const char* title, const std::vector<TrendBucket>& buckets, float overallRate) {
        TrendSection section;
        section.title = title;
        for (const auto& bucket : buckets)
            section.rows.push_back(MakeTrendRow(bucket, overallRate));
        return section;
    }
}

bool InsightsView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("insights");
    if (!constructor) return false;

    if (auto stat = constructor.RegisterStruct<RecapStatRow>()) {
        stat.RegisterMember("label", &RecapStatRow::label);
        stat.RegisterMember("value", &RecapStatRow::value);
    }
    constructor.RegisterArray<std::vector<RecapStatRow>>();
    if (auto mode = constructor.RegisterStruct<RecapModeRow>()) {
        mode.RegisterMember("name", &RecapModeRow::name);
        mode.RegisterMember("record", &RecapModeRow::record);
        mode.RegisterMember("mmr", &RecapModeRow::mmr);
        mode.RegisterMember("tone", &RecapModeRow::tone);
        mode.RegisterMember("win_width", &RecapModeRow::win_width);
    }
    constructor.RegisterArray<std::vector<RecapModeRow>>();
    if (auto person = constructor.RegisterStruct<PersonRow>()) {
        person.RegisterMember("name", &PersonRow::name);
        person.RegisterMember("platform", &PersonRow::platform);
        person.RegisterMember("games", &PersonRow::games);
        person.RegisterMember("record", &PersonRow::record);
        person.RegisterMember("rate", &PersonRow::rate);
        person.RegisterMember("last_seen", &PersonRow::last_seen);
        person.RegisterMember("tone", &PersonRow::tone);
        person.RegisterMember("other_record", &PersonRow::other_record);
        person.RegisterMember("encounter", &PersonRow::encounter);
        person.RegisterMember("primary_id", &PersonRow::primary_id);
        person.RegisterMember("can_note", &PersonRow::can_note);
        person.RegisterMember("has_note", &PersonRow::has_note);
    }
    constructor.RegisterArray<std::vector<PersonRow>>();
    if (auto row = constructor.RegisterStruct<TrendRow>()) {
        row.RegisterMember("label", &TrendRow::label);
        row.RegisterMember("rate", &TrendRow::rate);
        row.RegisterMember("games", &TrendRow::games);
        row.RegisterMember("width", &TrendRow::width);
        row.RegisterMember("tone", &TrendRow::tone);
    }
    constructor.RegisterArray<std::vector<TrendRow>>();
    if (auto section = constructor.RegisterStruct<TrendSection>()) {
        section.RegisterMember("title", &TrendSection::title);
        section.RegisterMember("subtitle", &TrendSection::subtitle);
        section.RegisterMember("tooltip", &TrendSection::tooltip);
        section.RegisterMember("rows", &TrendSection::rows);
    }
    constructor.RegisterArray<std::vector<TrendSection>>();
    if (auto playlist = constructor.RegisterStruct<TrendPlaylistOption>()) {
        playlist.RegisterMember("id", &TrendPlaylistOption::id);
        playlist.RegisterMember("label", &TrendPlaylistOption::label);
    }
    constructor.RegisterArray<std::vector<TrendPlaylistOption>>();
    if (auto pm = constructor.RegisterStruct<SessionPlaylistMmr>()) {
        pm.RegisterMember("playlist", &SessionPlaylistMmr::playlist);
        pm.RegisterMember("mmr", &SessionPlaylistMmr::mmr);
        pm.RegisterMember("label", &SessionPlaylistMmr::label);
        pm.RegisterMember("tone", &SessionPlaylistMmr::tone);
    }
    constructor.RegisterArray<std::vector<SessionPlaylistMmr>>();
    if (auto srow = constructor.RegisterStruct<SessionRow>()) {
        srow.RegisterMember("index", &SessionRow::index);
        srow.RegisterMember("session_id", &SessionRow::session_id);
        srow.RegisterMember("date", &SessionRow::date);
        srow.RegisterMember("time_range", &SessionRow::time_range);
        srow.RegisterMember("games", &SessionRow::games);
        srow.RegisterMember("record", &SessionRow::record);
        srow.RegisterMember("rate", &SessionRow::rate);
        srow.RegisterMember("win_width", &SessionRow::win_width);
        srow.RegisterMember("mmr", &SessionRow::mmr);
        srow.RegisterMember("mmr_tone", &SessionRow::mmr_tone);
        srow.RegisterMember("badge", &SessionRow::badge);
        srow.RegisterMember("badge_kind", &SessionRow::badge_kind);
        srow.RegisterMember("is_live", &SessionRow::is_live);
        srow.RegisterMember("selected", &SessionRow::selected);
        srow.RegisterMember("playlist_mmrs", &SessionRow::playlist_mmrs);
    }
    constructor.RegisterArray<std::vector<SessionRow>>();
    if (auto crow = constructor.RegisterStruct<RecapCompareRow>()) {
        crow.RegisterMember("label", &RecapCompareRow::label);
        crow.RegisterMember("current_val", &RecapCompareRow::current_val);
        crow.RegisterMember("previous_val", &RecapCompareRow::previous_val);
        crow.RegisterMember("delta", &RecapCompareRow::delta);
        crow.RegisterMember("tone", &RecapCompareRow::tone);
    }
    constructor.RegisterArray<std::vector<RecapCompareRow>>();
    if (auto prow = constructor.RegisterStruct<PlaystyleRecentRow>()) {
        prow.RegisterMember("label", &PlaystyleRecentRow::label);
        prow.RegisterMember("recent_val", &PlaystyleRecentRow::recent_val);
        prow.RegisterMember("previous_val", &PlaystyleRecentRow::previous_val);
        prow.RegisterMember("delta", &PlaystyleRecentRow::delta);
        prow.RegisterMember("tone", &PlaystyleRecentRow::tone);
    }
    constructor.RegisterArray<std::vector<PlaystyleRecentRow>>();
    if (auto srow = constructor.RegisterStruct<PlaystyleSplitRow>()) {
        srow.RegisterMember("sentence", &PlaystyleSplitRow::sentence);
        srow.RegisterMember("effect_label", &PlaystyleSplitRow::effect_label);
    }
    constructor.RegisterArray<std::vector<PlaystyleSplitRow>>();
    if (auto mrow = constructor.RegisterStruct<PlaystyleMetricRow>()) {
        mrow.RegisterMember("label", &PlaystyleMetricRow::label);
        mrow.RegisterMember("value", &PlaystyleMetricRow::value);
        mrow.RegisterMember("category", &PlaystyleMetricRow::category);
    }
    constructor.RegisterArray<std::vector<PlaystyleMetricRow>>();
    constructor.Bind("tab", &m_tabName);
    constructor.Bind("subtitle", &m_subtitle);
    constructor.Bind("recap_title", &m_recapTitle);
    constructor.Bind("recap_date", &m_recapDate);
    constructor.Bind("recap_wins", &m_recapWins);
    constructor.Bind("recap_losses", &m_recapLosses);
    constructor.Bind("recap_rate", &m_recapRate);
    constructor.Bind("recap_mmr", &m_recapMmr);
    constructor.Bind("recap_mmr_tone", &m_recapMmrTone);
    constructor.Bind("recap_games", &m_recapGames);
    constructor.Bind("recap_win_width", &m_recapWinWidth);
    constructor.Bind("recap_has_games", &m_recapHasGames);
    constructor.Bind("recap_stats", &m_recapStats);
    constructor.Bind("recap_has_telemetry", &m_recapHasTelemetry);
    constructor.Bind("recap_bpm", &m_recapBpm);
    constructor.Bind("recap_supersonic", &m_recapSupersonic);
    constructor.Bind("recap_hits", &m_recapHits);
    constructor.Bind("recap_modes", &m_recapModes);
    constructor.Bind("recap_can_step", &m_recapCanStep);
    constructor.Bind("recap_can_prev", &m_recapCanPrev);
    constructor.Bind("recap_can_next", &m_recapCanNext);
    constructor.Bind("recap_step_label", &m_recapStepLabel);
    constructor.Bind("recap_can_compare", &m_recapCanCompare);
    constructor.Bind("recap_comparing", &m_recapComparing);
    constructor.Bind("recap_compare_title", &m_recapCompareTitle);
    constructor.Bind("recap_compare_rows", &m_recapCompareRows);
    constructor.Bind("sessions", &m_sessions);
    constructor.Bind("sessions_empty", &m_sessionsEmpty);
    constructor.Bind("sessions_has_more", &m_sessionsHasMore);
    constructor.Bind("people_filter", &m_peopleFilterName);
    constructor.Bind("people", &m_people);
    constructor.Bind("people_empty", &m_peopleEmpty);
    constructor.Bind("people_other_label", &m_peopleOtherLabel);
    constructor.Bind("tilt_message", &m_tiltMessage);
    constructor.Bind("gap_callout", &m_gapCallout);
    constructor.Bind("trend_playlist_filter", &m_trendPlaylistFilter);
    constructor.Bind("trend_playlists", &m_trendPlaylists);
    constructor.Bind("trends_summary", &m_trendsSummary);
    constructor.Bind("trend_sections", &m_trendSections);
    constructor.Bind("trends_empty", &m_trendsEmpty);
    constructor.Bind("playstyle_has_data", &m_playstyleHasData);
    constructor.Bind("playstyle_sample_summary", &m_playstyleSampleSummary);
    constructor.Bind("playstyle_empty", &m_playstyleEmpty);
    constructor.Bind("playstyle_recent_label", &m_playstyleRecentLabel);
    constructor.Bind("playstyle_recent_form", &m_playstyleRecentForm);
    constructor.Bind("playstyle_metrics", &m_playstyleMetrics);
    constructor.Bind("playstyle_splits_empty", &m_playstyleSplitsEmpty);
    constructor.Bind("playstyle_splits", &m_playstyleSplits);
    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void InsightsView::Reset() {
    m_handle = {};
    m_bound = false;
    m_viewingArchivedSession = false;
    m_selectedSessionIndex = 0;
    m_sessionsVisibleLimit = kSessionsPageSize;
    m_recapComparing = false;
    m_trendPlaylistFilter = "All";
    m_trendPlaylists = {
        {"All", "All"},
        {"1v1", "1v1"},
        {"2v2", "2v2"},
        {"3v3", "3v3"},
    };
}

void InsightsView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void InsightsView::SetTab(Tab tab) {
    m_tab = tab;
    m_tabName = tab == Tab::Recap      ? "recap"
                : tab == Tab::Sessions ? "sessions"
                : tab == Tab::People   ? "people"
                : tab == Tab::Trends   ? "trends"
                                       : "playstyle";
    Dirty("tab");
}

void InsightsView::SetPeopleFilter(Insights::PeopleFilter filter) {
    m_peopleFilter = filter;
    m_peopleFilterName = filter == Insights::PeopleFilter::Teammates ? "teammates" : "rivals";
    Dirty("people_filter");
    RebuildPeople();
}

void InsightsView::SetNotedPlayers(const std::unordered_set<std::string>& keys, bool loaded) {
    if (keys == m_notedKeys && loaded == m_notesLoaded) return;
    m_notedKeys = keys;
    m_notesLoaded = loaded;
    RebuildPeople();
}

void InsightsView::SetTrendPlaylistFilter(const std::string& playlist) {
    const std::string next = (playlist == "1v1" || playlist == "2v2" || playlist == "3v3") ? playlist : "All";
    m_trendPlaylistFilter = next;
    Dirty("trend_playlist_filter");
    RebuildTrends();
    RebuildPlaystyle();
}

std::vector<RecapModeRow> InsightsView::BuildRecapModes(const SessionRecap& recap) {
    std::set<std::string> keys;
    for (const auto& [mode, _] : recap.gamemodes)
        keys.insert(mode);
    for (const auto& [playlist, _] : recap.totals.mmrChangeByPlaylist)
        keys.insert(playlist);

    std::vector<RecapModeRow> rows;
    for (const auto& key : keys) {
        const auto games = recap.gamemodes.find(key);
        const auto change = recap.totals.mmrChangeByPlaylist.find(key);
        const bool played = games != recap.gamemodes.end() && games->second.wins + games->second.losses > 0;
        if (!played && (change == recap.totals.mmrChangeByPlaylist.end() || change->second == 0)) continue;
        RecapModeRow row;
        row.name = MmrLabel(StringToMmrCategory(key));
        if (games != recap.gamemodes.end()) {
            row.record = FormatRecord(games->second.wins, games->second.losses);
            const int gamesPlayed = games->second.wins + games->second.losses;
            if (gamesPlayed > 0) row.win_width = Percent(static_cast<float>(games->second.wins) / static_cast<float>(gamesPlayed));
        } else {
            row.record = "-";
        }
        if (auto it = recap.totals.mmrChangeByPlaylist.find(key); it != recap.totals.mmrChangeByPlaylist.end()) {
            row.mmr = SignedNumber(it->second);
            row.tone = Tone(it->second);
        } else {
            row.mmr = "-";
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void InsightsView::SetRecap(const SessionRecap& recap, bool currentSession) {
    m_displayedRecap = recap;
    m_displayedIsLive = currentSession;
    if (currentSession) {
        m_liveRecap = recap;
    }

    const auto& totals = recap.totals;
    const int games = totals.wins + totals.losses;
    const bool unknownBackfillMmr = !currentSession && recap.source == "backfill" && !recap.HasKnownMmrChange();
    const int mmr = static_cast<int>(std::lround(totals.totalMmrChange));

    if (currentSession) {
        m_recapTitle = "Current session";
        m_recapDate = LocalDate(static_cast<int64_t>(std::time(nullptr)));
    } else if (m_viewingArchivedSession && recap.endedAtUnix > 0) {
        m_recapTitle = "Session recap · " + ShortDate(recap.endedAtUnix);
        if (recap.startedAtUnix > 0 && recap.startedAtUnix < recap.endedAtUnix) {
            m_recapDate = FormatTimeRange(recap.startedAtUnix, recap.endedAtUnix) + " · Ended " + LocalDate(recap.endedAtUnix);
        } else {
            m_recapDate = "Ended " + LocalDate(recap.endedAtUnix);
        }
    } else {
        m_recapTitle = "Session recap";
        m_recapDate = "Ended " + LocalDate(recap.endedAtUnix);
    }

    m_recapWins = std::to_string(totals.wins);
    m_recapLosses = std::to_string(totals.losses);
    m_recapRate = games > 0 ? Percent(static_cast<float>(totals.wins) / static_cast<float>(games)) : "-";
    m_recapWinWidth = games > 0 ? m_recapRate : "0%";
    m_recapGames = std::to_string(games) + (games == 1 ? " game" : " games");
    m_recapMmr = unknownBackfillMmr ? Rml::String("--") : Rml::String(SignedNumber(mmr));
    m_recapMmrTone = unknownBackfillMmr ? 0 : Tone(mmr);
    m_recapHasGames = games > 0;

    const std::string participation = totals.teamGoals > 0
                                          ? Percent(static_cast<float>(totals.goalParticipations) / static_cast<float>(totals.teamGoals))
                                          : "-";
    m_recapStats = {
        {"Goals", std::to_string(totals.goals)},
        {"Assists", std::to_string(totals.assists)},
        {"Saves", std::to_string(totals.saves)},
        {"Shots", std::to_string(totals.shots)},
        {"Demos", std::to_string(totals.demos)},
        {"Goal participation", participation},
    };
    m_recapModes = BuildRecapModes(recap);

    // Check if session matches have analytics + car telemetry
    float totalBpm = 0.0f;
    float totalSupersonicShare = 0.0f;
    int totalHits = 0;
    int telemetryMatchesCount = 0;

    for (const auto& m : m_analyticsMatchesSource) {
        bool inSession = false;
        if (currentSession) {
            inSession = (recap.startedAtUnix > 0 && m.timestampUnix >= recap.startedAtUnix - 60) ||
                        (recap.startedAtUnix == 0 && telemetryMatchesCount < recap.Games());
        } else {
            inSession = (recap.startedAtUnix > 0 && recap.endedAtUnix > 0 &&
                         m.timestampUnix >= recap.startedAtUnix - 60 && m.timestampUnix <= recap.endedAtUnix + 300);
        }
        if (!inSession) continue;
        if (!m.analytics.has_value()) continue;

        for (const auto& p : m.analytics->players) {
            if (p.car.has_value()) {
                totalBpm += p.car->BoostPerMinute();
                totalSupersonicShare += p.car->Share(p.car->supersonicSeconds);
                totalHits += p.hits;
                ++telemetryMatchesCount;
                break;
            }
        }
    }

    if (telemetryMatchesCount > 0) {
        m_recapHasTelemetry = true;
        m_recapBpm = std::to_string(static_cast<int>(std::lround(totalBpm / static_cast<float>(telemetryMatchesCount))));
        m_recapSupersonic = std::to_string(static_cast<int>(std::lround((totalSupersonicShare / static_cast<float>(telemetryMatchesCount)) * 100.0f))) + "%";
        m_recapHits = std::to_string(totalHits);
    } else {
        m_recapHasTelemetry = false;
        m_recapBpm.clear();
        m_recapSupersonic.clear();
        m_recapHits.clear();
    }

    RebuildComparison();
    for (const char* name : {"recap_title", "recap_date", "recap_wins", "recap_losses", "recap_rate", "recap_mmr",
                             "recap_mmr_tone", "recap_has_games", "recap_stats", "recap_modes", "recap_games", "recap_win_width",
                             "recap_has_telemetry", "recap_bpm", "recap_supersonic", "recap_hits"})
        Dirty(name);
}

void InsightsView::SetLiveAndEndedSession(const SessionRecap& liveRecap, const SessionRecap& endedRecap, bool preferEnded) {
    m_liveRecap = liveRecap;
    m_endedRecap = endedRecap;
    RebuildSessions();
    if (!m_viewingArchivedSession) {
        if (preferEnded) {
            SetRecap(endedRecap, false);
        } else {
            SetRecap(liveRecap, true);
        }
    }
}

void InsightsView::ClearArchivedSelection() {
    m_viewingArchivedSession = false;
    m_selectedSessionIndex = 0;
    m_recapComparing = false;
    Dirty("recap_comparing");
}

std::vector<TrendSection> InsightsView::BuildTrendSections(const TrendsReport& report, const GapReport& gap,
                                                           const Playstyle::AnalyticsTrendsReport& analyticsTrends) {
    if (report.games == 0 && gap.games == 0 && analyticsTrends.overtimeGames.games == 0 && analyticsTrends.totalKickoffs == 0) return {};
    std::vector<TrendSection> sections;
    const float overall = report.games > 0 ? report.WinRate() : gap.WinRate();
    if (report.games > 0) {
        sections.push_back(MakeSection("BY PLAYLIST", report.byPlaylist, overall));
        sections.push_back(MakeSection("TIME OF DAY", report.byTimeOfDay, overall));
        sections.push_back(MakeSection("GAMES INTO A SITTING", report.bySessionGame, overall));
        sections.push_back(MakeSection("AFTER THE PREVIOUS GAME", report.afterStreak, overall));
    }

    const float gapOverall = gap.games > 0 ? gap.WinRate() : overall;
    TrendSection lobbySection;
    lobbySection.title = "LOBBY STRENGTH";
    lobbySection.subtitle = "Opponents vs your team, MMR recorded at match time";
    lobbySection.tooltip =
        "Buckets compare opponent and team average MMR recorded at match time (pre- or post-match depending on reconciliation).";
    if (gap.games == 0) {
        lobbySection.rows.push_back({"Not enough ranked data", "-", "0 games", "0%", 0});
    } else {
        for (const auto& bucket : gap.buckets)
            lobbySection.rows.push_back(MakeGapWinRow(bucket, gapOverall));
        if (gap.carryGames > 0)
            lobbySection.rows.push_back(MakeCarryWinRow(gap.carryHigher, gapOverall));
    }
    sections.push_back(std::move(lobbySection));

    // Clutch & game state
    const bool hasClutch = analyticsTrends.overtimeGames.games > 0 ||
                           analyticsTrends.oneGoalGames.games > 0 ||
                           analyticsTrends.scoredFirst.games > 0 ||
                           analyticsTrends.concededFirst.games > 0 ||
                           analyticsTrends.comebacks.games > 0 ||
                           analyticsTrends.blownLeads.games > 0 ||
                           analyticsTrends.lateRegulationWinners > 0;
    if (hasClutch) {
        TrendSection clutch;
        clutch.title = "CLUTCH & GAME STATE";
        clutch.subtitle = "Performance in tight games and critical situations";
        clutch.tooltip = "Record in overtime, one-goal margins, comebacks when trailing, and leads preserved.";
        if (analyticsTrends.overtimeGames.games > 0)
            clutch.rows.push_back(MakeTrendRow(analyticsTrends.overtimeGames, overall));
        if (analyticsTrends.oneGoalGames.games > 0)
            clutch.rows.push_back(MakeTrendRow(analyticsTrends.oneGoalGames, overall));
        if (analyticsTrends.scoredFirst.games > 0)
            clutch.rows.push_back(MakeTrendRow(analyticsTrends.scoredFirst, overall));
        if (analyticsTrends.concededFirst.games > 0)
            clutch.rows.push_back(MakeTrendRow(analyticsTrends.concededFirst, overall));
        if (analyticsTrends.comebacks.games > 0) {
            TrendRow r;
            r.label = "Comebacks (when trailing)";
            r.rate = Percent(analyticsTrends.comebacks.WinRate());
            r.games = std::to_string(analyticsTrends.comebacks.wins) + " of " + std::to_string(analyticsTrends.comebacks.games);
            r.width = r.rate;
            r.tone = analyticsTrends.comebacks.WinRate() >= 0.35f ? 1 : (analyticsTrends.comebacks.WinRate() < 0.20f ? -1 : 0);
            clutch.rows.push_back(std::move(r));
        }
        if (analyticsTrends.blownLeads.games > 0) {
            TrendRow r;
            r.label = "Blown leads (when leading)";
            const float blownRate = analyticsTrends.blownLeads.WinRate();
            r.rate = Percent(blownRate);
            r.games = std::to_string(analyticsTrends.blownLeads.wins) + " of " + std::to_string(analyticsTrends.blownLeads.games);
            r.width = r.rate;
            r.tone = blownRate <= 0.15f ? 1 : (blownRate > 0.30f ? -1 : 0);
            clutch.rows.push_back(std::move(r));
        }
        if (analyticsTrends.lateRegulationWinners > 0) {
            TrendRow r;
            r.label = "Late regulation winners";
            r.rate = std::to_string(analyticsTrends.lateRegulationWinners);
            r.games = "Final 30s deciders";
            r.width = "100%";
            r.tone = 1;
            clutch.rows.push_back(std::move(r));
        }
        sections.push_back(std::move(clutch));
    }

    // Arenas
    if (!analyticsTrends.byArena.empty()) {
        TrendSection arenaSection;
        arenaSection.title = "ARENAS";
        arenaSection.subtitle = "Win rate by arena (minimum 3 games)";
        arenaSection.tooltip = "Performance on each field. OmniStats tracks every stadium automatically.";
        for (const auto& arena : analyticsTrends.byArena) {
            arenaSection.rows.push_back(MakeTrendRow(arena, overall));
        }
        sections.push_back(std::move(arenaSection));
    }

    // Kickoffs
    if (analyticsTrends.totalKickoffs > 0) {
        TrendSection kickoffSection;
        kickoffSection.title = "KICKOFFS";
        kickoffSection.subtitle = "Possession won and immediate goals after kickoffs";
        kickoffSection.tooltip = "First touches and possession control 3 seconds after kickoff, plus goals within 10 seconds.";
        {
            TrendRow r;
            r.label = "Kickoff possession won";
            const float rate = static_cast<float>(analyticsTrends.kickoffPossessionWon) /
                               static_cast<float>(analyticsTrends.totalKickoffs);
            r.rate = Percent(rate);
            r.games = std::to_string(analyticsTrends.kickoffPossessionWon) + " of " +
                      std::to_string(analyticsTrends.totalKickoffs);
            r.width = r.rate;
            r.tone = rate >= 0.50f ? 1 : -1;
            kickoffSection.rows.push_back(std::move(r));
        }
        {
            TrendRow r;
            r.label = "Kickoff goals (for)";
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << analyticsTrends.goalsAfterKickoffForPerGame << " / game";
            r.rate = ss.str();
            r.games = std::to_string(analyticsTrends.totalKickoffGoalsFor) + " total";
            r.width = "50%";
            r.tone = analyticsTrends.goalsAfterKickoffForPerGame >= 0.15f ? 1 : 0;
            kickoffSection.rows.push_back(std::move(r));
        }
        {
            TrendRow r;
            r.label = "Kickoff goals (against)";
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << analyticsTrends.goalsAfterKickoffAgainstPerGame << " / game";
            r.rate = ss.str();
            r.games = std::to_string(analyticsTrends.totalKickoffGoalsAgainst) + " total";
            r.width = "50%";
            r.tone = analyticsTrends.goalsAfterKickoffAgainstPerGame > analyticsTrends.goalsAfterKickoffForPerGame ? -1 : 0;
            kickoffSection.rows.push_back(std::move(r));
        }
        sections.push_back(std::move(kickoffSection));
    }

    // Match length
    const bool hasLength = analyticsTrends.regulationGames.games > 0 ||
                           analyticsTrends.overtimeLengthGames.games > 0 ||
                           analyticsTrends.endedEarlyGames.games > 0;
    if (hasLength) {
        TrendSection lengthSection;
        lengthSection.title = "MATCH LENGTH";
        lengthSection.subtitle = "Win rate by game conclusion";
        lengthSection.tooltip = "Outcomes in full regulation games, overtime, or early finishes (forfeits).";
        if (analyticsTrends.regulationGames.games > 0)
            lengthSection.rows.push_back(MakeTrendRow(analyticsTrends.regulationGames, overall));
        if (analyticsTrends.overtimeLengthGames.games > 0)
            lengthSection.rows.push_back(MakeTrendRow(analyticsTrends.overtimeLengthGames, overall));
        if (analyticsTrends.endedEarlyGames.games > 0)
            lengthSection.rows.push_back(MakeTrendRow(analyticsTrends.endedEarlyGames, overall));
        sections.push_back(std::move(lengthSection));
    }

    return sections;
}

void InsightsView::SetHistory(const std::string& primaryId, bool loaded, const std::vector<PersonRecord>& people,
                              const std::vector<MatchOutcome>& outcomes, const std::vector<MatchMmrContext>& mmrContext,
                              const std::vector<SessionRecap>& sessions,
                              const std::vector<AnalyticsMatchRow>& analyticsMatches) {
    m_hasAccount = !primaryId.empty();
    m_historyLoaded = loaded;
    m_peopleSource = people;
    m_outcomesSource = outcomes;
    m_mmrContextSource = mmrContext;
    m_sessionsSource = sessions;
    m_analyticsMatchesSource = analyticsMatches;
    RebuildPeople();
    RebuildTrends();
    RebuildPlaystyle();
    RebuildSessions();
    RebuildComparison();
}

void InsightsView::SetSessions(const std::vector<SessionRecap>& sessions) {
    m_sessionsSource = sessions;
    m_historyLoaded = true;
    m_hasAccount = true;
    RebuildSessions();
    RebuildComparison();
}

bool InsightsView::OpenSession(size_t index) {
    if (index >= m_allSessions.size()) return false;
    m_selectedSessionIndex = index;
    const SessionRecap& selected = m_allSessions[index];
    const bool isLive = (selected.source == "live_active");
    m_viewingArchivedSession = !isLive;
    SetRecap(selected, isLive);
    RebuildSessions();
    SetTab(Tab::Recap);
    return true;
}

bool InsightsView::OpenSessionById(int64_t id) {
    for (size_t i = 0; i < m_allSessions.size(); ++i) {
        if (m_allSessions[i].id == id && m_allSessions[i].source != "live_active") {
            return OpenSession(i);
        }
    }
    return false;
}

bool InsightsView::StepSession(int delta) {
    if (m_allSessions.empty() || delta == 0) return false;
    const int next = static_cast<int>(m_selectedSessionIndex) + delta;
    if (next < 0 || static_cast<size_t>(next) >= m_allSessions.size()) return false;
    return OpenSession(static_cast<size_t>(next));
}

void InsightsView::ToggleCompareWithPrevious() {
    if (!m_recapCanCompare) return;
    m_recapComparing = !m_recapComparing;
    Dirty("recap_comparing");
}

void InsightsView::LoadMoreSessions() {
    m_sessionsVisibleLimit += kSessionsPageSize;
    RebuildSessions();
}

void InsightsView::RebuildSessions() {
    m_allSessions.clear();
    if (m_liveRecap.valid && m_liveRecap.Games() > 0) {
        SessionRecap liveCopy = m_liveRecap;
        liveCopy.source = "live_active";
        liveCopy.id = 0;
        m_allSessions.push_back(std::move(liveCopy));
    }
    if (m_endedRecap.valid && m_endedRecap.Games() > 0) {
        const bool alreadyInDb = std::any_of(m_sessionsSource.begin(), m_sessionsSource.end(), [&](const SessionRecap& s) {
            if (m_endedRecap.id > 0 && s.id == m_endedRecap.id) return true;
            return s.endedAtUnix == m_endedRecap.endedAtUnix && s.Games() == m_endedRecap.Games();
        });
        if (!alreadyInDb) {
            m_allSessions.push_back(m_endedRecap);
        }
    }
    for (const auto& s : m_sessionsSource) {
        if (s.Games() <= 0) continue;
        m_allSessions.push_back(s);
    }

    if (m_selectedSessionIndex >= m_allSessions.size()) {
        m_selectedSessionIndex = 0;
    }

    std::vector<size_t> mmrRankedIndices;
    for (size_t i = 0; i < m_allSessions.size(); ++i) {
        if (m_allSessions[i].source != "live_active" && m_allSessions[i].HasKnownMmrChange()) {
            mmrRankedIndices.push_back(i);
        }
    }

    std::vector<std::pair<Rml::String, Rml::String>> badges(m_allSessions.size());
    for (size_t i = 0; i < m_allSessions.size(); ++i) {
        if (m_allSessions[i].source == "live_active") {
            badges[i] = {"Live", "live"};
        }
    }
    if (mmrRankedIndices.size() >= kMinSessionsForMmrBadge) {
        std::stable_sort(mmrRankedIndices.begin(), mmrRankedIndices.end(), [&](size_t a, size_t b) {
            const int mmrA = m_allSessions[a].NetMmrChange();
            const int mmrB = m_allSessions[b].NetMmrChange();
            if (mmrA != mmrB) return mmrA > mmrB;
            return m_allSessions[a].Games() > m_allSessions[b].Games();
        });
        const size_t bandCount = std::max<size_t>(1, mmrRankedIndices.size() / 10);
        for (size_t k = 0; k < bandCount && k < mmrRankedIndices.size(); ++k) {
            const size_t topIdx = mmrRankedIndices[k];
            if (m_allSessions[topIdx].NetMmrChange() > 0) {
                badges[topIdx] = {"Best", "best"};
            }
        }
        for (size_t k = 0; k < bandCount && k < mmrRankedIndices.size(); ++k) {
            const size_t botIdx = mmrRankedIndices[mmrRankedIndices.size() - 1 - k];
            if (m_allSessions[botIdx].NetMmrChange() < 0 && badges[botIdx].first.empty()) {
                badges[botIdx] = {"Worst", "worst"};
            }
        }
    }

    m_sessions.clear();
    const size_t visibleCount = std::min(m_allSessions.size(), m_sessionsVisibleLimit);
    m_sessions.reserve(visibleCount);
    for (size_t i = 0; i < visibleCount; ++i) {
        const SessionRecap& s = m_allSessions[i];
        const int games = s.Games();
        SessionRow row;
        row.index = static_cast<int>(i);
        row.is_live = (s.source == "live_active");
        row.selected = (i == m_selectedSessionIndex);
        row.session_id = row.is_live ? Rml::String("live") : Rml::String(std::to_string(s.id));
        if (row.is_live) {
            row.date = "Live";
            row.time_range = s.startedAtUnix > 0 ? ("Started " + LocalTimeHm(s.startedAtUnix)) : "In progress";
        } else {
            const int64_t refTs = s.endedAtUnix > 0 ? s.endedAtUnix : s.startedAtUnix;
            row.date = refTs > 0 ? ShortDate(refTs) : "Saved session";
            row.time_range = FormatTimeRange(s.startedAtUnix, s.endedAtUnix);
        }
        row.games = std::to_string(games) + (games == 1 ? " game" : " games");
        row.record = FormatRecord(s.totals.wins, s.totals.losses);
        const float rate = games > 0 ? static_cast<float>(s.totals.wins) / static_cast<float>(games) : 0.0f;
        row.rate = games > 0 ? Percent(rate) : "-";
        row.win_width = games > 0 ? row.rate : "0%";
        row.badge = badges[i].first;
        row.badge_kind = badges[i].second;

        if (s.HasKnownMmrChange()) {
            const int net = s.NetMmrChange();
            row.mmr = SignedNumber(net);
            row.mmr_tone = Tone(net);
            for (const auto& [playlist, delta] : s.totals.mmrChangeByPlaylist) {
                SessionPlaylistMmr pm;
                pm.playlist = MmrLabel(StringToMmrCategory(playlist));
                pm.mmr = SignedNumber(delta);
                pm.label = pm.playlist + " " + pm.mmr;
                pm.tone = Tone(delta);
                row.playlist_mmrs.push_back(std::move(pm));
            }
        } else {
            row.mmr = "--";
            row.mmr_tone = 0;
        }
        m_sessions.push_back(std::move(row));
    }

    m_sessionsHasMore = m_allSessions.size() > m_sessions.size();
    m_sessionsEmpty = !m_hasAccount      ? "Play a match so OmniStats can identify your account."
                      : !m_historyLoaded ? "Loading your session history..."
                                         : "No saved sessions yet. Completed sessions appear here.";
    Dirty("sessions");
    Dirty("sessions_empty");
    Dirty("sessions_has_more");
}

void InsightsView::RebuildComparison() {
    m_recapCanStep = m_allSessions.size() > 1;
    m_recapCanPrev = m_recapCanStep && m_selectedSessionIndex > 0;
    m_recapCanNext = m_recapCanStep && (m_selectedSessionIndex + 1 < m_allSessions.size());
    m_recapStepLabel = m_recapCanStep
                           ? (std::to_string(m_selectedSessionIndex + 1) + " / " + std::to_string(m_allSessions.size()))
                           : "";

    m_recapCompareRows.clear();
    m_recapCompareTitle.clear();
    const size_t prevIndex = m_selectedSessionIndex + 1;
    m_recapCanCompare = prevIndex < m_allSessions.size() && m_displayedRecap.Games() > 0;
    if (!m_recapCanCompare) {
        m_recapComparing = false;
    } else {
        const SessionRecap& cur = m_displayedRecap;
        const SessionRecap& prev = m_allSessions[prevIndex];
        const int64_t prevTs = prev.endedAtUnix > 0 ? prev.endedAtUnix : prev.startedAtUnix;
        m_recapCompareTitle = prevTs > 0 ? ("Compared with " + ShortDate(prevTs)) : "Compared with previous session";

        const int curGames = cur.Games();
        const int prevGames = prev.Games();
        const int curRatePct = curGames > 0 ? static_cast<int>(std::lround(100.0f * cur.totals.wins / curGames)) : 0;
        const int prevRatePct = prevGames > 0 ? static_cast<int>(std::lround(100.0f * prev.totals.wins / prevGames)) : 0;
        const int rateDelta = curRatePct - prevRatePct;

        m_recapCompareRows.push_back({
            "Games",
            std::to_string(curGames),
            std::to_string(prevGames),
            SignedNumber(curGames - prevGames),
            Tone(curGames - prevGames),
        });
        m_recapCompareRows.push_back({
            "Wins",
            std::to_string(cur.totals.wins),
            std::to_string(prev.totals.wins),
            SignedNumber(cur.totals.wins - prev.totals.wins),
            Tone(cur.totals.wins - prev.totals.wins),
        });
        m_recapCompareRows.push_back({
            "Win rate",
            std::to_string(curRatePct) + "%",
            std::to_string(prevRatePct) + "%",
            SignedNumber(rateDelta) + "%",
            Tone(rateDelta),
        });
        if (cur.HasKnownMmrChange() && prev.HasKnownMmrChange()) {
            const int mmrDelta = cur.NetMmrChange() - prev.NetMmrChange();
            m_recapCompareRows.push_back({
                "Net MMR",
                SignedNumber(cur.NetMmrChange()),
                SignedNumber(prev.NetMmrChange()),
                SignedNumber(mmrDelta),
                Tone(mmrDelta),
            });
        } else {
            m_recapCompareRows.push_back({
                "Net MMR",
                cur.HasKnownMmrChange() ? SignedNumber(cur.NetMmrChange()) : "--",
                prev.HasKnownMmrChange() ? SignedNumber(prev.NetMmrChange()) : "--",
                "--",
                0,
            });
        }
        m_recapCompareRows.push_back({
            "Goals",
            std::to_string(cur.totals.goals),
            std::to_string(prev.totals.goals),
            SignedNumber(cur.totals.goals - prev.totals.goals),
            Tone(cur.totals.goals - prev.totals.goals),
        });
        m_recapCompareRows.push_back({
            "Saves",
            std::to_string(cur.totals.saves),
            std::to_string(prev.totals.saves),
            SignedNumber(cur.totals.saves - prev.totals.saves),
            Tone(cur.totals.saves - prev.totals.saves),
        });
    }

    for (const char* name : {"recap_can_step", "recap_can_prev", "recap_can_next", "recap_step_label",
                             "recap_can_compare", "recap_comparing", "recap_compare_title", "recap_compare_rows"})
        Dirty(name);
}

void InsightsView::RebuildTrends() {
    const TrendsReport report = Insights::ComputeTrends(m_outcomesSource);
    const GapReport gap = Insights::ComputeGapTrends(m_mmrContextSource, m_trendPlaylistFilter);
    const Playstyle::AnalyticsTrendsReport analyticsTrends =
        Playstyle::ComputeAnalyticsTrends(m_analyticsMatchesSource, m_trendPlaylistFilter);
    m_trendSections = BuildTrendSections(report, gap, analyticsTrends);
    m_tiltMessage = report.tiltWarning;
    m_gapCallout = gap.callout;
    if (report.games > 0) {
        m_trendsSummary = std::to_string(report.games) + " games over " + std::to_string(report.sessions) +
                          (report.sessions == 1 ? " sitting" : " sittings") + ", " + Percent(report.WinRate()) +
                          " won overall. Green bars beat your average, red bars trail it.";
        m_subtitle = m_trendsSummary.substr(0, m_trendsSummary.find(','));
    } else {
        m_trendsSummary.clear();
        m_subtitle = "Sessions, people, and trends";
    }
    m_trendsEmpty = !m_hasAccount      ? "Play a match so OmniStats can identify your account."
                    : !m_historyLoaded ? "Loading your match history..."
                                       : "No saved matches yet. Trends show up once you have played a few games.";
    for (const char* name : {"trend_sections", "tilt_message", "gap_callout", "trends_summary", "trends_empty", "subtitle"})
        Dirty(name);
}

void InsightsView::RebuildPlaystyle() {
    const Playstyle::PlaystyleReport report =
        Playstyle::ComputePlaystyle(m_analyticsMatchesSource, m_trendPlaylistFilter);

    m_playstyleHasData = report.totalMatchesWithTelemetry > 0;
    m_playstyleSampleSummary = report.sampleSummary;
    m_playstyleEmpty = !m_hasAccount      ? "Play a match so OmniStats can identify your account."
                       : !m_historyLoaded ? "Loading your match history..."
                                          : report.emptyMessage;
    m_playstyleRecentLabel = report.recentFormSubtitle;

    m_playstyleRecentForm.clear();
    for (const auto& r : report.recentForm) {
        m_playstyleRecentForm.push_back({r.label, r.recentVal, r.previousVal, r.delta, r.tone});
    }

    m_playstyleMetrics.clear();
    for (const auto& m : report.metrics) {
        m_playstyleMetrics.push_back({m.label, m.formattedValue, m.category});
    }

    m_playstyleSplitsEmpty = report.splitsEmptyMessage;
    m_playstyleSplits.clear();
    for (const auto& s : report.topSplits) {
        m_playstyleSplits.push_back({s.sentence, s.effectLabel});
    }

    for (const char* name : {"playstyle_has_data", "playstyle_sample_summary", "playstyle_empty",
                             "playstyle_recent_label", "playstyle_recent_form", "playstyle_metrics",
                             "playstyle_splits_empty", "playstyle_splits"})
        Dirty(name);
}

void InsightsView::RebuildPeople() {
    m_people.clear();
    const bool teammates = m_peopleFilter == Insights::PeopleFilter::Teammates;
    for (const auto& person : Insights::RankPeople(m_peopleSource, m_peopleFilter, kPeopleLimit)) {
        const int wins = teammates ? person.winsWith : person.winsAgainst;
        const int losses = teammates ? person.lossesWith : person.lossesAgainst;
        const int games = wins + losses;
        PersonRow row;
        row.name = person.name.empty() ? person.primaryId : person.name;
        row.platform = PlatformFor(person.primaryId);
        row.games = std::to_string(games);
        row.record = FormatRecord(wins, losses);
        const float rate = static_cast<float>(wins) / static_cast<float>(games);
        row.rate = Percent(rate);
        row.tone = rate > 0.5f ? 1 : rate < 0.5f ? -1
                                                 : 0;
        row.last_seen = FormatClock(person.lastSeenUnix);
        const int otherWins = teammates ? person.winsAgainst : person.winsWith;
        const int otherLosses = teammates ? person.lossesAgainst : person.lossesWith;
        row.other_record = otherWins + otherLosses > 0 ? FormatRecord(otherWins, otherLosses) : "--";
        row.encounter = EncounterSummary(person);
        row.primary_id = person.primaryId;
        if (m_notesLoaded) {
            if (const auto identity = PlayerNotes::IdentityFromPrimaryId(person.primaryId)) {
                row.can_note = true;
                row.has_note = m_notedKeys.count(identity->Key()) > 0;
            }
        }
        m_people.push_back(std::move(row));
    }
    m_peopleOtherLabel = teammates ? "Vs" : "With";
    m_peopleEmpty = !m_hasAccount      ? "Play a match so OmniStats can identify your account."
                    : !m_historyLoaded ? "Loading your match history..."
                    : teammates        ? "No saved games with teammates yet."
                                       : "No saved games against opponents yet.";
    Dirty("people");
    Dirty("people_empty");
    Dirty("people_other_label");
}
