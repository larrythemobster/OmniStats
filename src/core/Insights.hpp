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
struct MatchMmrContext {
    int64_t endedAtUnix = 0;
    std::string playlist;
    bool win = false;
    int myMmr = 0;
    bool mmrEstimated = false;
    double teamAvg = 0.0;
    double oppAvg = 0.0;
    int teamCount = 0;
    int oppCount = 0;
};

struct ConfidenceInterval {
    double low = 0.0;
    double high = 0.0;

    bool Excludes(double value) const {
        return value < low || value > high;
    }
};

struct GapBucket {
    std::string label;
    int games = 0;
    int wins = 0;

    float WinRate() const {
        return games > 0 ? static_cast<float>(wins) / static_cast<float>(games) : 0.0f;
    }
};

struct GapReport {
    std::string playlistFilter;
    int games = 0;
    int wins = 0;
    std::vector<GapBucket> buckets;
    TrendBucket carryHigher;
    int carryGames = 0;
    std::string callout;
    std::string statusMessage;

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
    inline constexpr bool StartsNewSession(int64_t previousEndedAtUnix, int64_t endedAtUnix, bool hasPrevious = true) {
        return !hasPrevious || endedAtUnix - previousEndedAtUnix > kSessionGapSeconds;
    }

    struct SessionSlice {
        size_t beginIndex = 0;
        size_t endIndex = 0;
        int64_t startedAtUnix = 0;
        int64_t endedAtUnix = 0;
    };
    std::vector<SessionSlice> GroupSessionTimestamps(const std::vector<int64_t>& endedAtUnix);
    // A tilt warning needs this many games after two straight losses and a win
    // rate at least this far below the overall rate.
    inline constexpr int kTiltMinimumGames = 10;
    inline constexpr float kTiltMargin = 0.08f;

    // MMR gap = oppAvg - teamAvg.
    // Bucket boundaries:
    //   Much stronger: gap >= +kGapStrongLimit (+75 inclusive)
    //   Stronger:      +kGapEvenLimit < gap < +kGapStrongLimit (+25, +75 exclusive)
    //   Even:          -kGapEvenLimit <= gap <= +kGapEvenLimit (-25, +25 inclusive)
    //   Weaker:        -kGapStrongLimit < gap < -kGapEvenLimit (-75, -25 exclusive)
    //   Much weaker:   gap <= -kGapStrongLimit (-75 inclusive)
    inline constexpr double kGapEvenLimit = 25.0;
    inline constexpr double kGapStrongLimit = 75.0;
    inline constexpr int kGapMinimumBucketGames = 10;
    inline constexpr int kGapCalloutMinimumGames = 30;
    inline constexpr double kWilsonZ95 = 1.96;

    // `matches` must be in chronological order.
    TrendsReport ComputeTrends(const std::vector<MatchOutcome>& matches);
    GapReport ComputeGapTrends(const std::vector<MatchMmrContext>& matches, const std::string& playlistFilter = "All");
    ConfidenceInterval WilsonInterval(int wins, int games, double z = kWilsonZ95);

    enum class PeopleFilter { Teammates,
                              Rivals };
    // People with at least one game in the filtered relationship, most games first.
    std::vector<PersonRecord> RankPeople(std::vector<PersonRecord> people, PeopleFilter filter, size_t limit);
}
