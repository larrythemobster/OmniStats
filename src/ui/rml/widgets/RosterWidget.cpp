#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "core/game/RankProgress.hpp"
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

    RankProgress::Progress RankProgressFor(const PlayerData* player, const std::string& playlist, int mmr) {
        std::string reportedTier;
        if (player && playlist != "t") {
            const auto mmrIt = player->playlists.find(playlist);
            const auto tierIt = player->playlistTiers.find(playlist);
            if (mmrIt != player->playlists.end() && tierIt != player->playlistTiers.end() && mmrIt->second == mmr) reportedTier = tierIt->second;
        }
        return RankProgress::Compute(MMRFetcher::RankThresholdsForPlaylist(playlist), mmr, reportedTier);
    }

    std::string RenderRankProgress(const RankProgress::Progress& progress, const ConfigData& config) {
        using RankProgress::Status;
        if (progress.status == Status::Unavailable) return {};

        const auto rankName = [&](const std::string& label) { return Format::RankTier(label, config.use_roman_numerals); };
        const auto coloredName = [&](const std::string& label) {
            return "<span class='rank-progress-tier' style='color:" + CssColor(Format::RankColor(label)) + "'>" + Escape(rankName(label)) + "</span>";
        };
        const auto delta = [&](const char* tone, const std::string& value, const std::string& tip, bool alignRight) {
            return std::string("<span class='rank-progress-delta mono tooltip-host ") + tone + "'>" + Escape(value) + "<span class='tooltip-bubble" +
                   (alignRight ? " align-right" : "") + "'>" + Escape(tip) + "</span></span>";
        };

        std::ostringstream out;
        out << "<div class='rank-progress'><div class='row rank-progress-names'>" << coloredName(progress.rank);
        if (progress.status == Status::Placement) {
            out << "<span class='label'>placement</span></div>"
                << "<div class='label rank-progress-note'>Division progress appears once Rocket League assigns a rank.</div></div>";
            return out.str();
        }
        out << "<div class='grow'></div>";
        if (progress.next)
            out << coloredName(progress.next->label);
        else
            out << "<span class='label'>Top rank</span>";
        out << "</div><div class='row rank-progress-bar'>";

        std::string note;
        if (progress.floor && progress.previous) {
            const std::string floorText = rankName(progress.floor->label) + " (starts at " + std::to_string(progress.floor->mmr) + " MMR)";
            if (progress.aboveFloor > 0) {
                out << delta("loss", "-" + std::to_string(progress.aboveFloor),
                             "Lose " + std::to_string(progress.aboveFloor) + " MMR to reach lower boundary of " + floorText + ". Rocket League's demotion buffer can delay the drop to " +
                                 rankName(progress.previous->label) + ".",
                             false);
            } else if (progress.aboveFloor == 0) {
                out << delta("loss", "0",
                             "At lower boundary of " + floorText + ". Rocket League's demotion buffer can delay the drop to " +
                                 rankName(progress.previous->label) + ".",
                             false);
            } else {
                out << delta("neutral", "Grace",
                             "Rocket League gives you a grace period before demoting you, so falling below the MMR threshold does not immediately remove your rank.",
                             false);
                note = std::to_string(-progress.aboveFloor) + " MMR below the " + rankName(progress.floor->label) + " threshold";
            }
        }

        if (progress.floor && progress.next) {
            const float span = static_cast<float>(progress.next->mmr - progress.floor->mmr);
            const float position = span > 0.0f ? std::clamp(static_cast<float>(progress.mmr - progress.floor->mmr) / span, 0.0f, 1.0f) * 100.0f : 0.0f;
            ColorRGBA tint = Format::RankColor(progress.rank);
            const std::string solid = CssColor(tint);
            tint.a = 0.18f;
            out << "<div class='rank-progress-track' style='background-color:" << CssColor(tint) << "'><div class='rank-progress-fill' style='width:"
                << position << "%;background-color:" << solid << "'></div><div class='rank-progress-marker' style='left:" << position << "%'></div></div>";
        } else {
            out << "<div class='grow'></div>";
        }

        if (progress.next) {
            const std::string nextText = rankName(progress.next->label) + " (starts at " + std::to_string(progress.next->mmr) + " MMR)";
            if (progress.toNext <= 0) {
                out << delta("neutral", "Rank unchanged",
                             "You have reached the " + rankName(progress.next->label) + " MMR threshold, but your reported division has not changed.",
                             true);
                note = progress.toNext == 0
                           ? "At the " + rankName(progress.next->label) + " MMR threshold"
                           : std::to_string(progress.mmr - progress.next->mmr) + " MMR above the " + rankName(progress.next->label) + " threshold";
            } else {
                out << delta("win", "+" + std::to_string(progress.toNext),
                             "Gain " + std::to_string(progress.toNext) + " MMR to reach " + nextText + ".", true);
            }
        }
        out << "</div>";
        if (!note.empty()) {
            out << "<div class='label rank-progress-note'>" << Escape(note) << "</div>";
        }
        out << "</div>";
        return out.str();
    }
}

std::string RmlUiController::BestRankTier(const PlayerData& p) const {
    if (!p.rankTier.empty() && p.rankTier != "Unranked") return p.rankTier;
    if (p.mmr <= 0) return {};
    return MMRFetcher::GetRankTierForPlaylistMmr(BestRankSource(p, p.mmr, m_config.show_extra_playlists), p.mmr);
}

std::string OverlayPresenter::RenderPlayerRoster(int team, const char* label, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state) {
    std::vector<const PlayerData*> players;
    const std::string category = MmrCategoryToString(state ? state->ui.rosterMmrCategory.load() : MmrCategory::Best);
    for (const auto& [_, player] : snap.roster)
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
    const bool ranksOn = RankSourceAvailable(config);
    for (const auto* p : players) {
        const bool settled = p->fetched || !ranksOn;
        const bool self = !snap.myPrimaryId.empty() && p->primaryId == snap.myPrimaryId;
        const int mmr = mmrFor(*p);
        std::string tier = p->rankTier;
        std::string rankSource = category;
        if (category != "best") {
            if (auto it = p->playlistTiers.find(category); it != p->playlistTiers.end())
                tier = it->second;
            else
                tier = "Unranked";
        } else if (mmr > 0) {
            rankSource = BestRankSource(*p, mmr, config.show_extra_playlists);
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
        if (config.use_rank_icons) {
            const std::string rankTooltip = mmr > 0 || p->fetched ? Format::RankTier(tier, config.use_roman_numerals)
                                            : ranksOn             ? "Fetching rank..."
                                                                  : "Ranks are off";
            const int tierIndex = (settled || mmr > 0) ? TierResourceIndex(tier) : 23;
            const int division = (settled || mmr > 0) ? DivisionLevel(tier) : 0;
            const int filledDivisionIndex = DivisionColorResourceIndex(tier);
            out << "<div class='player-crest'><span class='rank-badge tooltip-host'><img class='rank-icon' src='res://images/Tiers/" << tierIndex << ".png'/>";
            if (division > 0) {
                out << "<span class='division-stack'>";
                for (int d = 4; d >= 1; --d) {
                    const int resource = d <= division ? filledDivisionIndex : 0;
                    out << "<img class='division-pill' src='res://images/Divisions/" << resource << ".png'/>";
                }
                out << "</span>";
            }
            if (!rankTooltip.empty()) out << "<span class='tooltip-bubble'>" << Escape(rankTooltip) << "</span>";
            out << "</span></div>";
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
        if (!self) {
            if (PlayerNotes::IdentityFromPrimaryId(p->primaryId)) {
                const std::string attrs = " data-action='player-note-open' data-pid='" + Escape(p->primaryId) + "' data-name='" + Escape(p->name) + "'";
                const auto identity = PlayerNotes::IdentityFromPrimaryId(p->primaryId);
                auto noteIt = identity ? snap.notePreviews.find(identity->Key()) : snap.notePreviews.end();
                if (noteIt != snap.notePreviews.end()) {
                    out << "<span class='note-badge tooltip-host'" + attrs + ">SCOUT<span class='tooltip-bubble note-preview'>" + Escape(noteIt->second) + "</span></span>";
                } else if (snap.notesLoaded) {
                    out << "<span class='note-add'" + attrs + ">Scout</span>";
                }
            }
        }
        out << "</div><div class='player-chips'>";

        // MMR chip. Without rank crests it also has to name the tier, and when
        // the roster shows the player's best rank it names the playlist that
        // produced it.
        out << "<span class='chip chip-mmr' style='color:" << CssColor(color) << "'>";
        if (!config.use_rank_icons)
            out << Escape(mmr > 0 ? Format::RankTier(tier, config.use_roman_numerals) : (p->fetched ? "Unranked" : ranksOn ? "Fetching"
                                                                                                                           : "No rank"))
                << ' ';
        else if (category == "best" && rankSource != "best")
            out << Escape(MmrLabel(StringToMmrCategory(rankSource))) << ' ';
        out << (mmr > 0 ? std::to_string(mmr) : (settled ? "-" : "...")) << "</span>";
        if (mmr > 0 && matchCount > 0) out << "<span class='chip'>" << matchCount << (matchCount == 1 ? " match" : " matches") << "</span>";
        if (config.show_account_wins_overlay && p->totalWins >= 0) out << "<span class='chip'>" << p->totalWins << " wins</span>";
        if (!self) {
            if (const auto identity = PlayerNotes::IdentityFromPrimaryId(p->primaryId)) {
                auto tagIt = snap.noteTags.find(identity->Key());
                if (tagIt != snap.noteTags.end()) {
                    for (size_t t = 0; t < tagIt->second.size() && t < 2; ++t)
                        out << "<span class='chip chip-tag'>" + Escape(tagIt->second[t]) + "</span>";
                    if (tagIt->second.size() > 2) out << "<span class='chip chip-tag'>+" + std::to_string(tagIt->second.size() - 2) + "</span>";
                }
            }
        }
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

std::string RmlUiController::RenderPlayerRoster(int team, const char* label) {
    return m_overlayPresenter->RenderPlayerRoster(team, label, m_snap, m_config, m_state);
}

std::string OverlayPresenter::RenderSelfRankProgress(bool card, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state) {
    const PlayerData* self = nullptr;
    if (!snap.myPrimaryId.empty()) {
        if (auto it = snap.roster.find(snap.myPrimaryId); it != snap.roster.end()) self = &it->second;
    }
    const std::string category = MmrCategoryToString(state ? state->ui.rankProgressMmrCategory.load() : MmrCategory::TwoVTwo);
    std::string playlist = category;
    int mmr = 0;
    if (self && category == "best") {
        mmr = self->mmr;
        if (mmr > 0) playlist = BestRankSource(*self, mmr, config.show_extra_playlists);
    } else if (self) {
        if (auto rating = self->playlists.find(category); rating != self->playlists.end()) mmr = rating->second;
    }
    const std::string progress = RenderRankProgress(RankProgressFor(self, playlist, mmr), config);
    if (!card) return progress.empty() ? std::string{} : "<div class='roster-rank-progress'>" + progress + "</div>";

    const std::string label = MmrLabel(StringToMmrCategory(playlist));
    if (progress.empty()) return "<div class='label'>No rank to show for " + Escape(label) + ".</div>";
    return "<div class='row rank-progress-card-head'><span class='badge'>" + Escape(label) + "</span></div>" + progress;
}

std::string RmlUiController::RenderSelfRankProgress(bool card) const {
    return m_overlayPresenter->RenderSelfRankProgress(card, m_snap, m_config, m_state);
}
