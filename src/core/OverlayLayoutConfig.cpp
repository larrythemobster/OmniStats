#include "OverlayLayoutConfig.hpp"
#include <algorithm>
#include <set>

namespace OverlayLayout {

    LayoutConfig DefaultOverlayLayout() {
        LayoutConfig config;
        config.version = 1;
        config.toolboxOpen = false;

        // 1. Session Card (Top-Left)
        {
            ContainerConfig c;
            c.id = "session_card";
            c.x = 24.0f;
            c.y = 24.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::SessionStats};
            config.containers.push_back(c);
        }

        // 2. Demo Tracker (Middle-Left)
        {
            ContainerConfig c;
            c.id = "demo_tracker";
            c.x = 24.0f;
            c.y = 150.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::DemoTracker};
            config.containers.push_back(c);
        }

        // 3. Lobby Ranks (Top-Middle)
        {
            ContainerConfig c;
            c.id = "lobby_ranks";
            c.x = 450.0f;
            c.y = 24.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::LobbyRanks};
            config.containers.push_back(c);
        }

        // 4. Previous Games (Top-Middle-ish, below Lobby Ranks)
        {
            ContainerConfig c;
            c.id = "previous_games";
            c.x = 400.0f;
            c.y = 120.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::PreviousGames};
            config.containers.push_back(c);
        }

        // 5. Main H2H Stack (Top-Right)
        {
            ContainerConfig c;
            c.id = "main_stack";
            c.x = 1500.0f; // Soft default, will position on right of screen
            c.y = 24.0f;
            c.w = 410.0f;
            c.h = 0.0f;
            c.widgets = {
                DashboardLayout::WidgetId::LiveRoster,
                DashboardLayout::WidgetId::LiveMatchStats,
                DashboardLayout::WidgetId::StreaksStats,
                DashboardLayout::WidgetId::GamemodeBreakdown};
            config.containers.push_back(c);
        }

        return config;
    }

    void Sanitize(LayoutConfig& layout) {
        if (layout.version < 1) {
            layout.version = 1;
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
