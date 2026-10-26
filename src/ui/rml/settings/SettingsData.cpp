#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
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

std::string SettingsPresenter::RenderData(const ConfigData& config, const std::shared_ptr<SessionState>& state, bool busy) {
    int detailedMatches = 0;
    std::string detailedSince;
    if (state) {
        std::lock_guard lock(state->ui.dbStatsMutex);
        detailedMatches = state->ui.cachedDbStats.detailedStatsMatchCount;
        detailedSince = state->ui.cachedDbStats.detailedStatsSinceDate;
    }
    const std::string detailedSummary =
        (detailedMatches > 0 && !detailedSince.empty())
            ? ("Detailed stats recorded since " + detailedSince + " · " + std::to_string(detailedMatches) + " matches")
            : "Detailed stats not yet recorded";

    std::ostringstream out;
    out << SectionStart("Privacy and diagnostics")
        << "<div class='setting-help section-help'>Startup diagnostics are required after accepting the privacy notice. Match data and player names are not included.</div>"
        << ToggleControl("crash_reports_enabled", "Upload crash reports", "Pending minidumps are sent on the next startup.", config.crash_reports_enabled)
        << SectionEnd();
    out << SectionStart("Local data")
        << ActionRow("Match history", detailedSummary, Button("open-data-folder", "Open folder", "ghost") + Button("export-data", "Export", "ghost", busy))
        << "<div class='setting-path mono'>" << Escape(Storage::GetDataDirectory()) << "</div>"
        << ActionRow("Merge another database", "Import matches from an older omnistats.db, such as a backup or another PC. Duplicates are skipped.",
                     Button("merge-database", "Merge", "ghost", busy))
        << ActionRow("Delete history and identity", "Removes local match history and the saved account identity. Settings, service tokens, and tracked accounts are kept.",
                     Button("delete-history", "Delete", "danger", busy))
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsData() {
    return m_settingsPresenter->RenderData(m_config, m_state, m_backgroundTaskRunning.load(std::memory_order_relaxed));
}
