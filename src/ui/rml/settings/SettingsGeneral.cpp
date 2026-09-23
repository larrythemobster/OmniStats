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

std::string RmlUiController::RenderSettingsGeneral() {
    std::ostringstream out;
    out << SectionStart("Window & Visibility")
        << ToggleControl("require_rl_focus", "Require Rocket League focus", "Hide the overlay while another app has focus.", m_config.require_rl_focus)
        << ToggleControl("second_monitor_mode", "Second-monitor dashboard", "Use OmniStats as a normal interactive dashboard window.", m_config.second_monitor_mode);
    if (m_config.second_monitor_mode) {
        out << ToggleControl("second_monitor_show_roster", "Show live roster", "", m_config.second_monitor_show_roster)
            << ToggleControl("second_monitor_show_session", "Show session information", "", m_config.second_monitor_show_session)
            << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("reset-dashboard", "Reset Dashboard Layout", "ghost") << "</div>";
    }
    out << SectionEnd();

    out << SectionStart("Match Behavior")
        << ToggleControl("show_match_summary", "Show post-match summary", "Displays the result and match statistics for 30 seconds.", m_config.show_match_summary)
        << ToggleControl("show_running_indicator", "Show running indicator", "Small status badge while the transparent overlay is active.", m_config.show_running_indicator)
        << ToggleControl("reset_session_on_close", "Reset session when OmniStats closes", "", m_config.reset_session_on_close)
        << SectionEnd();

    out << SectionStart("Startup & Updates")
        << ToggleControl("run_on_startup", "Run on Windows startup", "", m_config.run_on_startup)
        << ToggleControl("enable_auto_updates", "Automatically install updates", "Installs updates automatically on the next launch. OmniStats always checks for updates at startup, periodically while running, and when Settings is opened.", m_config.enable_auto_updates)
        << SectionEnd();

    out << SectionStart("Player Identity")
        << SelectRow("identity", "Local account", "Used for lifetime history before telemetry identifies you.", IdentityOptions(), m_config.last_primary_id)
        << SectionEnd();
    return out.str();
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
