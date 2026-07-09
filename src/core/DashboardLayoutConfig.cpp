#include "DashboardLayoutConfig.hpp"
#include <algorithm>
#include <map>

namespace DashboardLayout {

LayoutConfig DefaultLayout() {
    return {
        .version = kLayoutVersion,
        .leftColumnWeight = 0.48f,
        .widgets = {
            {WidgetId::LiveRoster,         Zone::Left,  0, 0.0f, false},
            {WidgetId::LiveMatchStats,    Zone::Left,  1, 0.0f, false},
            {WidgetId::LobbyRanks,        Zone::Left,  2, 0.0f, false},
            {WidgetId::MmrGraph,          Zone::Top,   0, 220.0f, false},
            {WidgetId::PreviousGames,     Zone::Top,   1, 0.0f, false},
            {WidgetId::SessionStats,      Zone::Right, 0, 0.0f, false},
            {WidgetId::DemoTracker,       Zone::Right, 1, 0.0f, false},
            {WidgetId::StreaksStats,      Zone::Right, 2, 0.0f, false},
            {WidgetId::GamemodeBreakdown, Zone::Right, 3, 0.0f, false},
        }
    };
}

void Sanitize(LayoutConfig& layout) {
    if (layout.version < kLayoutVersion) layout.version = kLayoutVersion;
    layout.leftColumnWeight = std::clamp(layout.leftColumnWeight, 0.25f, 0.75f);

    // Ensure all widgets are present
    std::vector<WidgetId> allIds = {
        WidgetId::LiveRoster,
        WidgetId::LiveMatchStats,
        WidgetId::SessionStats,
        WidgetId::MmrGraph,
        WidgetId::StreaksStats,
        WidgetId::GamemodeBreakdown,
        WidgetId::LobbyRanks,
        WidgetId::DemoTracker,
        WidgetId::PreviousGames
    };

    std::vector<WidgetPlacement> validWidgets;
    for (const auto& id : allIds) {
        bool found = false;
        for (const auto& w : layout.widgets) {
            if (w.id == id) {
                validWidgets.push_back(w);
                found = true;
                break;
            }
        }
        if (!found) {
            // Add missing with default placement
            LayoutConfig def = DefaultLayout();
            for (const auto& dw : def.widgets) {
                if (dw.id == id) {
                    validWidgets.push_back(dw);
                    break;
                }
            }
        }
    }

    layout.widgets = validWidgets;

    // Sort by zone and order
    std::sort(layout.widgets.begin(), layout.widgets.end(), [](const WidgetPlacement& a, const WidgetPlacement& b) {
        if (a.zone != b.zone) return static_cast<int>(a.zone) < static_cast<int>(b.zone);
        return a.order < b.order;
    });

    // Re-number order to be continuous
    std::map<Zone, int> counters;
    for (auto& w : layout.widgets) {
        w.order = counters[w.zone]++;
    }
}

const char* ToConfigString(WidgetId id) {
    switch (id) {
        case WidgetId::LiveRoster: return "live_roster";
        case WidgetId::LiveMatchStats: return "live_match_stats";
        case WidgetId::SessionStats: return "session_stats";
        case WidgetId::MmrGraph: return "mmr_graph";
        case WidgetId::StreaksStats: return "streaks_stats";
        case WidgetId::GamemodeBreakdown: return "gamemode_breakdown";
        case WidgetId::LobbyRanks: return "lobby_ranks";
        case WidgetId::DemoTracker: return "demo_tracker";
        case WidgetId::PreviousGames: return "previous_games";
    }
    return "unknown";
}

const char* ToConfigString(Zone zone) {
    switch (zone) {
        case Zone::Left: return "left";
        case Zone::Right: return "right";
        case Zone::Bottom: return "bottom";
        case Zone::Hidden: return "hidden";
        case Zone::Top: return "top";
    }
    return "unknown";
}

WidgetId WidgetIdFromConfigString(const std::string& value) {
    if (value == "live_roster") return WidgetId::LiveRoster;
    if (value == "live_match_stats") return WidgetId::LiveMatchStats;
    if (value == "session_stats") return WidgetId::SessionStats;
    if (value == "mmr_graph") return WidgetId::MmrGraph;
    if (value == "streaks_stats") return WidgetId::StreaksStats;
    if (value == "gamemode_breakdown") return WidgetId::GamemodeBreakdown;
    if (value == "lobby_ranks") return WidgetId::LobbyRanks;
    if (value == "demo_tracker") return WidgetId::DemoTracker;
    if (value == "previous_games") return WidgetId::PreviousGames;
    return WidgetId::LiveRoster;
}

Zone ZoneFromConfigString(const std::string& value) {
    if (value == "left") return Zone::Left;
    if (value == "right") return Zone::Right;
    if (value == "bottom") return Zone::Bottom;
    if (value == "hidden") return Zone::Hidden;
    if (value == "top") return Zone::Top;
    return Zone::Left;
}

const char* GetWidgetDisplayName(WidgetId id) {
    switch (id) {
        case WidgetId::LiveRoster: return "Live Match & Roster";
        case WidgetId::LiveMatchStats: return "Live Match Stats";
        case WidgetId::SessionStats: return "Session Stats";
        case WidgetId::MmrGraph: return "MMR Graph";
        case WidgetId::StreaksStats: return "Streaks & Stats";
        case WidgetId::GamemodeBreakdown: return "Gamemode Breakdown";
        case WidgetId::LobbyRanks: return "Lobby Ranks";
        case WidgetId::DemoTracker: return "Demolition Tracker";
        case WidgetId::PreviousGames: return "Previous Games";
    }
    return "Unknown Widget";
}

} // namespace DashboardLayout
