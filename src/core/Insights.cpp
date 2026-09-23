#include "core/Insights.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

namespace {
    void Add(TrendBucket& bucket, bool win) {
        ++bucket.games;
        if (win) ++bucket.wins;
    }

    std::vector<TrendBucket> Buckets(std::initializer_list<const char*> labels) {
        std::vector<TrendBucket> buckets;
        for (const char* label : labels)
            buckets.push_back({label, 0, 0});
        return buckets;
    }

    int Percent(float rate) {
        return static_cast<int>(std::lround(rate * 100.0f));
    }
}

namespace Insights {
    TrendsReport ComputeTrends(const std::vector<MatchOutcome>& matches) {
        TrendsReport report;
        report.byTimeOfDay = Buckets({"Night (00-06)", "Morning (06-12)", "Afternoon (12-18)", "Evening (18-24)"});
        report.bySessionGame = Buckets({"Games 1-3", "Games 4-6", "Games 7-9", "Game 10+"});
        report.afterStreak = Buckets({"After a win", "After 1 loss", "After 2 losses", "After 3+ losses"});
        report.afterTwoPlusLosses.label = "After 2+ losses";

        std::map<std::string, TrendBucket> playlists;
        int64_t previousEnd = 0;
        int gameInSession = 0;
        int lossStreak = 0;
        bool previousWon = false;

        for (const auto& match : matches) {
            const bool newSession = gameInSession == 0 || match.endedAtUnix - previousEnd > kSessionGapSeconds;
            if (newSession) {
                ++report.sessions;
                gameInSession = 0;
                lossStreak = 0;
            } else if (previousWon) {
                Add(report.afterStreak[0], match.win);
            } else {
                Add(report.afterStreak[static_cast<size_t>(std::min(lossStreak, 3))], match.win);
                if (lossStreak >= 2) Add(report.afterTwoPlusLosses, match.win);
            }

            ++gameInSession;
            Add(report.bySessionGame[static_cast<size_t>(std::min((gameInSession - 1) / 3, 3))], match.win);
            Add(report.byTimeOfDay[static_cast<size_t>(std::clamp(match.localHour, 0, 23) / 6)], match.win);

            TrendBucket& playlist = playlists[match.playlist.empty() ? "Unknown" : match.playlist];
            playlist.label = match.playlist.empty() ? "Unknown" : match.playlist;
            Add(playlist, match.win);

            ++report.games;
            if (match.win) ++report.wins;
            lossStreak = match.win ? 0 : lossStreak + 1;
            previousWon = match.win;
            previousEnd = match.endedAtUnix;
        }

        for (auto& [_, bucket] : playlists)
            report.byPlaylist.push_back(bucket);
        std::stable_sort(report.byPlaylist.begin(), report.byPlaylist.end(),
                         [](const TrendBucket& a, const TrendBucket& b) { return a.games > b.games; });

        const TrendBucket& tilt = report.afterTwoPlusLosses;
        if (tilt.games >= kTiltMinimumGames && tilt.WinRate() <= report.WinRate() - kTiltMargin) {
            report.tiltWarning = "You win " + std::to_string(Percent(tilt.WinRate())) +
                                 "% of games after two straight losses, compared with " +
                                 std::to_string(Percent(report.WinRate())) +
                                 "% overall. Taking a break after two losses in a row may help.";
        }
        return report;
    }

    std::vector<PersonRecord> RankPeople(std::vector<PersonRecord> people, PeopleFilter filter, size_t limit) {
        const auto games = [filter](const PersonRecord& person) {
            return filter == PeopleFilter::Teammates ? person.GamesWith() : person.GamesAgainst();
        };
        std::erase_if(people, [&](const PersonRecord& person) { return games(person) == 0; });
        std::stable_sort(people.begin(), people.end(), [&](const PersonRecord& a, const PersonRecord& b) {
            if (games(a) != games(b)) return games(a) > games(b);
            return a.lastSeenUnix > b.lastSeenUnix;
        });
        if (people.size() > limit) people.resize(limit);
        return people;
    }
}
