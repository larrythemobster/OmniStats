#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "core/state/SessionState.hpp"

namespace Playstyle {

    struct MetricSummary {
        std::string key;
        std::string label;
        std::string category; // "Boost", "Movement", "Mechanics & Ball", "Scoreboard"
        float overallMean = 0.0f;
        std::string formattedValue;
    };

    struct WinSplit {
        std::string metricKey;
        std::string sentence;
        float winMean = 0.0f;
        float lossMean = 0.0f;
        float effectSize = 0.0f; // standardized difference: (mean_w - mean_l) / pooled_sd
        std::string effectLabel; // e.g. "d = +0.82"
    };

    struct RecentFormMetric {
        std::string label;
        std::string recentVal;
        std::string previousVal;
        std::string delta;
        int tone = 0; // 1 = positive/improved, -1 = negative/dropped, 0 = neutral
    };

    struct PlaystyleReport {
        int totalMatchesWithTelemetry = 0;
        int winsWithTelemetry = 0;
        int lossesWithTelemetry = 0;
        std::string sampleSummary;
        std::string emptyMessage;

        // Recent form: last 10 vs previous 40
        int recentCount = 0;
        int previousCount = 0;
        std::string recentFormSubtitle;
        std::vector<RecentFormMetric> recentForm;

        // Full per-game metric averages
        std::vector<MetricSummary> metrics;

        // What separates your wins (top 5 by |effect size|, min 8 wins & 8 losses)
        std::string splitsEmptyMessage;
        std::vector<WinSplit> topSplits;
    };
    struct AnalyticsTrendsReport {
        // Clutch & game state
        TrendBucket overtimeGames;
        TrendBucket oneGoalGames;
        TrendBucket scoredFirst;
        TrendBucket concededFirst;
        TrendBucket comebacks;  // games = trailed at any point, wins = came back and won
        TrendBucket blownLeads; // games = led at any point, wins = blown leads (lost)
        int lateRegulationWinners = 0;

        // Arenas
        std::vector<TrendBucket> byArena;

        // Kickoffs
        int totalKickoffs = 0;
        int kickoffPossessionWon = 0;
        int totalKickoffGoalsFor = 0;
        int totalKickoffGoalsAgainst = 0;
        float goalsAfterKickoffForPerGame = 0.0f;
        float goalsAfterKickoffAgainstPerGame = 0.0f;

        // Match length
        TrendBucket regulationGames;
        TrendBucket overtimeLengthGames;
        TrendBucket endedEarlyGames;
    };

    // Check if a match matches the trend playlist filter ("All", "1v1", "2v2", "3v3")
    bool MatchMatchesPlaylist(const AnalyticsMatchRow& row, const std::string& playlistFilter);

    // Compute pure playstyle aggregations from matches newest-first
    PlaystyleReport ComputePlaystyle(const std::vector<AnalyticsMatchRow>& matches,
                                     const std::string& playlistFilter = "All");

    // Compute clutch, arenas, kickoffs, and match length trends from analytics matches
    AnalyticsTrendsReport ComputeAnalyticsTrends(const std::vector<AnalyticsMatchRow>& matches,
                                                 const std::string& playlistFilter = "All");

} // namespace Playstyle
