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

std::string RmlUiController::RenderLiveMatchStats() {
    const auto& s = m_snap.currentMatch;
    struct Row {
        const char* label;
        const char* key;
        std::string value;
    };
    std::vector<Row> play = {
        {"Saves", "match-saves", Format::PairCount(s.saves, s.savesSelf)},
        {"Shots", "match-shots", Format::PairCount(s.shots, s.shotsSelf)},
        {"Assists", "match-assists", Format::PairCount(s.assists, s.assistsSelf)},
        {"Demos", "match-demos", Format::PairCount(s.demos, s.demosSelf)},
        {"Crossbars", "match-crossbars", Format::PairCount(s.crossbars, s.crossbarsSelf)}};
    if (s.demoedSelf > 0) play.insert(play.end() - 1, {"Demoed", "match-demoed", std::to_string(s.demoedSelf)});
    std::vector<Row> fun = {
        {"Max goal speed", "match-max-goal-speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Max ball speed", "match-max-ball-speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Hardest crossbar", "match-hardest-crossbar", m_config.crossbar_display_mode == "speed" ? Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units) : Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", false)},
        {"Fastest goal", "match-fastest-goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)}};
    if (s.ownGoals > 0) fun.push_back({"Own goals", "match-own-goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});

    auto list = [](const auto& rows) {
        std::ostringstream html;
        html << "<div class='metric-list'>";
        for (const auto& row : rows) {
            html << "<div class='metric-row'><div class='metric-label'>" << row.label
                 << "</div><div class='metric-value mono live-value' data-live-value='" << row.key << "'>" << row.value << "</div></div>";
        }
        html << "</div>";
        return html.str();
    };
    return "<div class='stat-section-title'>PLAY</div>" + list(play) +
           "<div class='stat-section-title' style='margin-top:7dp'>FUN</div>" + list(fun);
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
        struct Row {
            const char* name;
            const char* key;
            std::string value;
        };
        std::vector<Row> rows;
        if (m_config.show_session_record) rows.push_back({"Record", "session-record", FormatRecord(stats.wins, stats.losses)});
        if (m_config.show_session_goals) rows.push_back({"Goals", "session-goals", std::to_string(stats.goals)});
        if (m_config.show_session_saves) rows.push_back({"Saves", "session-saves", std::to_string(stats.saves)});
        if (m_config.show_session_assists) rows.push_back({"Assists", "session-assists", std::to_string(stats.assists)});
        if (m_config.show_session_demos) rows.push_back({"Demos", "session-demos", std::to_string(stats.demos)});
        if (m_config.show_session_boost) rows.push_back({"Boost", "session-boost", std::to_string(CalculateSessionBoostPickedUp(m_snap.sessionTotals, m_snap.currentMatch, m_snap.matchFinalized))});
        if (m_config.show_session_goal_participation) rows.push_back({"Goal participation", "session-goal-participation", stats.teamGoals > 0 ? FormatNumber(gp, 0) + "%" : "--"});
        if (m_config.show_session_mmr_change) rows.push_back({"MMR", "session-mmr", (mmr >= 0 ? "+" : "") + std::to_string(mmr)});
        if (includeStreak && m_config.show_streaks_stats) rows.push_back({"Session", "session-net", std::to_string(stats.wins - stats.losses)});

        std::ostringstream out;
        out << "<div class='metric-list'>";
        for (const auto& row : rows) {
            std::string cls;
            if (std::string_view(row.name) == "MMR") cls = mmr > 0 ? " win" : mmr < 0 ? " loss"
                                                                                      : "";
            out << "<div class='metric-row'><div class='metric-label'>" << row.name
                << "</div><div class='metric-value mono live-value" << cls << "' data-live-value='" << row.key << "'>" << row.value << "</div></div>";
        }
        out << "</div>";
        return out.str();
    }

    auto renderRows = [](const char* title, const std::vector<std::pair<std::string, std::string>>& rows) {
        std::ostringstream html;
        html << "<div class='stat-section-title' style='margin-top:7dp'>" << title << "</div><div class='stat-grid cols-2 compact-stats'>";
        for (const auto& [name, value] : rows) {
            html << "<div class='stat-cell'><div class='label'>" << name << "</div><div class='value mono'>" << value << "</div></div>";
        }
        html << "</div>";
        return html.str();
    };

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
        std::string cls;
        if (name == "MMR change") cls = mmr > 0 ? " win" : mmr < 0 ? " loss"
                                                                   : "";
        out << "<div class='stat-cell'><div class='label'>" << name << "</div><div class='value mono" << cls << "'>" << value << "</div></div>";
    }
    out << "</div><div class='setting-help' style='margin-top:8dp'>Left = lobby total · Right = you</div>";
    out << renderRows("PLAY", play) << renderRows("FUN", fun);
    return out.str();
}

std::string RmlUiController::RenderDemoTracker() {
    const auto session = CalculateSessionDemolitionCounts(m_snap.sessionTotals, m_snap.currentMatch, m_snap.matchFinalized);
    const char* gameKdClass = DemoKdClass(m_snap.currentMatch.demosSelf, m_snap.currentMatch.demoedSelf);
    const char* sessionKdClass = DemoKdClass(session.demos, session.demoed);
    std::ostringstream out;
    out << "<div class='metric-pair'>"
        << "<div class='mini-metric'><div class='label'>GAME K/D</div><div class='value mono'><span class='live-value' data-live-value='demo-game-count'>" << m_snap.currentMatch.demosSelf << '-' << m_snap.currentMatch.demoedSelf
        << "</span> <span class='demo-kd live-value " << gameKdClass << "' style='margin-left:6dp' data-live-value='demo-game-kd'>" << FormatDemoKd(m_snap.currentMatch.demosSelf, m_snap.currentMatch.demoedSelf) << "</span></div></div>"
        << "<div class='mini-metric'><div class='label'>SESSION K/D</div><div class='value mono'><span class='live-value' data-live-value='demo-session-count'>" << session.demos << '-' << session.demoed
        << "</span> <span class='demo-kd live-value " << sessionKdClass << "' style='margin-left:6dp' data-live-value='demo-session-kd'>" << FormatDemoKd(session.demos, session.demoed) << "</span></div></div></div>";
    return out.str();
}
