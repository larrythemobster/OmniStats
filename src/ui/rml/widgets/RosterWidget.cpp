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

std::string RmlUiController::RenderRankBadge(const std::string& tier, bool fetched, const std::string& tooltip) const {
    const int tierIndex = fetched ? TierResourceIndex(tier) : 23;
    const int division = fetched ? DivisionLevel(tier) : 0;
    const int filledDivisionIndex = DivisionColorResourceIndex(tier);
    std::ostringstream out;
    out << "<span class='rank-badge tooltip-host'><img class='rank-icon' src='res://images/Tiers/" << tierIndex << ".png'/>";
    if (division > 0) {
        out << "<span class='division-stack'>";
        for (int i = 4; i >= 1; --i) {
            const int resource = i <= division ? filledDivisionIndex : 0;
            out << "<img class='division-pill' src='res://images/Divisions/" << resource << ".png'/>";
        }
        out << "</span>";
    }
    if (!tooltip.empty()) out << "<span class='tooltip-bubble'>" << Escape(tooltip) << "</span>";
    out << "</span>";
    return out.str();
}

std::string RmlUiController::RenderPlayerRoster(int team, const char* label) {
    std::vector<const PlayerData*> players;
    const std::string category = MmrCategoryToString(m_state ? m_state->ui.rosterMmrCategory.load() : MmrCategory::Best);
    for (const auto& [_, player] : m_snap.roster)
        if (player.team == team) players.push_back(&player);
    auto mmrFor = [&](const PlayerData& p) {
        if (category == "best") return p.mmr;
        if (auto it = p.playlists.find(category); it != p.playlists.end()) return it->second;
        return 0;
    };
    auto sortMmrFor = [&](const PlayerData& p) {
        const int selected = mmrFor(p);
        return selected > 0 ? selected : p.mmr;
    };
    std::sort(players.begin(), players.end(), [&](const PlayerData* a, const PlayerData* b) {
        const int ma = sortMmrFor(*a), mb = sortMmrFor(*b);
        if (ma != mb) return ma > mb;
        return a->name < b->name;
    });

    std::ostringstream out;
    const char* cls = team == 0 ? "blue" : "orange";
    out << "<div class='team-block'><div class='team-header'><div class='team-line " << cls << "'></div><div class='grow value'>" << label
        << "</div></div>";
    if (players.empty()) out << "<div class='muted'>Waiting for players...</div>";
    for (const auto* p : players) {
        const bool self = !m_snap.myPrimaryId.empty() && p->primaryId == m_snap.myPrimaryId;
        const int mmr = mmrFor(*p);
        std::string tier = p->rankTier;
        std::string rankSource = category;
        if (category != "best") {
            if (auto it = p->playlistTiers.find(category); it != p->playlistTiers.end())
                tier = it->second;
            else
                tier = "Unranked";
        } else if (mmr > 0) {
            // `best` is a presentation bucket, not a Rocket League playlist.
            // Preserve the previous roster behavior by showing which real playlist
            // supplied the best rank/MMR instead of presenting BEST as a mode.
            const auto matchesMmr = [&](const char* key) {
                auto it = p->playlists.find(key);
                return it != p->playlists.end() && it->second == mmr;
            };
            for (const char* key : {"2v2", "3v3", "1v1"}) {
                if (matchesMmr(key)) {
                    rankSource = key;
                    break;
                }
            }
            if (rankSource == "best" && m_config.show_extra_playlists) {
                for (const char* key : {"hoops", "rumble", "dropshot", "snowday", "heatseeker"}) {
                    if (matchesMmr(key)) {
                        rankSource = key;
                        break;
                    }
                }
            }
        }
        if (category != "casual" && (tier.empty() || tier == "Unranked") && mmr > 0) {
            tier = MMRFetcher::GetRankTierForPlaylistMmr(category == "best" ? rankSource : category, mmr);
        }
        std::string platform;
        if (const auto pos = p->primaryId.find('|'); pos != std::string::npos) platform = p->primaryId.substr(0, pos);
        const auto color = Format::RankColor(tier);

        const std::string matchSource = category == "best" && rankSource != "best" ? rankSource : category;
        auto matchesIt = p->playlistMatches.find(matchSource);
        if (matchesIt == p->playlistMatches.end() && category == "best") {
            matchesIt = p->playlistMatches.find("best");
        }
        const int matchCount = matchesIt != p->playlistMatches.end() ? matchesIt->second : 0;

        out << "<div class='player-row" << (self ? " self" : "") << "'>";
        if (m_config.use_rank_icons) {
            const std::string rankTooltip = p->fetched || mmr > 0 ? Format::RankTier(tier, m_config.use_roman_numerals) : "Fetching rank...";
            out << "<div class='player-crest'>" << RenderRankBadge(tier, p->fetched || mmr > 0, rankTooltip) << "</div>";
        }

        const std::string trackerUrl = TrackerUrlForPlayer(*p);
        out << "<div class='player-identity'><div class='player-headline'><span class='value player-name-text"
            << (trackerUrl.empty() ? "" : " player-link") << "'";
        if (!trackerUrl.empty()) out << " data-action='open-player-tracker' data-url='" << Escape(trackerUrl) << "'";
        out << ">" << Escape(p->name) << "</span>";
        if (!platform.empty()) {
            const PlatformKind platformKind = PlatformKindFor(platform);
            out << "<span class='badge " << PlatformClass(platformKind) << "'>"
                << Escape(PlatformBadgeLabel(platformKind, platform)) << "</span>";
        }
        out << "</div><div class='player-chips'>";

        // MMR chip. Without rank crests it also has to name the tier, and when
        // the roster shows the player's best rank it names the playlist that
        // produced it.
        out << "<span class='chip chip-mmr' style='color:" << CssColor(color) << "'>";
        if (!m_config.use_rank_icons)
            out << Escape(mmr > 0 ? Format::RankTier(tier, m_config.use_roman_numerals) : (p->fetched ? "Unranked" : "Fetching")) << ' ';
        else if (category == "best" && rankSource != "best")
            out << Escape(MmrLabel(StringToMmrCategory(rankSource))) << ' ';
        out << (mmr > 0 ? std::to_string(mmr) : (p->fetched ? "-" : "...")) << "</span>";

        if (mmr > 0 && matchCount > 0) out << "<span class='chip'>" << matchCount << (matchCount == 1 ? " match" : " matches") << "</span>";
        if (m_config.show_account_wins_overlay && p->totalWins >= 0) out << "<span class='chip'>" << p->totalWins << " wins</span>";
        // The chip binds to a stable per-player slot so telemetry updates only its text.
        const size_t slot = m_liveModel.PlayerSlot(p->primaryId);
        m_liveModel.SetPlayer(slot, ComputeLivePlayerStat(*p));
        out << "<span class='chip' data-if='players[" << slot << "].visible'>{{players[" << slot << "].text}}</span>";
        out << "</div></div>";

        // Trailing status column, one state per player: yourself, a player with
        // shared history, or someone new. A with/vs record against yourself is
        // meaningless, so YOU stands alone.
        const int withGames = p->lifetimeWinsWith + p->lifetimeLossesWith;
        const int againstGames = p->lifetimeWinsAgainst + p->lifetimeLossesAgainst;
        const bool hasEncounterRecord = p->hasLifetimeData && (withGames > 0 || againstGames > 0);
        out << "<div class='player-flags'>";
        if (self) {
            out << "<span class='badge accent'>YOU</span>";
        } else if (hasEncounterRecord) {
            if (withGames) out << "<div class='label'>with " << p->lifetimeWinsWith << '-' << p->lifetimeLossesWith << "</div>";
            if (againstGames) out << "<div class='label'>vs " << p->lifetimeWinsAgainst << '-' << p->lifetimeLossesAgainst << "</div>";
        } else {
            out << "<span class='badge'>NEW</span>";
        }
        out << "</div></div>";
    }
    out << "</div>";
    return out.str();
}
