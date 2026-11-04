#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/presenters/SettingsPresenter.hpp"
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string SettingsPresenter::RenderGeneral(const ConfigData& config) {
    std::ostringstream out;
    out << SectionStart("Display mode")
        << SelectRow("display_mode", "Use OmniStats as", "", {{"overlay", "In-game overlay"}, {"dashboard", "Second-monitor dashboard"}}, config.second_monitor_mode ? "dashboard" : "overlay")
        << ToggleControl("require_rl_focus", "Only show while Rocket League is active",
                         config.second_monitor_mode ? "Hides the dashboard when Rocket League isn't running." : "Hides the overlay when you tab out of Rocket League.",
                         config.require_rl_focus)
        << SectionEnd();

    const std::string summaryHelp = "Displays the result and match statistics for " +
                                    std::to_string(std::clamp(config.match_summary_seconds, 5, 60)) + " seconds.";
    out << SectionStart("Match behavior")
        << ToggleControl("show_match_summary", "Show post-match summary", summaryHelp, config.show_match_summary)
        << ToggleControl("show_running_indicator", "Show running indicator", "A small badge that shows the overlay is running.", config.show_running_indicator)
        << SectionEnd();
    out << SectionStart("Seasons")
        << ToggleControl("show_season_countdown", "Season countdown", "Shows time left on the dashboard and tournament card.", config.show_season_countdown)
        << SelectRow("season_reminder_days", "End-of-season reminder", "A short note between matches when the season is almost over.",
                     {{"0", "Off"}, {"3", "3 days before"}, {"7", "7 days before"}, {"14", "14 days before"}}, std::to_string(config.season_reminder_days))
        << ToggleControl("season_reward_reminders", "Remind me about rewards", "Only when you still need wins for your season rewards.", config.season_reward_reminders)
        << ToggleControl("show_season_recap", "Season recap", "Opens your final season card when the new season starts.", config.show_season_recap)
        << ToggleControl("group_history_by_season", "Group history by season", "Adds season filters to match history.", config.group_history_by_season)
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsGeneral() {
    return m_settingsPresenter->RenderGeneral(m_config);
}

std::vector<SelectOption> RmlUiController::IdentityOptions() const {
    std::vector<SelectOption> options = {{"", "Auto-detect"}};
    std::set<std::string> seen;
    const auto selectableIdentity = [&](const std::string& id) {
        return !id.empty() && (id.rfind("Unknown|", 0) != 0 || id == m_config.last_primary_id);
    };
    for (const auto& id : m_config.known_primary_ids)
        if (selectableIdentity(id)) seen.insert(id);
    for (const auto& [id, p] : m_snap.roster)
        if (selectableIdentity(id)) seen.insert(id);
    for (const auto& id : seen) {
        std::string label = id;
        if (auto it = m_snap.roster.find(id); it != m_snap.roster.end() && !it->second.name.empty()) label = it->second.name + " · " + id;
        options.push_back({id, std::move(label)});
    }
    return options;
}
