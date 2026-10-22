#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

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

namespace {
    // `best` is a presentation bucket, not a Rocket League playlist: name the
    // real playlist that supplied the rating, or "best" when none matches.
    std::string BestRankSource(const PlayerData& p, int mmr, bool extraPlaylists) {
        const auto matchesMmr = [&](const char* key) {
            auto it = p.playlists.find(key);
            return it != p.playlists.end() && it->second == mmr;
        };
        for (const char* key : {"2v2", "3v3", "1v1"})
            if (matchesMmr(key)) return key;
        if (extraPlaylists)
            for (const char* key : {"hoops", "rumble", "dropshot", "snowday", "heatseeker"})
                if (matchesMmr(key)) return key;
        return "best";
    }
}

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

const std::string* RmlUiController::NotePreviewFor(const std::string& primaryId) const {
    const auto identity = PlayerNotes::IdentityFromPrimaryId(primaryId);
    if (!identity) return nullptr;
    const auto it = m_snap.notePreviews.find(identity->Key());
    return it == m_snap.notePreviews.end() ? nullptr : &it->second;
}

std::string RmlUiController::RenderNoteControl(const std::string& primaryId, const std::string& name) const {
    if (!PlayerNotes::IdentityFromPrimaryId(primaryId)) return {};
    const std::string attrs = " data-action='player-note-open' data-pid='" + Escape(primaryId) + "' data-name='" + Escape(name) + "'";
    if (const std::string* preview = NotePreviewFor(primaryId)) {
        return "<span class='note-badge tooltip-host'" + attrs + ">SCOUT<span class='tooltip-bubble note-preview'>" + Escape(*preview) +
               "</span></span>";
    }
    if (!m_snap.notesLoaded) return {};
    return "<span class='note-add'" + attrs + ">Scout</span>";
}

std::string RmlUiController::RenderTagChips(const std::string& primaryId) const {
    const auto identity = PlayerNotes::IdentityFromPrimaryId(primaryId);
    if (!identity) return {};
    const auto it = m_snap.noteTags.find(identity->Key());
    if (it == m_snap.noteTags.end()) return {};
    const auto& tags = it->second;
    std::string out;
    for (size_t i = 0; i < tags.size() && i < 2; ++i)
        out += "<span class='chip chip-tag'>" + Escape(tags[i]) + "</span>";
    if (tags.size() > 2) out += "<span class='chip chip-tag'>+" + std::to_string(tags.size() - 2) + "</span>";
    return out;
}

std::string RmlUiController::BestRankTier(const PlayerData& p) const {
    if (!p.rankTier.empty() && p.rankTier != "Unranked") return p.rankTier;
    if (p.mmr <= 0) return {};
    return MMRFetcher::GetRankTierForPlaylistMmr(BestRankSource(p, p.mmr, m_config.show_extra_playlists), p.mmr);
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
    const bool ranksOn = RankSourceAvailable();
    for (const auto* p : players) {
        const bool settled = p->fetched || !ranksOn;
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
            rankSource = BestRankSource(*p, mmr, m_config.show_extra_playlists);
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
            const std::string rankTooltip = mmr > 0 || p->fetched ? Format::RankTier(tier, m_config.use_roman_numerals)
                                            : ranksOn             ? "Fetching rank..."
                                                                  : "Ranks are off";
            out << "<div class='player-crest'>" << RenderRankBadge(tier, settled || mmr > 0, rankTooltip) << "</div>";
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
        if (!self) out << RenderNoteControl(p->primaryId, p->name);
        out << "</div><div class='player-chips'>";

        // MMR chip. Without rank crests it also has to name the tier, and when
        // the roster shows the player's best rank it names the playlist that
        // produced it.
        out << "<span class='chip chip-mmr' style='color:" << CssColor(color) << "'>";
        if (!m_config.use_rank_icons)
            out << Escape(mmr > 0 ? Format::RankTier(tier, m_config.use_roman_numerals) : (p->fetched ? "Unranked" : ranksOn ? "Fetching"
                                                                                                                             : "No rank"))
                << ' ';
        else if (category == "best" && rankSource != "best")
            out << Escape(MmrLabel(StringToMmrCategory(rankSource))) << ' ';
        out << (mmr > 0 ? std::to_string(mmr) : (settled ? "-" : "...")) << "</span>";

        if (mmr > 0 && matchCount > 0) out << "<span class='chip'>" << matchCount << (matchCount == 1 ? " match" : " matches") << "</span>";
        if (m_config.show_account_wins_overlay && p->totalWins >= 0) out << "<span class='chip'>" << p->totalWins << " wins</span>";
        if (!self) out << RenderTagChips(p->primaryId);
        out << "</div></div>";

        // Trailing status column: yourself, both shared-history records, or a
        // confirmed first meeting. Nothing is shown until the record has loaded,
        // so a slow or failed lookup is never presented as NEW.
        const int withGames = p->lifetimeWinsWith + p->lifetimeLossesWith;
        const int againstGames = p->lifetimeWinsAgainst + p->lifetimeLossesAgainst;
        out << "<div class='player-flags'>";
        if (self) {
            out << "<span class='badge accent'>YOU</span>";
        } else if (p->hasLifetimeData && (withGames > 0 || againstGames > 0)) {
            if (withGames)
                out << "<div class='label encounter tooltip-host'>with " << FormatRecord(p->lifetimeWinsWith, p->lifetimeLossesWith)
                    << "<span class='tooltip-bubble align-right'>As teammates: " << Escape(EncounterDetail(p->lifetimeWinsWith, p->lifetimeLossesWith))
                    << "</span></div>";
            if (againstGames)
                out << "<div class='label encounter tooltip-host'>vs " << FormatRecord(p->lifetimeWinsAgainst, p->lifetimeLossesAgainst)
                    << "<span class='tooltip-bubble align-right'>As opponents: " << Escape(EncounterDetail(p->lifetimeWinsAgainst, p->lifetimeLossesAgainst))
                    << "</span></div>";
        } else if (p->hasLifetimeData) {
            out << "<span class='badge'>NEW</span>";
        }
        out << "</div></div>";
    }
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSelfRankProgress(bool card) const {
    const PlayerData* self = nullptr;
    if (!m_snap.myPrimaryId.empty()) {
        if (auto it = m_snap.roster.find(m_snap.myPrimaryId); it != m_snap.roster.end()) self = &it->second;
    }
    const std::string category = MmrCategoryToString(m_state ? m_state->ui.rankProgressMmrCategory.load() : MmrCategory::TwoVTwo);
    std::string playlist = category;
    int mmr = 0;
    if (self && category == "best") {
        mmr = self->mmr;
        if (mmr > 0) playlist = BestRankSource(*self, mmr, m_config.show_extra_playlists);
    } else if (self) {
        if (auto rating = self->playlists.find(category); rating != self->playlists.end()) mmr = rating->second;
    }
    const std::string progress = RenderRankProgress(RankProgressFor(self, playlist, mmr));
    if (!card) return progress.empty() ? std::string{} : "<div class='roster-rank-progress'>" + progress + "</div>";

    const std::string label = MmrLabel(StringToMmrCategory(playlist));
    if (progress.empty()) return "<div class='label'>No rank to show for " + Escape(label) + ".</div>";
    return "<div class='row rank-progress-card-head'><span class='badge'>" + Escape(label) + "</span></div>" + progress;
}
