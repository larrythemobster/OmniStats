#pragma once

// Aggregates behind the Insights window: people you play with or against, and
// win-rate trends across playlists, time of day, session length, and loss
// streaks. Everything here is plain data plus pure functions so it can be
// tested without a database or UI.

#include <cstdint>
#include <string>
#include <vector>

struct PersonRecord {
    std::string primaryId;
    std::string name;
    int winsWith = 0;
    int lossesWith = 0;
    int winsAgainst = 0;
    int lossesAgainst = 0;
    int64_t lastSeenUnix = 0;

    int GamesWith() const {
        return winsWith + lossesWith;
    }
    int GamesAgainst() const {
        return winsAgainst + lossesAgainst;
    }
};

struct MatchOutcome {
    int64_t endedAtUnix = 0;
    int localHour = 0; // 0-23 in the player's local time zone
    std::string playlist;
    bool win = false;
};

struct TrendBucket {
    std::string label;
    int games = 0;
    int wins = 0;

    float WinRate() const {
        return games > 0 ? static_cast<float>(wins) / static_cast<float>(games) : 0.0f;
    }
};

struct TrendsReport {
    int games = 0;
    int wins = 0;
    int sessions = 0;
    std::vector<TrendBucket> byPlaylist;
    std::vector<TrendBucket> byTimeOfDay;
    std::vector<TrendBucket> bySessionGame;
    std::vector<TrendBucket> afterStreak;
    // Win rate after two or more straight losses in one sitting, and the
    // warning shown when it falls well below the overall rate.
    TrendBucket afterTwoPlusLosses;
    std::string tiltWarning;

    float WinRate() const {
        return games > 0 ? static_cast<float>(wins) / static_cast<float>(games) : 0.0f;
    }
};

namespace Insights {
    // Consecutive matches more than this far apart belong to different sittings.
    inline constexpr int64_t kSessionGapSeconds = 2 * 60 * 60;
    // A tilt warning needs this many games after two straight losses and a win
    // rate at least this far below the overall rate.
    inline constexpr int kTiltMinimumGames = 10;
    inline constexpr float kTiltMargin = 0.08f;

    // `matches` must be in chronological order.
    TrendsReport ComputeTrends(const std::vector<MatchOutcome>& matches);

    enum class PeopleFilter { Teammates,
                              Rivals };
    // People with at least one game in the filtered relationship, most games first.
    std::vector<PersonRecord> RankPeople(std::vector<PersonRecord> people, PeopleFilter filter, size_t limit);
}
