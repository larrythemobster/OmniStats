#include "core/game/MatchStory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/game/MatchAnalytics.hpp"

namespace MatchStory {

    namespace {

        struct StoryCandidate {
            int priority = 0;
            std::string text;
        };

        std::string FormatClock(float seconds) {
            if (std::isnan(seconds) || seconds <= 0.0f) return "0:00";
            const int totalSec = std::max(0, static_cast<int>(std::lround(seconds)));
            const int mins = totalSec / 60;
            const int secs = totalSec % 60;
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
            return buf;
        }

    } // namespace

    std::vector<std::string> BuildMatchStory(const MatchDetail& detail) {
        std::vector<StoryCandidate> candidates;

        int myTeam = 0;
        if (!detail.ourTeam.empty()) {
            myTeam = detail.ourTeam.front().team;
        } else if (detail.analytics.has_value()) {
            if (!detail.accountPrimaryId.empty()) {
                if (const auto* meP = detail.analytics->FindPlayer(detail.accountPrimaryId)) {
                    if (meP->team == 0 || meP->team == 1) myTeam = meP->team;
                }
            }
            if (myTeam == 0) {
                for (const auto& p : detail.analytics->players) {
                    if (p.car.has_value() && (p.team == 0 || p.team == 1)) {
                        myTeam = p.team;
                        break;
                    }
                }
            }
        }

        if (detail.analytics.has_value() && !detail.analytics->Empty()) {
            const auto& a = *detail.analytics;
            const ScorelineSummary s = AnalyzeScoreline(a, myTeam, detail.win);
            const int opponent = (myTeam == 0) ? 1 : 0;

            // 1. Comeback / Blown lead
            if (s.comeback && s.maxDeficit >= 1) {
                std::string line;
                if (s.maxDeficit == 1) {
                    line = "Came back from a 1-goal deficit to win.";
                } else {
                    line = "Came back from a " + std::to_string(s.maxDeficit) + "-goal deficit to win.";
                }
                candidates.push_back({100 + s.maxDeficit * 5, std::move(line)});
            } else if (s.blownLead && s.maxLead >= 1) {
                std::string line;
                if (s.maxLead == 1) {
                    line = "Surrendered a 1-goal lead.";
                } else {
                    line = "Surrendered a " + std::to_string(s.maxLead) + "-goal lead.";
                }
                candidates.push_back({95 + s.maxLead * 5, std::move(line)});
            }

            // 2. Overtime winner / decider
            if (s.overtime || a.endedInOvertime || (detail.overtimeSeconds.has_value() && *detail.overtimeSeconds > 0.0f)) {
                std::string otDuration;
                if (detail.overtimeSeconds.has_value() && *detail.overtimeSeconds > 0.0f) {
                    otDuration = " (+" + FormatClock(*detail.overtimeSeconds) + ")";
                }
                if (detail.win) {
                    candidates.push_back({90, "Won in overtime" + otDuration + "."});
                } else {
                    candidates.push_back({90, "Lost in overtime" + otDuration + "."});
                }
            }

            // 3. Late winner
            if (s.lateWinner) {
                std::array<int, 2> score{};
                const int winner = detail.win ? myTeam : opponent;
                const GoalRecord* decisive = nullptr;
                for (const auto& g : a.goals) {
                    if (g.team != 0 && g.team != 1) continue;
                    const int winnerMarginBefore = score[winner] - score[1 - winner];
                    score[g.team]++;
                    if (g.team == winner && winnerMarginBefore == 0) decisive = &g;
                }
                if (decisive && decisive->clockSeconds.has_value()) {
                    const std::string clockStr = FormatClock(*decisive->clockSeconds);
                    if (decisive->team == myTeam) {
                        candidates.push_back({88, "Scored the match-winning goal with " + clockStr + " remaining."});
                    } else {
                        candidates.push_back({88, "Conceded the match-winning goal with " + clockStr + " remaining."});
                    }
                }
            }

            // 4. Early end / forfeit
            if (a.EndedEarly() && a.endClockSeconds.has_value()) {
                const std::string clockStr = FormatClock(*a.endClockSeconds);
                if (detail.win) {
                    candidates.push_back({85, "Opponents forfeited with " + clockStr + " remaining."});
                } else {
                    candidates.push_back({85, "Match ended early with " + clockStr + " remaining."});
                }
            }

            // 5. Departures / Teammate or opponent left
            for (const auto& dep : a.departures) {
                if (!detail.accountPrimaryId.empty() && dep.primaryId == detail.accountPrimaryId) continue;
                const std::string depName = dep.name.empty() ? "A player" : dep.name;
                std::string clockInfo;
                if (dep.clockSeconds.has_value()) {
                    clockInfo = " with " + FormatClock(*dep.clockSeconds) + " remaining";
                }
                if (dep.team == myTeam) {
                    candidates.push_back({86, "Teammate " + depName + " left the match" + clockInfo + "."});
                } else {
                    candidates.push_back({70, "Opponent " + depName + " left the match" + clockInfo + "."});
                }
                break;
            }

            // 6. Kickoff goals
            if (s.goalsAfterKickoffFor >= 1 && s.goalsAfterKickoffAgainst >= 1) {
                candidates.push_back({76, "Scored " + std::to_string(s.goalsAfterKickoffFor) + " and conceded " +
                                              std::to_string(s.goalsAfterKickoffAgainst) +
                                              " kickoff goals within 10 seconds of play."});
            } else if (s.goalsAfterKickoffFor >= 1) {
                const std::string word = s.goalsAfterKickoffFor == 1 ? " goal" : " goals";
                candidates.push_back(
                    {75, "Scored " + std::to_string(s.goalsAfterKickoffFor) + word + " within 10 seconds of kickoff."});
            } else if (s.goalsAfterKickoffAgainst >= 1) {
                const std::string word = s.goalsAfterKickoffAgainst == 1 ? " goal" : " goals";
                candidates.push_back(
                    {75, "Conceded " + std::to_string(s.goalsAfterKickoffAgainst) + word + " within 10 seconds of kickoff."});
            }

            // 7. Last-minute regulation goals
            if (s.lastMinuteGoalsFor >= 2) {
                candidates.push_back({74, "Scored " + std::to_string(s.lastMinuteGoalsFor) +
                                              " goals in the final minute of regulation."});
            } else if (s.lastMinuteGoalsAgainst >= 2) {
                candidates.push_back({74, "Conceded " + std::to_string(s.lastMinuteGoalsAgainst) +
                                              " goals in the final minute of regulation."});
            }

            // 8. Possession share vs outcome
            const float myPossession = a.PossessionShare(myTeam);
            if (myPossession >= 0.60f && !detail.win) {
                const int pct = std::clamp(static_cast<int>(std::lround(myPossession * 100.0f)), 0, 100);
                candidates.push_back({80, "Held " + std::to_string(pct) + "% possession despite the loss."});
            } else if (myPossession <= 0.40f && myPossession > 0.0f && detail.win) {
                const int pct = std::clamp(static_cast<int>(std::lround(myPossession * 100.0f)), 0, 100);
                candidates.push_back({80, "Won despite holding only " + std::to_string(pct) + "% possession."});
            } else if (myPossession >= 0.68f && detail.win) {
                const int pct = std::clamp(static_cast<int>(std::lround(myPossession * 100.0f)), 0, 100);
                candidates.push_back({65, "Controlled " + std::to_string(pct) + "% of possession in the win."});
            }

            // Find local player in PlayerAnalytics
            const PlayerAnalytics* me = nullptr;
            if (!detail.accountPrimaryId.empty()) {
                me = a.FindPlayer(detail.accountPrimaryId);
            }
            if (!me) {
                for (const auto& p : a.players) {
                    if (p.car.has_value()) {
                        me = &p;
                        break;
                    }
                }
            }
            if (!me && !detail.ourTeam.empty()) {
                for (const auto& p : detail.ourTeam) {
                    if (p.isMe) {
                        me = a.FindPlayer(p.primaryId);
                        break;
                    }
                }
            }

            // 9. Your share of team hits/touches
            int teamHits = 0;
            int teamPlayerCount = 0;
            for (const auto& p : a.players) {
                if (p.team == myTeam) {
                    teamHits += p.hits;
                    teamPlayerCount++;
                }
            }
            if (me && teamPlayerCount >= 2 && teamHits > 0) {
                const float share = static_cast<float>(me->hits) / static_cast<float>(teamHits);
                const float expected = 1.0f / static_cast<float>(teamPlayerCount);
                if (share >= expected + 0.15f) {
                    const int pct = std::clamp(static_cast<int>(std::lround(share * 100.0f)), 0, 100);
                    candidates.push_back({72, "Accounted for " + std::to_string(pct) + "% of your team's touches."});
                } else if (share <= expected - 0.15f && share > 0.0f) {
                    const int pct = std::clamp(static_cast<int>(std::lround(share * 100.0f)), 0, 100);
                    candidates.push_back({60, "Accounted for only " + std::to_string(pct) + "% of your team's touches."});
                }
            }

            // 10. Hardest hit in lobby
            if (me && me->maxHitSpeed >= 105.0f) {
                float maxLobbySpeed = 0.0f;
                for (const auto& p : a.players) {
                    maxLobbySpeed = std::max(maxLobbySpeed, p.maxHitSpeed);
                }
                if (me->maxHitSpeed >= maxLobbySpeed - 0.1f) {
                    candidates.push_back({68, "Recorded the fastest hit in the match."});
                }
            }

            // 11. Boost starvation
            if (me && me->car.has_value()) {
                const auto& car = *me->car;
                const float zeroShare = car.Share(car.zeroBoostSeconds);
                if (zeroShare >= 0.15f) {
                    const int pct = std::clamp(static_cast<int>(std::lround(zeroShare * 100.0f)), 0, 100);
                    candidates.push_back({66, "Spent " + std::to_string(pct) + "% of the match at zero boost."});
                }
            }

            // 12. Demo differential
            int ourDemos = 0;
            int theirDemos = 0;
            for (const auto& p : a.players) {
                if (p.team == myTeam) ourDemos += p.demosInflicted;
                if (p.team == opponent) theirDemos += p.demosInflicted;
            }
            const int demoDiff = ourDemos - theirDemos;
            if (demoDiff >= 4) {
                candidates.push_back(
                    {62, "Your team led demolitions " + std::to_string(ourDemos) + " to " + std::to_string(theirDemos) + "."});
            } else if (demoDiff <= -4) {
                candidates.push_back(
                    {62, "Opponents led demolitions " + std::to_string(theirDemos) + " to " + std::to_string(ourDemos) + "."});
            } else if (me && me->demosInflicted >= 4) {
                candidates.push_back({61, "Inflicted " + std::to_string(me->demosInflicted) + " demolitions."});
            }
        } else {
            // Legacy match without analytics: derive simple notable points if available
            if (detail.overtimeSeconds.has_value() && *detail.overtimeSeconds > 0.0f) {
                const std::string otDuration = " (+" + FormatClock(*detail.overtimeSeconds) + ")";
                candidates.push_back({90, (detail.win ? "Won in overtime" : "Lost in overtime") + otDuration + "."});
            }
            if (detail.localStats.has_value()) {
                const auto& ls = *detail.localStats;
                if (ls.crossbars >= 3) {
                    candidates.push_back({65, "Hit the crossbar " + std::to_string(ls.crossbars) + " times."});
                }
                if (ls.demoed >= 4) {
                    candidates.push_back({62, "Demolished " + std::to_string(ls.demoed) + " times."});
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const StoryCandidate& a, const StoryCandidate& b) {
            return a.priority > b.priority;
        });

        std::vector<std::string> results;
        const size_t limit = std::min<size_t>(5, candidates.size());
        results.reserve(limit);
        for (size_t i = 0; i < limit; ++i) {
            results.push_back(std::move(candidates[i].text));
        }
        return results;
    }

} // namespace MatchStory
