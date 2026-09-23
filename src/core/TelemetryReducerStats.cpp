#include "TelemetryReducer.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>
#include "core/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

void TelemetryReducer::HandleStatFeed(const nlohmann::json& data) {
    if (m_state->game.inReplay) return;
    if (!data.contains("EventName") || !data["EventName"].is_string()) return;
    std::string feedEvent = data["EventName"].get<std::string>();
    std::string mainName = "", mainId = "", secName = "", secId = "";

    if (data.contains("MainTarget") && data["MainTarget"].is_object()) {
        auto mt = data["MainTarget"];
        if (mt.contains("Name") && mt["Name"].is_string()) mainName = mt["Name"].get<std::string>();
        if (mt.contains("PrimaryId") && mt["PrimaryId"].is_string()) mainId = mt["PrimaryId"].get<std::string>();
    } else if (data.contains("Player") && data["Player"].is_object()) {
        auto p = data["Player"];
        if (p.contains("Name") && p["Name"].is_string()) mainName = p["Name"].get<std::string>();
        if (p.contains("PrimaryId") && p["PrimaryId"].is_string()) mainId = p["PrimaryId"].get<std::string>();
    }
    if (mainId == "Unknown" || mainId.rfind("Unknown|", 0) == 0) mainId = "Unknown|" + mainName;
    if (data.contains("SecondaryTarget") && data["SecondaryTarget"].is_object()) {
        auto st = data["SecondaryTarget"];
        if (st.contains("Name") && st["Name"].is_string()) secName = st["Name"].get<std::string>();
        if (st.contains("PrimaryId") && st["PrimaryId"].is_string()) secId = st["PrimaryId"].get<std::string>();
    }
    if (secId == "Unknown" || secId.rfind("Unknown|", 0) == 0) secId = "Unknown|" + secName;

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
    m_roundActive = false;
    m_state->game.currentMatch.goals++;
    std::string scorerName = "", scorerId = "";
    nlohmann::json scorer;
    bool hasScorer = false;
    if (data.contains("Scorer") && data["Scorer"].is_object()) {
        scorer = data["Scorer"];
        if (scorer.contains("Name") && scorer["Name"].is_string()) scorerName = scorer["Name"].get<std::string>();
        if (scorer.contains("PrimaryId") && scorer["PrimaryId"].is_string()) scorerId = scorer["PrimaryId"].get<std::string>();
        hasScorer = true;
    }
    if (scorerId == "Unknown" || scorerId.rfind("Unknown|", 0) == 0) scorerId = "Unknown|" + scorerName;
    bool isScorerSelf = !scorerId.empty() ? IsSelfById(scorerId) : IsSelf(scorerName);
    if (hasScorer) {
        if (isScorerSelf) m_state->game.currentMatch.goalsSelf++;
        if (!scorerId.empty() && m_state->game.roster.count(scorerId))
            m_state->game.roster[scorerId].goals++;
    }
    if (data.contains("GoalSpeed") && data["GoalSpeed"].is_number()) {
        float currentSpeed = data["GoalSpeed"].get<float>();
        m_state->game.currentMatch.maxGoalSpeed = std::max(m_state->game.currentMatch.maxGoalSpeed, currentSpeed);
        if (hasScorer && isScorerSelf)
            m_state->game.currentMatch.maxGoalSpeedSelf = std::max(m_state->game.currentMatch.maxGoalSpeedSelf, currentSpeed);
        if (!scorerId.empty() && m_state->game.roster.count(scorerId))
            m_state->game.roster[scorerId].maxGoalSpeed = std::max(m_state->game.roster[scorerId].maxGoalSpeed, currentSpeed);
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
