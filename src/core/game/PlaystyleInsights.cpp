#include "core/game/PlaystyleInsights.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>

namespace Playstyle {

    namespace {

        struct Sample {
            bool win = false;
            float boostPerMinute = 0.0f;
            float avgBoost = 0.0f;
            float timeAtZeroBoostPct = 0.0f;
            float timeFullBoostPct = 0.0f;
            float bigPads = 0.0f;
            float smallPads = 0.0f;
            float supersonicPct = 0.0f;
            float boostSpeedPct = 0.0f;
            float slowPct = 0.0f;
            float airPct = 0.0f;
            float wallPct = 0.0f;
            float avgSpeed = 0.0f;
            float powerslidesPerMin = 0.0f;
            float hits = 0.0f;
            float avgHitSpeed = 0.0f;
            float aerialHits = 0.0f;
            float firstTouchRate = 0.0f;
            float possessionPct = 0.0f;
            float demosInflicted = 0.0f;
            float demosTaken = 0.0f;
            float touches = 0.0f;
            float goals = 0.0f;
            float saves = 0.0f;
            float shots = 0.0f;
        };

        std::string FormatFloat(float val, int decimals = 1) {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(decimals) << val;
            return ss.str();
        }

        std::string FormatInt(float val) {
            return std::to_string(static_cast<int>(std::lround(val)));
        }

        std::string FormatPct(float val) {
            return std::to_string(static_cast<int>(std::lround(val))) + "%";
        }

        std::string FormatSpeed(float val) {
            return std::to_string(static_cast<int>(std::lround(val))) + " km/h";
        }

        std::pair<std::string, std::string> FormatDistinctPct(float w, float l) {
            const int iw = static_cast<int>(std::lround(w));
            const int il = static_cast<int>(std::lround(l));
            if (iw != il) return {std::to_string(iw) + "%", std::to_string(il) + "%"};
            return {FormatFloat(w, 1) + "%", FormatFloat(l, 1) + "%"};
        }

        std::pair<std::string, std::string> FormatDistinctInt(float w, float l) {
            const int iw = static_cast<int>(std::lround(w));
            const int il = static_cast<int>(std::lround(l));
            if (iw != il) return {std::to_string(iw), std::to_string(il)};
            return {FormatFloat(w, 1), FormatFloat(l, 1)};
        }

        std::pair<std::string, std::string> FormatDistinctSpeed(float w, float l) {
            const int iw = static_cast<int>(std::lround(w));
            const int il = static_cast<int>(std::lround(l));
            if (iw != il) return {std::to_string(iw) + " km/h", std::to_string(il) + " km/h"};
            return {FormatFloat(w, 1) + " km/h", FormatFloat(l, 1) + " km/h"};
        }

        std::pair<std::string, std::string> FormatDistinctFloat(float w, float l, int dec = 1) {
            std::string sw = FormatFloat(w, dec);
            std::string sl = FormatFloat(l, dec);
            if (sw == sl) {
                sw = FormatFloat(w, dec + 1);
                sl = FormatFloat(l, dec + 1);
            }
            return {sw, sl};
        }

        std::string FormatSignedDelta(float delta, const std::string& unit) {
            const int rounded = static_cast<int>(std::lround(delta));
            std::string s;
            if (rounded > 0)
                s = "+" + std::to_string(rounded);
            else
                s = std::to_string(rounded);
            if (!unit.empty()) s += " " + unit;
            return s;
        }

        std::string FormatSignedDelta1Dec(float delta, const std::string& unit) {
            const float rounded = std::lround(delta * 10.0f) / 10.0f;
            std::ostringstream ss;
            if (rounded > 0.0f) ss << "+";
            ss << std::fixed << std::setprecision(1) << rounded;
            if (!unit.empty()) ss << (unit == "%" ? "" : " ") << unit;
            return ss.str();
        }

        struct MetricDef {
            const char* key;
            const char* label;
            const char* category;
            float (*getter)(const Sample&);
            std::string (*formatter)(float);
            std::string (*sentenceBuilder)(float winMean, float lossMean);
        };

        const MetricDef kMetricDefs[] = {
            // Boost
            {"bpm", "Boost per minute", "Boost",
             [](const Sample& s) { return s.boostPerMinute; },
             [](float v) { return FormatInt(v) + " / min"; },
             [](float w, float l) { auto [sw, sl] = FormatDistinctInt(w, l); return "In wins you use " + sw + " boost per minute; in losses " + sl + "."; }},
            {"avg_boost", "Average boost", "Boost",
             [](const Sample& s) { return s.avgBoost; },
             [](float v) { return FormatInt(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctInt(w, l); return "In wins your average boost is " + sw + "; in losses " + sl + "."; }},
            {"zero_boost", "Time at 0 boost", "Boost",
             [](const Sample& s) { return s.timeAtZeroBoostPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time at zero boost; in losses " + sl + "."; }},
            {"full_boost", "Time full boost", "Boost",
             [](const Sample& s) { return s.timeFullBoostPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time with full boost; in losses " + sl + "."; }},
            {"big_pads", "Big pads / game", "Boost",
             [](const Sample& s) { return s.bigPads; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you collect " + sw + " big pads per game; in losses " + sl + "."; }},
            {"small_pads", "Small pads / game", "Boost",
             [](const Sample& s) { return s.smallPads; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you collect " + sw + " small pads per game; in losses " + sl + "."; }},

            // Movement
            {"supersonic", "Supersonic", "Movement",
             [](const Sample& s) { return s.supersonicPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time supersonic; in losses " + sl + "."; }},
            {"boost_speed", "Boost speed", "Movement",
             [](const Sample& s) { return s.boostSpeedPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time at boost speed; in losses " + sl + "."; }},
            {"slow_speed", "Slow speed", "Movement",
             [](const Sample& s) { return s.slowPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time slow; in losses " + sl + "."; }},
            {"air_time", "Air time", "Movement",
             [](const Sample& s) { return s.airPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time in the air; in losses " + sl + "."; }},
            {"wall_time", "Wall time", "Movement",
             [](const Sample& s) { return s.wallPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you spend " + sw + " of the time on the wall; in losses " + sl + "."; }},
            {"avg_speed", "Average speed", "Movement",
             [](const Sample& s) { return s.avgSpeed; },
             [](float v) { return FormatSpeed(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctSpeed(w, l); return "In wins your average speed is " + sw + "; in losses " + sl + "."; }},
            {"powerslides", "Powerslides / min", "Movement",
             [](const Sample& s) { return s.powerslidesPerMin; },
             [](float v) { return FormatFloat(v, 1) + " / min"; },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you powerslide " + sw + " times per minute; in losses " + sl + "."; }},

            // Mechanics & Ball
            {"hits", "Hits / game", "Mechanics & Ball",
             [](const Sample& s) { return s.hits; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " hits; in losses " + sl + "."; }},
            {"avg_hit_speed", "Average hit speed", "Mechanics & Ball",
             [](const Sample& s) { return s.avgHitSpeed; },
             [](float v) { return FormatSpeed(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctSpeed(w, l); return "In wins your average hit speed is " + sw + "; in losses " + sl + "."; }},
            {"aerial_hits", "Aerial hits / game", "Mechanics & Ball",
             [](const Sample& s) { return s.aerialHits; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " aerial hits; in losses " + sl + "."; }},
            {"first_touch", "First-touch rate", "Mechanics & Ball",
             [](const Sample& s) { return s.firstTouchRate; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins you win the first touch " + sw + " of the time; in losses " + sl + "."; }},
            {"possession", "Possession", "Mechanics & Ball",
             [](const Sample& s) { return s.possessionPct; },
             [](float v) { return FormatPct(v); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctPct(w, l); return "In wins your team holds " + sw + " possession; in losses " + sl + "."; }},
            {"demos_inflicted", "Demos inflicted / game", "Mechanics & Ball",
             [](const Sample& s) { return s.demosInflicted; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you inflict " + sw + " demos; in losses " + sl + "."; }},
            {"demos_taken", "Demos taken / game", "Mechanics & Ball",
             [](const Sample& s) { return s.demosTaken; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you take " + sw + " demos; in losses " + sl + "."; }},

            // Scoreboard
            {"touches", "Touches / game", "Scoreboard",
             [](const Sample& s) { return s.touches; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " touches; in losses " + sl + "."; }},
            {"goals", "Goals / game", "Scoreboard",
             [](const Sample& s) { return s.goals; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " goals; in losses " + sl + "."; }},
            {"saves", "Saves / game", "Scoreboard",
             [](const Sample& s) { return s.saves; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " saves; in losses " + sl + "."; }},
            {"shots", "Shots / game", "Scoreboard",
             [](const Sample& s) { return s.shots; },
             [](float v) { return FormatFloat(v, 1); },
             [](float w, float l) { auto [sw, sl] = FormatDistinctFloat(w, l, 1); return "In wins you average " + sw + " shots; in losses " + sl + "."; }},
        };

    } // namespace

    bool MatchMatchesPlaylist(const AnalyticsMatchRow& row, const std::string& playlistFilter) {
        if (playlistFilter.empty() || playlistFilter == "All" || playlistFilter == "all") {
            return true;
        }
        if (playlistFilter == "1v1") {
            if (row.playlistId == 10) return true;
            if (row.playlist == "Duel" || row.playlist == "duel" || row.playlist == "1v1" ||
                row.playlist == "Ranked Duel 1v1" || row.playlist.find("1v1") != std::string::npos) return true;
        } else if (playlistFilter == "2v2") {
            if (row.playlistId == 11) return true;
            if (row.playlist == "Doubles" || row.playlist == "doubles" || row.playlist == "2v2" ||
                row.playlist == "Ranked Doubles 2v2" || row.playlist.find("2v2") != std::string::npos) return true;
        } else if (playlistFilter == "3v3") {
            if (row.playlistId == 12 || row.playlistId == 13) return true;
            if (row.playlist == "Standard" || row.playlist == "standard" || row.playlist == "3v3" ||
                row.playlist == "Ranked Standard 3v3" || row.playlist.find("3v3") != std::string::npos) return true;
        }
        return row.playlist.find(playlistFilter) != std::string::npos;
    }

    PlaystyleReport ComputePlaystyle(const std::vector<AnalyticsMatchRow>& matches,
                                     const std::string& playlistFilter) {
        PlaystyleReport report;

        // Collect telemetry samples in original order (newest first)
        std::vector<Sample> samples;
        int totalMatchesInFilter = 0;
        for (const auto& match : matches) {
            if (!MatchMatchesPlaylist(match, playlistFilter)) continue;
            ++totalMatchesInFilter;
            if (!match.analytics.has_value()) continue;

            const auto& a = *match.analytics;
            const PlayerAnalytics* localPlayer = nullptr;
            for (const auto& p : a.players) {
                if (p.car.has_value()) {
                    localPlayer = &p;
                    break;
                }
            }
            if (!localPlayer || !localPlayer->car.has_value()) continue;

            const auto& car = *localPlayer->car;
            Sample s;
            s.win = match.win;

            // CarTelemetry metrics
            s.boostPerMinute = car.BoostPerMinute();
            s.avgBoost = car.AverageBoost();
            s.timeAtZeroBoostPct = car.Share(car.zeroBoostSeconds) * 100.0f;
            s.timeFullBoostPct = car.Share(car.fullBoostSeconds) * 100.0f;
            s.bigPads = static_cast<float>(car.bigPads);
            s.smallPads = static_cast<float>(car.smallPads);
            s.supersonicPct = car.Share(car.supersonicSeconds) * 100.0f;
            s.boostSpeedPct = car.Share(car.boostSpeedSeconds) * 100.0f;
            s.slowPct = car.Share(car.slowSeconds) * 100.0f;
            s.airPct = car.Share(car.airSeconds) * 100.0f;
            s.wallPct = car.Share(car.wallSeconds) * 100.0f;
            s.avgSpeed = car.AverageSpeed();
            s.powerslidesPerMin = car.sampledSeconds > 0.0f
                                      ? (static_cast<float>(car.powerslideCount) * 60.0f / car.sampledSeconds)
                                      : 0.0f;

            // PlayerAnalytics metrics
            s.hits = static_cast<float>(localPlayer->hits);
            s.avgHitSpeed = localPlayer->AverageHitSpeed();
            s.aerialHits = static_cast<float>(localPlayer->aerialHits);
            s.demosInflicted = static_cast<float>(localPlayer->demosInflicted);
            s.demosTaken = static_cast<float>(localPlayer->demosTaken);

            // Kickoff first touch rate
            if (!a.kickoffs.empty()) {
                s.firstTouchRate = (static_cast<float>(localPlayer->firstTouches) * 100.0f) /
                                   static_cast<float>(a.kickoffs.size());
            }

            // Possession
            const int myTeam = localPlayer->team >= 0 ? localPlayer->team : match.myTeam;
            if (myTeam >= 0 && myTeam < 2) {
                s.possessionPct = a.PossessionShare(myTeam) * 100.0f;
            }

            // Scoreboard fields
            s.touches = static_cast<float>(match.touches.value_or(localPlayer->hits));
            s.goals = static_cast<float>(match.goals.value_or(localPlayer->StatEvent("Goal")));
            s.saves = static_cast<float>(match.saves.value_or(localPlayer->StatEvent("Save") + localPlayer->StatEvent("EpicSave")));
            s.shots = static_cast<float>(match.shots.value_or(localPlayer->StatEvent("Shot")));

            samples.push_back(s);
        }

        report.totalMatchesWithTelemetry = static_cast<int>(samples.size());
        if (samples.empty()) {
            if (!playlistFilter.empty() && playlistFilter != "All" && playlistFilter != "all") {
                report.emptyMessage = "No saved " + playlistFilter + " matches with car telemetry yet. Telemetry is recorded live during your games.";
            } else {
                report.emptyMessage = "No saved matches with car telemetry yet. Telemetry is recorded live during your games.";
            }
            report.splitsEmptyMessage = "Need at least 8 wins and 8 losses with telemetry.";
            return report;
        }

        // Counts
        int wins = 0;
        int losses = 0;
        for (const auto& s : samples) {
            if (s.win)
                ++wins;
            else
                ++losses;
        }
        report.winsWithTelemetry = wins;
        report.lossesWithTelemetry = losses;

        if (totalMatchesInFilter > report.totalMatchesWithTelemetry) {
            report.sampleSummary = "Based on " + std::to_string(report.totalMatchesWithTelemetry) +
                                   " of " + std::to_string(totalMatchesInFilter) +
                                   (totalMatchesInFilter == 1 ? " match" : " matches") +
                                   " with car telemetry (" + std::to_string(wins) + "W - " + std::to_string(losses) + "L).";
        } else {
            report.sampleSummary = "Based on " + std::to_string(report.totalMatchesWithTelemetry) +
                                   (report.totalMatchesWithTelemetry == 1 ? " match" : " matches") +
                                   " with car telemetry (" + std::to_string(wins) + "W - " + std::to_string(losses) + "L).";
        }

        // 1. Overall averages for all 24 metrics
        const float countF = static_cast<float>(samples.size());
        for (const auto& def : kMetricDefs) {
            float sum = 0.0f;
            for (const auto& s : samples) {
                sum += def.getter(s);
            }
            const float mean = sum / countF;
            MetricSummary ms;
            ms.key = def.key;
            ms.label = def.label;
            ms.category = def.category;
            ms.overallMean = mean;
            ms.formattedValue = def.formatter(mean);
            report.metrics.push_back(std::move(ms));
        }

        // 2. Recent form: last 10 vs previous 40
        const int recentCount = std::min(10, static_cast<int>(samples.size()));
        const int previousStart = 10;
        const int previousCount = (static_cast<int>(samples.size()) > 10)
                                      ? std::min(40, static_cast<int>(samples.size()) - 10)
                                      : 0;
        report.recentCount = recentCount;
        report.previousCount = previousCount;

        if (previousCount > 0) {
            report.recentFormSubtitle = "Last " + std::to_string(recentCount) + " vs previous " +
                                        std::to_string(previousCount) + " matches";
        } else {
            report.recentFormSubtitle = "Last " + std::to_string(recentCount) + " matches (need 11+ for comparison)";
        }

        auto getRecentFormMetric = [&](const char* label,
                                       float (*getter)(const Sample&),
                                       std::string (*fmt)(float),
                                       bool isInt,
                                       bool isPct,
                                       bool isSpeed) -> RecentFormMetric {
            float recentSum = 0.0f;
            for (int i = 0; i < recentCount; ++i) {
                recentSum += getter(samples[i]);
            }
            const float recentMean = recentSum / static_cast<float>(recentCount);

            RecentFormMetric rfm;
            rfm.label = label;
            rfm.recentVal = fmt(recentMean);

            if (previousCount > 0) {
                float prevSum = 0.0f;
                for (int i = previousStart; i < previousStart + previousCount; ++i) {
                    prevSum += getter(samples[i]);
                }
                const float prevMean = prevSum / static_cast<float>(previousCount);
                rfm.previousVal = fmt(prevMean);
                const float delta = recentMean - prevMean;

                if (isPct) {
                    rfm.delta = FormatSignedDelta1Dec(delta, "%");
                    const float r10 = std::lround(delta * 10.0f) / 10.0f;
                    rfm.tone = r10 > 0.0f ? 1 : (r10 < 0.0f ? -1 : 0);
                } else if (isSpeed) {
                    rfm.delta = FormatSignedDelta(delta, "km/h");
                    const int rounded = static_cast<int>(std::lround(delta));
                    rfm.tone = rounded > 0 ? 1 : (rounded < 0 ? -1 : 0);
                } else if (isInt) {
                    rfm.delta = FormatSignedDelta(delta, "");
                    const int rounded = static_cast<int>(std::lround(delta));
                    rfm.tone = rounded > 0 ? 1 : (rounded < 0 ? -1 : 0);
                } else {
                    rfm.delta = FormatSignedDelta1Dec(delta, "");
                    const float r10 = std::lround(delta * 10.0f) / 10.0f;
                    rfm.tone = r10 > 0.0f ? 1 : (r10 < 0.0f ? -1 : 0);
                }
            } else {
                rfm.previousVal = "—";
                rfm.delta = "—";
                rfm.tone = 0;
            }
            return rfm;
        };

        report.recentForm.push_back(getRecentFormMetric("Boost per minute", [](const Sample& s) { return s.boostPerMinute; }, [](float v) { return FormatInt(v); }, true, false, false));
        report.recentForm.push_back(getRecentFormMetric("Supersonic", [](const Sample& s) { return s.supersonicPct; }, [](float v) { return FormatPct(v); }, false, true, false));
        report.recentForm.push_back(getRecentFormMetric("Avg hit speed", [](const Sample& s) { return s.avgHitSpeed; }, [](float v) { return FormatSpeed(v); }, false, false, true));
        report.recentForm.push_back(getRecentFormMetric("Hits / game", [](const Sample& s) { return s.hits; }, [](float v) { return FormatFloat(v, 1); }, false, false, false));

        // 3. What separates your wins (only metrics with >=8 wins and >=8 losses)
        if (wins < 8 || losses < 8) {
            std::ostringstream ss;
            ss << "Need at least 8 wins and 8 losses with telemetry (have " << wins << " wins and " << losses
               << " losses; need ";
            if (wins < 8 && losses < 8) {
                ss << (8 - wins) << " more wins and " << (8 - losses) << " more losses).";
            } else if (wins < 8) {
                ss << (8 - wins) << " more wins).";
            } else {
                ss << (8 - losses) << " more losses).";
            }
            report.splitsEmptyMessage = ss.str();
        } else {
            report.splitsEmptyMessage.clear();
            std::vector<WinSplit> splits;
            const float nw = static_cast<float>(wins);
            const float nl = static_cast<float>(losses);

            for (const auto& def : kMetricDefs) {
                float sumW = 0.0f;
                float sumL = 0.0f;
                for (const auto& s : samples) {
                    const float val = def.getter(s);
                    if (s.win)
                        sumW += val;
                    else
                        sumL += val;
                }
                const float meanW = sumW / nw;
                const float meanL = sumL / nl;

                // Sample variances
                float sqErrW = 0.0f;
                float sqErrL = 0.0f;
                for (const auto& s : samples) {
                    const float val = def.getter(s);
                    if (s.win) {
                        const float d = val - meanW;
                        sqErrW += d * d;
                    } else {
                        const float d = val - meanL;
                        sqErrL += d * d;
                    }
                }

                const float varW = nw > 1.0f ? sqErrW / (nw - 1.0f) : 0.0f;
                const float varL = nl > 1.0f ? sqErrL / (nl - 1.0f) : 0.0f;
                const float pooledVar = (nw + nl > 2.0f)
                                            ? ((nw - 1.0f) * varW + (nl - 1.0f) * varL) / (nw + nl - 2.0f)
                                            : 0.0f;
                const float pooledSd = pooledVar > 0.0f ? std::sqrt(pooledVar) : 0.0f;

                float d = 0.0f;
                if (pooledSd > 1e-4f) {
                    d = (meanW - meanL) / pooledSd;
                } else if (std::abs(meanW - meanL) > 1e-4f) {
                    // Within-group variance is zero; fall back to combined sample SD so perfectly separated groups are not dropped.
                    float totalSqErr = 0.0f;
                    const float grandMean = (sumW + sumL) / (nw + nl);
                    for (const auto& s : samples) {
                        const float diff = def.getter(s) - grandMean;
                        totalSqErr += diff * diff;
                    }
                    const float totalSd = (nw + nl > 1.0f && totalSqErr > 0.0f)
                                              ? std::sqrt(totalSqErr / (nw + nl - 1.0f))
                                              : 0.0f;
                    if (totalSd > 1e-4f) {
                        d = (meanW - meanL) / totalSd;
                    }
                }
                WinSplit split;
                split.metricKey = def.key;
                split.winMean = meanW;
                split.lossMean = meanL;
                split.effectSize = d;
                std::ostringstream ss;
                ss << "d = " << (d >= 0.0f ? "+" : "") << std::fixed << std::setprecision(2) << d;
                split.effectLabel = ss.str();
                split.sentence = def.sentenceBuilder(meanW, meanL);
                splits.push_back(std::move(split));
            }

            // Top 5 by |effect size|
            std::stable_sort(splits.begin(), splits.end(), [](const WinSplit& a, const WinSplit& b) {
                return std::abs(a.effectSize) > std::abs(b.effectSize);
            });

            const size_t keep = std::min<size_t>(5, splits.size());
            for (size_t i = 0; i < keep; ++i) {
                if (std::abs(splits[i].effectSize) > 1e-3f) {
                    report.topSplits.push_back(std::move(splits[i]));
                }
            }
            if (report.topSplits.empty()) {
                report.splitsEmptyMessage = "No significant playstyle differences found between your wins and losses yet.";
            }
        }

        return report;
    }
    AnalyticsTrendsReport ComputeAnalyticsTrends(const std::vector<AnalyticsMatchRow>& matches,
                                                 const std::string& playlistFilter) {
        AnalyticsTrendsReport report;
        report.overtimeGames.label = "Overtime games";
        report.oneGoalGames.label = "One-goal games";
        report.scoredFirst.label = "Scored first";
        report.concededFirst.label = "Conceded first";
        report.comebacks.label = "Comebacks (when trailing)";
        report.blownLeads.label = "Blown leads (when leading)";
        report.regulationGames.label = "Regulation";
        report.overtimeLengthGames.label = "Overtime";
        report.endedEarlyGames.label = "Ended early (forfeit)";

        std::map<std::string, TrendBucket> arenas;
        int analyticsMatchCount = 0;

        for (const auto& match : matches) {
            if (!MatchMatchesPlaylist(match, playlistFilter)) continue;

            // Overtime & one-goal games
            const bool isOt = (match.analytics.has_value() && match.analytics->endedInOvertime) ||
                              (match.overtimeSeconds.has_value() && *match.overtimeSeconds > 0.0f);
            if (isOt) {
                ++report.overtimeGames.games;
                if (match.win) ++report.overtimeGames.wins;
            }
            if (std::abs(match.ourScore - match.theirScore) == 1) {
                ++report.oneGoalGames.games;
                if (match.win) ++report.oneGoalGames.wins;
            }

            // Arenas
            if (!match.arena.empty()) {
                TrendBucket& ab = arenas[match.arena];
                ab.label = match.arena;
                ++ab.games;
                if (match.win) ++ab.wins;
            }

            if (match.analytics.has_value()) {
                const auto& a = *match.analytics;
                ++analyticsMatchCount;

                // Scored first vs conceded first
                if (!a.goals.empty()) {
                    const int firstGoalTeam = a.goals.front().team;
                    if (firstGoalTeam == match.myTeam && match.myTeam >= 0) {
                        ++report.scoredFirst.games;
                        if (match.win) ++report.scoredFirst.wins;
                    } else if (firstGoalTeam >= 0) {
                        ++report.concededFirst.games;
                        if (match.win) ++report.concededFirst.wins;
                    }
                }

                // Scoreline summary: comebacks, blown leads, late winners, kickoff goals
                const ScorelineSummary s = AnalyzeScoreline(a, match.myTeam, match.win);
                if (s.maxDeficit > 0) {
                    ++report.comebacks.games;
                    if (s.comeback) ++report.comebacks.wins;
                }
                if (s.maxLead > 0) {
                    ++report.blownLeads.games;
                    if (s.blownLead) ++report.blownLeads.wins;
                }
                if (s.lateWinner) {
                    ++report.lateRegulationWinners;
                }
                report.totalKickoffGoalsFor += s.goalsAfterKickoffFor;
                report.totalKickoffGoalsAgainst += s.goalsAfterKickoffAgainst;

                // Kickoffs
                for (const auto& ko : a.kickoffs) {
                    ++report.totalKickoffs;
                    if (ko.possessionTeam == match.myTeam && match.myTeam >= 0) {
                        ++report.kickoffPossessionWon;
                    }
                }

                // Match length
                if (a.endedInOvertime) {
                    ++report.overtimeLengthGames.games;
                    if (match.win) ++report.overtimeLengthGames.wins;
                } else if (a.EndedEarly()) {
                    ++report.endedEarlyGames.games;
                    if (match.win) ++report.endedEarlyGames.wins;
                } else {
                    ++report.regulationGames.games;
                    if (match.win) ++report.regulationGames.wins;
                }
            }
        }

        if (analyticsMatchCount > 0) {
            report.goalsAfterKickoffForPerGame =
                static_cast<float>(report.totalKickoffGoalsFor) / static_cast<float>(analyticsMatchCount);
            report.goalsAfterKickoffAgainstPerGame =
                static_cast<float>(report.totalKickoffGoalsAgainst) / static_cast<float>(analyticsMatchCount);
        }

        for (auto& [_, ab] : arenas) {
            if (ab.games >= 3) {
                report.byArena.push_back(ab);
            }
        }
        std::stable_sort(report.byArena.begin(), report.byArena.end(),
                         [](const TrendBucket& a, const TrendBucket& b) { return a.games > b.games; });

        return report;
    }

} // namespace Playstyle
