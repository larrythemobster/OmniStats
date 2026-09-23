#include "ui/rml/views/InsightsView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <set>

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    constexpr size_t kPeopleLimit = 40;
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
        section.RegisterMember("rows", &TrendSection::rows);
    }
    constructor.RegisterArray<std::vector<TrendSection>>();

    constructor.Bind("tab", &m_tabName);
    constructor.Bind("subtitle", &m_subtitle);
    constructor.Bind("recap_title", &m_recapTitle);
    constructor.Bind("recap_date", &m_recapDate);
    constructor.Bind("recap_wins", &m_recapWins);
    constructor.Bind("recap_losses", &m_recapLosses);
    constructor.Bind("recap_rate", &m_recapRate);
    constructor.Bind("recap_mmr", &m_recapMmr);
    constructor.Bind("recap_mmr_tone", &m_recapMmrTone);
    constructor.Bind("recap_has_games", &m_recapHasGames);
    constructor.Bind("recap_stats", &m_recapStats);
    constructor.Bind("recap_modes", &m_recapModes);
    constructor.Bind("people_filter", &m_peopleFilterName);
    constructor.Bind("people", &m_people);
    constructor.Bind("people_empty", &m_peopleEmpty);
    constructor.Bind("tilt_message", &m_tiltMessage);
    constructor.Bind("trends_summary", &m_trendsSummary);
    constructor.Bind("trend_sections", &m_trendSections);
    constructor.Bind("trends_empty", &m_trendsEmpty);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void InsightsView::Reset() {
    m_handle = {};
    m_bound = false;
}

void InsightsView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void InsightsView::SetTab(Tab tab) {
    m_tab = tab;
    m_tabName = tab == Tab::Recap ? "recap" : tab == Tab::People ? "people"
                                                                 : "trends";
    Dirty("tab");
}

void InsightsView::SetPeopleFilter(Insights::PeopleFilter filter) {
    m_peopleFilter = filter;
    m_peopleFilterName = filter == Insights::PeopleFilter::Teammates ? "teammates" : "rivals";
    Dirty("people_filter");
    RebuildPeople();
}

std::vector<RecapModeRow> InsightsView::BuildRecapModes(const SessionRecap& recap) {
    std::set<std::string> keys;
    for (const auto& [mode, _] : recap.gamemodes)
        keys.insert(mode);
    for (const auto& [playlist, _] : recap.totals.mmrChangeByPlaylist)
        keys.insert(playlist);

    std::vector<RecapModeRow> rows;
    for (const auto& key : keys) {
        RecapModeRow row;
        row.name = MmrLabel(StringToMmrCategory(key));
        if (auto it = recap.gamemodes.find(key); it != recap.gamemodes.end())
            row.record = FormatRecord(it->second.wins, it->second.losses);
        else
            row.record = "-";
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
    const auto& totals = recap.totals;
    const int games = totals.wins + totals.losses;
    const int mmr = static_cast<int>(std::lround(totals.totalMmrChange));

    m_recapTitle = currentSession ? "CURRENT SESSION" : "SESSION RECAP";
    m_recapDate = currentSession ? LocalDate(static_cast<int64_t>(std::time(nullptr)))
                                 : "Ended " + LocalDate(recap.endedAtUnix);
    m_recapWins = std::to_string(totals.wins);
    m_recapLosses = std::to_string(totals.losses);
    m_recapRate = games > 0 ? Percent(static_cast<float>(totals.wins) / static_cast<float>(games)) : "-";
    m_recapMmr = SignedNumber(mmr);
    m_recapMmrTone = Tone(mmr);
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

    for (const char* name : {"recap_title", "recap_date", "recap_wins", "recap_losses", "recap_rate", "recap_mmr",
                             "recap_mmr_tone", "recap_has_games", "recap_stats", "recap_modes"})
        Dirty(name);
}

std::vector<TrendSection> InsightsView::BuildTrendSections(const TrendsReport& report) {
    if (report.games == 0) return {};
    const float overall = report.WinRate();
    return {
        MakeSection("BY PLAYLIST", report.byPlaylist, overall),
        MakeSection("TIME OF DAY", report.byTimeOfDay, overall),
        MakeSection("GAMES INTO A SITTING", report.bySessionGame, overall),
        MakeSection("AFTER THE PREVIOUS GAME", report.afterStreak, overall),
    };
}

void InsightsView::SetHistory(const std::string& primaryId, bool loaded, const std::vector<PersonRecord>& people,
                              const std::vector<MatchOutcome>& outcomes) {
    m_hasAccount = !primaryId.empty();
    m_historyLoaded = loaded;
    m_peopleSource = people;
    RebuildPeople();

    const TrendsReport report = Insights::ComputeTrends(outcomes);
    m_trendSections = BuildTrendSections(report);
    m_tiltMessage = report.tiltWarning;
    if (report.games > 0) {
        m_trendsSummary = std::to_string(report.games) + " games over " + std::to_string(report.sessions) +
                          (report.sessions == 1 ? " sitting" : " sittings") + ", " + Percent(report.WinRate()) +
                          " won overall. Green bars beat your average, red bars trail it.";
        m_subtitle = m_trendsSummary.substr(0, m_trendsSummary.find(','));
    } else {
        m_trendsSummary.clear();
        m_subtitle = "Your saved match history";
    }
    m_trendsEmpty = !m_hasAccount ? "Play a match so OmniStats can identify your account."
                    : !loaded     ? "Loading your match history..."
                                  : "No saved matches yet. Trends show up once you have played a few games.";
    for (const char* name : {"trend_sections", "tilt_message", "trends_summary", "trends_empty", "subtitle"})
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
        m_people.push_back(std::move(row));
    }
    m_peopleEmpty = !m_hasAccount      ? "Play a match so OmniStats can identify your account."
                    : !m_historyLoaded ? "Loading your match history..."
                    : teammates        ? "No saved games with teammates yet."
                                       : "No saved games against opponents yet.";
    Dirty("people");
    Dirty("people_empty");
}
