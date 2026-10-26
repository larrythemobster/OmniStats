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

std::string RenderLobbyRanksImpl(const RmlRenderSnapshot& snap, const ConfigData& config) {
    struct Playlist {
        const char* key;
        const char* label;
        bool show;
    };
    const std::vector<Playlist> playlists = {
        {"1v1", "1v1", config.show_lobby_rank_1v1}, {"2v2", "2v2", config.show_lobby_rank_2v2}, {"3v3", "3v3", config.show_lobby_rank_3v3}, {"casual", "Casual", config.show_lobby_rank_casual}, {"t", "Tourney", config.show_lobby_rank_tourny}, {"hoops", "Hoops", config.show_extra_playlists && config.show_lobby_rank_hoops}, {"rumble", "Rumble", config.show_extra_playlists && config.show_lobby_rank_rumble}, {"dropshot", "Dropshot", config.show_extra_playlists && config.show_lobby_rank_dropshot}, {"snowday", "Snow", config.show_extra_playlists && config.show_lobby_rank_snowday}, {"heatseeker", "Heat", config.show_extra_playlists && config.show_lobby_rank_heatseeker}};
    std::vector<const PlayerData*> players;
    for (const auto& [_, p] : snap.roster)
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
        out << "<div class='lobby-rank-cell'><div class='lobby-rank-heading'>" << Escape(pl.label) << "</div></div>";
    }
    out << "</div>";
    const bool rankSource = RankSourceAvailable(config, snap.accountSignedIn);
    for (const auto* p : players) {
        std::string platform;
        if (const auto pos = p->primaryId.find('|'); pos != std::string::npos) platform = p->primaryId.substr(0, pos);
        const PlatformKind platformKind = PlatformKindFor(platform);

        const bool self = p->primaryId == snap.myPrimaryId;
        out << "<div class='player-row lobby-rank-row" << (self ? " self" : "") << "'><div class='player-name'><span class='value'>" << Escape(p->name) << "</span>";
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
                tier = MmrService::GetRankTierForPlaylistMmr(pl.key, mmr);
            }
            out << "<div class='lobby-rank-cell tooltip-host' style='color:" << CssColor(Format::RankColor(tier)) << "'>";
            if (mmr > 0) {
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
                    out << Escape(Format::RankTier(tier, config.use_roman_numerals)) << " · MMR " << mmr;
                } else {
                    out << "Unranked · MMR " << mmr;
                }
                if (matches > 0) out << " · " << matches << " games this season";
                out << "</span>";
            } else {
                const bool pending = rankSource && !p->fetched;
                out << "<div class='lobby-rank-value'><div>" << (pending ? "..." : "-") << "</div></div>";
                if (pending) out << "<span class='tooltip-bubble'>Fetching rank...</span>";
            }
            out << "</div>";
        }
        out << "</div>";
    }
    if (players.empty()) out << "<div class='muted'>Waiting for lobby ranks...</div>";
    out << "</div>";
    return out.str();
}

std::string OverlayPresenter::RenderLobbyRanks(const RmlRenderSnapshot& snap, const ConfigData& config) {
    return RenderLobbyRanksImpl(snap, config);
}

std::string RmlUiController::RenderLobbyRanks() {
    return RenderLobbyRanksImpl(m_snap, m_config);
}
