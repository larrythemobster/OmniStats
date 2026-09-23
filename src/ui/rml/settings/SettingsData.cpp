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

std::string RmlUiController::RenderSettingsData() {
    std::ostringstream out;
    out << SectionStart("Privacy & Diagnostics")
        << "<div class='setting-help'>Startup diagnostics are required after accepting the privacy notice. Match data and player names are not included.</div>"
        << ToggleControl("crash_reports_enabled", "Upload crash reports", "Pending minidumps are sent on the next startup.", m_config.crash_reports_enabled)
        << SectionEnd();
    out << SectionStart("Local Data")
        << "<div class='setting-help mono'>" << Escape(Storage::GetDataDirectory()) << "</div><div class='row wrap gap-sm' style='margin-top:8dp'>"
        << Button("open-data-folder", "Open Data Folder") << Button("export-data", "Export Local Data") << Button("merge-database", "Merge Database") << Button("delete-history", "Delete History & Identity", "danger") << "</div>"
        << "<div class='setting-help' style='margin-top:8dp'>Merge matches and history from an older omnistats.db file (such as a backup or from another PC) into your current database. Duplicates are automatically skipped.</div>"
        << "<div class='setting-help' style='margin-top:4dp'>Deleting local history also clears the saved local account identity. Settings and service tokens are kept.</div>" << SectionEnd();
    return out.str();
}
