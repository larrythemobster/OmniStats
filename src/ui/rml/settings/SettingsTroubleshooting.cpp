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

std::string SettingsPresenter::RenderTroubleshooting(const ConfigData& config, const std::shared_ptr<SessionState>& state, const std::string& statsApiPathError) {
    StatsApiConfig::CheckResult result;
    if (state) {
        std::lock_guard lock(state->ui.statsApiMutex);
        result = state->ui.statsApiResult;
    }
    std::ostringstream out;
    out << SectionStart("Rocket League connection")
        << "<div class='setting-help section-help'>OmniStats reads live stats through Rocket League's local Stats API.</div>"
        << ToggleControl("check_stats_api_config_on_startup", "Check Stats API config on startup", "Offers to turn the Stats API on at launch when it's off.", config.check_stats_api_config_on_startup)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Detected path</div><div class='setting-help mono'>" << Escape(result.path.empty() ? "(none)" : result.path) << "</div></div></div>"
        << ActionRow("Status", StatsApiConfig::GetStatusMessage(result.status),
                     Button("statsapi-check", "Check again", "ghost") +
                         (result.status != StatsApiConfig::Status::Valid && !result.path.empty() ? Button("statsapi-fix", "Fix config", "primary") : std::string{}));
    if (!result.message.empty() && result.status != StatsApiConfig::Status::Valid) out << "<div class='setting-help loss'>" << Escape(result.message) << "</div>";
    if (result.status != StatsApiConfig::Status::Valid && result.rlRunning) {
        out << "<div class='setting-notice'>Restart Rocket League after fixing for changes to take effect.</div>";
    }
    out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Manual config path</div><div class='setting-help'>TAStatsAPI.ini, or DefaultStatsAPI.ini when TAStatsAPI.ini does not exist.</div></div><div class='row gap-xs'><input class='text' style='width:300dp' data-setting='statsapi_path' value='" << Escape(config.rocket_league_stats_api_config_path) << "'/>" << Button("statsapi-browse", "Browse...", "ghost") << "</div></div>";
    if (!statsApiPathError.empty()) out << "<div class='setting-help loss'>" << Escape(statsApiPathError) << "</div>";
    out << SectionEnd();
    out << SectionStart("Diagnostics and logs")
        << ToggleControl("debug_logging", "Verbose debug logging", "Off by default. Sensitive identifiers are redacted when disabled.", config.debug_logging)
        << ActionRow("Support bundle", "A redacted ZIP with recent logs and system diagnostics.", Button("create-support-bundle", "Create", "primary"))
        << ActionRow("Diagnostics summary", "A short text summary you can paste into a support message.", Button("copy-diagnostics-summary", "Copy", "ghost"))
        << ActionRow("Log file", "", Button("show-log", "Show", "ghost"))
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsTroubleshooting() {
    return m_settingsPresenter->RenderTroubleshooting(m_config, m_state, m_statsApiPathError);
}
