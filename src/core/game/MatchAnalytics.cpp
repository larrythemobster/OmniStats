#include "core/game/MatchAnalytics.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace {
    constexpr float kFieldThirdY = 5120.0f / 3.0f;
    constexpr float kAerialHitZ = 300.0f;
    constexpr float kCrossbarZ = 642.775f;
    constexpr float kHardHitKph = 100.0f;
    // Tuned on recorded online matches: closest match to the game's Touches stat.
    constexpr double kTouchMergeSeconds = 0.03;
    constexpr float kGoalLineY = 5000.0f;
    constexpr float kUuToKph = 0.036f;
    constexpr float kBoostSpeedKph = 1400.0f * kUuToKph;
    constexpr float kSupersonicKph = 2200.0f * kUuToKph;
    // Raw maxima above these can only be uu/s; the Stats API docs and observed payloads disagree on units.
    constexpr float kCarRawUuThreshold = 150.0f;
    constexpr float kBallRawUuThreshold = 400.0f;
    constexpr float kMaxSampleSeconds = 0.5f;
    constexpr float kKickoffPossessionSeconds = 3.0f;
    constexpr float kKickoffGoalSeconds = 10.0f;
    constexpr int kSmallPadBoost = 12;
    constexpr float kMidfieldPadY = 100.0f;

    const nlohmann::json* Child(const nlohmann::json& j, const char* key) {
        if (!j.is_object()) return nullptr;
        const auto it = j.find(key);
        return it == j.end() ? nullptr : &*it;
    }

    std::optional<float> Number(const nlohmann::json& j, const char* key) {
        const auto* v = Child(j, key);
        if (!v || !v->is_number()) return std::nullopt;
        const float f = v->get<float>();
        return std::isfinite(f) ? std::optional<float>(f) : std::nullopt;
    }

    std::optional<int> Integer(const nlohmann::json& j, const char* key) {
        const auto* v = Child(j, key);
        if (!v || !v->is_number()) return std::nullopt;
        return static_cast<int>(std::lround(v->get<double>()));
    }

    std::optional<bool> Bool(const nlohmann::json& j, const char* key) {
        const auto* v = Child(j, key);
        if (!v || !v->is_boolean()) return std::nullopt;
        return v->get<bool>();
    }

    std::string String(const nlohmann::json& j, const char* key) {
        const auto* v = Child(j, key);
        return v && v->is_string() ? v->get<std::string>() : std::string{};
    }

    std::optional<FieldPoint> Point(const nlohmann::json& j, const char* key) {
        const auto* v = Child(j, key);
        if (!v || !v->is_object()) return std::nullopt;
        const auto x = Number(*v, "X");
        const auto y = Number(*v, "Y");
        const auto z = Number(*v, "Z");
        if (!x || !y || !z) return std::nullopt;
        return FieldPoint{*x, *y, *z};
    }

    std::string NormalizeId(const std::string& id, const std::string& name) {
        if (id == "Unknown" || id.rfind("Unknown|", 0) == 0) return "Unknown|" + name;
        return id;
    }

    int TeamOf(const nlohmann::json& j) {
        const auto team = Integer(j, "TeamNum");
        return team && (*team == 0 || *team == 1) ? *team : -1;
    }

    float Round1(float v) {
        return std::round(v * 10.0f) / 10.0f;
    }

    float SpeedKph(float raw, bool rawIsUu) {
        return rawIsUu ? raw * kUuToKph : raw;
    }

    template <typename T>
    void ReadOpt(const nlohmann::json& j, const char* key, std::optional<T>& out) {
        const auto* v = Child(j, key);
        if (v && v->is_number()) out = v->get<T>();
    }

    template <typename T>
    void Read(const nlohmann::json& j, const char* key, T& out) {
        const auto* v = Child(j, key);
        if (!v) return;
        if constexpr (std::is_same_v<T, bool>) {
            if (v->is_boolean()) out = v->get<bool>();
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (v->is_string()) out = v->get<std::string>();
        } else {
            if (v->is_number()) out = v->get<T>();
        }
    }

    nlohmann::json PointJson(const FieldPoint& p) {
        return nlohmann::json::array({Round1(p.x), Round1(p.y), Round1(p.z)});
    }

    std::optional<FieldPoint> PointFromJson(const nlohmann::json* j) {
        if (!j || !j->is_array() || j->size() != 3) return std::nullopt;
        for (const auto& v : *j)
            if (!v.is_number()) return std::nullopt;
        return FieldPoint{(*j)[0].get<float>(), (*j)[1].get<float>(), (*j)[2].get<float>()};
    }

    template <size_t N, typename T>
    nlohmann::json ArrayJson(const std::array<T, N>& a) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& v : a) {
            if constexpr (std::is_floating_point_v<T>) {
                out.push_back(Round1(v));
            } else {
                out.push_back(v);
            }
        }
        return out;
    }

    template <size_t N, typename T>
    void ReadArray(const nlohmann::json& j, const char* key, std::array<T, N>& out) {
        const auto* v = Child(j, key);
        if (!v || !v->is_array() || v->size() != N) return;
        for (size_t i = 0; i < N; ++i)
            if ((*v)[i].is_number()) out[i] = (*v)[i].get<T>();
    }
} // namespace

float CarTelemetry::AverageSpeed() const {
    return sampledSeconds > 0.0f ? distance * kUuToKph / sampledSeconds : 0.0f;
}

float CarTelemetry::AverageBoost() const {
    return sampledSeconds > 0.0f ? boostIntegral / sampledSeconds : 0.0f;
}

float CarTelemetry::BoostPerMinute() const {
    return sampledSeconds > 0.0f ? static_cast<float>(boostUsed) * 60.0f / sampledSeconds : 0.0f;
}

float CarTelemetry::Share(float seconds) const {
    return sampledSeconds > 0.0f ? seconds / sampledSeconds : 0.0f;
}

float PlayerAnalytics::AverageHitSpeed() const {
    return hits > 0 ? hitSpeedSum / static_cast<float>(hits) : 0.0f;
}

int PlayerAnalytics::StatEvent(const std::string& eventName) const {
    const auto it = statEvents.find(eventName);
    return it == statEvents.end() ? 0 : it->second;
}

bool MatchAnalytics::Empty() const {
    return playSeconds <= 0.0f && players.empty() && goals.empty() && hits.empty();
}

bool MatchAnalytics::EndedEarly() const {
    return endClockSeconds.has_value() && *endClockSeconds >= 1.0f && !endedInOvertime;
}

float MatchAnalytics::AverageBallSpeed() const {
    return playSeconds > 0.0f ? ballSpeedIntegral / playSeconds : 0.0f;
}

float MatchAnalytics::PossessionShare(int team) const {
    if (team != 0 && team != 1) return 0.0f;
    const float total = possessionSeconds[0] + possessionSeconds[1];
    return total > 0.0f ? possessionSeconds[team] / total : 0.0f;
}

const PlayerAnalytics* MatchAnalytics::FindPlayer(const std::string& primaryId) const {
    for (const auto& p : players)
        if (p.primaryId == primaryId) return &p;
    return nullptr;
}

nlohmann::json MatchAnalyticsToJson(const MatchAnalytics& a) {
    nlohmann::json j;
    j["v"] = a.formatVersion;
    j["playSeconds"] = Round1(a.playSeconds);
    j["possession"] = ArrayJson(a.possessionSeconds);
    j["ballSpeedIntegral"] = Round1(a.ballSpeedIntegral);
    j["maxBallSpeed"] = Round1(a.maxBallSpeed);
    j["pauses"] = a.pauses;
    j["pausedSeconds"] = Round1(a.pausedSeconds);
    if (a.endClockSeconds) j["endClock"] = Round1(*a.endClockSeconds);
    j["endedInOvertime"] = a.endedInOvertime;

    auto& players = j["players"] = nlohmann::json::array();
    for (const auto& p : a.players) {
        nlohmann::json pj{{"id", p.primaryId},
                          {"name", p.name},
                          {"team", p.team},
                          {"hits", p.hits},
                          {"aerialHits", p.aerialHits},
                          {"highAerialHits", p.highAerialHits},
                          {"hardHits", p.hardHits},
                          {"firstTouches", p.firstTouches},
                          {"hitsByThird", ArrayJson(p.hitsByThird)},
                          {"hitSpeedSum", Round1(p.hitSpeedSum)},
                          {"maxHitSpeed", Round1(p.maxHitSpeed)},
                          {"demosInflicted", p.demosInflicted},
                          {"demosTaken", p.demosTaken},
                          {"statEvents", p.statEvents}};
        if (p.car) {
            const auto& c = *p.car;
            pj["car"] = {{"sampledSeconds", Round1(c.sampledSeconds)},
                         {"distance", std::round(c.distance)},
                         {"maxSpeed", Round1(c.maxSpeed)},
                         {"slowSeconds", Round1(c.slowSeconds)},
                         {"boostSpeedSeconds", Round1(c.boostSpeedSeconds)},
                         {"supersonicSeconds", Round1(c.supersonicSeconds)},
                         {"groundSeconds", Round1(c.groundSeconds)},
                         {"airSeconds", Round1(c.airSeconds)},
                         {"wallSeconds", Round1(c.wallSeconds)},
                         {"powerslideSeconds", Round1(c.powerslideSeconds)},
                         {"powerslideCount", c.powerslideCount},
                         {"boostingSeconds", Round1(c.boostingSeconds)},
                         {"boostIntegral", Round1(c.boostIntegral)},
                         {"zeroBoostSeconds", Round1(c.zeroBoostSeconds)},
                         {"fullBoostSeconds", Round1(c.fullBoostSeconds)},
                         {"boostBandSeconds", ArrayJson(c.boostBandSeconds)},
                         {"boostUsed", c.boostUsed},
                         {"boostCollected", c.boostCollected},
                         {"bigPads", c.bigPads},
                         {"smallPads", c.smallPads},
                         {"stolenBigPads", c.stolenBigPads},
                         {"stolenSmallPads", c.stolenSmallPads},
                         {"padsFromPickupEvents", c.padsFromPickupEvents},
                         {"demolishedSeconds", Round1(c.demolishedSeconds)}};
        }
        players.push_back(std::move(pj));
    }

    auto& goals = j["goals"] = nlohmann::json::array();
    for (const auto& g : a.goals) {
        nlohmann::json gj{{"t", Round1(g.playSeconds)},
                          {"ot", g.overtime},
                          {"team", g.team},
                          {"scorerId", g.scorerId},
                          {"scorerName", g.scorerName},
                          {"score", ArrayJson(g.scoreAfter)}};
        if (g.clockSeconds) gj["clock"] = Round1(*g.clockSeconds);
        if (!g.assisterId.empty() || !g.assisterName.empty()) {
            gj["assisterId"] = g.assisterId;
            gj["assisterName"] = g.assisterName;
        }
        if (g.goalSpeed) gj["speed"] = Round1(*g.goalSpeed);
        if (g.lastTouchSpeed) gj["touchSpeed"] = Round1(*g.lastTouchSpeed);
        if (g.location) gj["loc"] = PointJson(*g.location);
        if (g.roundSeconds) gj["round"] = Round1(*g.roundSeconds);
        goals.push_back(std::move(gj));
    }

    // Hits are stored as rows against a player table to keep long matches small.
    std::vector<std::string> hitPlayers;
    auto& hits = j["hits"] = nlohmann::json::array();
    for (const auto& h : a.hits) {
        auto it = std::find(hitPlayers.begin(), hitPlayers.end(), h.playerId);
        const size_t index = static_cast<size_t>(it - hitPlayers.begin());
        if (it == hitPlayers.end()) hitPlayers.push_back(h.playerId);
        hits.push_back({Round1(h.playSeconds), index, h.team, std::round(h.location.x), std::round(h.location.y),
                        std::round(h.location.z), Round1(h.speed)});
    }
    j["hitPlayers"] = hitPlayers;

    auto& kickoffs = j["kickoffs"] = nlohmann::json::array();
    for (const auto& k : a.kickoffs) {
        kickoffs.push_back({{"t", Round1(k.playSeconds)},
                            {"firstTouchId", k.firstTouchId},
                            {"firstTouchTeam", k.firstTouchTeam},
                            {"possessionTeam", k.possessionTeam},
                            {"goalTeam", k.goalTeam}});
    }

    auto& timeline = j["possessionTimeline"] = nlohmann::json::array();
    for (const auto& w : a.possessionTimeline)
        timeline.push_back({Round1(w.endPlaySeconds), Round1(w.seconds[0]), Round1(w.seconds[1])});

    auto& departures = j["departures"] = nlohmann::json::array();
    for (const auto& d : a.departures) {
        nlohmann::json dj{{"id", d.primaryId}, {"name", d.name}, {"team", d.team}, {"t", Round1(d.playSeconds)}};
        if (d.clockSeconds) dj["clock"] = Round1(*d.clockSeconds);
        departures.push_back(std::move(dj));
    }
    return j;
}

bool IsGoalReplayEcho(const nlohmann::json& goalScored) {
    const auto* scorer = Child(goalScored, "Scorer");
    return scorer && scorer->is_object() && String(*scorer, "Name").empty();
}

std::optional<MatchAnalytics> MatchAnalyticsFromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    MatchAnalytics a;
    Read(j, "v", a.formatVersion);
    if (a.formatVersion < 1 || a.formatVersion > MatchAnalytics::kFormatVersion) return std::nullopt;
    Read(j, "playSeconds", a.playSeconds);
    ReadArray(j, "possession", a.possessionSeconds);
    Read(j, "ballSpeedIntegral", a.ballSpeedIntegral);
    Read(j, "maxBallSpeed", a.maxBallSpeed);
    Read(j, "pauses", a.pauses);
    Read(j, "pausedSeconds", a.pausedSeconds);
    ReadOpt(j, "endClock", a.endClockSeconds);
    Read(j, "endedInOvertime", a.endedInOvertime);

    if (const auto* players = Child(j, "players"); players && players->is_array()) {
        for (const auto& pj : *players) {
            if (!pj.is_object()) continue;
            PlayerAnalytics p;
            Read(pj, "id", p.primaryId);
            Read(pj, "name", p.name);
            Read(pj, "team", p.team);
            Read(pj, "hits", p.hits);
            Read(pj, "aerialHits", p.aerialHits);
            Read(pj, "highAerialHits", p.highAerialHits);
            Read(pj, "hardHits", p.hardHits);
            Read(pj, "firstTouches", p.firstTouches);
            ReadArray(pj, "hitsByThird", p.hitsByThird);
            Read(pj, "hitSpeedSum", p.hitSpeedSum);
            Read(pj, "maxHitSpeed", p.maxHitSpeed);
            Read(pj, "demosInflicted", p.demosInflicted);
            Read(pj, "demosTaken", p.demosTaken);
            if (const auto* events = Child(pj, "statEvents"); events && events->is_object()) {
                for (const auto& [name, count] : events->items())
                    if (count.is_number_integer()) p.statEvents[name] = count.get<int>();
            }
            if (const auto* cj = Child(pj, "car"); cj && cj->is_object()) {
                CarTelemetry c;
                Read(*cj, "sampledSeconds", c.sampledSeconds);
                Read(*cj, "distance", c.distance);
                Read(*cj, "maxSpeed", c.maxSpeed);
                Read(*cj, "slowSeconds", c.slowSeconds);
                Read(*cj, "boostSpeedSeconds", c.boostSpeedSeconds);
                Read(*cj, "supersonicSeconds", c.supersonicSeconds);
                Read(*cj, "groundSeconds", c.groundSeconds);
                Read(*cj, "airSeconds", c.airSeconds);
                Read(*cj, "wallSeconds", c.wallSeconds);
                Read(*cj, "powerslideSeconds", c.powerslideSeconds);
                Read(*cj, "powerslideCount", c.powerslideCount);
                Read(*cj, "boostingSeconds", c.boostingSeconds);
                Read(*cj, "boostIntegral", c.boostIntegral);
                Read(*cj, "zeroBoostSeconds", c.zeroBoostSeconds);
                Read(*cj, "fullBoostSeconds", c.fullBoostSeconds);
                ReadArray(*cj, "boostBandSeconds", c.boostBandSeconds);
                Read(*cj, "boostUsed", c.boostUsed);
                Read(*cj, "boostCollected", c.boostCollected);
                Read(*cj, "bigPads", c.bigPads);
                Read(*cj, "smallPads", c.smallPads);
                Read(*cj, "stolenBigPads", c.stolenBigPads);
                Read(*cj, "stolenSmallPads", c.stolenSmallPads);
                Read(*cj, "padsFromPickupEvents", c.padsFromPickupEvents);
                Read(*cj, "demolishedSeconds", c.demolishedSeconds);
                p.car = c;
            }
            a.players.push_back(std::move(p));
        }
    }

    if (const auto* goals = Child(j, "goals"); goals && goals->is_array()) {
        for (const auto& gj : *goals) {
            if (!gj.is_object()) continue;
            GoalRecord g;
            Read(gj, "t", g.playSeconds);
            ReadOpt(gj, "clock", g.clockSeconds);
            Read(gj, "ot", g.overtime);
            Read(gj, "team", g.team);
            Read(gj, "scorerId", g.scorerId);
            Read(gj, "scorerName", g.scorerName);
            Read(gj, "assisterId", g.assisterId);
            Read(gj, "assisterName", g.assisterName);
            ReadOpt(gj, "speed", g.goalSpeed);
            ReadOpt(gj, "touchSpeed", g.lastTouchSpeed);
            g.location = PointFromJson(Child(gj, "loc"));
            ReadOpt(gj, "round", g.roundSeconds);
            ReadArray(gj, "score", g.scoreAfter);
            a.goals.push_back(std::move(g));
        }
    }

    std::vector<std::string> hitPlayers;
    if (const auto* hp = Child(j, "hitPlayers"); hp && hp->is_array())
        for (const auto& id : *hp)
            hitPlayers.push_back(id.is_string() ? id.get<std::string>() : std::string{});
    if (const auto* hits = Child(j, "hits"); hits && hits->is_array()) {
        for (const auto& row : *hits) {
            if (!row.is_array() || row.size() != 7) continue;
            bool numeric = true;
            for (const auto& v : row)
                numeric = numeric && v.is_number();
            if (!numeric) continue;
            const size_t index = row[1].get<size_t>();
            if (index >= hitPlayers.size()) continue;
            a.hits.push_back({row[0].get<float>(), hitPlayers[index], row[2].get<int>(),
                              FieldPoint{row[3].get<float>(), row[4].get<float>(), row[5].get<float>()},
                              row[6].get<float>()});
        }
    }

    if (const auto* kickoffs = Child(j, "kickoffs"); kickoffs && kickoffs->is_array()) {
        for (const auto& kj : *kickoffs) {
            if (!kj.is_object()) continue;
            KickoffRecord k;
            Read(kj, "t", k.playSeconds);
            Read(kj, "firstTouchId", k.firstTouchId);
            Read(kj, "firstTouchTeam", k.firstTouchTeam);
            Read(kj, "possessionTeam", k.possessionTeam);
            Read(kj, "goalTeam", k.goalTeam);
            a.kickoffs.push_back(std::move(k));
        }
    }

    if (const auto* timeline = Child(j, "possessionTimeline"); timeline && timeline->is_array()) {
        for (const auto& row : *timeline) {
            if (!row.is_array() || row.size() != 3 || !row[0].is_number() || !row[1].is_number() || !row[2].is_number())
                continue;
            a.possessionTimeline.push_back({row[0].get<float>(), {row[1].get<float>(), row[2].get<float>()}});
        }
    }

    if (const auto* departures = Child(j, "departures"); departures && departures->is_array()) {
        for (const auto& dj : *departures) {
            if (!dj.is_object()) continue;
            PlayerDeparture d;
            Read(dj, "id", d.primaryId);
            Read(dj, "name", d.name);
            Read(dj, "team", d.team);
            Read(dj, "t", d.playSeconds);
            ReadOpt(dj, "clock", d.clockSeconds);
            a.departures.push_back(std::move(d));
        }
    }
    return a;
}

ScorelineSummary AnalyzeScoreline(const MatchAnalytics& analytics, int myTeam, bool won) {
    ScorelineSummary s;
    if (myTeam != 0 && myTeam != 1) return s;
    const int opponent = 1 - myTeam;
    const int winner = won ? myTeam : opponent;
    std::array<int, 2> score{};
    const GoalRecord* decisive = nullptr;
    for (const auto& g : analytics.goals) {
        if (g.team != 0 && g.team != 1) continue;
        const int winnerMarginBefore = score[winner] - score[1 - winner];
        score[g.team]++;
        const int diff = score[myTeam] - score[opponent];
        s.maxLead = std::max(s.maxLead, diff);
        s.maxDeficit = std::max(s.maxDeficit, -diff);
        if (g.overtime) s.overtime = true;
        if (g.team == winner && winnerMarginBefore == 0) decisive = &g;
        if (!g.overtime && g.clockSeconds && *g.clockSeconds <= 60.0f) {
            if (g.team == myTeam) {
                s.lastMinuteGoalsFor++;
            } else {
                s.lastMinuteGoalsAgainst++;
            }
        }
    }
    s.overtime = s.overtime || analytics.endedInOvertime;
    s.comeback = won && s.maxDeficit > 0;
    s.blownLead = !won && s.maxLead > 0;
    s.lateWinner = decisive && !decisive->overtime && decisive->clockSeconds && *decisive->clockSeconds <= 30.0f;
    for (const auto& k : analytics.kickoffs) {
        if (k.goalTeam == myTeam) s.goalsAfterKickoffFor++;
        if (k.goalTeam == opponent) s.goalsAfterKickoffAgainst++;
    }
    return s;
}
