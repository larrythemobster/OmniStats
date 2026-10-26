#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/presenters/SettingsPresenter.hpp"
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/omninet/OmniNet.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string SettingsPresenter::RenderRanks(const ConfigData& config, const RmlRenderSnapshot& snap) {
    const std::string rosterCategory = MmrCategoryToString(StringToMmrCategory(config.mmr_category));
    const std::string graphCategory = MmrCategoryToString(StringToMmrCategory(config.graph_mmr_category));
    const std::string rankCategory = MmrCategoryToString(StringToMmrCategory(config.rank_progress_mmr_category));
    std::ostringstream out;
    out << SectionStart("Player ranks")
        << SelectRow("mmr_category", "Player MMR category", "Rank displayed beside live lobby players.", MmrCategoryOptions(true, config.show_extra_playlists), rosterCategory)
        << ToggleControl("auto_switch_mmr_category", "Automatically follow current playlist", "", config.auto_switch_mmr_category)
        << ToggleControl("show_extra_playlists", "Show extra playlists", "Hoops, Rumble, Dropshot, Snow Day, and Heatseeker.", config.show_extra_playlists)
        << SectionEnd();

    out << SectionStart("Rank progress")
        << ToggleControl("rank_progress_follow_current_playlist", "Follow current playlist", "", config.rank_progress_follow_current_playlist)
        << SelectRow("rank_progress_mmr_category", "Default rank playlist", "Playlist shown when not following the active match.", MmrCategoryOptions(true, config.show_extra_playlists), rankCategory, config.rank_progress_follow_current_playlist)
        << SectionEnd();

    out << SectionStart("Personal MMR graph")
        << ToggleControl("graph_follow_current_playlist", "Follow current playlist", "", config.graph_follow_current_playlist)
        << SelectRow("graph_mmr_category", "Default graph category", "", MmrCategoryOptions(false, config.show_extra_playlists), graphCategory)
        << SectionEnd();

    out << SectionStart("Your ranks");
    const PlayerData* me = nullptr;
    std::string effectiveId = snap.myPrimaryId.empty() ? config.last_primary_id : snap.myPrimaryId;
    if (!effectiveId.empty()) {
        if (auto it = snap.roster.find(effectiveId); it != snap.roster.end()) me = &it->second;
    }
    if (me) {
        out << "<div class='rank-table'><div class='rank-table-row header'><div>Playlist</div><div>Rank</div><div>MMR</div><div>Matches</div></div>";
        for (auto category : MmrCategories(false, config.show_extra_playlists)) {
            const std::string key = MmrCategoryToString(category);
            std::string tier = "Unranked";
            if (auto it = me->playlistTiers.find(key); it != me->playlistTiers.end()) tier = it->second;
            int mmr = 0;
            if (auto it = me->playlists.find(key); it != me->playlists.end()) mmr = it->second;
            int matches = 0;
            if (auto it = me->playlistMatches.find(key); it != me->playlistMatches.end()) matches = it->second;
            out << "<div class='rank-table-row'><div>" << Escape(MmrLabel(category)) << "</div><div style='color:" << CssColor(Format::RankColor(tier)) << "'>" << Escape(Format::RankTier(tier, config.use_roman_numerals)) << "</div><div class='mono'>" << (mmr > 0 ? std::to_string(mmr) : "-") << "</div><div class='mono'>" << (matches > 0 ? std::to_string(matches) : "-") << "</div></div>";
        }
        out << "</div>";
    } else {
        out << "<div class='setting-help'>Join a match to see your ranks. If your account isn't detected, pick it in General.</div>";
        if (!RankSourceAvailable(config) && OmniNet::Available()) out << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("open-rank-settings", "Connect live ranks", "primary") << "</div>";
    }
    out << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsRanks() {
    return m_settingsPresenter->RenderRanks(m_config, m_snap);
}
