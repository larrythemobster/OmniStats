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

namespace {
    // Maps a live-model tone variable (-1, 0, +1) onto the loss/muted/win classes.
    std::string ToneClasses(std::string_view toneVariable, bool mutedWhenNeutral) {
        std::string out = " data-class-win='" + std::string(toneVariable) + " == 1' data-class-loss='" + std::string(toneVariable) + " == -1'";
        if (mutedWhenNeutral) out += " data-class-muted='" + std::string(toneVariable) + " == 0'";
        return out;
    }

    // A metric row whose value is the live-model variable `variable`.
    std::string BoundMetricRow(std::string_view label, std::string_view variable, std::string_view toneVariable = {}) {
        std::string out = "<div class='metric-row'><div class='metric-label'>" + Escape(label) + "</div><div class='metric-value mono'";
        if (!toneVariable.empty()) out += ToneClasses(toneVariable, false);
        out += ">{{" + std::string(variable) + "}}</div></div>";
        return out;
    }

    std::string BoundKdMetric(const char* label, const char* countVariable, const char* kdVariable, const char* toneVariable) {
        return std::string("<div class='mini-metric'><div class='label'>") + label + "</div><div class='value mono'><span>{{" + countVariable +
               "}}</span> <span class='demo-kd' style='margin-left:6dp'" + ToneClasses(toneVariable, true) + ">{{" + kdVariable + "}}</span></div></div>";
    }
}

std::string RmlUiController::RenderLiveMatchStats() {
    const auto& s = m_snap.currentMatch;
    std::string play = "<div class='metric-list'>" + BoundMetricRow("Saves", "match_saves") + BoundMetricRow("Shots", "match_shots") +
                       BoundMetricRow("Assists", "match_assists") + BoundMetricRow("Demos", "match_demos");
    if (s.demoedSelf > 0) play += BoundMetricRow("Demoed", "match_demoed");
    play += BoundMetricRow("Crossbars", "match_crossbars") + "</div>";

    std::string fun = "<div class='metric-list'>" + BoundMetricRow("Max goal speed", "match_max_goal_speed") +
                      BoundMetricRow("Max ball speed", "match_max_ball_speed") + BoundMetricRow("Hardest crossbar", "match_hardest_crossbar") +
                      BoundMetricRow("Fastest goal", "match_fastest_goal");
    if (s.ownGoals > 0) fun += BoundMetricRow("Own goals", "match_own_goals");
    fun += "</div>";

    return "<div class='stat-section-title'>PLAY</div>" + play +
           "<div class='stat-section-title' style='margin-top:7dp'>FUN</div>" + fun;
}

std::string RmlUiController::RenderStreaksStats() {
    int cw = 0, cl = 0, lw = 0, ll = 0;
    if (m_state) {
        std::lock_guard lock(m_state->ui.dbStatsMutex);
        cw = m_state->ui.cachedDbStats.currentWins;
        cl = m_state->ui.cachedDbStats.currentLosses;
        lw = m_state->ui.cachedDbStats.longestWins;
        ll = m_state->ui.cachedDbStats.longestLosses;
    }
    std::ostringstream out;
    out << "<div class='metric-list'><div class='metric-row'><div class='metric-label'>Current streak</div><div class='metric-value "
        << (cw ? "win" : cl ? "loss"
                            : "")
        << "'>";
    if (cw)
        out << '+' << cw;
    else if (cl)
        out << '-' << cl;
    else
        out << "0";
    out << "</div></div><div class='metric-row'><div class='metric-label'>Longest win</div><div class='metric-value'>" << lw << "</div></div>";
    if (m_config.show_longest_loss_streak) {
        out << "<div class='metric-row'><div class='metric-label'>Longest loss</div><div class='metric-value loss'>" << ll << "</div></div>";
    }
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderGamemodeBreakdown(GamemodeBreakdownScope scope) {
    std::ostringstream out;
    const std::array<std::pair<const char*, bool>, 3> modes{{{"1v1", m_config.show_gamemode_record_1v1}, {"2v2", m_config.show_gamemode_record_2v2}, {"3v3", m_config.show_gamemode_record_3v3}}};
    bool any = false;
    out << "<div class='col gap-xs gamemode-breakdown'>";
    for (const auto& [mode, enabled] : modes) {
        if (!enabled) continue;
        any = true;
        int wins = 0, losses = 0, total = 0;
        if (scope == GamemodeBreakdownScope::CurrentSession) {
            if (auto it = m_snap.sessionGamemodes.find(mode); it != m_snap.sessionGamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        } else if (m_state) {
            std::lock_guard lock(m_state->ui.dbStatsMutex);
            if (auto it = m_state->ui.cachedDbStats.gamemodes.find(mode); it != m_state->ui.cachedDbStats.gamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        }
        const float pct = total > 0 ? (100.0f * wins / static_cast<float>(total)) : 0.0f;
        out << "<div class='row'><div class='grow value'>" << mode << "</div><span class='mono'>" << wins << '-' << losses << "</span><span class='label' style='width:70dp;text-align:right'>";
        if (total)
            out << FormatNumber(pct, 1) << "%";
        else
            out << '-';
        out << "</span><span class='label' style='width:45dp;text-align:right'>" << total << "</span></div>";
    }
    if (!any) out << "<div class='muted'>No gamemodes selected.</div>";
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSessionStats(bool compact, bool includeStreak) {
    const auto& stats = m_snap.sessionTotals;
    const int mmr = static_cast<int>(std::lround(stats.totalMmrChange));
    const float gp = stats.teamGoals > 0 ? 100.0f * static_cast<float>(stats.goalParticipations) / static_cast<float>(stats.teamGoals) : 0.0f;

    if (compact) {
        std::string out = "<div class='metric-list'>";
        if (m_config.show_session_record) out += BoundMetricRow("Record", "session_record");
        if (m_config.show_session_goals) out += BoundMetricRow("Goals", "session_goals");
        if (m_config.show_session_saves) out += BoundMetricRow("Saves", "session_saves");
        if (m_config.show_session_assists) out += BoundMetricRow("Assists", "session_assists");
        if (m_config.show_session_demos) out += BoundMetricRow("Demos", "session_demos");
        if (m_config.show_session_boost) out += BoundMetricRow("Boost", "session_boost");
        if (m_config.show_session_goal_participation) out += BoundMetricRow("Goal participation", "session_goal_participation");
        if (m_config.show_session_mmr_change) out += BoundMetricRow("MMR", "session_mmr", "session_mmr_tone");
        if (includeStreak && m_config.show_streaks_stats) out += BoundMetricRow("Session", "session_net");
        out += "</div>";
        return out;
    }

    std::vector<std::pair<std::string, std::string>> summary = {
        {"Record", FormatRecord(stats.wins, stats.losses)},
        {"Goals", std::to_string(stats.goals)},
        {"Goal participation", stats.teamGoals > 0 ? std::to_string(stats.goalParticipations) + " / " + std::to_string(stats.teamGoals) + " (" + FormatNumber(gp, 0) + "%)" : "--"},
        {"MMR change", (mmr >= 0 ? "+" : "") + std::to_string(mmr)}};
    if (includeStreak) summary.push_back({"Session", std::to_string(stats.wins - stats.losses)});

    std::vector<std::pair<std::string, std::string>> play = {
        {"Saves", Format::PairCount(stats.savesTotal, stats.saves)},
        {"Shots", Format::PairCount(stats.shotsTotal, stats.shots)},
        {"Assists", Format::PairCount(stats.assistsTotal, stats.assists)},
        {"Goal participation", stats.teamGoals > 0 ? std::to_string(stats.goalParticipations) + " / " + std::to_string(stats.teamGoals) + " (" + FormatNumber(gp, 0) + "%)" : "--"},
        {"Demos", Format::PairCount(stats.demosTotal, stats.demos)},
        {"Crossbars", Format::PairCount(stats.crossbarsTotal, stats.crossbars)}};

    std::vector<std::pair<std::string, std::string>> fun = {
        {"Max goal speed", Format::PairSpeed(stats.maxGoalSpeed, stats.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Max ball speed", Format::PairSpeed(stats.maxBallSpeed, stats.maxBallSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Hardest crossbar hit", m_config.crossbar_display_mode == "speed"
                                     ? Format::PairSpeed(stats.maxImpactForce * 0.036f, stats.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units)
                                     : Format::PairSpeed(stats.maxImpactForce, stats.maxImpactForceSelf, true, "", false)},
        {"Fastest goal", Format::PairFastest(stats.fastestGoalTime, stats.fastestGoalTimeSelf)}};
    if (stats.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(stats.ownGoals, stats.ownGoalsSelf)});

    std::ostringstream out;
    out << "<div class='stat-grid cols-2 compact-stats'>";
    for (const auto& [name, value] : summary) {
        const char* cls = name != "MMR change" ? "" : mmr > 0 ? "win"
                                                  : mmr < 0   ? "loss"
                                                              : "";
        out << StatCell(name, value, cls);
    }
    out << "</div><div class='setting-help' style='margin-top:8dp'>Left = lobby total · Right = you</div>";
    out << StatGrid("PLAY", play) << StatGrid("FUN", fun);
    return out.str();
}

std::string RmlUiController::RenderDemoTracker() {
    return "<div class='metric-pair'>" + BoundKdMetric("GAME K/D", "demo_game_count", "demo_game_kd", "demo_game_tone") +
           BoundKdMetric("SESSION K/D", "demo_session_count", "demo_session_kd", "demo_session_tone") + "</div>";
}
