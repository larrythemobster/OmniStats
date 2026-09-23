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

std::string RmlUiController::RenderSettingsCards() {
    std::ostringstream out;
    out << SectionStart("Overlay Layout")
        << ToggleControl("overlay_edit_mode", "Edit overlay layout", "Move and resize native overlay containers.", m_state && m_state->ui.dashboardLayoutEditMode.load())
        << "<div class='row wrap gap-sm' style='margin-top:8dp'>"
        << Button("reset-overlay", "Reset Overlay Layout", "ghost")
        << Button("reset-theme-and-layout", "Reset Theme & Layout to Default", "ghost")
        << "</div>" << SectionEnd();

    out << SectionStart("Session Card")
        << ToggleControl("show_session_record", "Record", "", m_config.show_session_record)
        << ToggleControl("show_session_goals", "Goals", "", m_config.show_session_goals)
        << ToggleControl("show_session_saves", "Saves", "", m_config.show_session_saves)
        << ToggleControl("show_session_demos", "Demos", "", m_config.show_session_demos)
        << ToggleControl("show_session_boost", "Boost", "", m_config.show_session_boost)
        << ToggleControl("show_session_assists", "Assists", "", m_config.show_session_assists)
        << ToggleControl("show_session_goal_participation", "Goal participation", "", m_config.show_session_goal_participation)
        << ToggleControl("show_session_mmr_change", "MMR change", "", m_config.show_session_mmr_change)
        << SectionEnd();

    out << SectionStart("Visible Cards")
        << ToggleControl("use_rank_icons", "Use rank & playlist icons", "", m_config.use_rank_icons)
        << ToggleControl("show_lobby_ranks_overlay", "Lobby ranks", "", m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_account_wins_overlay", "Account wins", "", m_config.show_account_wins_overlay)
        << ToggleControl("show_demo_tracker_overlay", "Demolition tracker", "", m_config.show_demo_tracker_overlay)
        << ToggleControl("show_previous_games_summary", "Previous games", "", m_config.show_previous_games_summary)
        << ToggleControl("show_streaks_stats", "Streaks & stats", "", m_config.show_streaks_stats)
        << ToggleControl("show_gamemode_breakdown", "Gamemode breakdown", "", m_config.show_gamemode_breakdown)
        << SectionEnd();

    out << SectionStart("Lobby Rank Playlists")
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
    out << SectionStart("Card Details")
        << SelectRow("previous_games_limit", "Previous games to show", "", gameCounts, std::to_string(m_config.previous_games_limit), !m_config.show_previous_games_summary)
        << ToggleControl("show_longest_loss_streak", "Show longest loss streak", "", m_config.show_longest_loss_streak, !m_config.show_streaks_stats)
        << ToggleControl("show_gamemode_record_1v1", "1v1 breakdown", "", m_config.show_gamemode_record_1v1, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_2v2", "2v2 breakdown", "", m_config.show_gamemode_record_2v2, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_3v3", "3v3 breakdown", "", m_config.show_gamemode_record_3v3, !m_config.show_gamemode_breakdown)
        << SelectRow("gamemode_breakdown_scope", "Gamemode breakdown scope", "", GamemodeScopeOptions(), GamemodeScopeValue(m_config), !m_config.show_gamemode_breakdown)
        << SectionEnd();
    return out.str();
}
