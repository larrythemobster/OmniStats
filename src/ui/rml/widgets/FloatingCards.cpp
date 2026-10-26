#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "network/telemetry/TelemetryManager.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

// The session view and post-match summary live outside overlay_layout, so they
// carry their own persisted position. An unset position keeps the legacy
// centered placement, and they are only draggable while the overlay accepts
// mouse input (Settings open); in game the overlay stays click-through.
namespace {
    std::string FloatingCardClassHelper(bool interactive) {
        return interactive ? " floating-card floating-card-movable" : " floating-card";
    }
    std::string FloatingCardStyleHelper(float x, float y, float widthDp, float dpiScale, float uiScale) {
        std::ostringstream style;
        style << " style='width:" << widthDp << "dp;";
        if (x >= 0.0f && y >= 0.0f) {
            const float rmlScale = SanitizedScale(dpiScale) * SanitizedUiScale(uiScale);
            style << "left:" << x / std::max(rmlScale, 0.01f) << "dp;top:" << y / std::max(rmlScale, 0.01f) << "dp;margin-left:0;";
        } else {
            style << "margin-left:" << -widthDp * 0.5f << "dp;";
        }
        style << "'";
        return style.str();
    }
}

std::string OverlayPresenter::RenderMatchSummary(const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state, bool interactive, float dpiScale) {
    const int myTeam = snap.matchSummaryMyTeam;
    const int score0 = snap.matchSummaryScore[0];
    const int score1 = snap.matchSummaryScore[1];
    int winner = snap.matchSummaryWinnerTeam;
    if (winner != 0 && winner != 1) {
        winner = score0 > score1 ? 0 : score1 > score0 ? 1
                                                       : -1;
    }
    std::string result = "DRAW";
    std::string resultClass = "muted";
    if (snap.lastMatchWasVoid) {
        result = "VOID";
    } else if ((myTeam == 0 || myTeam == 1) && winner != -1) {
        if (winner == myTeam) {
            result = "WIN";
            resultClass = "win";
        } else {
            result = "LOSS";
            resultClass = "loss";
        }
    }
    const int myScore = myTeam == 1 ? score1 : score0;
    const int theirScore = myTeam == 1 ? score0 : score1;

    const auto& s = snap.currentMatch;
    std::vector<std::pair<std::string, std::string>> play;
    if (s.saves > 0) play.push_back({"Saves", Format::PairCount(s.saves, s.savesSelf)});
    if (s.shots > 0) play.push_back({"Shots", Format::PairCount(s.shots, s.shotsSelf)});
    if (s.assists > 0) play.push_back({"Assists", Format::PairCount(s.assists, s.assistsSelf)});
    if (s.demos > 0) play.push_back({"Demos", Format::PairCount(s.demos, s.demosSelf)});
    if (s.demoedSelf > 0) play.push_back({"Demoed", std::to_string(s.demoedSelf)});
    if (s.crossbars > 0) play.push_back({"Crossbars", Format::PairCount(s.crossbars, s.crossbarsSelf)});
    if (s.boostPickedUp > 0) play.push_back({"Boost", Format::PairCount(s.boostPickedUp, s.boostPickedUpSelf)});

    std::vector<std::pair<std::string, std::string>> fun;
    if (s.maxGoalSpeed > 0) fun.push_back({"Max goal speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", config.imperial_units)});
    if (s.maxBallSpeed > 0) fun.push_back({"Max ball speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", config.imperial_units)});
    if (s.maxImpactForce > 0) {
        fun.push_back({"Hardest crossbar hit", config.crossbar_display_mode == "speed"
                                                   ? Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", config.imperial_units)
                                                   : Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", false)});
    }
    if (s.fastestGoalTime > 0) fun.push_back({"Fastest goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)});
    if (s.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});

    std::string otTag;
    if (snap.matchSummaryOvertimeSeconds > 0.0f) {
        int total = static_cast<int>(std::round(snap.matchSummaryOvertimeSeconds));
        if (total < 0) total = 0;
        int m = total / 60;
        int sec = total % 60;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "OT %d:%02d", m, sec);
        otTag = std::string(" <span class='muted' style='font-size:13dp;font-weight:normal'>\xC2\xB7 ") + buf + "</span>";
    }

    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClassHelper(interactive) << "' data-action='floating-card-drag' data-card='match-summary'"
        << FloatingCardStyleHelper(config.match_summary_x, config.match_summary_y, 420.0f, dpiScale, config.ui_scale)
        << "><div class='row'><div class='grow value " << resultClass << "' style='font-size:20dp'>" << result << otTag << "</div>"
        << "<div class='value mono' style='font-size:20dp'>" << myScore << '-' << theirScore << "</div></div>";
    out << StatGrid("PLAY", play) << StatGrid("FUN", fun);
    if (!snap.lastMatchWasVoid) {
        out << "<div class='row match-summary-actions'><button class='ghost compact' data-action='history-open' data-source='match-summary'";
        if (!snap.matchGuid.empty()) out << " data-match-guid='" << Escape(snap.matchGuid) << "'";
        out << ">Details</button>";
        if (state && !snap.matchGuid.empty()) {
            std::string bcUrl;
            {
                std::lock_guard lock(state->ui.replayUploadMutex);
                if (state->ui.lastBallchasingMatchGuid == snap.matchGuid) bcUrl = state->ui.lastBallchasingUrl;
            }
            if (bcUrl.rfind("https://ballchasing.com/", 0) == 0) {
                out << "<button class='ghost compact' data-action='open-ballchasing-url' data-url='"
                    << Escape(bcUrl) << "'>Ballchasing</button>";
            }
        }
        out << "</div>";
    }
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderMatchSummary() {
    return m_overlayPresenter->RenderMatchSummary(m_snap, m_config, m_state, WantsInteraction(), m_dpiScale);
}

std::string OverlayPresenter::RenderSessionView(const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state, bool interactive, float dpiScale) {
    const auto category = state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const bool graph = state && state->ui.showGraphView.load();
    const bool lifetime = graph && snap.showLifetimeGraph;
    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClassHelper(interactive) << "' data-action='floating-card-drag' data-card='session-view'"
        << FloatingCardStyleHelper(config.session_view_x, config.session_view_y, 450.0f, dpiScale, config.ui_scale)
        << "><div class='row session-view-head'><div class='card-title grow'>"
        << (lifetime ? "LIFETIME MMR · " : graph ? "SESSION MMR · "
                                                 : "SESSION · ")
        << Escape(MmrLabel(category)) << "</div>" << (graph ? RenderGraphPanControls(snap, state) : std::string{})
        << "<span class='badge'>F7 view · F6 playlist</span></div>";
    out << (graph ? RenderMmrGraph(false, false, snap, config, state) : RenderSessionStats(true, true, snap, config));
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSessionView() {
    if (m_telemetry && m_state && m_state->ui.showGraphView.load()) {
        m_telemetry->RecordMilestone(TelemetryManager::Milestone::MmrGraphView);
    }
    return m_overlayPresenter->RenderSessionView(m_snap, m_config, m_state, WantsInteraction(), m_dpiScale);
}
