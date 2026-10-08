#include "core/Insights.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <string_view>

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

    std::vector<GapBucket> GapBuckets() {
        return {
            {"Much stronger (+75 or more)", 0, 0, 0, 0, 0.0, 0.0},
            {"Stronger (+25 to +75)", 0, 0, 0, 0, 0.0, 0.0},
            {"Even (±25)", 0, 0, 0, 0, 0.0, 0.0},
            {"Weaker (-25 to -75)", 0, 0, 0, 0, 0.0, 0.0},
            {"Much weaker (-75 or less)", 0, 0, 0, 0, 0.0, 0.0},
        };
    }

    size_t GapBucketIndex(double gap) {
        if (gap >= Insights::kGapStrongLimit) return 0;
        if (gap > Insights::kGapEvenLimit) return 1;
        if (gap >= -Insights::kGapEvenLimit) return 2;
        if (gap > -Insights::kGapStrongLimit) return 3;
        return 4;
    }

    struct CanonicalPlaylist {
        std::string_view key;
        std::string_view display;
        int teamSize = 0;
    };

    CanonicalPlaylist NormalizePlaylist(std::string_view raw) {
        if (raw == "1v1" || raw == "Duel" || raw == "duel") return {"1v1", "1v1", 1};
        if (raw == "2v2" || raw == "Doubles" || raw == "doubles") return {"2v2", "2v2", 2};
        if (raw == "3v3" || raw == "Standard" || raw == "standard") return {"3v3", "3v3", 3};
        if (raw == "hoops" || raw == "Hoops") return {"hoops", "Hoops", 2};
        if (raw == "rumble" || raw == "Rumble") return {"rumble", "Rumble", 3};
        if (raw == "dropshot" || raw == "Dropshot") return {"dropshot", "Dropshot", 3};
        if (raw == "snowday" || raw == "Snow Day" || raw == "Snowday") return {"snowday", "Snow Day", 3};
        if (raw == "heatseeker" || raw == "Heatseeker") return {"heatseeker", "Heatseeker", 2};
        return {"", "", 0};
    }

    int Percent(float rate) {
        return static_cast<int>(std::lround(rate * 100.0f));
    }

    std::string BuildGapCallout(const std::vector<GapBucket>& buckets, std::string_view playlistLabel) {
        const std::string inPlaylist = playlistLabel.empty() ? "" : " in " + std::string(playlistLabel);
        auto formatBucketCallout = [&](size_t idx, const GapBucket& bucket, const ConfidenceInterval& ci) -> std::string {
            const std::string pct = std::to_string(Percent(bucket.WinRate())) + "%";
            if (idx <= 1) {
                if (ci.low > 0.5) {
                    return "You win " + pct + " against stronger teams" + inPlaylist + ". You're climbing on merit.";
                }
                if (ci.high < 0.5) {
                    return "You win " + pct + " against stronger teams" + inPlaylist + ". Tougher lobbies are costing you games.";
                }
            } else if (idx == 2) {
                if (ci.high < 0.5) {
                    return "Against even lobbies you win " + pct + inPlaylist + ". Your losses aren't mostly mismatches.";
                }
                if (ci.low > 0.5) {
                    return "Against even lobbies you win " + pct + inPlaylist + ". You're outplaying peers at your rank.";
                }
            } else {
                if (ci.low > 0.5) {
                    return "You win " + pct + " against weaker teams" + inPlaylist + ". You close out favorable lobbies.";
                }
                if (ci.high < 0.5) {
                    return "You win " + pct + " against weaker teams" + inPlaylist + ". You're dropping games to lower-rated lobbies.";
                }
            }
            return {};
        };

        // Prefer notable patterns first: beating stronger teams, struggling in even lobbies,
        // then any remaining significant bucket with the most games.
        for (size_t idx : {size_t{1}, size_t{0}}) {
            if (idx < buckets.size() && buckets[idx].games >= Insights::kGapCalloutMinimumGames) {
                const ConfidenceInterval ci = Insights::WilsonInterval(buckets[idx].wins, buckets[idx].games);
                if (ci.low > 0.5) return formatBucketCallout(idx, buckets[idx], ci);
            }
        }
        if (buckets.size() > 2 && buckets[2].games >= Insights::kGapCalloutMinimumGames) {
            const ConfidenceInterval ci = Insights::WilsonInterval(buckets[2].wins, buckets[2].games);
            if (ci.Excludes(0.5)) return formatBucketCallout(2, buckets[2], ci);
        }

        size_t bestIdx = buckets.size();
        int bestGames = 0;
        ConfidenceInterval bestCi{};
        for (size_t idx = 0; idx < buckets.size(); ++idx) {
            if (buckets[idx].games < Insights::kGapCalloutMinimumGames) continue;
            const ConfidenceInterval ci = Insights::WilsonInterval(buckets[idx].wins, buckets[idx].games);
            if (ci.Excludes(0.5) && buckets[idx].games > bestGames) {
                bestIdx = idx;
                bestGames = buckets[idx].games;
                bestCi = ci;
            }
        }
        if (bestIdx < buckets.size()) return formatBucketCallout(bestIdx, buckets[bestIdx], bestCi);
        return {};
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
    ConfidenceInterval WilsonInterval(int wins, int games, double z) {
        if (games <= 0) return {0.0, 0.0};
        const double n = static_cast<double>(games);
        const double w = static_cast<double>(std::clamp(wins, 0, games));
        const double p = w / n;
        const double z2 = z * z;
        const double denom = 1.0 + z2 / n;
        const double center = (p + z2 / (2.0 * n)) / denom;
        const double halfWidth = (z * std::sqrt((p * (1.0 - p)) / n + z2 / (4.0 * n * n))) / denom;
        return {
            std::clamp(center - halfWidth, 0.0, 1.0),
            std::clamp(center + halfWidth, 0.0, 1.0),
        };
    }

    GapReport ComputeGapTrends(const std::vector<MatchMmrContext>& matches, const std::string& playlistFilter) {
        GapReport report;
        report.playlistFilter = playlistFilter.empty() ? "All" : playlistFilter;
        report.buckets = GapBuckets();
        report.carryHigher.label = "Carry factor (above teammates)";

        const bool matchAll = playlistFilter.empty() || playlistFilter == "All" || playlistFilter == "all";
        const CanonicalPlaylist targetPl = matchAll ? CanonicalPlaylist{} : NormalizePlaylist(playlistFilter);
        if (!matchAll && targetPl.teamSize == 0) {
            report.statusMessage = "Not enough ranked data";
            return report;
        }

        struct PrevMatch {
            size_t bucketIndex = 0;
            int64_t endedAtUnix = 0;
            int myMmr = 0;
            bool mmrEstimated = false;
            bool win = false;
        };
        std::map<std::string, PrevMatch, std::less<>> prevByPlaylist;

        for (const auto& match : matches) {
            const CanonicalPlaylist pl = NormalizePlaylist(match.playlist);
            if (pl.teamSize <= 0) continue;
            if (!matchAll && pl.key != targetPl.key) continue;
            if (match.teamCount != pl.teamSize || match.oppCount != pl.teamSize) continue;
            if (match.teamAvg <= 0.0 || match.oppAvg <= 0.0 || match.myMmr <= 0) continue;

            const double gap = match.oppAvg - match.teamAvg;
            const size_t idx = GapBucketIndex(gap);
            GapBucket& bucket = report.buckets[idx];
            ++bucket.games;
            if (match.win) ++bucket.wins;
            ++report.games;
            if (match.win) ++report.wins;

            if (pl.teamSize > 1) {
                const double teammatesAvg =
                    (match.teamAvg * static_cast<double>(match.teamCount) - static_cast<double>(match.myMmr)) /
                    static_cast<double>(match.teamCount - 1);
                const double carry = static_cast<double>(match.myMmr) - teammatesAvg;
                ++report.carryGames;
                if (match.win) ++report.carryWins;
                report.totalCarry += carry;
                if (carry > 0.0) {
                    Add(report.carryHigher, match.win);
                }
            }

            if (auto it = prevByPlaylist.find(pl.key); it != prevByPlaylist.end()) {
                const PrevMatch& prev = it->second;
                const int64_t elapsed = match.endedAtUnix - prev.endedAtUnix;
                if (!prev.mmrEstimated && !match.mmrEstimated && elapsed >= 0 && elapsed <= kSessionGapSeconds) {
                    const double delta = static_cast<double>(match.myMmr - prev.myMmr);
                    GapBucket& prevBucket = report.buckets[prev.bucketIndex];
                    if (prev.win) {
                        ++prevBucket.winDeltas;
                        prevBucket.totalWinDelta += delta;
                    } else {
                        ++prevBucket.lossDeltas;
                        prevBucket.totalLossDelta += delta;
                    }
                }
            }
            prevByPlaylist[std::string(pl.key)] = {idx, match.endedAtUnix, match.myMmr, match.mmrEstimated, match.win};
        }

        if (report.games == 0) {
            report.statusMessage = "Not enough ranked data";
            return report;
        }

        report.callout = BuildGapCallout(report.buckets, matchAll ? std::string_view{} : targetPl.display);
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
