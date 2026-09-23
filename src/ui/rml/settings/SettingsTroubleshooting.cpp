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

std::string RmlUiController::RenderSettingsTroubleshooting() {
    StatsApiConfig::CheckResult result;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
    }
    std::ostringstream out;
    out << SectionStart("Rocket League Connection")
        << "<div class='setting-help'>OmniStats reads live stats through Rocket League's local Stats API.</div>"
        << ToggleControl("check_stats_api_config_on_startup", "Check Stats API config on startup", "", m_config.check_stats_api_config_on_startup)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Detected path</div><div class='setting-help mono'>" << Escape(result.path.empty() ? "(none)" : result.path) << "</div></div></div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Status</div><div class='setting-help'>" << Escape(StatsApiConfig::GetStatusMessage(result.status)) << "</div></div></div>";
    if (!result.message.empty() && result.status != StatsApiConfig::Status::Valid) out << "<div class='setting-help loss'>" << Escape(result.message) << "</div>";
    if (result.status != StatsApiConfig::Status::Valid && result.rlRunning) {
        out << "<div class='setting-help' style='color:" << CssColor(m_config.themeAccent) << "'>Restart Rocket League after fixing for changes to take effect.</div>";
    }
    out << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("statsapi-check", "Check Again") << (result.status != StatsApiConfig::Status::Valid && !result.path.empty() ? Button("statsapi-fix", "Fix Config", "primary") : "") << "</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Manual config path</div><div class='setting-help'>DefaultStatsAPI.ini</div></div><input class='text' style='width:300dp' data-setting='statsapi_path' value='" << Escape(m_config.rocket_league_stats_api_config_path) << "'/></div>";
    if (!m_statsApiPathError.empty()) out << "<div class='setting-help loss'>" << Escape(m_statsApiPathError) << "</div>";
    out << SectionEnd();
    out << SectionStart("Diagnostics & Logs")
        << ToggleControl("debug_logging", "Verbose debug logging", "Off by default; sensitive identifiers are redacted when disabled.", m_config.debug_logging)
        << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("show-log", "Show Log File") << "</div>" << SectionEnd();
    return out.str();
}
