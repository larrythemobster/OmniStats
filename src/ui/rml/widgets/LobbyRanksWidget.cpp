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

std::string RmlUiController::RenderLobbyRanks() {
    struct Playlist {
        const char* key;
        const char* label;
        bool show;
    };
    const std::vector<Playlist> playlists = {
        {"1v1", "1v1", m_config.show_lobby_rank_1v1}, {"2v2", "2v2", m_config.show_lobby_rank_2v2}, {"3v3", "3v3", m_config.show_lobby_rank_3v3}, {"casual", "Casual", m_config.show_lobby_rank_casual}, {"t", "Tourney", m_config.show_lobby_rank_tourny}, {"hoops", "Hoops", m_config.show_extra_playlists && m_config.show_lobby_rank_hoops}, {"rumble", "Rumble", m_config.show_extra_playlists && m_config.show_lobby_rank_rumble}, {"dropshot", "Dropshot", m_config.show_extra_playlists && m_config.show_lobby_rank_dropshot}, {"snowday", "Snow", m_config.show_extra_playlists && m_config.show_lobby_rank_snowday}, {"heatseeker", "Heat", m_config.show_extra_playlists && m_config.show_lobby_rank_heatseeker}};
    std::vector<const PlayerData*> players;
    for (const auto& [_, p] : m_snap.roster)
        players.push_back(&p);
    std::sort(players.begin(), players.end(), [](const PlayerData* a, const PlayerData* b) {
        if (a->team != b->team) return a->team < b->team;
        auto mmr = [](const PlayerData* p) { auto it = p->playlists.find("2v2"); return it == p->playlists.end() ? p->mmr : it->second; };
        if (mmr(a) != mmr(b)) return mmr(a) > mmr(b);
        return a->name < b->name;
    });
    std::ostringstream out;
    out << "<div class='lobby-rank-table'><div class='player-row lobby-rank-row lobby-rank-header'><div class='player-name'><div class='lobby-rank-heading'>Name</div></div>";
    for (const auto& pl : playlists) {
        if (!pl.show) continue;
        // Wrap header text in a block: RmlUi drops bare text nodes in a flex
        // container, which is what left the lobby header row blank.
        out << "<div class='lobby-rank-cell'><div class='lobby-rank-heading'>" << Escape(pl.label) << "</div></div>";
    }
    out << "</div>";
    for (const auto* p : players) {
        std::string platform;
        if (const auto pos = p->primaryId.find('|'); pos != std::string::npos) platform = p->primaryId.substr(0, pos);
        const PlatformKind platformKind = PlatformKindFor(platform);

        out << "<div class='player-row lobby-rank-row" << (p->primaryId == m_snap.myPrimaryId ? " self" : "") << "'><div class='player-name'><span class='value'>" << Escape(p->name) << "</span>";
        if (!platform.empty())
            out << "<div class='label " << PlatformClass(platformKind) << "'>"
                << Escape(PlatformDisplayName(platformKind, platform)) << "</div>";
        out << "</div>";
        for (const auto& pl : playlists) {
            if (!pl.show) continue;
            int mmr = 0;
            if (auto it = p->playlists.find(pl.key); it != p->playlists.end()) mmr = it->second;
            const int matches = [&]() { auto it = p->playlistMatches.find(pl.key); return it == p->playlistMatches.end() ? 0 : it->second; }();
            std::string tier = "Unranked";
            if (auto it = p->playlistTiers.find(pl.key); it != p->playlistTiers.end()) tier = it->second;
            if (std::string_view(pl.key) != "casual" && (tier.empty() || tier == "Unranked") && mmr > 0) {
                tier = MMRFetcher::GetRankTierForPlaylistMmr(pl.key, mmr);
            }
            out << "<div class='lobby-rank-cell tooltip-host' style='color:" << CssColor(Format::RankColor(tier)) << "'>";
            if (mmr > 0) {
                // Rank above the rating, with the season games played trailing the
                // rating on the same line: a third line made the table taller than
                // the roster it sits under.
                out << "<div class='lobby-rank-value'>";
                if (tier != "Unranked" && !tier.empty() && std::string_view(pl.key) != "casual") {
                    out << "<div class='mono'>" << Escape(Format::AbbreviateRank(tier)) << "</div>";
                }
                out << "<div><span class='mono'>" << mmr << "</span>";
                if (matches > 0) out << "<span class='lobby-rank-matches'>(" << matches << ")</span>";
                out << "</div></div>";
                out << "<span class='tooltip-bubble'>";
                if (std::string_view(pl.key) == "casual") {
                    out << "Casual · MMR " << mmr;
                } else if (tier != "Unranked" && !tier.empty()) {
                    out << Escape(Format::RankTier(tier, m_config.use_roman_numerals)) << " · MMR " << mmr;
                } else {
                    out << "Unranked · MMR " << mmr;
                }
                if (matches > 0) out << " · " << matches << " games this season";
                out << "</span>";
            } else {
                out << "<div class='lobby-rank-value'><div>" << (p->fetched ? "-" : "...") << "</div></div>";
                if (!p->fetched) out << "<span class='tooltip-bubble'>Fetching rank...</span>";
            }
            out << "</div>";
        }
        out << "</div>";
    }
    if (players.empty()) out << "<div class='muted'>Waiting for lobby ranks...</div>";
    out << "</div>";
    return out.str();
}
