#include "core/game/MatchAnalyticsTracker.hpp"

#include <algorithm>
#include <cstddef>

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

    float SpeedKph(float raw, bool rawIsUu) {
        return rawIsUu ? raw * kUuToKph : raw;
    }

    std::optional<float> Positive(const std::optional<float>& value) {
        return value && *value > 0.0f ? value : std::nullopt;
    }
} // namespace

PlayerAnalytics& MatchAnalyticsTracker::PlayerFor(const std::string& primaryId) {
    const auto it = m_playerIndex.find(primaryId);
    if (it != m_playerIndex.end()) return m_data.players[it->second];
    m_playerIndex.emplace(primaryId, m_data.players.size());
    PlayerAnalytics p;
    p.primaryId = primaryId;
    m_data.players.push_back(std::move(p));
    return m_data.players.back();
}

std::string MatchAnalyticsTracker::ResolvePlayer(const TelemetryEvents::PlayerRef& ref) const {
    if (!ref.primaryId.empty()) return ref.primaryId;
    // Shortcuts are reused when a player replaces a bot mid-match, so names win over shortcuts.
    if (!ref.name.empty()) {
        std::string match;
        bool ambiguous = false;
        for (const auto& p : m_data.players) {
            if (p.name != ref.name || (ref.team != -1 && p.team != ref.team)) continue;
            ambiguous = !match.empty();
            match = p.primaryId;
        }
        if (!match.empty() && !ambiguous) return match;
    }
    if (ref.shortcut) {
        const auto it = m_shortcutToId.find(*ref.shortcut);
        if (it != m_shortcutToId.end()) return it->second;
    }
    return {};
}

void MatchAnalyticsTracker::AddPlayTime(float seconds) {
    m_data.playSeconds += seconds;
    if (m_possessionTeam == 0 || m_possessionTeam == 1) {
        m_data.possessionSeconds[static_cast<size_t>(m_possessionTeam)] += seconds;
        m_window.seconds[static_cast<size_t>(m_possessionTeam)] += seconds;
    }
    if (m_data.playSeconds >= m_window.endPlaySeconds + MatchAnalytics::kPossessionWindowSeconds) {
        m_window.endPlaySeconds = m_data.playSeconds;
        m_data.possessionTimeline.push_back(m_window);
        m_window = PossessionWindow{m_data.playSeconds, {}};
    }
    CloseKickoffWindows();
}

void MatchAnalyticsTracker::CloseKickoffWindows() {
    if (!m_kickoffPossessionPending || m_data.kickoffs.empty()) return;
    auto& k = m_data.kickoffs.back();
    if (m_data.playSeconds - k.playSeconds < kKickoffPossessionSeconds) return;
    k.possessionTeam = m_possessionTeam;
    m_kickoffPossessionPending = false;
}

void MatchAnalyticsTracker::OnUpdateState(const TelemetryEvents::UpdateStateEvent& ev, double nowSeconds) {
    for (const auto& pj : ev.players) {
        if (pj.primaryId.empty()) continue;
        auto& p = PlayerFor(pj.primaryId);
        if (!pj.name.empty()) p.name = pj.name;
        if (pj.team != -1) p.team = pj.team;
        if (pj.shortcut) m_shortcutToId[*pj.shortcut] = pj.primaryId;
    }

    for (size_t t = 0; t < 2; ++t) {
        if (ev.teamScorePresent[t]) m_scoreboard[t] = ev.teamScores[t];
    }
    const std::optional<float> clock = ev.timeSeconds;
    // The clock only moves during live play, which also covers joining mid-round.
    if (clock && m_lastClock && *clock != *m_lastClock && !ev.bReplay && !m_paused) m_roundActive = true;
    if (clock) m_lastClock = clock;
    m_lastOvertime = ev.bOvertime;

    const double previous = m_lastSampleAt.value_or(nowSeconds);
    m_lastSampleAt = nowSeconds;
    const float dt = std::clamp(static_cast<float>(nowSeconds - previous), 0.0f, kMaxSampleSeconds);
    if (ev.bReplay || !m_roundActive || m_paused || dt <= 0.0f) return;

    if (ev.hasBall) {
        if (ev.ballTeam == 0 || ev.ballTeam == 1) m_possessionTeam = ev.ballTeam;
        if (ev.hasBallSpeed) m_ballSpeedSamples.emplace_back(ev.ballSpeed, dt);
    }
    AddPlayTime(dt);

    for (const auto& pj : ev.players) {
        // Car state is sent only for players on your team (opponents only during replays). Zero and
        // false values are omitted, so a missing field inside a car-state entry means 0 or false.
        if (!pj.hasCarField && !pj.bDemolished) continue;
        if (pj.primaryId.empty()) continue;
        auto& p = PlayerFor(pj.primaryId);
        if (!p.car) p.car.emplace();
        auto& car = *p.car;
        auto& raw = m_rawCar[pj.primaryId];

        if (pj.bDemolished || !pj.bHasCar) {
            car.demolishedSeconds += dt;
            raw.respawning = true;
            raw.wasPowersliding = false;
            continue;
        }
        car.sampledSeconds += dt;

        raw.speedSamples.push_back({pj.speed, dt, pj.bSupersonic.value_or(false) ? 1 : 0});
        if (pj.bOnWall) {
            car.wallSeconds += dt;
        } else if (pj.bOnGround) {
            car.groundSeconds += dt;
        } else {
            car.airSeconds += dt;
        }
        if (pj.bPowersliding) {
            car.powerslideSeconds += dt;
            if (!raw.wasPowersliding) car.powerslideCount++;
        }
        raw.wasPowersliding = pj.bPowersliding;
        if (pj.bBoosting) car.boostingSeconds += dt;

        const int amount = std::clamp(pj.boost.value_or(0), 0, 100);
        car.boostIntegral += static_cast<float>(amount) * dt;
        if (amount <= 0) car.zeroBoostSeconds += dt;
        if (amount >= 100) car.fullBoostSeconds += dt;
        car.boostBandSeconds[static_cast<size_t>(std::min(3, amount / 25))] += dt;
        if (raw.lastBoost) {
            const int delta = amount - *raw.lastBoost;
            if (raw.respawning) {
                // Respawn grants a fixed amount that is neither collected nor used.
                if (delta != 0) raw.respawning = false;
            } else if (delta > 0) {
                car.boostCollected += delta;
                if (!car.padsFromPickupEvents) {
                    if (delta > kSmallPadBoost) {
                        car.bigPads++;
                    } else {
                        car.smallPads++;
                    }
                }
            } else if (delta < 0) {
                car.boostUsed -= delta;
            }
        }
        raw.lastBoost = amount;
    }
}

void MatchAnalyticsTracker::OnCountdownBegin() {
    m_roundActive = false;
    m_awaitingFirstTouch = false;
    for (auto& [id, raw] : m_rawCar) {
        raw.lastBoost.reset();
        raw.respawning = false;
        raw.wasPowersliding = false;
    }
}

void MatchAnalyticsTracker::OnRoundStarted(double nowSeconds) {
    m_roundActive = !m_paused;
    m_lastSampleAt = nowSeconds;
    m_possessionTeam = -1;
    // Catches up on goals missed before we connected; a lagging scoreboard must not undo observed goals.
    for (size_t t = 0; t < 2; ++t)
        m_score[t] = std::max(m_score[t], m_scoreboard[t]);
    m_awaitingFirstTouch = true;
    m_kickoffPossessionPending = true;
    KickoffRecord k;
    k.playSeconds = m_data.playSeconds;
    m_data.kickoffs.push_back(std::move(k));
}

void MatchAnalyticsTracker::OnBallHit(const TelemetryEvents::BallHitEvent& ev, double nowSeconds) {
    if (ev.hitters.empty()) return;
    const float speed = ev.postHitSpeed;
    const std::optional<FieldPoint>& location = ev.location;
    if (!m_paused) m_roundActive = true;

    const bool first = m_awaitingFirstTouch;
    m_awaitingFirstTouch = false;
    for (const auto& h : ev.hitters) {
        const std::string id = ResolvePlayer(h);
        if (id.empty()) continue;
        auto& p = PlayerFor(id);
        if (p.name.empty()) p.name = h.name;
        const int team = h.team != -1 ? h.team : p.team;
        if (p.team == -1) p.team = team;
        if (team == 0 || team == 1) m_possessionTeam = team;
        // BallHit fires on every physics frame a car stays in contact with the ball; the game's Touches
        // stat counts one per contact. Frames of one contact arrive back to back, so each hit chains to
        // the previous one when it arrives within kTouchMergeSeconds.
        auto& last = m_lastHit[id];
        if (last.counted && nowSeconds - last.at <= kTouchMergeSeconds) {
            last.at = nowSeconds;
            if (speed > last.speed) {
                p.hitSpeedSum += speed - last.speed;
                if (last.speed < kHardHitKph && speed >= kHardHitKph) p.hardHits++;
                last.speed = speed;
            }
            p.maxHitSpeed = std::max(p.maxHitSpeed, speed);
            continue;
        }
        last = {nowSeconds, speed, true};
        p.hits++;
        p.hitSpeedSum += speed;
        p.maxHitSpeed = std::max(p.maxHitSpeed, speed);
        if (speed >= kHardHitKph) p.hardHits++;
        if (location) {
            if (location->z >= kAerialHitZ) p.aerialHits++;
            if (location->z >= kCrossbarZ) p.highAerialHits++;
            if (team == 0 || team == 1) {
                const float forward = team == 0 ? location->y : -location->y;
                p.hitsByThird[forward < -kFieldThirdY ? 0 : (forward > kFieldThirdY ? 2 : 1)]++;
            }
            if (m_data.hits.size() < MatchAnalytics::kMaxHits)
                m_data.hits.push_back({m_data.playSeconds, id, team, *location, speed});
        }
        if (first) {
            p.firstTouches++;
            if (!m_data.kickoffs.empty() && m_data.kickoffs.back().firstTouchId.empty()) {
                m_data.kickoffs.back().firstTouchId = id;
                m_data.kickoffs.back().firstTouchTeam = team;
            }
        }
    }
    m_data.maxBallSpeed = std::max(m_data.maxBallSpeed, speed);
}

void MatchAnalyticsTracker::OnBoostPickup(const TelemetryEvents::BoostPickupEvent& ev) {
    if (ev.bReplay) return;
    const std::string id = ResolvePlayer(ev.player);
    if (id.empty()) return;
    auto& p = PlayerFor(id);
    if (!p.car) p.car.emplace();
    auto& car = *p.car;
    if (!car.padsFromPickupEvents) {
        car.padsFromPickupEvents = true;
        car.bigPads = car.smallPads = 0;
    }
    const bool big = ev.boostType == "BoostType_Pill";
    (big ? car.bigPads : car.smallPads)++;
    const int team = ev.player.team != -1 ? ev.player.team : p.team;
    if (ev.location && (team == 0 || team == 1)) {
        const float forward = team == 0 ? ev.location->y : -ev.location->y;
        if (forward > kMidfieldPadY) (big ? car.stolenBigPads : car.stolenSmallPads)++;
    }
}

void MatchAnalyticsTracker::OnGoalScored(const TelemetryEvents::GoalScoredEvent& ev) {
    // Online, the live event's ImpactLocation is often the previous goal's (or a placeholder), but the
    // game always re-sends GoalScored with an empty scorer when the replay ends, even when it is skipped
    // instantly, and that repeat carries this goal's location. It must lie behind the goal line the
    // scoring team attacks (team 0 attacks +Y).
    if (ev.isEcho) {
        const auto& location = ev.impactLocation;
        if (!location || m_data.goals.empty() || m_data.goals.back().location) return;
        auto& goal = m_data.goals.back();
        const float attackY = goal.team == 0 ? location->y : (goal.team == 1 ? -location->y : 0.0f);
        if (attackY >= kGoalLineY) goal.location = location;
        return;
    }
    m_roundActive = false;
    m_awaitingFirstTouch = false;
    GoalRecord g;
    g.playSeconds = m_data.playSeconds;
    g.clockSeconds = m_lastClock;
    g.overtime = m_lastOvertime;
    if (ev.hasScorer) {
        g.scorerId = ResolvePlayer(ev.scorer);
        g.scorerName = ev.scorer.name;
        g.team = ev.scorer.team;
        if (g.team == -1) {
            if (const auto* p = m_data.FindPlayer(g.scorerId)) g.team = p->team;
        }
    }
    if (ev.assister) {
        g.assisterId = ResolvePlayer(*ev.assister);
        g.assisterName = ev.assister->name;
    }
    g.goalSpeed = Positive(ev.goalSpeed);
    g.lastTouchSpeed = Positive(ev.lastTouchSpeed);
    g.roundSeconds = Positive(ev.roundSeconds);
    if (g.team == 0 || g.team == 1) {
        m_score[static_cast<size_t>(g.team)]++;
        if (!m_data.kickoffs.empty()) {
            auto& k = m_data.kickoffs.back();
            if (k.goalTeam == -1 && m_data.playSeconds - k.playSeconds <= kKickoffGoalSeconds) k.goalTeam = g.team;
        }
    }
    g.scoreAfter = m_score;
    m_data.goals.push_back(std::move(g));
}

void MatchAnalyticsTracker::OnStatfeed(const TelemetryEvents::StatfeedEvent& ev) {
    if (ev.eventName.empty()) return;
    const std::string mainId = ResolvePlayer(ev.mainTarget);
    if (mainId.empty()) return;
    auto& p = PlayerFor(mainId);
    p.statEvents[ev.eventName]++;
    if (ev.eventName == "Demolish") {
        p.demosInflicted++;
        if (ev.secondaryTarget) {
            const std::string victimId = ResolvePlayer(*ev.secondaryTarget);
            if (!victimId.empty()) PlayerFor(victimId).demosTaken++;
        }
    }
}

void MatchAnalyticsTracker::OnPlayerLeft(const TelemetryEvents::PlayerLeftEvent& ev) {
    if (ev.primaryId.empty() && ev.name.empty()) return;
    PlayerDeparture d;
    d.primaryId = ev.primaryId;
    d.name = ev.name;
    if (const auto* p = m_data.FindPlayer(d.primaryId)) {
        d.team = p->team;
        if (d.name.empty()) d.name = p->name;
    }
    d.playSeconds = m_data.playSeconds;
    d.clockSeconds = m_lastClock;
    m_data.departures.push_back(std::move(d));
}

void MatchAnalyticsTracker::OnPaused(double nowSeconds) {
    if (m_paused) return;
    m_paused = true;
    m_pausedAt = nowSeconds;
    m_data.pauses++;
}

void MatchAnalyticsTracker::OnUnpaused(double nowSeconds) {
    if (!m_paused) return;
    m_paused = false;
    if (m_pausedAt) m_data.pausedSeconds += static_cast<float>(std::max(0.0, nowSeconds - *m_pausedAt));
    m_pausedAt.reset();
    m_lastSampleAt = nowSeconds;
}

void MatchAnalyticsTracker::OnMatchEnded() {
    m_roundActive = false;
    m_data.endClockSeconds = m_lastClock;
    m_data.endedInOvertime = m_lastOvertime;
}

MatchAnalytics MatchAnalyticsTracker::Snapshot() const {
    MatchAnalytics out = m_data;
    if (m_window.seconds[0] > 0.0f || m_window.seconds[1] > 0.0f) {
        PossessionWindow partial = m_window;
        partial.endPlaySeconds = out.playSeconds;
        out.possessionTimeline.push_back(partial);
    }

    float ballRawMax = 0.0f;
    for (const auto& [speed, seconds] : m_ballSpeedSamples)
        ballRawMax = std::max(ballRawMax, speed);
    const bool ballUu = ballRawMax > kBallRawUuThreshold;
    out.ballSpeedIntegral = 0.0f;
    for (const auto& [speed, seconds] : m_ballSpeedSamples)
        out.ballSpeedIntegral += SpeedKph(speed, ballUu) * seconds;

    for (auto& p : out.players) {
        if (!p.car) continue;
        const auto rawIt = m_rawCar.find(p.primaryId);
        if (rawIt == m_rawCar.end()) continue;
        const auto& samples = rawIt->second.speedSamples;
        float rawMax = 0.0f;
        for (const auto& s : samples)
            rawMax = std::max(rawMax, s.raw);
        const bool uu = rawMax > kCarRawUuThreshold;
        auto& car = *p.car;
        for (const auto& s : samples) {
            const float kph = SpeedKph(s.raw, uu);
            car.distance += kph / kUuToKph * s.seconds;
            car.maxSpeed = std::max(car.maxSpeed, kph);
            const bool supersonic = s.supersonic == -1 ? kph >= kSupersonicKph : s.supersonic == 1;
            if (supersonic) {
                car.supersonicSeconds += s.seconds;
            } else if (kph >= kBoostSpeedKph) {
                car.boostSpeedSeconds += s.seconds;
            } else {
                car.slowSeconds += s.seconds;
            }
        }
    }
    return out;
}
