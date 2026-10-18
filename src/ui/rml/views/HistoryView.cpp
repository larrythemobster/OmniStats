#include "ui/rml/views/HistoryView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <unordered_set>

#include "core/game/MatchAnalytics.hpp"
#include "core/game/MatchStory.hpp"
#include "core/config/Config.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    std::string SignedDelta(int value) {
        if (value > 0) return "+" + std::to_string(value);
        return std::to_string(value);
    }

    int DeltaTone(int value) {
        return value > 0 ? 1 : value < 0 ? -1
                                         : 0;
    }

    std::string FormatDateTime(int64_t unixSeconds) {
        if (unixSeconds <= 0) return "-";
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return "-";
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%a %d %b %Y, %H:%M", &local);
        return buffer;
    }

    std::string FormatMinutesSeconds(float seconds) {
        if (std::isnan(seconds) || seconds <= 0.0f) return "0:00";
        const int totalSec = std::max(0, static_cast<int>(std::lround(seconds)));
        const int mins = totalSec / 60;
        const int secs = totalSec % 60;
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%d:%02d", mins, secs);
        return buf;
    }

    std::string FormatDurationAndOvertime(const std::optional<float>& durationSeconds,
                                          const std::optional<float>& overtimeSeconds) {
        std::string out;
        if (durationSeconds.has_value() && *durationSeconds > 0.0f) {
            out = FormatMinutesSeconds(*durationSeconds);
        }
        if (overtimeSeconds.has_value() && *overtimeSeconds > 0.0f) {
            const std::string ot = "+" + FormatMinutesSeconds(*overtimeSeconds) + " OT";
            if (out.empty()) {
                out = ot;
            } else {
                out += " (" + ot + ")";
            }
        }
        return out;
    }

    std::string FormatOptInt(const std::optional<int>& val) {
        return val.has_value() ? std::to_string(*val) : "--";
    }

    std::string ShortPlaylistBadge(const MatchRow& row) {
        if (!row.ranked) return "Casual";
        if (row.gamemode == "1v1") return "1v1";
        if (row.gamemode == "2v2") return "2v2";
        if (row.gamemode == "3v3") return "3v3";
        if (!row.playlist.empty()) return row.playlist;
        return row.gamemode.empty() ? "Unknown" : row.gamemode;
    }

    std::string PrettifyEventName(const std::string& raw) {
        if (raw == "Shot") return "Shots";
        if (raw == "Save") return "Saves";
        if (raw == "EpicSave") return "Epic Saves";
        if (raw == "Assist") return "Assists";
        if (raw == "Goal") return "Goals";
        if (raw == "Demolish" || raw == "Demolition") return "Demos";
        if (raw == "Clear") return "Clears";
        if (raw == "Center") return "Centers";
        if (raw == "AerialHit") return "Aerial Hits";
        if (raw == "AerialGoal") return "Aerial Goals";
        if (raw == "BicycleHit") return "Bicycle Hits";
        if (raw == "TurtleGoal") return "Turtle Goals";
        if (raw == "HatTrick") return "Hat Tricks";
        if (raw == "Playmaker") return "Playmakers";
        if (raw == "Savior") return "Saviors";
        std::string out;
        for (size_t i = 0; i < raw.size(); ++i) {
            if (i > 0 && std::isupper(static_cast<unsigned char>(raw[i])) &&
                std::islower(static_cast<unsigned char>(raw[i - 1]))) {
                out += ' ';
            }
            out += raw[i];
        }
        return out;
    }

    std::string FormatSecAndPct(float sec, float total) {
        char buf[64]{};
        const float pct = total > 0.0f ? std::clamp(sec / total * 100.0f, 0.0f, 100.0f) : 0.0f;
        std::snprintf(buf, sizeof(buf), "%.1fs (%.1f%%)", sec, pct);
        return buf;
    }

    std::string FormatSpeed(float kmh, bool imperial) {
        if (imperial) {
            const int mph = static_cast<int>(std::lround(kmh * 0.621371f));
            return std::to_string(mph) + " mph";
        }
        const int k = static_cast<int>(std::lround(kmh));
        return std::to_string(k) + " km/h";
    }

    std::string FormatSec(float sec) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.1fs", sec);
        return buf;
    }

    std::string PctStr(float part, float total) {
        if (total <= 0.0f || part <= 0.0f) return "0%";
        const float pct = std::clamp(part / total * 100.0f, 0.0f, 100.0f);
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%.1f%%", pct);
        return buf;
    }

    HistoryDetailPlayerRow FormatDetailPlayer(const MatchDetailPlayer& p,
                                              const MatchAnalytics* analytics = nullptr,
                                              const std::unordered_set<std::string>* notedKeys = nullptr,
                                              bool notesLoaded = false,
                                              bool imperial = false) {
        HistoryDetailPlayerRow row;
        row.name = p.name.empty() ? p.primaryId : p.name;
        row.primary_id = p.primaryId;
        if (!p.platform.empty()) {
            const PlatformKind kind = PlatformKindFor(p.platform);
            row.platform = PlatformBadgeLabel(kind, p.platform);
            row.platform_class = std::string("badge ") + PlatformClass(kind);
        }
        row.is_me = p.isMe;
        if (p.mmr > 0) {
            row.mmr = (p.mmrEstimated ? "~" : "") + std::to_string(p.mmr);
        } else {
            row.mmr = "--";
        }
        row.tier = p.tier.empty() ? "Unranked" : p.tier;
        row.tier_color = RmlUiController::CssColor(Format::RankColor(p.tier));
        if (p.isMe) {
            row.encounter = "You";
        } else if (p.encounter) {
            row.encounter = EncounterSummary(*p.encounter);
        }
        if (!p.isMe && notesLoaded && notedKeys) {
            if (const auto identity = PlayerNotes::IdentityFromPrimaryId(p.primaryId)) {
                row.can_note = true;
                row.has_note = notedKeys->count(identity->Key()) > 0;
            }
        }
        row.score = p.hasStats ? FormatOptInt(p.score) : "--";
        row.goals = p.hasStats ? FormatOptInt(p.goals) : "--";
        row.assists = p.hasStats ? FormatOptInt(p.assists) : "--";
        row.saves = p.hasStats ? FormatOptInt(p.saves) : "--";
        row.shots = p.hasStats ? FormatOptInt(p.shots) : "--";
        row.demos = p.hasStats ? FormatOptInt(p.demos) : "--";

        const PlayerAnalytics* pa = analytics ? analytics->FindPlayer(p.primaryId) : nullptr;
        if (p.touches.has_value()) {
            row.touches = std::to_string(*p.touches);
        } else if (pa) {
            row.touches = std::to_string(pa->hits);
        } else {
            row.touches = "--";
        }

        if (pa) {
            row.avg_hit_speed = pa->hits > 0
                                    ? FormatSpeed(pa->AverageHitSpeed(), imperial)
                                    : "--";
            row.aerial_hits = std::to_string(pa->aerialHits);
            row.first_touches = std::to_string(pa->firstTouches);
            row.demos_inflicted = std::to_string(pa->demosInflicted);
            row.demos_taken = std::to_string(pa->demosTaken);
            row.hits_by_third = std::to_string(pa->hitsByThird[0]) + " / " + std::to_string(pa->hitsByThird[1]) +
                                " / " + std::to_string(pa->hitsByThird[2]);
        } else {
            row.avg_hit_speed = "--";
            row.aerial_hits = "--";
            row.first_touches = "--";
            row.demos_inflicted = p.hasStats ? FormatOptInt(p.demos) : "--";
            row.demos_taken = "--";
            row.hits_by_third = "--";
        }
        return row;
    }
} // namespace

bool HistoryView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("history");
    if (!constructor) return false;

    if (auto opt = constructor.RegisterStruct<HistoryFilterOption>()) {
        opt.RegisterMember("id", &HistoryFilterOption::id);
        opt.RegisterMember("label", &HistoryFilterOption::label);
    }
    constructor.RegisterArray<std::vector<HistoryFilterOption>>();

    if (auto badge = constructor.RegisterStruct<HistoryMatchBadge>()) {
        badge.RegisterMember("label", &HistoryMatchBadge::label);
        badge.RegisterMember("badge_class", &HistoryMatchBadge::badge_class);
    }
    constructor.RegisterArray<std::vector<HistoryMatchBadge>>();

    if (auto row = constructor.RegisterStruct<HistoryMatchRow>()) {
        row.RegisterMember("match_id", &HistoryMatchRow::match_id);
        row.RegisterMember("result", &HistoryMatchRow::result);
        row.RegisterMember("win", &HistoryMatchRow::win);
        row.RegisterMember("playlist", &HistoryMatchRow::playlist);
        row.RegisterMember("score", &HistoryMatchRow::score);
        row.RegisterMember("mmr_delta", &HistoryMatchRow::mmr_delta);
        row.RegisterMember("mmr_tone", &HistoryMatchRow::mmr_tone);
        row.RegisterMember("stat_line", &HistoryMatchRow::stat_line);
        row.RegisterMember("badges", &HistoryMatchRow::badges);
        row.RegisterMember("arena", &HistoryMatchRow::arena);
        row.RegisterMember("teammates", &HistoryMatchRow::teammates);
        row.RegisterMember("time", &HistoryMatchRow::time);
        row.RegisterMember("has_replay", &HistoryMatchRow::has_replay);
    }
    constructor.RegisterArray<std::vector<HistoryMatchRow>>();

    if (auto player = constructor.RegisterStruct<HistoryDetailPlayerRow>()) {
        player.RegisterMember("name", &HistoryDetailPlayerRow::name);
        player.RegisterMember("platform", &HistoryDetailPlayerRow::platform);
        player.RegisterMember("platform_class", &HistoryDetailPlayerRow::platform_class);
        player.RegisterMember("is_me", &HistoryDetailPlayerRow::is_me);
        player.RegisterMember("mmr", &HistoryDetailPlayerRow::mmr);
        player.RegisterMember("tier", &HistoryDetailPlayerRow::tier);
        player.RegisterMember("tier_color", &HistoryDetailPlayerRow::tier_color);
        player.RegisterMember("encounter", &HistoryDetailPlayerRow::encounter);
        player.RegisterMember("primary_id", &HistoryDetailPlayerRow::primary_id);
        player.RegisterMember("can_note", &HistoryDetailPlayerRow::can_note);
        player.RegisterMember("has_note", &HistoryDetailPlayerRow::has_note);
        player.RegisterMember("score", &HistoryDetailPlayerRow::score);
        player.RegisterMember("goals", &HistoryDetailPlayerRow::goals);
        player.RegisterMember("assists", &HistoryDetailPlayerRow::assists);
        player.RegisterMember("saves", &HistoryDetailPlayerRow::saves);
        player.RegisterMember("shots", &HistoryDetailPlayerRow::shots);
        player.RegisterMember("demos", &HistoryDetailPlayerRow::demos);
        player.RegisterMember("touches", &HistoryDetailPlayerRow::touches);
        player.RegisterMember("avg_hit_speed", &HistoryDetailPlayerRow::avg_hit_speed);
        player.RegisterMember("aerial_hits", &HistoryDetailPlayerRow::aerial_hits);
        player.RegisterMember("first_touches", &HistoryDetailPlayerRow::first_touches);
        player.RegisterMember("demos_inflicted", &HistoryDetailPlayerRow::demos_inflicted);
        player.RegisterMember("demos_taken", &HistoryDetailPlayerRow::demos_taken);
        player.RegisterMember("hits_by_third", &HistoryDetailPlayerRow::hits_by_third);
    }
    constructor.RegisterArray<std::vector<HistoryDetailPlayerRow>>();

    if (auto event = constructor.RegisterStruct<HistoryEventRow>()) {
        event.RegisterMember("clock", &HistoryEventRow::clock);
        event.RegisterMember("type", &HistoryEventRow::type);
        event.RegisterMember("player", &HistoryEventRow::player);
        event.RegisterMember("detail", &HistoryEventRow::detail);
        event.RegisterMember("category", &HistoryEventRow::category);
    }
    constructor.RegisterArray<std::vector<HistoryEventRow>>();

    if (auto win = constructor.RegisterStruct<HistoryTimelineWindow>()) {
        win.RegisterMember("our_height", &HistoryTimelineWindow::our_height);
        win.RegisterMember("their_height", &HistoryTimelineWindow::their_height);
    }
    constructor.RegisterArray<std::vector<HistoryTimelineWindow>>();

    if (auto goal = constructor.RegisterStruct<HistoryTimelineGoal>()) {
        goal.RegisterMember("x_pct", &HistoryTimelineGoal::x_pct);
        goal.RegisterMember("score_label", &HistoryTimelineGoal::score_label);
        goal.RegisterMember("scorer", &HistoryTimelineGoal::scorer);
        goal.RegisterMember("is_our_team", &HistoryTimelineGoal::is_our_team);
    }
    constructor.RegisterArray<std::vector<HistoryTimelineGoal>>();

    if (auto cmp = constructor.RegisterStruct<HistoryTeamCompareRow>()) {
        cmp.RegisterMember("label", &HistoryTeamCompareRow::label);
        cmp.RegisterMember("our_val", &HistoryTeamCompareRow::our_val);
        cmp.RegisterMember("their_val", &HistoryTeamCompareRow::their_val);
        cmp.RegisterMember("tone", &HistoryTeamCompareRow::tone);
    }
    constructor.RegisterArray<std::vector<HistoryTeamCompareRow>>();

    constructor.RegisterArray<std::vector<Rml::String>>();

    if (auto mech = constructor.RegisterStruct<HistoryMechanicsRow>()) {
        mech.RegisterMember("player_name", &HistoryMechanicsRow::player_name);
        mech.RegisterMember("is_me", &HistoryMechanicsRow::is_me);
        mech.RegisterMember("is_total", &HistoryMechanicsRow::is_total);
        mech.RegisterMember("values", &HistoryMechanicsRow::values);
    }
    constructor.RegisterArray<std::vector<HistoryMechanicsRow>>();

    if (auto dot = constructor.RegisterStruct<HistoryFieldDot>()) {
        dot.RegisterMember("x_pct", &HistoryFieldDot::x_pct);
        dot.RegisterMember("y_pct", &HistoryFieldDot::y_pct);
        dot.RegisterMember("dot_class", &HistoryFieldDot::dot_class);
    }
    constructor.RegisterArray<std::vector<HistoryFieldDot>>();

    if (auto fgoal = constructor.RegisterStruct<HistoryFieldGoal>()) {
        fgoal.RegisterMember("x_pct", &HistoryFieldGoal::x_pct);
        fgoal.RegisterMember("y_pct", &HistoryFieldGoal::y_pct);
        fgoal.RegisterMember("marker_class", &HistoryFieldGoal::marker_class);
        fgoal.RegisterMember("label", &HistoryFieldGoal::label);
    }
    constructor.RegisterArray<std::vector<HistoryFieldGoal>>();

    constructor.Bind("subtitle", &m_subtitle);
    constructor.Bind("playlist_filter", &m_playlistFilter);
    constructor.Bind("playlist_options", &m_playlistOptions);
    constructor.Bind("result_filter", &m_resultFilter);
    constructor.Bind("date_filter", &m_dateFilter);
    constructor.Bind("sort_filter", &m_sortFilter);
    constructor.Bind("with_player", &m_withPlayer);
    constructor.Bind("against_player", &m_againstPlayer);
    constructor.Bind("arena_filter", &m_arenaFilter);
    constructor.Bind("name_search", &m_nameSearch);
    constructor.Bind("rows", &m_rows);
    constructor.Bind("loading", &m_loading);
    constructor.Bind("has_more", &m_hasMore);
    constructor.Bind("empty_message", &m_emptyMessage);
    constructor.Bind("detail_open", &m_detailOpen);
    constructor.Bind("detail_loading", &m_detailLoading);
    constructor.Bind("detail_tab", &m_detailTab);
    constructor.Bind("detail_result", &m_detailResult);
    constructor.Bind("detail_win", &m_detailWin);
    constructor.Bind("detail_score", &m_detailScore);
    constructor.Bind("detail_playlist", &m_detailPlaylist);
    constructor.Bind("detail_arena", &m_detailArena);
    constructor.Bind("detail_time", &m_detailTime);
    constructor.Bind("detail_mmr_change", &m_detailMmrChange);
    constructor.Bind("detail_mmr_tone", &m_detailMmrTone);
    constructor.Bind("detail_duration", &m_detailDuration);
    constructor.Bind("detail_v1_note", &m_detailV1Note);
    constructor.Bind("detail_our_team", &m_detailOurTeam);
    constructor.Bind("detail_their_team", &m_detailTheirTeam);
    constructor.Bind("detail_ballchasing_url", &m_detailBallchasingUrl);

    // Overview
    constructor.Bind("detail_story_lines", &m_detailStoryLines);
    constructor.Bind("timeline_has_data", &m_timelineHasData);
    constructor.Bind("timeline_windows", &m_timelineWindows);
    constructor.Bind("timeline_goals", &m_timelineGoals);
    constructor.Bind("team_compare_rows", &m_teamCompareRows);

    // Boost & movement
    constructor.Bind("has_car_telemetry", &m_hasCarTelemetry);
    constructor.Bind("has_legacy_stats", &m_hasLegacyStats);
    constructor.Bind("telemetry_note", &m_telemetryNote);
    constructor.Bind("car_bpm", &m_carBpm);
    constructor.Bind("car_avg_boost", &m_carAvgBoost);
    constructor.Bind("car_zero_boost", &m_carZeroBoost);
    constructor.Bind("car_full_boost", &m_carFullBoost);
    constructor.Bind("car_boost_collected", &m_carBoostCollected);
    constructor.Bind("car_boost_used", &m_carBoostUsed);
    constructor.Bind("car_big_pads", &m_carBigPads);
    constructor.Bind("car_small_pads", &m_carSmallPads);
    constructor.Bind("car_stolen_pads", &m_carStolenPads);
    constructor.Bind("car_avg_speed", &m_carAvgSpeed);
    constructor.Bind("car_max_speed", &m_carMaxSpeed);
    constructor.Bind("car_powerslides", &m_carPowerslides);
    constructor.Bind("car_demo_time", &m_carDemoTime);
    constructor.Bind("boost_band_0", &m_boostBand0);
    constructor.Bind("boost_band_1", &m_boostBand1);
    constructor.Bind("boost_band_2", &m_boostBand2);
    constructor.Bind("boost_band_3", &m_boostBand3);
    constructor.Bind("speed_slow_pct", &m_speedSlowPct);
    constructor.Bind("speed_boost_pct", &m_speedBoostPct);
    constructor.Bind("speed_supersonic_pct", &m_speedSupersonicPct);
    constructor.Bind("speed_slow_label", &m_speedSlowLabel);
    constructor.Bind("speed_boost_label", &m_speedBoostLabel);
    constructor.Bind("speed_supersonic_label", &m_speedSupersonicLabel);
    constructor.Bind("surface_ground_pct", &m_surfaceGroundPct);
    constructor.Bind("surface_air_pct", &m_surfaceAirPct);
    constructor.Bind("surface_wall_pct", &m_surfaceWallPct);
    constructor.Bind("surface_ground_label", &m_surfaceGroundLabel);
    constructor.Bind("surface_air_label", &m_surfaceAirLabel);
    constructor.Bind("surface_wall_label", &m_surfaceWallLabel);
    constructor.Bind("legacy_boost_collected", &m_legacyBoostCollected);
    constructor.Bind("legacy_demoed", &m_legacyDemoed);
    constructor.Bind("legacy_crossbars", &m_legacyCrossbars);
    constructor.Bind("legacy_hardest_crossbar", &m_legacyHardestCrossbar);
    constructor.Bind("legacy_max_ball_speed", &m_legacyMaxBallSpeed);
    constructor.Bind("legacy_ownGoals", &m_legacyOwnGoals);

    // Mechanics
    constructor.Bind("has_mechanics", &m_hasMechanics);
    constructor.Bind("mechanics_columns", &m_mechanicsColumns);
    constructor.Bind("mechanics_rows", &m_mechanicsRows);

    // Field
    constructor.Bind("field_has_data", &m_fieldHasData);
    constructor.Bind("field_dots", &m_fieldDots);
    constructor.Bind("field_goals", &m_fieldGoals);
    constructor.Bind("field_touches_count", &m_fieldTouchesCount);
    constructor.Bind("pitch_legend_soft", &m_pitchLegendSoft);
    constructor.Bind("pitch_legend_med", &m_pitchLegendMed);
    constructor.Bind("pitch_legend_hard", &m_pitchLegendHard);

    // Events
    constructor.Bind("detail_events", &m_detailEvents);
    constructor.Bind("event_filter", &m_eventFilter);
    constructor.Bind("event_team_filter", &m_eventTeamFilter);
    constructor.Bind("event_team_available", &m_eventTeamAvailable);
    constructor.Bind("event_empty", &m_eventEmpty);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void HistoryView::Reset() {
    m_handle = {};
    m_bound = false;
    m_account.clear();
    m_activeRequestId = 0;
    m_activeDetailRequestId = 0;
    m_sourceRows.clear();
    m_totalCount = 0;
    m_detailSource.reset();
    m_detailEvents.clear();
    m_eventFilter = "all";
    m_eventTeamFilter = "all";
    m_eventTeamAvailable = false;
    m_subtitle = "Your saved match history";
    m_playlistFilter = "All";
    m_playlistOptions = {
        {"All", "All"},
        {"1v1", "1v1"},
        {"2v2", "2v2"},
        {"3v3", "3v3"},
        {"Extra", "Extra"},
        {"Ranked", "Ranked"},
        {"Casual", "Casual"},
    };
    m_resultFilter = "all";
    m_dateFilter = "all";
    m_sortFilter = "newest";
    m_withPlayer.clear();
    m_againstPlayer.clear();
    m_arenaFilter.clear();
    m_nameSearch.clear();
    m_rows.clear();
    m_loading = false;
    m_hasMore = false;
    m_emptyMessage = "Play a match so OmniStats can identify your account.";
    m_detailOpen = false;
    m_detailLoading = false;
    m_detailTab = "overview";
    m_detailOurTeam.clear();
    m_detailTheirTeam.clear();
    m_detailStoryLines.clear();
    m_timelineHasData = false;
    m_timelineWindows.clear();
    m_timelineGoals.clear();
    m_teamCompareRows.clear();
    m_hasCarTelemetry = false;
    m_hasLegacyStats = false;
    m_hasMechanics = false;
    m_mechanicsColumns.clear();
    m_mechanicsRows.clear();
    m_fieldHasData = false;
    m_fieldDots.clear();
    m_fieldGoals.clear();
    m_pitchLegendSoft = "< 70 km/h (Soft)";
    m_pitchLegendMed = "70 - 100 km/h (Medium)";
    m_pitchLegendHard = ">= 100 km/h (Hard)";
}

void HistoryView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void HistoryView::SetAccount(const std::string& account) {
    m_account = account;
    RebuildRows();
}

void HistoryView::SetPlaylistFilter(const std::string& playlist) {
    m_playlistFilter = playlist;
    Dirty("playlist_filter");
}

void HistoryView::SetResultFilter(const std::string& result) {
    m_resultFilter = result;
    Dirty("result_filter");
}

void HistoryView::SetDateFilter(const std::string& dateRange) {
    m_dateFilter = dateRange;
    Dirty("date_filter");
}

void HistoryView::SetSortFilter(const std::string& sort) {
    m_sortFilter = sort;
    Dirty("sort_filter");
}

void HistoryView::SetWithPlayer(const std::string& player) {
    m_withPlayer = player;
    Dirty("with_player");
}

void HistoryView::SetAgainstPlayer(const std::string& player) {
    m_againstPlayer = player;
    Dirty("against_player");
}

void HistoryView::SetArenaFilter(const std::string& arena) {
    m_arenaFilter = arena;
    Dirty("arena_filter");
}

void HistoryView::SetNameSearch(const std::string& search) {
    m_nameSearch = search;
    Dirty("name_search");
}

void HistoryView::ClearFilters() {
    m_playlistFilter = "All";
    m_resultFilter = "all";
    m_dateFilter = "all";
    m_sortFilter = "newest";
    m_withPlayer.clear();
    m_againstPlayer.clear();
    m_arenaFilter.clear();
    m_nameSearch.clear();

    for (const char* name : {"playlist_filter", "result_filter", "date_filter", "sort_filter",
                             "with_player", "against_player", "arena_filter", "name_search"}) {
        Dirty(name);
    }
}

MatchQuery HistoryView::BuildQuery(int offset, int limit, int64_t nowUnix) const {
    MatchQuery q;
    q.account = m_account;
    q.offset = offset;
    q.limit = limit;

    if (m_playlistFilter == "1v1" || m_playlistFilter == "2v2" || m_playlistFilter == "3v3" ||
        m_playlistFilter == "Extra") {
        q.playlistLabel = m_playlistFilter;
    } else if (m_playlistFilter == "Ranked") {
        q.ranked = true;
    } else if (m_playlistFilter == "Casual") {
        q.ranked = false;
    }

    if (m_resultFilter == "win") {
        q.win = true;
    } else if (m_resultFilter == "loss") {
        q.win = false;
    }

    if (nowUnix <= 0) {
        nowUnix = static_cast<int64_t>(std::time(nullptr));
    }

    if (m_dateFilter == "7d") {
        q.fromUnix = nowUnix - 7 * 86400;
    } else if (m_dateFilter == "30d") {
        q.fromUnix = nowUnix - 30 * 86400;
    } else if (m_dateFilter == "90d") {
        q.fromUnix = nowUnix - 90 * 86400;
    }

    if (m_sortFilter == "oldest") {
        q.sort = MatchQuery::Oldest;
    } else if (m_sortFilter == "biggest_win") {
        q.sort = MatchQuery::BiggestWin;
    } else if (m_sortFilter == "biggest_loss") {
        q.sort = MatchQuery::BiggestLoss;
    } else if (m_sortFilter == "margin") {
        q.sort = MatchQuery::ScoreMargin;
    } else {
        q.sort = MatchQuery::Newest;
    }

    q.withPlayer = m_withPlayer;
    q.againstPlayer = m_againstPlayer;
    q.arena = m_arenaFilter;
    q.nameSearch = m_nameSearch;
    return q;
}

uint64_t HistoryView::BeginQuery(bool append) {
    if (!append) {
        m_activeRequestId++;
        m_loading = true;
        m_emptyMessage = "Loading match history...";
        for (const char* name : {"loading", "empty_message"})
            Dirty(name);
    }
    return m_activeRequestId;
}

bool HistoryView::ApplyQueryResult(uint64_t requestId, const std::vector<MatchRow>& rows, int totalCount, bool append) {
    if (requestId != m_activeRequestId) return false;
    if (append) {
        m_sourceRows.insert(m_sourceRows.end(), rows.begin(), rows.end());
    } else {
        m_sourceRows = rows;
    }
    m_totalCount = totalCount;
    m_loading = false;
    RebuildRows();
    return true;
}

uint64_t HistoryView::BeginDetail(int64_t matchId) {
    (void)matchId;
    m_activeDetailRequestId++;
    m_detailOpen = true;
    m_detailLoading = true;
    m_detailTab = "overview";
    m_detailSource.reset();
    RebuildDetail();
    return m_activeDetailRequestId;
}

bool HistoryView::ApplyDetailResult(uint64_t requestId, const std::optional<MatchDetail>& detail) {
    if (requestId != m_activeDetailRequestId) return false;
    m_appliedDetailRequestId = requestId;
    m_detailLoading = false;
    m_detailSource = detail;
    RebuildDetail();
    return true;
}

void HistoryView::CloseDetail() {
    m_detailOpen = false;
    m_detailLoading = false;
    m_detailSource.reset();
    m_detailEvents.clear();
    m_eventTeamAvailable = false;
    RebuildDetail();
    Dirty("detail_open");
}

void HistoryView::SetDetailTab(const std::string& tab) {
    if (tab == "overview" || tab == "scoreboard" || tab == "boost" ||
        tab == "mechanics" || tab == "field" || tab == "events") {
        m_detailTab = tab;
    } else {
        m_detailTab = "overview";
    }
    Dirty("detail_tab");
}

void HistoryView::SetEventFilter(const std::string& filter) {
    m_eventFilter = filter;
    RebuildDetail();
    Dirty("event_filter");
}

void HistoryView::SetEventTeamFilter(const std::string& filter) {
    m_eventTeamFilter = filter;
    RebuildDetail();
    Dirty("event_team_filter");
}

void HistoryView::SetNotedPlayers(const std::unordered_set<std::string>& keys, bool loaded) {
    m_notedKeys = keys;
    m_notesLoaded = loaded;
    if (m_detailOpen && m_detailSource.has_value()) {
        RebuildDetail();
    }
}

void HistoryView::RebuildRows() {
    m_rows.clear();
    m_rows.reserve(m_sourceRows.size());
    for (const auto& src : m_sourceRows) {
        HistoryMatchRow row;
        row.match_id = std::to_string(src.matchId);
        row.result = src.win ? "W" : "L";
        row.win = src.win;
        row.playlist = ShortPlaylistBadge(src);
        row.score = std::to_string(src.ourScore) + "-" + std::to_string(src.theirScore);
        if (src.mmrDelta.has_value()) {
            row.mmr_delta = SignedDelta(*src.mmrDelta);
            row.mmr_tone = DeltaTone(*src.mmrDelta);
        } else {
            row.mmr_delta = "--";
            row.mmr_tone = 0;
        }

        if (src.myGoals.has_value() || src.myAssists.has_value() || src.mySaves.has_value() ||
            src.myShots.has_value()) {
            row.stat_line = std::to_string(src.myGoals.value_or(0)) + "G " +
                            std::to_string(src.myAssists.value_or(0)) + "A " +
                            std::to_string(src.mySaves.value_or(0)) + "Sv " +
                            std::to_string(src.myShots.value_or(0)) + "Sh";
        } else {
            row.stat_line = "--";
        }

        if (src.mvp) {
            row.badges.push_back({"MVP", "badge badge-mvp"});
        }
        if (src.overtime || (src.scoreline && src.scoreline->overtime)) {
            row.badges.push_back({"OT", "badge badge-ot"});
        }
        if (src.scoreline && src.scoreline->comeback) {
            row.badges.push_back({"Comeback", "badge badge-comeback"});
        }
        if (src.scoreline && src.scoreline->blownLead) {
            row.badges.push_back({"Blown lead", "badge badge-blown"});
        }
        if (src.scoreline && src.scoreline->lateWinner) {
            row.badges.push_back({"Late win", "badge badge-late"});
        }
        if (src.endedEarly) {
            row.badges.push_back({"FF", "badge badge-early"});
        }

        row.arena = src.arena.empty() ? "Unknown Arena" : src.arena;
        if (!src.teammates.empty()) {
            std::string joined = "with ";
            for (size_t i = 0; i < src.teammates.size(); ++i) {
                if (i > 0) joined += ", ";
                joined += src.teammates[i];
            }
            row.teammates = std::move(joined);
        } else {
            row.teammates = "Solo";
        }
        row.time = FormatClock(src.timestampUnix);
        row.has_replay = src.hasBallchasing;
        m_rows.push_back(std::move(row));
    }

    m_hasMore = static_cast<int>(m_sourceRows.size()) < m_totalCount;
    m_subtitle = m_totalCount > 0 ? Rml::String(std::to_string(m_totalCount) + (m_totalCount == 1 ? " match" : " matches"))
                                  : Rml::String("Your saved match history");

    if (m_account.empty()) {
        m_emptyMessage = "Play a match so OmniStats can identify your account.";
    } else if (m_loading) {
        m_emptyMessage = "Loading match history...";
    } else {
        m_emptyMessage = "No saved matches match the current filters.";
    }

    for (const char* name : {"rows", "loading", "has_more", "subtitle", "empty_message"}) {
        Dirty(name);
    }
}

void HistoryView::RebuildDetail() {
    m_detailOurTeam.clear();
    m_detailTheirTeam.clear();
    m_detailEvents.clear();
    m_eventTeamAvailable = false;
    m_detailStoryLines.clear();
    m_timelineHasData = false;
    m_timelineWindows.clear();
    m_timelineGoals.clear();
    m_teamCompareRows.clear();
    m_hasCarTelemetry = false;
    m_hasLegacyStats = false;
    m_telemetryNote.clear();
    m_hasMechanics = false;
    m_mechanicsColumns.clear();
    m_mechanicsRows.clear();
    m_fieldHasData = false;
    m_fieldDots.clear();
    m_fieldGoals.clear();

    if (!m_detailSource.has_value() || !m_detailSource->found) {
        m_detailResult.clear();
        m_detailWin = false;
        m_detailScore.clear();
        m_detailPlaylist.clear();
        m_detailArena.clear();
        m_detailTime.clear();
        m_detailMmrChange.clear();
        m_detailMmrTone = 0;
        m_detailDuration.clear();
        m_detailV1Note.clear();
        m_detailBallchasingUrl.clear();
    } else {
        const MatchDetail& d = *m_detailSource;
        m_detailResult = d.win ? "WIN" : "LOSS";
        m_detailWin = d.win;
        m_detailScore = std::to_string(d.ourScore) + " - " + std::to_string(d.theirScore);
        m_detailPlaylist = (d.ranked ? "Ranked " : "") + (d.playlist.empty() ? std::string("Match") : d.playlist);
        if (!d.ranked && d.playlist == "Casual") m_detailPlaylist = "Casual";
        m_detailArena = d.arena.empty() ? "Unknown Arena" : d.arena;
        m_detailTime = FormatDateTime(d.timestampUnix);

        if (d.mmrBefore.has_value() && d.mmrAfter.has_value()) {
            const int delta = *d.mmrAfter - *d.mmrBefore;
            m_detailMmrChange = std::to_string(*d.mmrBefore) + " -> " + std::to_string(*d.mmrAfter) +
                                " (" + SignedDelta(delta) + ")";
            m_detailMmrTone = DeltaTone(delta);
        } else if (d.mmrAfter.has_value()) {
            m_detailMmrChange = (d.mmrEstimated ? "~" : "") + std::to_string(*d.mmrAfter);
            m_detailMmrTone = 0;
        } else {
            m_detailMmrChange.clear();
            m_detailMmrTone = 0;
        }

        m_detailDuration = FormatDurationAndOvertime(d.durationSeconds, d.overtimeSeconds);
        m_detailV1Note = d.hasPlayerStats ? "" : "Per-player scoreboard stats were not recorded for this match.";
        m_detailBallchasingUrl = d.ballchasingUrl;

        // 1. Match Story
        const auto story = MatchStory::BuildMatchStory(d);
        for (const auto& line : story) {
            m_detailStoryLines.push_back(line);
        }

        const bool imperial = Config::Read().imperial_units;
        int ourTeam = 0;
        if (!d.ourTeam.empty()) {
            ourTeam = d.ourTeam.front().team;
        } else if (d.analytics.has_value()) {
            if (!d.accountPrimaryId.empty()) {
                if (const auto* mePa = d.analytics->FindPlayer(d.accountPrimaryId)) {
                    if (mePa->team == 0 || mePa->team == 1) ourTeam = mePa->team;
                }
            }
        }
        const int theirTeam = (ourTeam == 0) ? 1 : 0;

        // 2. Scoreboard players
        for (const auto& p : d.ourTeam) {
            m_detailOurTeam.push_back(FormatDetailPlayer(p, d.analytics ? &*d.analytics : nullptr, &m_notedKeys, m_notesLoaded, imperial));
        }
        for (const auto& p : d.theirTeam) {
            m_detailTheirTeam.push_back(FormatDetailPlayer(p, d.analytics ? &*d.analytics : nullptr, &m_notedKeys, m_notesLoaded, imperial));
        }

        // 3. Team comparison table
        int ourShots = 0, theirShots = 0;
        int ourAssists = 0, theirAssists = 0;
        int ourSaves = 0, theirSaves = 0;
        for (const auto& p : d.ourTeam) {
            ourShots += p.shots.value_or(0);
            ourAssists += p.assists.value_or(0);
            ourSaves += p.saves.value_or(0);
        }
        for (const auto& p : d.theirTeam) {
            theirShots += p.shots.value_or(0);
            theirAssists += p.assists.value_or(0);
            theirSaves += p.saves.value_or(0);
        }

        auto addCompare = [&](std::string label, std::string ourVal, std::string theirVal, int tone) {
            m_teamCompareRows.push_back({std::move(label), std::move(ourVal), std::move(theirVal), tone});
        };

        addCompare("Goals", std::to_string(d.ourScore), std::to_string(d.theirScore),
                   d.ourScore > d.theirScore ? 1 : d.ourScore < d.theirScore ? -1
                                                                             : 0);
        addCompare("Shots", std::to_string(ourShots), std::to_string(theirShots),
                   ourShots > theirShots ? 1 : ourShots < theirShots ? -1
                                                                     : 0);

        const int ourShotPct = ourShots > 0 ? std::clamp(d.ourScore * 100 / ourShots, 0, 100) : 0;
        const int theirShotPct = theirShots > 0 ? std::clamp(d.theirScore * 100 / theirShots, 0, 100) : 0;
        addCompare("Shooting %", std::to_string(ourShotPct) + "%", std::to_string(theirShotPct) + "%",
                   ourShotPct > theirShotPct ? 1 : ourShotPct < theirShotPct ? -1
                                                                             : 0);
        addCompare("Assists", std::to_string(ourAssists), std::to_string(theirAssists),
                   ourAssists > theirAssists ? 1 : ourAssists < theirAssists ? -1
                                                                             : 0);
        addCompare("Saves", std::to_string(ourSaves), std::to_string(theirSaves),
                   ourSaves > theirSaves ? 1 : ourSaves < theirSaves ? -1
                                                                     : 0);

        if (d.analytics.has_value() && !d.analytics->Empty()) {
            const auto& a = *d.analytics;
            const int ourPoss = std::clamp(static_cast<int>(std::lround(a.PossessionShare(ourTeam) * 100.0f)), 0, 100);
            const int theirPoss = std::clamp(static_cast<int>(std::lround(a.PossessionShare(theirTeam) * 100.0f)), 0, 100);
            addCompare("Possession %", std::to_string(ourPoss) + "%", std::to_string(theirPoss) + "%",
                       ourPoss > theirPoss ? 1 : ourPoss < theirPoss ? -1
                                                                     : 0);

            int ourHits = 0, theirHits = 0;
            int ourAerials = 0, theirAerials = 0;
            int ourDemos = 0, theirDemos = 0;
            float ourSpeedSum = 0.0f, theirSpeedSum = 0.0f;
            int ourFirstTouches = 0, theirFirstTouches = 0;

            for (const auto& p : a.players) {
                if (p.team == ourTeam) {
                    ourHits += p.hits;
                    ourAerials += p.aerialHits;
                    ourDemos += p.demosInflicted;
                    ourSpeedSum += p.hitSpeedSum;
                    ourFirstTouches += p.firstTouches;
                } else if (p.team == theirTeam) {
                    theirHits += p.hits;
                    theirAerials += p.aerialHits;
                    theirDemos += p.demosInflicted;
                    theirSpeedSum += p.hitSpeedSum;
                    theirFirstTouches += p.firstTouches;
                }
            }

            const auto touchTotal = [](const std::vector<MatchDetailPlayer>& team) {
                std::optional<int> total;
                for (const auto& p : team)
                    if (p.touches) total = total.value_or(0) + *p.touches;
                return total;
            };
            const auto ourTouches = touchTotal(d.ourTeam);
            const auto theirTouches = touchTotal(d.theirTeam);
            if (ourTouches && theirTouches)
                addCompare("Touches", std::to_string(*ourTouches), std::to_string(*theirTouches),
                           *ourTouches > *theirTouches ? 1 : *ourTouches < *theirTouches ? -1
                                                                                         : 0);
            addCompare("Aerial Hits", std::to_string(ourAerials), std::to_string(theirAerials),
                       ourAerials > theirAerials ? 1 : ourAerials < theirAerials ? -1
                                                                                 : 0);
            addCompare("Demos Inflicted", std::to_string(ourDemos), std::to_string(theirDemos),
                       ourDemos > theirDemos ? 1 : ourDemos < theirDemos ? -1
                                                                         : 0);

            const float ourAvgSpd = ourHits > 0 ? (ourSpeedSum / static_cast<float>(ourHits)) : 0.0f;
            const float theirAvgSpd = theirHits > 0 ? (theirSpeedSum / static_cast<float>(theirHits)) : 0.0f;
            addCompare("Avg Hit Speed",
                       ourHits > 0 ? FormatSpeed(ourAvgSpd, imperial) : "--",
                       theirHits > 0 ? FormatSpeed(theirAvgSpd, imperial) : "--",
                       ourAvgSpd > theirAvgSpd ? 1 : ourAvgSpd < theirAvgSpd ? -1
                                                                             : 0);

            addCompare("Kickoff First Touches", std::to_string(ourFirstTouches), std::to_string(theirFirstTouches),
                       ourFirstTouches > theirFirstTouches ? 1 : ourFirstTouches < theirFirstTouches ? -1
                                                                                                     : 0);

            int ourKoGoals = 0, theirKoGoals = 0;
            for (const auto& k : a.kickoffs) {
                if (k.goalTeam == ourTeam) ourKoGoals++;
                if (k.goalTeam == theirTeam) theirKoGoals++;
            }
            addCompare("Kickoff Goals", std::to_string(ourKoGoals), std::to_string(theirKoGoals),
                       ourKoGoals > theirKoGoals ? 1 : ourKoGoals < theirKoGoals ? -1
                                                                                 : 0);
        }

        // 4. Timeline
        if (d.analytics.has_value() && !d.analytics->Empty()) {
            const auto& a = *d.analytics;
            const float totalDuration = a.playSeconds > 0.0f ? a.playSeconds : (d.durationSeconds.value_or(300.0f));

            for (const auto& w : a.possessionTimeline) {
                const float totalSec = w.seconds[0] + w.seconds[1];
                const float ourSec = (ourTeam == 0) ? w.seconds[0] : w.seconds[1];
                const float theirSec = (ourTeam == 0) ? w.seconds[1] : w.seconds[0];
                const int ourPct = totalSec > 0.0f ? std::clamp(static_cast<int>(std::lround(ourSec / totalSec * 100.0f)), 0, 100) : 50;
                const int theirPct = 100 - ourPct;
                m_timelineWindows.push_back({std::to_string(ourPct) + "%", std::to_string(theirPct) + "%"});
            }

            for (const auto& g : a.goals) {
                if (totalDuration <= 0.0f) continue;
                const float pct = std::clamp(g.playSeconds / totalDuration * 100.0f, 2.0f, 98.0f);
                char pctBuf[32]{};
                std::snprintf(pctBuf, sizeof(pctBuf), "%.1f%%", pct);

                const int ourG = (ourTeam == 0) ? g.scoreAfter[0] : g.scoreAfter[1];
                const int theirG = (ourTeam == 0) ? g.scoreAfter[1] : g.scoreAfter[0];
                const std::string scoreLabel = std::to_string(ourG) + "-" + std::to_string(theirG);
                const bool isOurs = (g.team == ourTeam);
                m_timelineGoals.push_back({pctBuf, scoreLabel, g.scorerName, isOurs});
            }
            m_timelineHasData = (!m_timelineWindows.empty() || !m_timelineGoals.empty());
        }

        // 5. Boost & movement
        const PlayerAnalytics* me = nullptr;
        if (d.analytics.has_value()) {
            if (!d.accountPrimaryId.empty()) {
                me = d.analytics->FindPlayer(d.accountPrimaryId);
            }
            if (!me) {
                for (const auto& p : d.analytics->players) {
                    if (p.car.has_value()) {
                        me = &p;
                        break;
                    }
                }
            }
            if (!me && !d.ourTeam.empty()) {
                for (const auto& p : d.ourTeam) {
                    if (p.isMe) {
                        me = d.analytics->FindPlayer(p.primaryId);
                        break;
                    }
                }
            }
        }

        if (me && me->car.has_value()) {
            m_hasCarTelemetry = true;
            m_telemetryNote = "Rocket League only provides car telemetry for your own car.";
            const auto& c = *me->car;
            m_carBpm = std::to_string(static_cast<int>(std::lround(c.BoostPerMinute())));
            m_carAvgBoost = std::to_string(static_cast<int>(std::lround(c.AverageBoost())));
            m_carZeroBoost = FormatSecAndPct(c.zeroBoostSeconds, c.sampledSeconds);
            m_carFullBoost = FormatSecAndPct(c.fullBoostSeconds, c.sampledSeconds);
            m_carBoostCollected = std::to_string(c.boostCollected);
            m_carBoostUsed = std::to_string(c.boostUsed);
            m_carBigPads = std::to_string(c.bigPads);
            m_carSmallPads = std::to_string(c.smallPads);
            m_carStolenPads = c.padsFromPickupEvents
                                  ? (std::to_string(c.stolenBigPads) + " big / " + std::to_string(c.stolenSmallPads) + " small")
                                  : "—";
            m_carAvgSpeed = FormatSpeed(c.AverageSpeed(), imperial);
            m_carMaxSpeed = FormatSpeed(c.maxSpeed, imperial);
            m_carPowerslides = std::to_string(c.powerslideCount) + " (" + FormatSec(c.powerslideSeconds) + ")";
            m_carDemoTime = FormatSec(c.demolishedSeconds);

            const float totalBands = c.boostBandSeconds[0] + c.boostBandSeconds[1] +
                                     c.boostBandSeconds[2] + c.boostBandSeconds[3];
            m_boostBand0 = PctStr(c.boostBandSeconds[0], totalBands);
            m_boostBand1 = PctStr(c.boostBandSeconds[1], totalBands);
            m_boostBand2 = PctStr(c.boostBandSeconds[2], totalBands);
            m_boostBand3 = PctStr(c.boostBandSeconds[3], totalBands);

            const float totalSpeed = c.slowSeconds + c.boostSpeedSeconds + c.supersonicSeconds;
            m_speedSlowPct = PctStr(c.slowSeconds, totalSpeed);
            m_speedBoostPct = PctStr(c.boostSpeedSeconds, totalSpeed);
            m_speedSupersonicPct = PctStr(c.supersonicSeconds, totalSpeed);
            m_speedSlowLabel = FormatSecAndPct(c.slowSeconds, c.sampledSeconds);
            m_speedBoostLabel = FormatSecAndPct(c.boostSpeedSeconds, c.sampledSeconds);
            m_speedSupersonicLabel = FormatSecAndPct(c.supersonicSeconds, c.sampledSeconds);

            const float totalSurface = c.groundSeconds + c.airSeconds + c.wallSeconds;
            m_surfaceGroundPct = PctStr(c.groundSeconds, totalSurface);
            m_surfaceAirPct = PctStr(c.airSeconds, totalSurface);
            m_surfaceWallPct = PctStr(c.wallSeconds, totalSurface);
            m_surfaceGroundLabel = FormatSecAndPct(c.groundSeconds, c.sampledSeconds);
            m_surfaceAirLabel = FormatSecAndPct(c.airSeconds, c.sampledSeconds);
            m_surfaceWallLabel = FormatSecAndPct(c.wallSeconds, c.sampledSeconds);
        } else if (d.localStats.has_value()) {
            m_hasLegacyStats = true;
            m_telemetryNote = "Detailed tracking applies to matches played from this version on.";
            m_legacyBoostCollected = std::to_string(d.localStats->boostCollected);
            m_legacyDemoed = std::to_string(d.localStats->demoed);
            m_legacyCrossbars = std::to_string(d.localStats->crossbars);
            m_legacyHardestCrossbar = d.localStats->hardestCrossbar
                                          ? FormatSpeed(*d.localStats->hardestCrossbar, imperial)
                                          : "--";
            m_legacyMaxBallSpeed = d.localStats->maxBallSpeed
                                       ? FormatSpeed(*d.localStats->maxBallSpeed, imperial)
                                       : "--";
            m_legacyOwnGoals = std::to_string(d.localStats->ownGoals);
        }

        // 6. Mechanics matrix
        if (d.analytics.has_value() && !d.analytics->Empty()) {
            const auto& a = *d.analytics;
            std::unordered_set<std::string> activeEvents;
            for (const auto& p : a.players) {
                for (const auto& [name, count] : p.statEvents) {
                    if (count > 0) activeEvents.insert(name);
                }
            }

            const std::vector<std::string> priorityOrder = {
                "Shot", "Save", "EpicSave", "Assist", "Goal", "Clear", "Center",
                "Demolish", "AerialHit", "AerialGoal", "BicycleHit", "TurtleGoal",
                "HatTrick", "Playmaker", "Savior"};

            std::vector<std::string> orderedEvents;
            for (const auto& ev : priorityOrder) {
                if (activeEvents.count(ev)) {
                    orderedEvents.push_back(ev);
                    activeEvents.erase(ev);
                }
            }
            std::vector<std::string> remaining(activeEvents.begin(), activeEvents.end());
            std::sort(remaining.begin(), remaining.end());
            orderedEvents.insert(orderedEvents.end(), remaining.begin(), remaining.end());

            for (const auto& ev : orderedEvents) {
                m_mechanicsColumns.push_back(PrettifyEventName(ev));
            }

            auto buildTeamRows = [&](int teamNum, const std::string& teamTitle) {
                std::vector<int> teamTotals(orderedEvents.size(), 0);
                for (const auto& p : a.players) {
                    if (p.team != teamNum) continue;
                    HistoryMechanicsRow row;
                    row.player_name = p.name.empty() ? p.primaryId : p.name;
                    row.is_me = (me && p.primaryId == me->primaryId);
                    row.is_total = false;
                    for (size_t i = 0; i < orderedEvents.size(); ++i) {
                        const int count = p.StatEvent(orderedEvents[i]);
                        teamTotals[i] += count;
                        row.values.push_back(count > 0 ? std::to_string(count) : "—");
                    }
                    m_mechanicsRows.push_back(std::move(row));
                }
                HistoryMechanicsRow totalRow;
                totalRow.player_name = teamTitle + " Total";
                totalRow.is_total = true;
                for (int t : teamTotals) {
                    totalRow.values.push_back(std::to_string(t));
                }
                m_mechanicsRows.push_back(std::move(totalRow));
            };

            buildTeamRows(ourTeam, "Our Team");
            buildTeamRows(theirTeam, "Opponents");
            m_hasMechanics = !m_mechanicsColumns.empty();
        }

        // 7. Field top-down pitch
        if (d.analytics.has_value() && !d.analytics->Empty()) {
            const auto& a = *d.analytics;
            const std::string myPid = me ? me->primaryId : d.accountPrimaryId;

            size_t touchLimit = 150;
            size_t touchCount = 0;
            for (const auto& h : a.hits) {
                if (!myPid.empty() && h.playerId != myPid) continue;
                touchCount++;
                if (m_fieldDots.size() >= touchLimit) continue;

                float X = h.location.x;
                float Y = h.location.y;
                if (ourTeam == 1) {
                    X = -X;
                    Y = -Y;
                }
                const float xPct = std::clamp((X - (-4096.0f)) / 8192.0f * 100.0f, 2.0f, 98.0f);
                const float yPct = std::clamp((5120.0f - Y) / 10240.0f * 100.0f, 2.0f, 98.0f);

                char xBuf[32]{}, yBuf[32]{};
                std::snprintf(xBuf, sizeof(xBuf), "%.1f%%", xPct);
                std::snprintf(yBuf, sizeof(yBuf), "%.1f%%", yPct);

                std::string dotClass = "pitch-dot pitch-dot-soft";
                if (h.speed >= 100.0f) {
                    dotClass = "pitch-dot pitch-dot-hard";
                } else if (h.speed >= 70.0f) {
                    dotClass = "pitch-dot pitch-dot-medium";
                }
                m_fieldDots.push_back({xBuf, yBuf, std::move(dotClass)});
            }

            for (const auto& g : a.goals) {
                if (!g.location.has_value()) continue;
                float X = g.location->x;
                float Y = g.location->y;
                if (ourTeam == 1) {
                    X = -X;
                    Y = -Y;
                }
                const float xPct = std::clamp((X - (-4096.0f)) / 8192.0f * 100.0f, 3.0f, 97.0f);
                const float yPct = std::clamp((5120.0f - Y) / 10240.0f * 100.0f, 3.0f, 97.0f);

                char xBuf[32]{}, yBuf[32]{};
                std::snprintf(xBuf, sizeof(xBuf), "%.1f%%", xPct);
                std::snprintf(yBuf, sizeof(yBuf), "%.1f%%", yPct);

                const bool isOurGoal = (g.team == ourTeam);
                std::string markerClass = isOurGoal ? "pitch-goal pitch-goal-our" : "pitch-goal pitch-goal-their";
                std::string label = g.scorerName.empty() ? "Goal" : g.scorerName;
                m_fieldGoals.push_back({xBuf, yBuf, std::move(markerClass), std::move(label)});
            }

            m_fieldHasData = (!m_fieldDots.empty() || !m_fieldGoals.empty());
            m_fieldTouchesCount = std::to_string(touchCount) + " touches recorded";
            if (imperial) {
                m_pitchLegendSoft = "< 43 mph (Soft)";
                m_pitchLegendMed = "43 - 62 mph (Medium)";
                m_pitchLegendHard = ">= 62 mph (Hard)";
            } else {
                m_pitchLegendSoft = "< 70 km/h (Soft)";
                m_pitchLegendMed = "70 - 100 km/h (Medium)";
                m_pitchLegendHard = ">= 100 km/h (Hard)";
            }
        }

        // 8. Event log
        auto labelForType = [](const std::string& type) -> std::string {
            if (type == "goal") return "Goal";
            if (type == "save") return "Save";
            if (type == "epic_save") return "Epic Save";
            if (type == "assist") return "Assist";
            if (type == "shot") return "Shot";
            if (type == "demolition") return "Demolition";
            if (type == "own_goal") return "Own Goal";
            if (type == "crossbar") return "Crossbar";
            return "Event";
        };

        const std::optional<int> teamFilterValue = d.ourTeam.empty() ? std::nullopt : std::optional<int>(ourTeam);
        for (const auto& e : d.events) {
            if (teamFilterValue && e.team && (*e.team == 0 || *e.team == 1)) {
                m_eventTeamAvailable = true;
            }
            std::string category = "other";
            if (e.type == "goal" || e.type == "own_goal") {
                category = "goals";
            } else if (e.type == "save" || e.type == "epic_save") {
                category = "saves";
            } else if (e.type == "shot") {
                category = "shots";
            } else if (e.type == "demolition") {
                category = "demolitions";
            }

            if (m_eventFilter != "all" && m_eventFilter != category) continue;
            if (m_eventTeamFilter != "all") {
                if (!teamFilterValue || !e.team) continue;
                const bool ours = *teamFilterValue == *e.team;
                if ((m_eventTeamFilter == "ours") != ours) continue;
            }

            HistoryEventRow row;
            row.category = category;
            row.type = labelForType(e.type);
            std::string playerName = e.actorName.empty() ? (e.actorId.empty() ? "Unknown player" : e.actorId) : e.actorName;
            if (e.type == "goal") {
                const std::string assist = !e.secondaryName.empty() ? e.secondaryName : e.secondaryId;
                if (!assist.empty()) {
                    playerName += " (assist " + assist + ")";
                }
            }
            row.player = std::move(playerName);
            row.clock = e.matchClockSeconds ? FormatMinutesSeconds(static_cast<float>(*e.matchClockSeconds)) : "--:--";

            if (e.type == "demolition" && (!e.secondaryName.empty() || !e.secondaryId.empty())) {
                row.detail = "-> " + (!e.secondaryName.empty() ? e.secondaryName : e.secondaryId);
            } else if (e.type == "crossbar" && e.value.has_value()) {
                row.detail = std::to_string(static_cast<int>(std::lround(*e.value))) + " force";
            }

            if (e.ballSpeed.has_value()) {
                const std::string speed = FormatSpeed(*e.ballSpeed, imperial);
                if (row.detail.empty()) {
                    row.detail = speed;
                } else {
                    row.detail += " · " + speed;
                }
            }
            m_detailEvents.push_back(std::move(row));
        }
        m_eventEmpty = d.events.empty() ? "No detailed events were recorded for this match." : "No events match this filter.";
    }

    for (const char* name : {
             "detail_open", "detail_loading", "detail_tab", "detail_result", "detail_win", "detail_score",
             "detail_playlist", "detail_arena", "detail_time", "detail_mmr_change", "detail_mmr_tone",
             "detail_duration", "detail_v1_note", "detail_our_team", "detail_their_team",
             "detail_ballchasing_url", "detail_story_lines", "timeline_has_data", "timeline_windows",
             "timeline_goals", "team_compare_rows", "has_car_telemetry", "has_legacy_stats",
             "telemetry_note", "car_bpm", "car_avg_boost", "car_zero_boost", "car_full_boost",
             "car_boost_collected", "car_boost_used", "car_big_pads", "car_small_pads",
             "car_stolen_pads", "car_avg_speed", "car_max_speed", "car_powerslides", "car_demo_time",
             "boost_band_0", "boost_band_1", "boost_band_2", "boost_band_3", "speed_slow_pct",
             "speed_boost_pct", "speed_supersonic_pct", "speed_slow_label", "speed_boost_label",
             "speed_supersonic_label", "surface_ground_pct", "surface_air_pct", "surface_wall_pct",
             "surface_ground_label", "surface_air_label", "surface_wall_label", "legacy_boost_collected",
             "legacy_demoed", "legacy_crossbars", "legacy_hardest_crossbar", "legacy_max_ball_speed",
             "legacy_ownGoals", "has_mechanics", "mechanics_columns", "mechanics_rows",
             "field_has_data", "field_dots", "field_goals", "field_touches_count", "detail_events",
             "event_empty", "event_team_available", "pitch_legend_soft", "pitch_legend_med",
             "pitch_legend_hard"}) {
        Dirty(name);
    }
}

std::string HistoryView::BuildScoreboardSummary(const MatchDetail& detail) {
    if (!detail.found) return {};
    std::ostringstream out;
    out << (detail.win ? "WIN" : "LOSS") << " " << detail.ourScore << "-" << detail.theirScore
        << " | " << (detail.playlist.empty() ? "Match" : detail.playlist)
        << " | " << (detail.arena.empty() ? "Unknown Arena" : detail.arena)
        << " | " << FormatDateTime(detail.timestampUnix) << "\n";

    if (detail.mmrBefore.has_value() && detail.mmrAfter.has_value()) {
        const int delta = *detail.mmrAfter - *detail.mmrBefore;
        out << "MMR: " << *detail.mmrBefore << " -> " << *detail.mmrAfter << " (" << SignedDelta(delta) << ")\n";
    } else if (detail.mmrAfter.has_value()) {
        out << "MMR: " << *detail.mmrAfter << "\n";
    }

    const std::string dur = FormatDurationAndOvertime(detail.durationSeconds, detail.overtimeSeconds);
    if (!dur.empty()) {
        out << "Duration: " << dur << "\n";
    }

    const auto story = MatchStory::BuildMatchStory(detail);
    if (!story.empty()) {
        out << "\nStory:\n";
        for (const auto& line : story) {
            out << "- " << line << "\n";
        }
    }

    if (detail.analytics.has_value() && !detail.analytics->Empty()) {
        const bool imperial = Config::Read().imperial_units;
        const auto& a = *detail.analytics;
        const int ourTeam = detail.ourTeam.empty() ? 0 : detail.ourTeam.front().team;
        const int theirTeam = (ourTeam == 0) ? 1 : 0;
        out << "\nPossession: " << std::clamp(static_cast<int>(std::lround(a.PossessionShare(ourTeam) * 100.0f)), 0, 100) << "% - "
            << std::clamp(static_cast<int>(std::lround(a.PossessionShare(theirTeam) * 100.0f)), 0, 100) << "%\n";
        out << "Ball Speed: Avg " << FormatSpeed(a.AverageBallSpeed(), imperial) << " | Max "
            << FormatSpeed(a.maxBallSpeed, imperial) << "\n";

        for (const auto& p : a.players) {
            if (p.car.has_value()) {
                const auto& c = *p.car;
                out << "\nYour Telemetry: " << static_cast<int>(std::lround(c.BoostPerMinute())) << " BPM | Avg Boost "
                    << static_cast<int>(std::lround(c.AverageBoost())) << " | Zero Boost "
                    << std::clamp(static_cast<int>(std::lround(c.Share(c.zeroBoostSeconds) * 100.0f)), 0, 100) << "% | Supersonic "
                    << std::clamp(static_cast<int>(std::lround(c.Share(c.supersonicSeconds) * 100.0f)), 0, 100) << "%\n";
                break;
            }
        }
    } else if (detail.localStats.has_value()) {
        const auto& ls = *detail.localStats;
        out << "\nStats: Boost Collected " << ls.boostCollected << " | Demos Taken " << ls.demoed
            << " | Crossbars " << ls.crossbars << "\n";
    }

    const bool summaryImperial = Config::Read().imperial_units;
    auto writeTeam = [&](const char* heading, const std::vector<MatchDetailPlayer>& team) {
        out << "\n"
            << heading << " (Name | Tier | MMR | Score | G | A | Sv | Sh | Dem | Touches | Met)\n";
        for (const auto& p : team) {
            const HistoryDetailPlayerRow r = FormatDetailPlayer(p, detail.analytics ? &*detail.analytics : nullptr, nullptr, false, summaryImperial);
            out << "- " << r.name;
            if (!r.platform.empty()) out << " (" << r.platform << ")";
            out << " | " << r.tier << " | " << r.mmr
                << " | " << r.score << " | " << r.goals << " | " << r.assists
                << " | " << r.saves << " | " << r.shots << " | " << r.demos
                << " | " << r.touches
                << " | " << r.encounter << "\n";
        }
    };

    writeTeam("OUR TEAM", detail.ourTeam);
    writeTeam("OPPONENTS", detail.theirTeam);
    return out.str();
}

std::string HistoryView::FormatScoreboardText() const {
    if (!m_detailSource.has_value() || !m_detailSource->found) return {};
    return BuildScoreboardSummary(*m_detailSource);
}
