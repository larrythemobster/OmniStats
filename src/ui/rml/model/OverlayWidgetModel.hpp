#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>
#include <string>
#include <vector>
#include <memory>

#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"

#include "ui/rml/controller/RmlRenderSnapshot.hpp"
namespace Rml {
    class Context;
}
namespace Tournaments {
    class TournamentScheduleService;
}

struct RosterDivisionDot {
    bool filled = false;
    bool active = false;
};
inline bool operator==(const RosterDivisionDot& a, const RosterDivisionDot& b) noexcept {
    return a.filled == b.filled && a.active == b.active;
}

struct RosterPlayerItem {
    bool self = false;
    Rml::String name;
    Rml::String tracker_url;
    bool has_tracker = false;
    Rml::String platform;
    Rml::String platform_class;
    bool has_platform = false;
    Rml::String primary_id;
    bool can_note = false;
    bool has_note = false;
    Rml::String note_preview;
    bool use_rank_icons = false;
    Rml::String tier_icon;
    bool has_division = false;
    std::vector<RosterDivisionDot> division_dots;
    Rml::String rank_tooltip;
    bool has_tooltip = false;
    Rml::String tier_color;
    Rml::String tier_label;
    Rml::String mmr_text;
    Rml::String match_count_text;
    bool has_match_count = false;
    Rml::String total_wins_text;
    bool has_total_wins = false;
    std::vector<Rml::String> tags;
    bool has_tags = false;
    int flag_kind = 0; // 0=none, 1=you, 2=with_and_against, 3=with_only, 4=against_only, 5=new
    Rml::String with_record;
    Rml::String with_detail;
    Rml::String against_record;
    Rml::String against_detail;
};
inline bool operator==(const RosterPlayerItem& a, const RosterPlayerItem& b) noexcept {
    return a.self == b.self && a.name == b.name && a.tracker_url == b.tracker_url &&
           a.has_tracker == b.has_tracker && a.platform == b.platform && a.platform_class == b.platform_class &&
           a.has_platform == b.has_platform && a.primary_id == b.primary_id && a.can_note == b.can_note &&
           a.has_note == b.has_note && a.note_preview == b.note_preview && a.use_rank_icons == b.use_rank_icons &&
           a.tier_icon == b.tier_icon && a.has_division == b.has_division &&
           a.division_dots == b.division_dots && a.rank_tooltip == b.rank_tooltip && a.has_tooltip == b.has_tooltip &&
           a.tier_color == b.tier_color && a.tier_label == b.tier_label &&
           a.mmr_text == b.mmr_text && a.match_count_text == b.match_count_text && a.has_match_count == b.has_match_count &&
           a.total_wins_text == b.total_wins_text && a.has_total_wins == b.has_total_wins && a.tags == b.tags &&
           a.has_tags == b.has_tags && a.flag_kind == b.flag_kind && a.with_record == b.with_record &&
           a.with_detail == b.with_detail && a.against_record == b.against_record && a.against_detail == b.against_detail;
}
inline bool operator!=(const RosterPlayerItem& a, const RosterPlayerItem& b) noexcept {
    return !(a == b);
}

struct LobbyRankCell {
    Rml::String tier_label;
    Rml::String mmr_text;
    Rml::String match_count_text;
    Rml::String placeholder;
    Rml::String color;
    Rml::String tooltip;
    bool has_tier_label = false;
    bool has_match_count = false;
    bool has_mmr = false;
    bool has_tooltip = false;
};
inline bool operator==(const LobbyRankCell& a, const LobbyRankCell& b) noexcept {
    return a.tier_label == b.tier_label && a.mmr_text == b.mmr_text &&
           a.match_count_text == b.match_count_text && a.placeholder == b.placeholder &&
           a.color == b.color && a.tooltip == b.tooltip &&
           a.has_tier_label == b.has_tier_label && a.has_match_count == b.has_match_count &&
           a.has_mmr == b.has_mmr && a.has_tooltip == b.has_tooltip;
}
inline bool operator!=(const LobbyRankCell& a, const LobbyRankCell& b) noexcept {
    return !(a == b);
}

struct LobbyRankRow {
    Rml::String name;
    Rml::String platform;
    Rml::String platform_class;
    bool self = false;
    bool has_platform = false;
    std::vector<LobbyRankCell> cells;
};
inline bool operator==(const LobbyRankRow& a, const LobbyRankRow& b) noexcept {
    return a.name == b.name && a.platform == b.platform && a.platform_class == b.platform_class &&
           a.self == b.self && a.has_platform == b.has_platform && a.cells == b.cells;
}
inline bool operator!=(const LobbyRankRow& a, const LobbyRankRow& b) noexcept {
    return !(a == b);
}

struct LobbyRankColumn {
    Rml::String label;
};
inline bool operator==(const LobbyRankColumn& a, const LobbyRankColumn& b) noexcept {
    return a.label == b.label;
}
inline bool operator!=(const LobbyRankColumn& a, const LobbyRankColumn& b) noexcept {
    return !(a == b);
}

struct GraphBandItem {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    Rml::String color;
    Rml::String label;
    Rml::String label_color;
    float label_left = 0.0f;
    float label_top = 0.0f;
    bool has_label = false;
};

struct GraphGridlineItem {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    bool boundary = false;
};

struct GraphLabelItem {
    float right = 0.0f;
    float top = 0.0f;
    Rml::String text;
};

struct GraphRankLineItem {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    Rml::String color;
    Rml::String text;
    bool tier_start = false;
    bool below = false;
};

struct GraphBaselineDashItem {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
};

struct GraphDotItem {
    float left = 0.0f;
    float top = 0.0f;
    Rml::String cls;
    Rml::String tip;
    Rml::String label;
    Rml::String color;
    bool has_label = false;
    bool estimated = false;
};
inline bool operator==(const GraphDotItem& a, const GraphDotItem& b) noexcept {
    return a.left == b.left && a.top == b.top && a.cls == b.cls &&
           a.tip == b.tip && a.label == b.label && a.color == b.color &&
           a.has_label == b.has_label && a.estimated == b.estimated;
}
inline bool operator!=(const GraphDotItem& a, const GraphDotItem& b) noexcept {
    return !(a == b);
}

struct PreviousGameItem {
    int64_t match_id = 0;
    Rml::String match_guid;
    Rml::String playlist;
    Rml::String score;
    Rml::String mmr;
    Rml::String time;
    Rml::String win_loss_class;
    Rml::String even_odd_class;
};
inline bool operator==(const PreviousGameItem& a, const PreviousGameItem& b) noexcept {
    return a.match_id == b.match_id && a.match_guid == b.match_guid &&
           a.playlist == b.playlist && a.score == b.score && a.mmr == b.mmr &&
           a.time == b.time && a.win_loss_class == b.win_loss_class &&
           a.even_odd_class == b.even_odd_class;
}
inline bool operator!=(const PreviousGameItem& a, const PreviousGameItem& b) noexcept {
    return !(a == b);
}

struct GamemodeBreakdownRow {
    Rml::String mode;
    Rml::String record;
    Rml::String pct;
    Rml::String total;
};
inline bool operator==(const GamemodeBreakdownRow& a, const GamemodeBreakdownRow& b) noexcept {
    return a.mode == b.mode && a.record == b.record && a.pct == b.pct && a.total == b.total;
}
inline bool operator!=(const GamemodeBreakdownRow& a, const GamemodeBreakdownRow& b) noexcept {
    return !(a == b);
}

struct StreaksStatsModel {
    Rml::String current_streak;
    Rml::String current_streak_class;
    Rml::String longest_win;
    Rml::String longest_loss;
    bool show_longest_loss = false;
};

struct TournamentModel {
    Rml::String status_text;
    bool has_next = false;
    Rml::String title;
    Rml::String region;
    Rml::String countdown;
    bool starting_soon = false;
    bool topbar_visible = false;
    Rml::String topbar_title;
    Rml::String topbar_countdown;
    bool topbar_starting_soon = false;
    std::vector<Rml::String> topbar_slots;
};

struct StatRowItem {
    Rml::String label;
    Rml::String value;
    Rml::String cls;
};
inline bool operator==(const StatRowItem& a, const StatRowItem& b) noexcept {
    return a.label == b.label && a.value == b.value && a.cls == b.cls;
}
inline bool operator!=(const StatRowItem& a, const StatRowItem& b) noexcept {
    return !(a == b);
}

struct MatchSummaryModel {
    bool visible = false;
    Rml::String result;
    Rml::String result_class;
    Rml::String ot_tag;
    Rml::String score;
    Rml::String mmr_delta;
    Rml::String mmr_tone_class;
    Rml::String rating;
    std::vector<StatRowItem> play_stats;
    std::vector<StatRowItem> fun_stats;
};

class OverlayWidgetModel {
  public:
    OverlayWidgetModel() = default;

    bool Bind(Rml::DataModelConstructor& constructor);
    bool Create(Rml::Context* context);
    void Reset();
    void SetHandle(Rml::DataModelHandle handle) {
        m_handle = handle;
        m_bound = bool(handle);
    }
    // Updates model data from snapshot and marks changed variables dirty.
    // Returns true if any bound variable changed.
    bool Update(const RmlRenderSnapshot& snap,
                const ConfigData& config,
                const std::shared_ptr<SessionState>& state,
                const Tournaments::TournamentScheduleService* tournaments);

    // Getters for bound collections/values
    const std::vector<RosterPlayerItem>& RosterBlue() const noexcept {
        return m_rosterBlue;
    }
    const std::vector<RosterPlayerItem>& RosterOrange() const noexcept {
        return m_rosterOrange;
    }
    const std::vector<LobbyRankColumn>& LobbyColumns() const noexcept {
        return m_lobbyColumns;
    }
    const std::vector<LobbyRankRow>& LobbyPlayers() const noexcept {
        return m_lobbyPlayers;
    }
    const std::vector<PreviousGameItem>& PreviousGames() const noexcept {
        return m_previousGames;
    }

  private:
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;

    // Roster
    std::vector<RosterPlayerItem> m_rosterBlue;
    std::vector<RosterPlayerItem> m_rosterOrange;
    bool m_rosterWaitingBlue = true;
    bool m_rosterWaitingOrange = true;
    Rml::String m_rosterRankProgress;

    // Lobby Ranks
    std::vector<LobbyRankColumn> m_lobbyColumns;
    std::vector<LobbyRankRow> m_lobbyPlayers;
    bool m_lobbyRanksWaiting = true;

    // Previous Games
    std::vector<PreviousGameItem> m_previousGames;
    bool m_previousGamesLoading = true;
    bool m_previousGamesEmpty = false;
    int m_previousGamesLimit = 10;
    int m_previousGamesSessionWins = 0;
    int m_previousGamesSessionLosses = 0;

    // MMR Graph
    bool m_graphEmpty = true;
    Rml::String m_graphEmptyMessage;
    Rml::String m_graphEmptySubmessage;
    Rml::String m_graphCategory;
    Rml::String m_graphPoints;
    float m_graphFillTo = 0.86f;
    std::vector<GraphBandItem> m_graphBands;
    std::vector<GraphGridlineItem> m_graphGridlines;
    std::vector<GraphLabelItem> m_graphLabels;
    std::vector<GraphRankLineItem> m_graphRankLines;
    std::vector<GraphBaselineDashItem> m_graphBaselines;
    std::vector<GraphDotItem> m_graphDots;
    bool m_graphPanVisible = false;
    Rml::String m_graphPanLabel;
    bool m_graphPanOlderDisabled = false;
    bool m_graphPanNewerDisabled = false;

    // Stats
    StreaksStatsModel m_streaks;
    std::vector<GamemodeBreakdownRow> m_gamemodeRows;
    bool m_gamemodeEmpty = false;

    // Tournament
    TournamentModel m_tournament;

    // Match Summary
    MatchSummaryModel m_matchSummary;
};
