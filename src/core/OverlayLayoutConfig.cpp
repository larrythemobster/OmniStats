#include "OverlayLayoutConfig.hpp"
#include <algorithm>
#include <set>

namespace OverlayLayout {

    LayoutConfig DefaultOverlayLayout() {
        LayoutConfig config;
        config.version = 3;
        config.toolboxOpen = false;

        // 1. Demo Tracker, Session Stats, Streaks Stats (docked)
        {
            ContainerConfig c;
            c.id = "demo_tracker";
            c.x = 1270.0f;
            c.y = 0.0f;
            c.w = 220.0f;
            c.h = 489.0f;
            c.widgets = {
                DashboardLayout::WidgetId::DemoTracker,
                DashboardLayout::WidgetId::StreaksStats,
                DashboardLayout::WidgetId::SessionStats};
            config.containers.push_back(c);
        }

        // 2. Lobby Ranks
        {
            ContainerConfig c;
            c.id = "lobby_ranks";
            c.x = 743.5f;
            c.y = 774.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::LobbyRanks};
            config.containers.push_back(c);
        }

        // 3. Previous Games
        {
            ContainerConfig c;
            c.id = "previous_games";
            c.x = 427.5f;
            c.y = 0.0f;
            c.w = 316.0f;
            c.h = 415.0f;
            c.widgets = {DashboardLayout::WidgetId::PreviousGames};
            config.containers.push_back(c);
        }

        // 4. Main H2H Stack (Live Roster, Live Match Stats, Gamemode Breakdown) (docked)
        {
            ContainerConfig c;
            c.id = "main_stack";
            c.x = 1490.0f;
            c.y = 0.0f;
            c.w = 430.0f;
            c.h = 759.0f;
            c.widgets = {
                DashboardLayout::WidgetId::LiveRoster,
                DashboardLayout::WidgetId::LiveMatchStats,
                DashboardLayout::WidgetId::GamemodeBreakdown};
            config.containers.push_back(c);
        }

        return config;
    }

    void Sanitize(LayoutConfig& layout) {
        if (layout.version < 1) {
            layout.version = 1;
        }

        // Version 2 compacted the lobby-rank table to single-line rows and
        // narrower columns. Widths persisted against the old metrics leave a
        // large empty card, so drop them once and let the container auto-size.
        if (layout.version < 2) {
            for (auto& container : layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
            layout.version = 2;
        }

        // Version 3 calculates lobby-rank table width dynamically from enabled
        // playlist columns. Reset previously clamped oversized dimensions so
        // containers auto-size to the new compact metrics.
        if (layout.version < 3) {
            for (auto& container : layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
            layout.version = 3;
        }

        // Ensure we don't have duplicate widgets across multiple containers
        std::set<DashboardLayout::WidgetId> seenWidgets;
        std::vector<ContainerConfig> validContainers;

        for (auto& c : layout.containers) {
            std::vector<DashboardLayout::WidgetId> validWidgets;
            for (auto w : c.widgets) {
                if (seenWidgets.find(w) == seenWidgets.end()) {
                    seenWidgets.insert(w);
                    validWidgets.push_back(w);
                }
            }
            if (!validWidgets.empty()) {
                c.widgets = validWidgets;
                validContainers.push_back(c);
            }
        }
        layout.containers = validContainers;
    }

} // namespace OverlayLayout
