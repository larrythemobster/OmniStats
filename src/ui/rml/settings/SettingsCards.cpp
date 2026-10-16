#include "ui/config/WidgetSettings.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "network/api/AccountClient.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
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

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string RmlUiController::RenderSettingsCards() {
    std::ostringstream out;
    const bool dashboard = m_config.second_monitor_mode;
    out << SectionStart(dashboard ? "Dashboard cards" : "Overlay cards")
        << ActionRow("Card positions", dashboard ? "Move and resize cards on the dashboard." : "Move, resize, and dock cards on screen.",
                     Button(dashboard ? "dashboard-edit" : "overlay-edit-toggle", "Position cards", "ghost"));
    std::vector<DashboardLayout::WidgetId> cards = {
        DashboardLayout::WidgetId::LiveRoster,
        DashboardLayout::WidgetId::LiveMatchStats, DashboardLayout::WidgetId::SessionStats,
        DashboardLayout::WidgetId::RankProgress, DashboardLayout::WidgetId::MmrGraph,
        DashboardLayout::WidgetId::PreviousGames, DashboardLayout::WidgetId::LobbyRanks,
        DashboardLayout::WidgetId::DemoTracker, DashboardLayout::WidgetId::StreaksStats,
        DashboardLayout::WidgetId::GamemodeBreakdown};
    if (!dashboard) cards.insert(cards.begin(), DashboardLayout::WidgetId::Tournaments);
    out << "<div class='row card-list-head'><span class='grow'>Card</span>";
    if (!dashboard) out << "<span class='card-head-rule'>Show when</span>";
    out << "<span class='card-head-toggle'>On</span></div>";
    for (auto widget : cards) {
        const bool tournament = widget == DashboardLayout::WidgetId::Tournaments;
        const bool enabled = WidgetSettings::Enabled(m_config, widget, dashboard);
        const bool disabled = tournament && !AccountClient::Instance().IsSignedIn();
        const std::string dom = WidgetDomId(widget);
        out << "<div class='display-card-setting'><div class='setting-row card-row'><div class='setting-info'><div class='setting-name'>"
            << Escape(tournament ? "Tournament countdown" : DashboardLayout::GetWidgetDisplayName(widget)) << "</div>";
        if (disabled) out << "<div class='setting-help'>Sign in under Accounts &amp; services to see tournaments.</div>";
        out << "</div>";
        const bool showRules = enabled && !disabled && !dashboard;
        const auto vis = OverlayLayout::GetWidgetVisibility(m_config.overlay_layout, widget);
        if (showRules) {
            out << "<div class='row card-rule'>"
                << SelectControl("widget_vis_mode:" + dom, OverlayVisibilityModeOptions(), OverlayLayout::ToConfigString(vis.mode)) << "</div>";
        }
        out << ToggleSwitch("widget_enabled:" + dom, enabled, disabled) << "</div>";
        if (showRules && vis.mode == OverlayLayout::Visibility::AfterEvent) {
            out << "<div class='row card-rule card-rule-sub'><span class='card-rule-label'>After</span>"
                << SelectControl("widget_vis_event:" + dom, OverlayVisibilityEventOptions(), OverlayLayout::ToConfigString(vis.event))
                << "<span class='card-rule-label'>for</span>"
                << SelectControl("widget_vis_seconds:" + dom, OverlayVisibilitySecondsOptions(), std::to_string(vis.seconds), "seconds-select")
                << "</div>";
        }
        if (showRules && m_showCardDetails) {
            const bool keyHeldOption = vis.mode == OverlayLayout::Visibility::AlwaysInMatch ||
                                       vis.mode == OverlayLayout::Visibility::MenusOnly ||
                                       vis.mode == OverlayLayout::Visibility::AfterEvent;
            out << "<div class='display-card-options'>";
            if (keyHeldOption) out << ToggleControl("widget_vis_key_held:" + dom, "Also show while key held", "", vis.alsoWhileKeyHeld);
            out << ToggleControl("widget_vis_hide_replay:" + dom, "Hide during goal replays", "", vis.hideDuringReplay)
                << "</div>";
        }
        out << "</div>";
    }
    out << SectionEnd()
        << "<div class='settings-disclosure'>"
        << Button("toggle-card-details", m_showCardDetails ? "Hide card options" : "Show card options", "ghost")
        << "</div>";
    if (!m_showCardDetails) return out.str();

    out << SectionStart("Session card")
        << ToggleControl("show_session_record", "Record", "", m_config.show_session_record)
        << ToggleControl("show_session_goals", "Goals", "", m_config.show_session_goals)
        << ToggleControl("show_session_saves", "Saves", "", m_config.show_session_saves)
        << ToggleControl("show_session_demos", "Demos", "", m_config.show_session_demos)
        << ToggleControl("show_session_boost", "Boost", "", m_config.show_session_boost)
        << ToggleControl("show_session_assists", "Assists", "", m_config.show_session_assists)
        << ToggleControl("show_session_goal_participation", "Goal participation", "", m_config.show_session_goal_participation)
        << ToggleControl("show_session_mmr_change", "MMR change", "", m_config.show_session_mmr_change)
        << SectionEnd();

    out << SectionStart("Rank labels")
        << ToggleControl("use_rank_icons", "Use rank & playlist icons", "", m_config.use_rank_icons)
        << ToggleControl("show_account_wins_overlay", "Account wins", "", m_config.show_account_wins_overlay)
        << SectionEnd();

    out << SectionStart("Lobby rank playlists")
        << ToggleControl("show_lobby_rank_1v1", "1v1", "", m_config.show_lobby_rank_1v1, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_2v2", "2v2", "", m_config.show_lobby_rank_2v2, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_3v3", "3v3", "", m_config.show_lobby_rank_3v3, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_casual", "Casual", "", m_config.show_lobby_rank_casual, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_tourny", "Tournament", "", m_config.show_lobby_rank_tourny, !m_config.show_lobby_ranks_overlay);
    if (m_config.show_extra_playlists) {
        out << ToggleControl("show_lobby_rank_hoops", "Hoops", "", m_config.show_lobby_rank_hoops, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_rumble", "Rumble", "", m_config.show_lobby_rank_rumble, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_dropshot", "Dropshot", "", m_config.show_lobby_rank_dropshot, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_snowday", "Snow Day", "", m_config.show_lobby_rank_snowday, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_heatseeker", "Heatseeker", "", m_config.show_lobby_rank_heatseeker, !m_config.show_lobby_ranks_overlay);
    }
    out << SectionEnd();

    std::vector<SelectOption> gameCounts;
    for (int count : {10, 20, 30, 40, 50})
        gameCounts.push_back({std::to_string(count), std::to_string(count)});
    out << SectionStart("Card details")
        << SelectRow("previous_games_limit", "Previous games to show", "", gameCounts, std::to_string(m_config.previous_games_limit), !m_config.show_previous_games_summary)
        << ToggleControl("show_longest_loss_streak", "Show longest loss streak", "", m_config.show_longest_loss_streak, !m_config.show_streaks_stats)
        << ToggleControl("show_gamemode_record_1v1", "1v1 breakdown", "", m_config.show_gamemode_record_1v1, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_2v2", "2v2 breakdown", "", m_config.show_gamemode_record_2v2, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_3v3", "3v3 breakdown", "", m_config.show_gamemode_record_3v3, !m_config.show_gamemode_breakdown)
        << SelectRow("gamemode_breakdown_scope", "Gamemode breakdown scope", "", GamemodeScopeOptions(), GamemodeScopeValue(m_config), !m_config.show_gamemode_breakdown)
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsReminders() {
    std::ostringstream out;
    out << SectionStart("Tournament reminders")
        << "<div class='setting-help section-help'>Reminders work even when the tournament countdown card is off.</div>";
    const bool signedIn = AccountClient::Instance().IsSignedIn();
    const auto tournaments = Tournaments::TournamentScheduleService::Instance().Snapshot();
    if (!signedIn) out << "<div class='setting-notice'>Sign in under Accounts &amp; services to get the tournament schedule.</div>";
    const bool disabled = !signedIn;
    std::string regionHelp = "Not detected yet. Open Rocket League to detect your region.";
    if (tournaments->regionSource == "ping")
        regionHelp = "Auto uses your lowest-ping server (" + tournaments->detectedServer + ", " + std::to_string(tournaments->detectedPingMs) + " ms).";
    else if (tournaments->regionSource == "matches")
        regionHelp = "Auto uses your recent matchmaking region.";
    const std::string autoLabel = tournaments->group.empty() ? "Auto" : "Auto (" + tournaments->group + ")";
    out << SelectRow("tournament_region", "Region", regionHelp,
                     {{"auto", autoLabel}, {"US", "US"}, {"EU", "Europe"}, {"OCE", "Oceania"}, {"SAM", "South America"}, {"ASIA", "Asia"}, {"ME", "Middle East"}, {"SAF", "South Africa"}},
                     m_config.tournament_region, disabled)
        << SelectRow("tournament_reminder_minutes", "Remind me", "A short notice appears on screen, even during matches.",
                     {{"0", "Off"}, {"5", "5 min before"}, {"10", "10 min before"}, {"15", "15 min before"}}, std::to_string(m_config.tournament_reminder_minutes), disabled)
        << ToggleControl("tournament_reminder_standard_only", "Standard tournaments only", "Only 2v2 and 3v3 Soccar.", m_config.tournament_reminder_standard_only, disabled);
    if (m_config.second_monitor_mode)
        out << ToggleControl("tournament_sound_cue", "Sound cue", "Play a sound when a tournament is about to start.", m_config.tournament_sound_cue, disabled);
    out << SectionEnd();
    return out.str();
}
