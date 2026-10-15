#include "core/reducer/TelemetryReducer.hpp"
#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "utils/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <utility>
#include <cctype>
#include <optional>
#include "core/reducer/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

void TelemetryReducer::HandleStatFeed(const nlohmann::json& data) {
    if (m_state->game.inReplay) return;
    if (!data.contains("EventName") || !data["EventName"].is_string()) return;
    std::string feedEvent = data["EventName"].get<std::string>();
    std::string mainName = "", mainId = "", secName = "", secId = "";

    const nlohmann::json* mainTarget = nullptr;
    if (data.contains("MainTarget") && data["MainTarget"].is_object()) {
        mainTarget = &data["MainTarget"];
    } else if (data.contains("Player") && data["Player"].is_object()) {
        mainTarget = &data["Player"];
    }
    if (mainTarget) {
        if (mainTarget->contains("Name") && (*mainTarget)["Name"].is_string()) mainName = (*mainTarget)["Name"].get<std::string>();
        mainId = ResolveRosterPlayerIdLocked(*mainTarget);
    }
    if (data.contains("SecondaryTarget") && data["SecondaryTarget"].is_object()) {
        const auto& st = data["SecondaryTarget"];
        if (st.contains("Name") && st["Name"].is_string()) secName = st["Name"].get<std::string>();
        secId = ResolveRosterPlayerIdLocked(st);
    }

    bool isMainSelf = !mainId.empty() ? IsSelfById(mainId) : IsSelf(mainName);
    bool isSecSelf = !secId.empty() ? IsSelfById(secId) : IsSelf(secName);

    if (feedEvent == "Save" || feedEvent == "EpicSave" || feedEvent == "Epic Save") {
        m_state->game.currentMatch.saves++;
        if (isMainSelf) m_state->game.currentMatch.savesSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].saves++;
    } else if (feedEvent == "Shot") {
        m_state->game.currentMatch.shots++;
        if (isMainSelf) m_state->game.currentMatch.shotsSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].shots++;
    } else if (feedEvent == "Demolish") {
        m_state->game.currentMatch.demos++;
        if (isMainSelf) m_state->game.currentMatch.demosSelf++;
        if (isSecSelf) m_state->game.currentMatch.demoedSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].demos++;
        if (!secId.empty()) m_boostRespawnPending.insert(secId);
    } else if (feedEvent == "OwnGoal") {
        m_state->game.currentMatch.ownGoals++;
        if (isMainSelf) m_state->game.currentMatch.ownGoalsSelf++;
        std::cout << "[Event] OWN GOAL by " << PrivacyLog::Sensitive(mainName, "player name") << "!\n";
    } else if (feedEvent == "Assist") {
        m_state->game.currentMatch.assists++;
        if (isMainSelf) m_state->game.currentMatch.assistsSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].assists++;
    }
}

void TelemetryReducer::HandleGoalScored(const nlohmann::json& data, SideEffects& effects) {
    if (m_state->game.inReplay) return;
    const int64_t nowMs = SteadyNowMs();
    if (AcceptsUiMatchEventLocked(data, nowMs)) {
        m_state->ui.lastGoalMs.store(nowMs, std::memory_order_relaxed);
        m_countdownSeenThisRound = false;
    }
    // A goal tagged with a finalized, destroyed, or other active match is a late
    // duplicate; GUID-less goals (freeplay) are always current.
    const std::string goalGuid = ExtractEventMatchGuid(data);
    const bool staleGoal =
        m_nonLiveReplayActive ||
        (!goalGuid.empty() &&
         (m_finalizedMatchGuids.count(goalGuid) > 0 || m_pendingDestroyedMatches.count(goalGuid) > 0 ||
          (m_state->game.inMatch && !m_state->game.matchFinalized && !m_state->game.matchGuid.empty() &&
           goalGuid != m_state->game.matchGuid)));
    if (staleGoal) return;
    m_roundActive = false;
    m_state->game.currentMatch.goals++;
    std::string scorerName = "", scorerId = "";
    bool hasScorer = false;
    if (data.contains("Scorer") && data["Scorer"].is_object()) {
        const auto& scorer = data["Scorer"];
        if (scorer.contains("Name") && scorer["Name"].is_string()) scorerName = scorer["Name"].get<std::string>();
        scorerId = ResolveRosterPlayerIdLocked(scorer);
        hasScorer = true;
    }
    bool isScorerSelf = !scorerId.empty() ? IsSelfById(scorerId) : IsSelf(scorerName);
    if (hasScorer) {
        if (isScorerSelf) m_state->game.currentMatch.goalsSelf++;
        if (!scorerId.empty() && m_state->game.roster.count(scorerId))
            m_state->game.roster[scorerId].goals++;
    }
    if (data.contains("GoalSpeed") && data["GoalSpeed"].is_number()) {
        const float currentSpeed = data["GoalSpeed"].get<float>();
        if (std::isfinite(currentSpeed) && currentSpeed > 0.0f) {
            m_state->game.currentMatch.maxGoalSpeed = std::max(m_state->game.currentMatch.maxGoalSpeed, currentSpeed);
            if (hasScorer && isScorerSelf)
                m_state->game.currentMatch.maxGoalSpeedSelf = std::max(m_state->game.currentMatch.maxGoalSpeedSelf, currentSpeed);
            if (!scorerId.empty() && m_state->game.roster.count(scorerId))
                m_state->game.roster[scorerId].maxGoalSpeed = std::max(m_state->game.roster[scorerId].maxGoalSpeed, currentSpeed);
            m_state->game.lastGoalSpeed = currentSpeed;
            if (hasScorer && isScorerSelf) m_state->game.lastGoalSpeedSelf = currentSpeed;
        }
    }
    if (data.contains("GoalTime") && data["GoalTime"].is_number()) {
        float time = data["GoalTime"].get<float>();
        if (time > 0) {
            if (m_state->game.currentMatch.fastestGoalTime == 0.0f || time < m_state->game.currentMatch.fastestGoalTime)
                m_state->game.currentMatch.fastestGoalTime = time;
            if (hasScorer && isScorerSelf)
                if (m_state->game.currentMatch.fastestGoalTimeSelf == 0.0f || time < m_state->game.currentMatch.fastestGoalTimeSelf)
                    m_state->game.currentMatch.fastestGoalTimeSelf = time;
            if (!scorerId.empty() && m_state->game.roster.count(scorerId))
                if (m_state->game.roster[scorerId].fastestGoalTime == 0.0f || time < m_state->game.roster[scorerId].fastestGoalTime)
                    m_state->game.roster[scorerId].fastestGoalTime = time;
        }
    }
    std::cout << "[Event] GOAL SCORED!\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

void TelemetryReducer::HandleBallHit(const nlohmann::json& data) {
    if (data.contains("Ball") && data["Ball"].is_object()) {
        auto ball = data["Ball"];
        if (ball.contains("PostHitSpeed") && ball["PostHitSpeed"].is_number()) {
            float sp = ball["PostHitSpeed"].get<float>();
            m_state->game.currentMatch.maxBallSpeed = std::max(m_state->game.currentMatch.maxBallSpeed, sp);
            if (data.contains("Players") && data["Players"].is_array()) {
                for (const auto& h : data["Players"]) {
                    if (h.is_object()) {
                        std::string pid = (h.contains("PrimaryId") && h["PrimaryId"].is_string()) ? h["PrimaryId"].get<std::string>() : "";
                        std::string name = (h.contains("Name") && h["Name"].is_string()) ? h["Name"].get<std::string>() : "";
                        if (pid == "Unknown" || pid.rfind("Unknown|", 0) == 0) pid = "Unknown|" + name;
                        if (!pid.empty() ? IsSelfById(pid) : IsSelf(name)) {
                            m_state->game.currentMatch.maxBallSpeedSelf = std::max(m_state->game.currentMatch.maxBallSpeedSelf, sp);
                            break;
                        }
                    }
                }
            }
        }
    }
}

void TelemetryReducer::HandleCrossbarHit(const nlohmann::json& data) {
    m_state->game.currentMatch.crossbars++;
    std::string toucherName = "", toucherId = "";
    if (data.contains("BallLastTouch") && data["BallLastTouch"].is_object()) {
        auto lt = data["BallLastTouch"];
        if (lt.contains("Player") && lt["Player"].is_object()) {
            auto ltp = lt["Player"];
            if (ltp.contains("Name") && ltp["Name"].is_string()) toucherName = ltp["Name"].get<std::string>();
            if (ltp.contains("PrimaryId") && ltp["PrimaryId"].is_string()) toucherId = ltp["PrimaryId"].get<std::string>();
        }
    }
    if (toucherId == "Unknown" || toucherId.rfind("Unknown|", 0) == 0) toucherId = "Unknown|" + toucherName;
    bool isToucherSelf = !toucherId.empty() ? IsSelfById(toucherId) : IsSelf(toucherName);
    if (isToucherSelf) m_state->game.currentMatch.crossbarsSelf++;
    if (data.contains("ImpactForce") && data["ImpactForce"].is_number()) {
        float ifo = data["ImpactForce"].get<float>();
        m_state->game.currentMatch.maxImpactForce = std::max(m_state->game.currentMatch.maxImpactForce, ifo);
        if (isToucherSelf) m_state->game.currentMatch.maxImpactForceSelf = std::max(m_state->game.currentMatch.maxImpactForceSelf, ifo);
    }
    std::cout << "[Event] CROSSBAR HIT! Toucher: " << (toucherName.empty() ? "None" : PrivacyLog::Sensitive(toucherName, "player name")) << "\n";
}

void TelemetryReducer::RecordJournalEventLocked(const std::string& type,
                                                const nlohmann::json& data,
                                                const char* actorField,
                                                const char* secondaryField,
                                                const char* source) {
    // Keep UI/freeplay/replay events out of persistent match history.
    if (!m_state->game.inMatch || m_state->game.matchFinalized ||
        m_state->game.inReplay || m_nonLiveReplayActive ||
        m_state->game.fallbackNonRecordableContext ||
        PlaylistMetadata::IsNonRecordable(m_state->game.playlistId) ||
        m_matchEvents.size() >= 512) return;
    const std::string guid = ExtractEventMatchGuid(data);
    if (m_missingGuidAssociationBlockedByReconnect && guid.empty() &&
        m_state->game.matchGuid.empty()) return;
    if (!guid.empty() && !m_state->game.matchGuid.empty() && guid != m_state->game.matchGuid) return;
    if (!guid.empty() && (m_finalizedMatchGuids.count(guid) || m_pendingDestroyedMatches.count(guid))) return;

    MatchJournalEvent event;
    event.type = type;
    event.source = source;
    event.observedAtUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    auto readPlayer = [&](const nlohmann::json& player, std::string& id, std::string& name) {
        if (!player.is_object()) return;
        if (player.contains("Name") && player["Name"].is_string()) name = player["Name"].get<std::string>();
        id = ResolveRosterPlayerIdLocked(player);
        if (id.empty() && player.contains("PrimaryId") && player["PrimaryId"].is_string()) {
            const std::string candidate = player["PrimaryId"].get<std::string>();
            if (candidate != "Unknown" && candidate.rfind("Unknown|", 0) != 0) id = candidate;
        }
    };
    if (actorField && data.contains(actorField)) readPlayer(data[actorField], event.actorId, event.actorName);
    if (secondaryField && data.contains(secondaryField))
        readPlayer(data[secondaryField], event.secondaryId, event.secondaryName);
    if (type == "crossbar" && data.contains("BallLastTouch") && data["BallLastTouch"].is_object()) {
        const auto& touch = data["BallLastTouch"];
        if (touch.contains("Player")) readPlayer(touch["Player"], event.actorId, event.actorName);
    }
    const auto rosterIt = m_state->game.roster.find(event.actorId);
    if (rosterIt != m_state->game.roster.end() &&
        (rosterIt->second.team == 0 || rosterIt->second.team == 1))
        event.team = rosterIt->second.team;
    // Only explicitly named remaining-clock fields represent game time.
    for (const char* field : {"SecondsRemaining", "TimeRemaining"}) {
        if (data.contains(field) && data[field].is_number()) {
            const double seconds = data[field].get<double>();
            if (std::isfinite(seconds) && seconds >= 0.0 && seconds <= 3600.0) event.matchClockSeconds = seconds;
            break;
        }
    }
    if (!event.matchClockSeconds && m_hasObservedMatchClock &&
        Now() >= m_lastMatchClockSampleAt &&
        Now() - m_lastMatchClockSampleAt <= std::chrono::seconds(5))
        event.matchClockSeconds = static_cast<double>(m_state->game.timeSeconds);
    if (type == "goal" && data.contains("GoalSpeed") && data["GoalSpeed"].is_number()) {
        const double speed = data["GoalSpeed"].get<double>();
        if (std::isfinite(speed) && speed > 0) event.ballSpeed = speed;
    }
    if (type == "crossbar" && data.contains("ImpactForce") && data["ImpactForce"].is_number()) {
        const double impact = data["ImpactForce"].get<double>();
        if (std::isfinite(impact)) event.value = impact;
    }
    // Native unique IDs are authoritative; no generated sequence number is treated as one.
    for (const char* field : {"EventId", "EventID", "EventGuid"}) {
        if (data.contains(field) && (data[field].is_string() || data[field].is_number_integer())) {
            event.eventKey = source;
            event.eventKey += ":" + type + ":";
            event.eventKey += data[field].is_string() ? data[field].get<std::string>() : data[field].dump();
            break;
        }
    }
    if (!event.eventKey.empty()) {
        if (!m_eventNativeKeys.insert(event.eventKey).second) return;
    } else {
        // Suppress retransmission of the *same* payload in a short window,
        // without merging genuine repeated actions later in the match.
        const std::string signature = type + ":" + data.dump();
        const auto now = Now();
        if (signature == m_lastEventSignature && now >= m_lastEventAt &&
            now - m_lastEventAt < std::chrono::milliseconds(300)) return;
        m_lastEventSignature = signature;
        m_lastEventAt = now;
    }
    m_matchEvents.push_back(std::move(event));
}
