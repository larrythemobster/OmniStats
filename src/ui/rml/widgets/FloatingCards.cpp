#include "ui/rml/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

// The session view and post-match summary live outside overlay_layout, so they
// carry their own persisted position. An unset position keeps the legacy
// centered placement, and they are only draggable while the overlay accepts
// mouse input (Settings open); in game the overlay stays click-through.
std::string RmlUiController::FloatingCardClass() const {
    return WantsInteraction() ? " floating-card floating-card-movable" : " floating-card";
}

std::string RmlUiController::FloatingCardStyle(float x, float y, float widthDp) const {
    std::ostringstream style;
    style << " style='width:" << widthDp << "dp;";
    if (x >= 0.0f && y >= 0.0f) {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        style << "left:" << x / std::max(rmlScale, 0.01f) << "dp;top:" << y / std::max(rmlScale, 0.01f) << "dp;margin-left:0;";
    } else {
        style << "margin-left:" << -widthDp * 0.5f << "dp;";
    }
    style << "'";
    return style.str();
}

std::string RmlUiController::RenderMatchSummary() {
    const int myTeam = m_snap.matchSummaryMyTeam;
    const int score0 = m_snap.matchSummaryScore[0];
    const int score1 = m_snap.matchSummaryScore[1];
    int winner = m_snap.matchSummaryWinnerTeam;
    if (winner != 0 && winner != 1) {
        winner = score0 > score1 ? 0 : score1 > score0 ? 1
                                                       : -1;
    }
    std::string result = "DRAW";
    std::string resultClass = "muted";
    if (m_snap.lastMatchWasVoid) {
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

    const auto& s = m_snap.currentMatch;
    std::vector<std::pair<std::string, std::string>> play;
    if (s.saves > 0) play.push_back({"Saves", Format::PairCount(s.saves, s.savesSelf)});
    if (s.shots > 0) play.push_back({"Shots", Format::PairCount(s.shots, s.shotsSelf)});
    if (s.assists > 0) play.push_back({"Assists", Format::PairCount(s.assists, s.assistsSelf)});
    if (s.demos > 0) play.push_back({"Demos", Format::PairCount(s.demos, s.demosSelf)});
    if (s.demoedSelf > 0) play.push_back({"Demoed", std::to_string(s.demoedSelf)});
    if (s.crossbars > 0) play.push_back({"Crossbars", Format::PairCount(s.crossbars, s.crossbarsSelf)});
    if (s.boostPickedUp > 0) play.push_back({"Boost", Format::PairCount(s.boostPickedUp, s.boostPickedUpSelf)});

    std::vector<std::pair<std::string, std::string>> fun;
    if (s.maxGoalSpeed > 0) fun.push_back({"Max goal speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)});
    if (s.maxBallSpeed > 0) fun.push_back({"Max ball speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", m_config.imperial_units)});
    if (s.maxImpactForce > 0) {
        fun.push_back({"Hardest crossbar hit", m_config.crossbar_display_mode == "speed"
                                                   ? Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units)
                                                   : Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", false)});
    }
    if (s.fastestGoalTime > 0) fun.push_back({"Fastest goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)});
    if (s.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});

    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClass() << "' data-action='floating-card-drag' data-card='match-summary'"
        << FloatingCardStyle(m_config.match_summary_x, m_config.match_summary_y, 420.0f)
        << "><div class='row'><div class='grow value " << resultClass << "' style='font-size:20dp'>" << result << "</div>"
        << "<div class='value mono' style='font-size:20dp'>" << myScore << '-' << theirScore << "</div></div>";
    out << StatGrid("PLAY", play) << StatGrid("FUN", fun) << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSessionView() {
    const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const bool graph = m_state && m_state->ui.showGraphView.load();
    const bool lifetime = graph && m_snap.showLifetimeGraph;
    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClass() << "' data-action='floating-card-drag' data-card='session-view'"
        << FloatingCardStyle(m_config.session_view_x, m_config.session_view_y, 450.0f)
        << "><div class='row'><div class='card-title grow'>"
        << (lifetime ? "LIFETIME MMR · " : graph ? "SESSION MMR · "
                                                 : "SESSION · ")
        << Escape(MmrLabel(category)) << "</div><span class='badge'>F7 view · F6 playlist</span></div>";
    out << (graph ? RenderMmrGraph(false) : RenderSessionStats(true, true));
    out << "</div>";
    return out.str();
}
