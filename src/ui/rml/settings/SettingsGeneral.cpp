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

std::string RmlUiController::RenderSettingsGeneral() {
    std::ostringstream out;
    out << SectionStart("Display mode")
        << SelectRow("display_mode", "Use OmniStats as", "", {{"overlay", "In-game overlay"}, {"dashboard", "Second-monitor dashboard"}}, m_config.second_monitor_mode ? "dashboard" : "overlay")
        << ToggleControl("require_rl_focus", "Only show while Rocket League is active", "The dashboard stays visible while Rocket League is running.", m_config.require_rl_focus)
        << SectionEnd();

    const std::string summaryHelp = "Displays the result and match statistics for " +
                                    std::to_string(std::clamp(m_config.match_summary_seconds, 5, 60)) + " seconds.";
    out << SectionStart("Match Behavior")
        << ToggleControl("show_match_summary", "Show post-match summary", summaryHelp, m_config.show_match_summary)
        << ToggleControl("show_running_indicator", "Show running indicator", "Small status badge while the transparent overlay is active.", m_config.show_running_indicator)
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
