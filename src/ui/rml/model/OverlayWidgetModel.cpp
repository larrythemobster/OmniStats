#include "ui/rml/model/OverlayWidgetModel.hpp"
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <ctime>
#include "core/game/StatsScope.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlRenderSnapshot.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "ui/rml/widgets/TournamentWidget.hpp"
using namespace RmlUiDetail;

namespace {
    std::string BestRankSourceHelper(const PlayerData& p, int mmr, bool extraPlaylists) {
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

    constexpr int kVisibleGraphMatches = 25;
}

bool OverlayWidgetModel::Bind(Rml::DataModelConstructor& constructor) {
    if (!constructor) return false;

    if (auto dot = constructor.RegisterStruct<RosterDivisionDot>()) {
        dot.RegisterMember("filled", &RosterDivisionDot::filled);
        dot.RegisterMember("active", &RosterDivisionDot::active);
    }
    constructor.RegisterArray<std::vector<RosterDivisionDot>>();

    if (auto p = constructor.RegisterStruct<RosterPlayerItem>()) {
        p.RegisterMember("self", &RosterPlayerItem::self);
        p.RegisterMember("name", &RosterPlayerItem::name);
        p.RegisterMember("tracker_url", &RosterPlayerItem::tracker_url);
        p.RegisterMember("has_tracker", &RosterPlayerItem::has_tracker);
        p.RegisterMember("platform", &RosterPlayerItem::platform);
        p.RegisterMember("platform_class", &RosterPlayerItem::platform_class);
        p.RegisterMember("has_platform", &RosterPlayerItem::has_platform);
        p.RegisterMember("primary_id", &RosterPlayerItem::primary_id);
        p.RegisterMember("can_note", &RosterPlayerItem::can_note);
        p.RegisterMember("has_note", &RosterPlayerItem::has_note);
        p.RegisterMember("note_preview", &RosterPlayerItem::note_preview);
        p.RegisterMember("use_rank_icons", &RosterPlayerItem::use_rank_icons);
        p.RegisterMember("tier_icon", &RosterPlayerItem::tier_icon);
        p.RegisterMember("has_division", &RosterPlayerItem::has_division);
        p.RegisterMember("division_dots", &RosterPlayerItem::division_dots);
        p.RegisterMember("rank_tooltip", &RosterPlayerItem::rank_tooltip);
        p.RegisterMember("has_tooltip", &RosterPlayerItem::has_tooltip);
        p.RegisterMember("tier_color", &RosterPlayerItem::tier_color);
        p.RegisterMember("tier_label", &RosterPlayerItem::tier_label);
        p.RegisterMember("mmr_text", &RosterPlayerItem::mmr_text);
        p.RegisterMember("match_count_text", &RosterPlayerItem::match_count_text);
        p.RegisterMember("has_match_count", &RosterPlayerItem::has_match_count);
        p.RegisterMember("total_wins_text", &RosterPlayerItem::total_wins_text);
        p.RegisterMember("has_total_wins", &RosterPlayerItem::has_total_wins);
        p.RegisterMember("tags", &RosterPlayerItem::tags);
        p.RegisterMember("has_tags", &RosterPlayerItem::has_tags);
        p.RegisterMember("flag_kind", &RosterPlayerItem::flag_kind);
        p.RegisterMember("with_record", &RosterPlayerItem::with_record);
        p.RegisterMember("with_detail", &RosterPlayerItem::with_detail);
        p.RegisterMember("against_record", &RosterPlayerItem::against_record);
        p.RegisterMember("against_detail", &RosterPlayerItem::against_detail);
    }
    constructor.RegisterArray<std::vector<RosterPlayerItem>>();
    constructor.RegisterArray<std::vector<Rml::String>>();

    if (auto c = constructor.RegisterStruct<LobbyRankCell>()) {
        c.RegisterMember("text", &LobbyRankCell::text);
        c.RegisterMember("color", &LobbyRankCell::color);
        c.RegisterMember("tooltip", &LobbyRankCell::tooltip);
    }
    constructor.RegisterArray<std::vector<LobbyRankCell>>();

    if (auto r = constructor.RegisterStruct<LobbyRankRow>()) {
        r.RegisterMember("name", &LobbyRankRow::name);
        r.RegisterMember("cells", &LobbyRankRow::cells);
    }
    constructor.RegisterArray<std::vector<LobbyRankRow>>();

    if (auto col = constructor.RegisterStruct<LobbyRankColumn>()) {
        col.RegisterMember("label", &LobbyRankColumn::label);
    }
    constructor.RegisterArray<std::vector<LobbyRankColumn>>();

    if (auto g = constructor.RegisterStruct<PreviousGameItem>()) {
        g.RegisterMember("match_id", &PreviousGameItem::match_id);
        g.RegisterMember("match_guid", &PreviousGameItem::match_guid);
        g.RegisterMember("playlist", &PreviousGameItem::playlist);
        g.RegisterMember("score", &PreviousGameItem::score);
        g.RegisterMember("mmr", &PreviousGameItem::mmr);
        g.RegisterMember("time", &PreviousGameItem::time);
        g.RegisterMember("win_loss_class", &PreviousGameItem::win_loss_class);
        g.RegisterMember("even_odd_class", &PreviousGameItem::even_odd_class);
    }
    constructor.RegisterArray<std::vector<PreviousGameItem>>();

    if (auto b = constructor.RegisterStruct<GraphBandItem>()) {
        b.RegisterMember("left", &GraphBandItem::left);
        b.RegisterMember("top", &GraphBandItem::top);
        b.RegisterMember("width", &GraphBandItem::width);
        b.RegisterMember("height", &GraphBandItem::height);
        b.RegisterMember("color", &GraphBandItem::color);
        b.RegisterMember("label", &GraphBandItem::label);
        b.RegisterMember("label_color", &GraphBandItem::label_color);
        b.RegisterMember("label_left", &GraphBandItem::label_left);
        b.RegisterMember("label_top", &GraphBandItem::label_top);
        b.RegisterMember("has_label", &GraphBandItem::has_label);
    }
    constructor.RegisterArray<std::vector<GraphBandItem>>();

    if (auto gl = constructor.RegisterStruct<GraphGridlineItem>()) {
        gl.RegisterMember("left", &GraphGridlineItem::left);
        gl.RegisterMember("top", &GraphGridlineItem::top);
        gl.RegisterMember("width", &GraphGridlineItem::width);
        gl.RegisterMember("boundary", &GraphGridlineItem::boundary);
    }
    constructor.RegisterArray<std::vector<GraphGridlineItem>>();

    if (auto lbl = constructor.RegisterStruct<GraphLabelItem>()) {
        lbl.RegisterMember("right", &GraphLabelItem::right);
        lbl.RegisterMember("top", &GraphLabelItem::top);
        lbl.RegisterMember("text", &GraphLabelItem::text);
    }
    constructor.RegisterArray<std::vector<GraphLabelItem>>();

    if (auto rl = constructor.RegisterStruct<GraphRankLineItem>()) {
        rl.RegisterMember("left", &GraphRankLineItem::left);
        rl.RegisterMember("top", &GraphRankLineItem::top);
        rl.RegisterMember("width", &GraphRankLineItem::width);
        rl.RegisterMember("color", &GraphRankLineItem::color);
        rl.RegisterMember("text", &GraphRankLineItem::text);
        rl.RegisterMember("tier_start", &GraphRankLineItem::tier_start);
        rl.RegisterMember("below", &GraphRankLineItem::below);
    }
    constructor.RegisterArray<std::vector<GraphRankLineItem>>();

    if (auto bl = constructor.RegisterStruct<GraphBaselineDashItem>()) {
        bl.RegisterMember("left", &GraphBaselineDashItem::left);
        bl.RegisterMember("top", &GraphBaselineDashItem::top);
        bl.RegisterMember("width", &GraphBaselineDashItem::width);
    }
    constructor.RegisterArray<std::vector<GraphBaselineDashItem>>();

    if (auto d = constructor.RegisterStruct<GraphDotItem>()) {
        d.RegisterMember("left", &GraphDotItem::left);
        d.RegisterMember("top", &GraphDotItem::top);
        d.RegisterMember("cls", &GraphDotItem::cls);
        d.RegisterMember("tip", &GraphDotItem::tip);
        d.RegisterMember("label", &GraphDotItem::label);
        d.RegisterMember("color", &GraphDotItem::color);
        d.RegisterMember("has_label", &GraphDotItem::has_label);
        d.RegisterMember("estimated", &GraphDotItem::estimated);
    }
    constructor.RegisterArray<std::vector<GraphDotItem>>();

    if (auto gm = constructor.RegisterStruct<GamemodeBreakdownRow>()) {
        gm.RegisterMember("mode", &GamemodeBreakdownRow::mode);
        gm.RegisterMember("record", &GamemodeBreakdownRow::record);
        gm.RegisterMember("pct", &GamemodeBreakdownRow::pct);
        gm.RegisterMember("total", &GamemodeBreakdownRow::total);
    }
    constructor.RegisterArray<std::vector<GamemodeBreakdownRow>>();

    constructor.Bind("roster_blue", &m_rosterBlue);
    constructor.Bind("roster_orange", &m_rosterOrange);
    constructor.Bind("roster_waiting_blue", &m_rosterWaitingBlue);
    constructor.Bind("roster_waiting_orange", &m_rosterWaitingOrange);
    constructor.Bind("roster_rank_progress", &m_rosterRankProgress);

    constructor.Bind("lobby_columns", &m_lobbyColumns);
    constructor.Bind("lobby_players", &m_lobbyPlayers);
    constructor.Bind("lobby_waiting", &m_lobbyRanksWaiting);

    constructor.Bind("previous_games", &m_previousGames);
    constructor.Bind("previous_games_loading", &m_previousGamesLoading);
    constructor.Bind("previous_games_empty", &m_previousGamesEmpty);
    constructor.Bind("previous_games_footer_session", &m_previousGamesFooterSession);

    constructor.Bind("graph_empty", &m_graphEmpty);
    constructor.Bind("graph_empty_message", &m_graphEmptyMessage);
    constructor.Bind("graph_empty_submessage", &m_graphEmptySubmessage);
    constructor.Bind("graph_category", &m_graphCategory);
    constructor.Bind("graph_points", &m_graphPoints);
    constructor.Bind("graph_fill_to", &m_graphFillTo);
    constructor.Bind("graph_bands", &m_graphBands);
    constructor.Bind("graph_gridlines", &m_graphGridlines);
    constructor.Bind("graph_labels", &m_graphLabels);
    constructor.Bind("graph_rank_lines", &m_graphRankLines);
    constructor.Bind("graph_baselines", &m_graphBaselines);
    constructor.Bind("graph_dots", &m_graphDots);
    constructor.Bind("graph_pan_visible", &m_graphPanVisible);
    constructor.Bind("graph_pan_label", &m_graphPanLabel);
    constructor.Bind("graph_pan_older_disabled", &m_graphPanOlderDisabled);
    constructor.Bind("graph_pan_newer_disabled", &m_graphPanNewerDisabled);

    constructor.Bind("streaks_current", &m_streaks.current_streak);
    constructor.Bind("streaks_current_class", &m_streaks.current_streak_class);
    constructor.Bind("streaks_longest_win", &m_streaks.longest_win);
    constructor.Bind("streaks_longest_loss", &m_streaks.longest_loss);
    constructor.Bind("streaks_show_longest_loss", &m_streaks.show_longest_loss);

    constructor.Bind("gamemode_rows", &m_gamemodeRows);
    constructor.Bind("gamemode_empty", &m_gamemodeEmpty);

    if (auto s = constructor.RegisterStruct<StatRowItem>()) {
        s.RegisterMember("label", &StatRowItem::label);
        s.RegisterMember("value", &StatRowItem::value);
        s.RegisterMember("cls", &StatRowItem::cls);
    }
    constructor.RegisterArray<std::vector<StatRowItem>>();

    constructor.Bind("tournament_status_text", &m_tournament.status_text);
    constructor.Bind("tournament_has_next", &m_tournament.has_next);
    constructor.Bind("tournament_title", &m_tournament.title);
    constructor.Bind("tournament_region", &m_tournament.region);
    constructor.Bind("tournament_countdown", &m_tournament.countdown);
    constructor.Bind("tournament_starting_soon", &m_tournament.starting_soon);

    constructor.Bind("match_summary_visible", &m_matchSummary.visible);
    constructor.Bind("match_summary_result", &m_matchSummary.result);
    constructor.Bind("match_summary_result_class", &m_matchSummary.result_class);
    constructor.Bind("match_summary_ot_tag", &m_matchSummary.ot_tag);
    constructor.Bind("match_summary_score", &m_matchSummary.score);
    constructor.Bind("match_summary_mmr_delta", &m_matchSummary.mmr_delta);
    constructor.Bind("match_summary_mmr_tone_class", &m_matchSummary.mmr_tone_class);
    constructor.Bind("match_summary_rating", &m_matchSummary.rating);
    constructor.Bind("match_summary_play_stats", &m_matchSummary.play_stats);
    constructor.Bind("match_summary_fun_stats", &m_matchSummary.fun_stats);
    m_handle = constructor.GetModelHandle();
    m_bound = bool(m_handle);
    return true;
}

bool OverlayWidgetModel::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("overlay_widgets");
    return Bind(constructor);
}

void OverlayWidgetModel::Reset() {
    m_handle = {};
    m_bound = false;
}

void OverlayWidgetModel::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

bool OverlayWidgetModel::Update(const RmlRenderSnapshot& snap,
                                const ConfigData& config,
                                const std::shared_ptr<SessionState>& state,
                                const Tournaments::TournamentScheduleService* tournaments) {
    bool changed = false;

    // 1. Roster
    const std::string category = MmrCategoryToString(state ? state->ui.rosterMmrCategory.load() : MmrCategory::Best);
    const bool ranksOn = RankSourceAvailable(config);

    auto buildRosterTeam = [&](int team, std::vector<RosterPlayerItem>& target, bool& waitingTarget, const char* teamVar, const char* waitingVar) {
        std::vector<const PlayerData*> players;
        for (const auto& [_, player] : snap.roster) {
            if (player.team == team) players.push_back(&player);
        }
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

        std::vector<RosterPlayerItem> nextItems;
        nextItems.reserve(players.size());
        for (const auto* p : players) {
            RosterPlayerItem item;
            const bool settled = p->fetched || !ranksOn;
            item.self = !snap.myPrimaryId.empty() && p->primaryId == snap.myPrimaryId;
            item.name = p->name;
            const int mmr = mmrFor(*p);
            std::string tier = p->rankTier;
            std::string rankSource = category;
            if (category != "best") {
                if (auto it = p->playlistTiers.find(category); it != p->playlistTiers.end())
                    tier = it->second;
                else
                    tier = "Unranked";
            } else if (mmr > 0) {
                rankSource = BestRankSourceHelper(*p, mmr, config.show_extra_playlists);
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

            item.use_rank_icons = config.use_rank_icons;
            if (item.use_rank_icons) {
                const int tierIndex = (settled || mmr > 0) ? TierResourceIndex(tier) : 23;
                item.tier_icon = "res://images/Tiers/" + std::to_string(tierIndex) + ".png";
                const int div = (settled || mmr > 0) ? DivisionLevel(tier) : 0;
                item.has_division = (div > 0);
                if (div > 0) {
                    for (int d = 1; d <= 4; ++d) {
                        item.division_dots.push_back({d <= div, d == div});
                    }
                }
                item.rank_tooltip = mmr > 0 || p->fetched ? Format::RankTier(tier, config.use_roman_numerals)
                                    : ranksOn             ? "Fetching rank..."
                                                          : "Ranks are off";
                item.has_tooltip = !item.rank_tooltip.empty();
            }

            item.tracker_url = TrackerUrlForPlayer(*p);
            item.has_tracker = !item.tracker_url.empty();

            if (!platform.empty()) {
                const PlatformKind platformKind = PlatformKindFor(platform);
                item.platform = PlatformBadgeLabel(platformKind, platform);
                item.platform_class = std::string("badge ") + PlatformClass(platformKind);
                item.has_platform = true;
            }

            item.primary_id = p->primaryId;
            item.can_note = !item.self;
            // Notes preview
            const auto identity = PlayerNotes::IdentityFromPrimaryId(p->primaryId);
            if (identity) {
                auto it = snap.notePreviews.find(identity->Key());
                if (it != snap.notePreviews.end()) {
                    item.has_note = true;
                    item.note_preview = it->second;
                }
            }

            item.tier_color = CssColor(color);
            item.tier_label = Format::RankTier(tier, config.use_roman_numerals);
            item.mmr_text = (mmr > 0 ? std::to_string(mmr) : (settled ? "-" : "..."));

            if (mmr > 0 && matchCount > 0) {
                item.match_count_text = std::to_string(matchCount) + (matchCount == 1 ? " match" : " matches");
                item.has_match_count = true;
            }
            if (config.show_account_wins_overlay && p->totalWins >= 0) {
                item.total_wins_text = std::to_string(p->totalWins) + " wins";
                item.has_total_wins = true;
            }

            // Tags
            if (identity) {
                auto it = snap.noteTags.find(identity->Key());
                if (it != snap.noteTags.end()) {
                    for (const auto& tag : it->second) {
                        item.tags.push_back(tag);
                    }
                    item.has_tags = !item.tags.empty();
                }
            }

            // Flags
            const int withGames = p->lifetimeWinsWith + p->lifetimeLossesWith;
            const int againstGames = p->lifetimeWinsAgainst + p->lifetimeLossesAgainst;
            if (item.self) {
                item.flag_kind = 1;
            } else if (p->hasLifetimeData && (withGames > 0 || againstGames > 0)) {
                item.flag_kind = 2;
                if (withGames) {
                    item.with_record = "with " + FormatRecord(p->lifetimeWinsWith, p->lifetimeLossesWith);
                    item.with_detail = "As teammates: " + EncounterDetail(p->lifetimeWinsWith, p->lifetimeLossesWith);
                }
                if (againstGames) {
                    item.against_record = "vs " + FormatRecord(p->lifetimeWinsAgainst, p->lifetimeLossesAgainst);
                    item.against_detail = "As opponents: " + EncounterDetail(p->lifetimeWinsAgainst, p->lifetimeLossesAgainst);
                }
            } else if (p->hasLifetimeData) {
                item.flag_kind = 5;
            }
            nextItems.push_back(std::move(item));
        }

        const bool nextWaiting = nextItems.empty();
        if (waitingTarget != nextWaiting) {
            waitingTarget = nextWaiting;
            Dirty(waitingVar);
            changed = true;
        }

        // Compare items
        bool teamChanged = (target.size() != nextItems.size());
        if (!teamChanged) {
            for (size_t i = 0; i < target.size(); ++i) {
                if (target[i].name != nextItems[i].name || target[i].mmr_text != nextItems[i].mmr_text ||
                    target[i].tier_color != nextItems[i].tier_color || target[i].has_note != nextItems[i].has_note ||
                    target[i].flag_kind != nextItems[i].flag_kind || target[i].match_count_text != nextItems[i].match_count_text) {
                    teamChanged = true;
                    break;
                }
            }
        }
        if (teamChanged) {
            target = std::move(nextItems);
            Dirty(teamVar);
            changed = true;
        }
    };

    buildRosterTeam(0, m_rosterBlue, m_rosterWaitingBlue, "roster_blue", "roster_waiting_blue");
    buildRosterTeam(1, m_rosterOrange, m_rosterWaitingOrange, "roster_orange", "roster_waiting_orange");

    // 2. Previous games
    const bool prevLoading = !snap.recentSavedMatchesLoaded;
    const bool prevEmpty = snap.recentSavedMatches.empty();
    if (m_previousGamesLoading != prevLoading) {
        m_previousGamesLoading = prevLoading;
        Dirty("previous_games_loading");
        changed = true;
    }
    if (m_previousGamesEmpty != prevEmpty) {
        m_previousGamesEmpty = prevEmpty;
        Dirty("previous_games_empty");
        changed = true;
    }
    std::vector<PreviousGameItem> nextGames;
    const int configuredLimit = std::clamp(config.previous_games_limit, 10, kPreviousGamesMaxLimit);
    const int limit = std::min<int>(configuredLimit, static_cast<int>(snap.recentSavedMatches.size()));
    nextGames.reserve(limit);
    for (int i = 0; i < limit; ++i) {
        const auto& match = snap.recentSavedMatches[static_cast<size_t>(i)];
        PreviousGameItem item;
        item.match_id = match.matchId;
        item.match_guid = match.matchGuid;
        item.playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
        item.score = std::to_string(match.ourScore) + "-" + std::to_string(match.theirScore);
        item.mmr = (match.pendingTrackerConfirmation ? "***" : (match.mmrEstimated && match.mmr > 0 ? "~" + std::to_string(match.mmr) : (match.mmr > 0 ? std::to_string(match.mmr) : "--")));
        item.time = FormatClock(match.endedAtUnix);
        item.win_loss_class = match.win ? "match-win" : "match-loss";
        item.even_odd_class = (i % 2 == 0 ? "match-even" : "match-odd");
        nextGames.push_back(std::move(item));
    }
    bool gamesChanged = (m_previousGames.size() != nextGames.size());
    if (!gamesChanged) {
        for (size_t i = 0; i < m_previousGames.size(); ++i) {
            if (m_previousGames[i].match_id != nextGames[i].match_id ||
                m_previousGames[i].score != nextGames[i].score ||
                m_previousGames[i].mmr != nextGames[i].mmr) {
                gamesChanged = true;
                break;
            }
        }
    }
    if (gamesChanged) {
        m_previousGames = std::move(nextGames);
        Dirty("previous_games");
        changed = true;
    }
    // 3. Lobby Ranks
    struct PlaylistDef {
        const char* key;
        const char* label;
        bool show;
    };
    const std::vector<PlaylistDef> playlistDefs = {
        {"1v1", "1v1", config.show_lobby_rank_1v1},
        {"2v2", "2v2", config.show_lobby_rank_2v2},
        {"3v3", "3v3", config.show_lobby_rank_3v3},
        {"casual", "Casual", config.show_lobby_rank_casual},
        {"t", "Tourney", config.show_lobby_rank_tourny},
        {"hoops", "Hoops", config.show_extra_playlists && config.show_lobby_rank_hoops},
        {"rumble", "Rumble", config.show_extra_playlists && config.show_lobby_rank_rumble},
        {"dropshot", "Dropshot", config.show_extra_playlists && config.show_lobby_rank_dropshot},
        {"snowday", "Snow", config.show_extra_playlists && config.show_lobby_rank_snowday},
        {"heatseeker", "Heat", config.show_extra_playlists && config.show_lobby_rank_heatseeker}};

    std::vector<LobbyRankColumn> nextCols;
    for (const auto& pl : playlistDefs) {
        if (pl.show) nextCols.push_back({pl.label});
    }
    if (m_lobbyColumns.size() != nextCols.size() || m_lobbyColumns != nextCols) {
        m_lobbyColumns = std::move(nextCols);
        Dirty("lobby_columns");
        changed = true;
    }

    std::vector<const PlayerData*> lobbyPlayers;
    for (const auto& [_, p] : snap.roster)
        lobbyPlayers.push_back(&p);
    std::sort(lobbyPlayers.begin(), lobbyPlayers.end(), [](const PlayerData* a, const PlayerData* b) {
        if (a->team != b->team) return a->team < b->team;
        auto mmr = [](const PlayerData* p) { auto it = p->playlists.find("2v2"); return it == p->playlists.end() ? p->mmr : it->second; };
        if (mmr(a) != mmr(b)) return mmr(a) > mmr(b);
        return a->name < b->name;
    });

    std::vector<LobbyRankRow> nextRows;
    nextRows.reserve(lobbyPlayers.size());
    for (const auto* p : lobbyPlayers) {
        LobbyRankRow row;
        row.name = p->name;
        for (const auto& pl : playlistDefs) {
            if (!pl.show) continue;
            int mmr = 0;
            if (auto it = p->playlists.find(pl.key); it != p->playlists.end()) mmr = it->second;
            const int matches = [&]() { auto it = p->playlistMatches.find(pl.key); return it == p->playlistMatches.end() ? 0 : it->second; }();
            std::string tier = "Unranked";
            if (auto it = p->playlistTiers.find(pl.key); it != p->playlistTiers.end()) tier = it->second;
            if (std::string_view(pl.key) != "casual" && (tier.empty() || tier == "Unranked") && mmr > 0) {
                tier = MMRFetcher::GetRankTierForPlaylistMmr(pl.key, mmr);
            }
            LobbyRankCell cell;
            cell.color = CssColor(Format::RankColor(tier));
            if (mmr > 0) {
                cell.text = std::to_string(mmr);
                if (matches > 0) cell.text += " (" + std::to_string(matches) + ")";
                cell.tooltip = (std::string_view(pl.key) == "casual") ? ("Casual · MMR " + std::to_string(mmr))
                                                                      : (Format::RankTier(tier, config.use_roman_numerals) + " · MMR " + std::to_string(mmr));
            } else {
                cell.text = "-";
            }
            row.cells.push_back(std::move(cell));
        }
        nextRows.push_back(std::move(row));
    }
    if (m_lobbyPlayers.size() != nextRows.size() || m_lobbyPlayers != nextRows) {
        m_lobbyPlayers = std::move(nextRows);
        Dirty("lobby_players");
        changed = true;
    }
    const bool lobbyWaiting = m_lobbyPlayers.empty();
    if (m_lobbyRanksWaiting != lobbyWaiting) {
        m_lobbyRanksWaiting = lobbyWaiting;
        Dirty("lobby_waiting");
        changed = true;
    }

    // 4. MMR Graph
    std::string graphPl;
    std::vector<float> ratings;
    const std::vector<bool>* estimated = nullptr;
    float baseline = 0.0f;
    if (snap.showLifetimeGraph) {
        graphPl = MmrCategoryToString(state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo);
        ratings = snap.lifetimeMmrY;
        if (!ratings.empty()) baseline = ratings.front();
    } else {
        const auto cat = state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
        graphPl = MmrCategoryToString(cat);
        if (auto it = snap.playlistHistoryY.find(graphPl); it != snap.playlistHistoryY.end()) {
            ratings = it->second;
        }
        if (auto it = snap.playlistHistoryEstimated.find(graphPl); it != snap.playlistHistoryEstimated.end()) {
            estimated = &it->second;
        }
        if (auto it = snap.playlistInitialMmr.find(graphPl); it != snap.playlistInitialMmr.end()) {
            baseline = static_cast<float>(it->second);
        }
    }
    const int graphOffset = state ? state->ui.graphOffset.load() : 0;
    const auto graphWin = ComputeGraphWindow(static_cast<int>(ratings.size()), graphOffset, kVisibleGraphMatches);
    const bool graphIsEmpty = (graphWin.count == 0);
    if (m_graphEmpty != graphIsEmpty) {
        m_graphEmpty = graphIsEmpty;
        Dirty("graph_empty");
        changed = true;
    }
    if (graphIsEmpty) {
        const std::string emptyMsg = snap.showLifetimeGraph ? "No matches recorded yet." : "No matches recorded this session.";
        if (m_graphEmptyMessage != emptyMsg) {
            m_graphEmptyMessage = emptyMsg;
            Dirty("graph_empty_message");
            changed = true;
        }
    } else {
        std::vector<float> plotted;
        plotted.reserve(graphWin.count);
        for (int i = 0; i < graphWin.count; ++i) {
            plotted.push_back(ratings[static_cast<size_t>(graphWin.firstIndex + i)]);
        }
        float minRating = plotted.front();
        float maxRating = plotted.front();
        for (float r : plotted) {
            minRating = std::min(minRating, r);
            maxRating = std::max(maxRating, r);
        }
        if (baseline > 0.0f) {
            minRating = std::min(minRating, baseline);
            maxRating = std::max(maxRating, baseline);
        }
        const float range = std::max(maxRating - minRating, 20.0f);
        const float yMin = minRating - range * 0.1f;
        const float yMax = maxRating + range * 0.1f;
        const float ySpan = std::max(yMax - yMin, 1.0f);

        auto xPct = [&](size_t i) -> float {
            if (plotted.size() <= 1) return 50.0f;
            return static_cast<float>(i) / static_cast<float>(plotted.size() - 1) * 100.0f;
        };
        auto yPct = [&](float r) -> float {
            return (1.0f - (r - yMin) / ySpan) * 100.0f;
        };

        std::ostringstream pts;
        for (size_t i = 0; i < plotted.size(); ++i) {
            if (i > 0) pts << ';';
            pts << (xPct(i) / 100.0f) << ',' << (yPct(plotted[i]) / 100.0f);
        }
        const std::string nextPoints = pts.str();
        if (m_graphPoints != nextPoints) {
            m_graphPoints = nextPoints;
            Dirty("graph_points");
            changed = true;
        }
        const float nextFillTo = yMax / 100.0f;
        if (std::abs(m_graphFillTo - nextFillTo) > 0.001f) {
            m_graphFillTo = nextFillTo;
            Dirty("graph_fill_to");
            changed = true;
        }

        std::vector<GraphDotItem> nextDots;
        nextDots.reserve(plotted.size());
        for (size_t i = 0; i < plotted.size(); ++i) {
            GraphDotItem dot;
            dot.left = xPct(i);
            dot.top = yPct(plotted[i]);
            const float compare = i > 0 ? plotted[i - 1] : baseline;
            const bool win = plotted[i] > compare;
            const bool loss = plotted[i] < compare;
            dot.cls = win ? "win" : loss ? "loss"
                                         : "neutral";
            if (i == 0) dot.cls += " start";
            if (i + 1 == plotted.size()) dot.cls += " current";
            const size_t origIdx = static_cast<size_t>(graphWin.firstIndex + static_cast<int>(i));
            if (estimated && origIdx < estimated->size() && (*estimated)[origIdx]) {
                dot.estimated = true;
                dot.cls += " estimated";
            }
            dot.tip = std::to_string(static_cast<int>(std::lround(plotted[i])));
            nextDots.push_back(std::move(dot));
        }
        if (m_graphDots.size() != nextDots.size() || m_graphDots != nextDots) {
            m_graphDots = std::move(nextDots);
            Dirty("graph_dots");
            changed = true;
        }

        const bool panVis = graphWin.total > kVisibleGraphMatches;
        if (m_graphPanVisible != panVis) {
            m_graphPanVisible = panVis;
            Dirty("graph_pan_visible");
            changed = true;
        }
        if (panVis) {
            const std::string panLbl = std::to_string(graphWin.firstIndex + 1) + "-" +
                                       std::to_string(graphWin.firstIndex + graphWin.count) + " of " + std::to_string(graphWin.total);
            if (m_graphPanLabel != panLbl) {
                m_graphPanLabel = panLbl;
                Dirty("graph_pan_label");
                changed = true;
            }
            if (m_graphPanOlderDisabled != !graphWin.canPanOlder) {
                m_graphPanOlderDisabled = !graphWin.canPanOlder;
                Dirty("graph_pan_older_disabled");
                changed = true;
            }
            if (m_graphPanNewerDisabled != !graphWin.canPanNewer) {
                m_graphPanNewerDisabled = !graphWin.canPanNewer;
                Dirty("graph_pan_newer_disabled");
                changed = true;
            }
        }
    }

    // 5. Streaks
    int cw = 0, cl = 0, lw = 0, ll = 0;
    if (state) {
        std::lock_guard lock(state->ui.dbStatsMutex);
        cw = state->ui.cachedDbStats.currentWins;
        cl = state->ui.cachedDbStats.currentLosses;
        lw = state->ui.cachedDbStats.longestWins;
        ll = state->ui.cachedDbStats.longestLosses;
    }
    const std::string nextCurStreak = cw ? ("+" + std::to_string(cw)) : cl ? ("-" + std::to_string(cl))
                                                                           : "0";
    const std::string nextCurClass = cw ? "win" : cl ? "loss"
                                                     : "";
    const std::string nextLongestWin = std::to_string(lw);
    const std::string nextLongestLoss = std::to_string(ll);
    const bool nextShowLoss = config.show_longest_loss_streak;
    if (m_streaks.current_streak != nextCurStreak) {
        m_streaks.current_streak = nextCurStreak;
        Dirty("streaks_current");
        changed = true;
    }
    if (m_streaks.current_streak_class != nextCurClass) {
        m_streaks.current_streak_class = nextCurClass;
        Dirty("streaks_current_class");
        changed = true;
    }
    if (m_streaks.longest_win != nextLongestWin) {
        m_streaks.longest_win = nextLongestWin;
        Dirty("streaks_longest_win");
        changed = true;
    }
    if (m_streaks.longest_loss != nextLongestLoss) {
        m_streaks.longest_loss = nextLongestLoss;
        Dirty("streaks_longest_loss");
        changed = true;
    }
    if (m_streaks.show_longest_loss != nextShowLoss) {
        m_streaks.show_longest_loss = nextShowLoss;
        Dirty("streaks_show_longest_loss");
        changed = true;
    }

    // 6. Gamemode Breakdown
    const auto breakdownScope = ScopeFromConfigString(config.gamemode_breakdown_scope);
    const std::array<std::pair<const char*, bool>, 3> modes{{{"1v1", config.show_gamemode_record_1v1},
                                                             {"2v2", config.show_gamemode_record_2v2},
                                                             {"3v3", config.show_gamemode_record_3v3}}};
    std::vector<GamemodeBreakdownRow> nextGmRows;
    for (const auto& [mode, enabled] : modes) {
        if (!enabled) continue;
        int wins = 0, losses = 0, total = 0;
        if (breakdownScope == GamemodeBreakdownScope::CurrentSession) {
            if (auto it = snap.sessionGamemodes.find(mode); it != snap.sessionGamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        } else if (state) {
            std::lock_guard lock(state->ui.dbStatsMutex);
            if (auto it = state->ui.cachedDbStats.gamemodes.find(mode); it != state->ui.cachedDbStats.gamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        }
        const float pct = total > 0 ? (100.0f * wins / static_cast<float>(total)) : 0.0f;
        GamemodeBreakdownRow row;
        row.mode = mode;
        row.record = std::to_string(wins) + "-" + std::to_string(losses);
        row.pct = total ? (FormatNumber(pct, 1) + "%") : "-";
        row.total = std::to_string(total);
        nextGmRows.push_back(std::move(row));
    }
    const bool gmEmpty = nextGmRows.empty();
    if (m_gamemodeEmpty != gmEmpty) {
        m_gamemodeEmpty = gmEmpty;
        Dirty("gamemode_empty");
        changed = true;
    }
    if (m_gamemodeRows.size() != nextGmRows.size() || m_gamemodeRows != nextGmRows) {
        m_gamemodeRows = std::move(nextGmRows);
        Dirty("gamemode_rows");
        changed = true;
    }

    // 7. Tournament
    const int64_t nowSec = static_cast<int64_t>(std::time(nullptr));
    const auto tSnap = tournaments ? tournaments->Snapshot() : nullptr;
    if (tSnap) {
        const auto* next = Tournaments::NextSlot(tSnap->slots, nowSec, false);
        if (next) {
            const int64_t diff = next->startUnix - nowSec;
            m_tournament.has_next = true;
            m_tournament.title = next->title;
            m_tournament.region = tSnap->group;
            m_tournament.countdown = TournamentUi::Countdown(diff);
            m_tournament.starting_soon = (diff <= 15 * 60);
            m_tournament.status_text = "";
        } else {
            m_tournament.has_next = false;
            m_tournament.status_text = "No upcoming tournaments.";
        }
        Dirty("tournament_has_next");
        Dirty("tournament_title");
        Dirty("tournament_region");
        Dirty("tournament_countdown");
        Dirty("tournament_starting_soon");
        Dirty("tournament_status_text");
        changed = true;
    }

    // 8. Match Summary
    const bool summaryVis = state && state->ui.showMatchSummary.load(std::memory_order_relaxed);
    if (m_matchSummary.visible != summaryVis) {
        m_matchSummary.visible = summaryVis;
        Dirty("match_summary_visible");
        changed = true;
    }
    if (summaryVis) {
        const int myTeam = snap.matchSummaryMyTeam;
        const int s0 = snap.matchSummaryScore[0];
        const int s1 = snap.matchSummaryScore[1];
        int winner = snap.matchSummaryWinnerTeam;
        if (winner != 0 && winner != 1) {
            winner = s0 > s1 ? 0 : s1 > s0 ? 1
                                           : -1;
        }
        std::string result = "DRAW";
        std::string resultCls = "muted";
        if (snap.lastMatchWasVoid) {
            result = "VOID";
        } else if ((myTeam == 0 || myTeam == 1) && winner != -1) {
            if (winner == myTeam) {
                result = "WIN";
                resultCls = "win";
            } else {
                result = "LOSS";
                resultCls = "loss";
            }
        }
        const int myScore = myTeam == 1 ? s1 : s0;
        const int theirScore = myTeam == 1 ? s0 : s1;
        m_matchSummary.result = result;
        m_matchSummary.result_class = resultCls;
        m_matchSummary.score = std::to_string(myScore) + "-" + std::to_string(theirScore);
        Dirty("match_summary_result");
        Dirty("match_summary_result_class");
        Dirty("match_summary_score");
        changed = true;
    }

    return changed;
}
