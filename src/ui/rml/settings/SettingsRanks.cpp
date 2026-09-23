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

std::string RmlUiController::RenderSettingsRanks() {
    const std::string rosterCategory = MmrCategoryToString(StringToMmrCategory(m_config.mmr_category));
    const std::string graphCategory = MmrCategoryToString(StringToMmrCategory(m_config.graph_mmr_category));
    std::ostringstream out;
    out << SectionStart("Player Ranks")
        << SelectRow("mmr_category", "Player MMR category", "Rank displayed beside live lobby players.", MmrCategoryOptions(true, m_config.show_extra_playlists), rosterCategory)
        << ToggleControl("auto_switch_mmr_category", "Automatically follow current playlist", "", m_config.auto_switch_mmr_category)
        << ToggleControl("show_extra_playlists", "Show extra playlists", "Hoops, Rumble, Dropshot, Snow Day, and Heatseeker.", m_config.show_extra_playlists)
        << SectionEnd();

    out << SectionStart("Personal MMR Graph")
        << ToggleControl("graph_follow_current_playlist", "Follow current playlist", "", m_config.graph_follow_current_playlist)
        << SelectRow("graph_mmr_category", "Default graph category", "", MmrCategoryOptions(false, m_config.show_extra_playlists), graphCategory)
        << SectionEnd();

    out << SectionStart("Your Ranks");
    const PlayerData* me = nullptr;
    std::string effectiveId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    if (!effectiveId.empty()) {
        if (auto it = m_snap.roster.find(effectiveId); it != m_snap.roster.end()) me = &it->second;
    }
    if (me) {
        out << "<div class='rank-table'><div class='rank-table-row header'><div>Playlist</div><div>Rank</div><div>MMR</div><div>Matches</div></div>";
        for (auto category : MmrCategories(false, m_config.show_extra_playlists)) {
            const std::string key = MmrCategoryToString(category);
            std::string tier = "Unranked";
            if (auto it = me->playlistTiers.find(key); it != me->playlistTiers.end()) tier = it->second;
            int mmr = 0;
            if (auto it = me->playlists.find(key); it != me->playlists.end()) mmr = it->second;
            int matches = 0;
            if (auto it = me->playlistMatches.find(key); it != me->playlistMatches.end()) matches = it->second;
            out << "<div class='rank-table-row'><div>" << Escape(MmrLabel(category)) << "</div><div style='color:" << CssColor(Format::RankColor(tier)) << "'>" << Escape(Format::RankTier(tier, m_config.use_roman_numerals)) << "</div><div class='mono'>" << (mmr > 0 ? std::to_string(mmr) : "-") << "</div><div class='mono'>" << (matches > 0 ? std::to_string(matches) : "-") << "</div></div>";
        }
        out << "</div>";
    } else {
        out << "<div class='setting-help'>Join a match to see your ranks. If your account is not detected, select it in General. Rank lookup can be enabled in Integrations.</div>";
    }
    out << SectionEnd();
    return out.str();
}
