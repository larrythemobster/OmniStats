#include "ui/config/WidgetSettings.hpp"
#include <algorithm>

namespace WidgetSettings {
    namespace {
        bool Allowed(const ConfigData& c, DashboardLayout::WidgetId widget, bool dashboard) {
            using Id = DashboardLayout::WidgetId;
            switch (widget) {
            case Id::LiveRoster:
                return !dashboard || c.second_monitor_show_roster;
            case Id::SessionStats:
                return !dashboard || c.second_monitor_show_session;
            case Id::LobbyRanks:
                return c.show_lobby_ranks_overlay;
            case Id::DemoTracker:
                return c.show_demo_tracker_overlay;
            case Id::RankProgress:
                return c.show_rank_progress;
            case Id::PreviousGames:
                return c.show_previous_games_summary;
            case Id::StreaksStats:
                return c.show_streaks_stats;
            case Id::GamemodeBreakdown:
                return c.show_gamemode_breakdown;
            case Id::Tournaments:
                return !dashboard && c.show_tournaments;
            default:
                return true;
            }
        }
        void EnableLegacyFlag(ConfigData& c, DashboardLayout::WidgetId widget) {
            using Id = DashboardLayout::WidgetId;
            switch (widget) {
            case Id::LiveRoster:
                c.second_monitor_show_roster = true;
                break;
            case Id::SessionStats:
                c.second_monitor_show_session = true;
                break;
            case Id::LobbyRanks:
                c.show_lobby_ranks_overlay = true;
                break;
            case Id::DemoTracker:
                c.show_demo_tracker_overlay = true;
                break;
            case Id::RankProgress:
                c.show_rank_progress = true;
                break;
            case Id::PreviousGames:
                c.show_previous_games_summary = true;
                break;
            case Id::StreaksStats:
                c.show_streaks_stats = true;
                break;
            case Id::GamemodeBreakdown:
                c.show_gamemode_breakdown = true;
                break;
            case Id::Tournaments:
                c.show_tournaments = true;
                break;
            default:
                break;
            }
        }
    }
    bool Enabled(const ConfigData& config, DashboardLayout::WidgetId widget, bool dashboard) {
        if (!Allowed(config, widget, dashboard)) return false;
        if (dashboard) {
            for (const auto& placement : config.dashboard_layout.widgets)
                if (placement.id == widget) return placement.zone != DashboardLayout::Zone::Hidden && placement.enabled;
            return false;
        }
        if (std::find(config.overlay_layout.disabledWidgets.begin(), config.overlay_layout.disabledWidgets.end(), widget) != config.overlay_layout.disabledWidgets.end()) return false;
        for (const auto& container : config.overlay_layout.containers)
            if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) return true;
        return false;
    }
    void SetEnabled(ConfigData& config, DashboardLayout::WidgetId widget, bool dashboard, bool enabled, float width, float height) {
        if (enabled) EnableLegacyFlag(config, widget);
        if (dashboard) {
            DashboardLayout::Sanitize(config.dashboard_layout);
            for (auto& placement : config.dashboard_layout.widgets) {
                if (placement.id != widget) continue;
                placement.enabled = enabled;
                if (enabled && placement.zone == DashboardLayout::Zone::Hidden) {
                    placement.zone = widget == DashboardLayout::WidgetId::Tournaments ? DashboardLayout::Zone::Top : DashboardLayout::Zone::Right;
                    placement.order = static_cast<int>(config.dashboard_layout.widgets.size());
                }
            }
            DashboardLayout::Sanitize(config.dashboard_layout);
            return;
        }
        auto& disabled = config.overlay_layout.disabledWidgets;
        std::erase(disabled, widget);
        if (!enabled) {
            disabled.push_back(widget);
            return;
        }
        for (const auto& container : config.overlay_layout.containers)
            if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) return;
        OverlayLayout::ContainerConfig container;
        container.id = "card_" + std::string(DashboardLayout::ToConfigString(widget));
        container.x = std::max(12.0f, (width - 340.0f) * 0.5f);
        container.y = std::max(12.0f, height * 0.25f);
        container.widgets = {widget};
        container.visibility.mode = OverlayLayout::Visibility::Always;
        config.overlay_layout.containers.push_back(std::move(container));
        OverlayLayout::Sanitize(config.overlay_layout);
    }
}
