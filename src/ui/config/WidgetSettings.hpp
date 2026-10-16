#pragma once
#include "core/config/Config.hpp"

namespace WidgetSettings {
    bool Enabled(const ConfigData& config, DashboardLayout::WidgetId widget, bool dashboard);
    void SetEnabled(ConfigData& config, DashboardLayout::WidgetId widget, bool dashboard, bool enabled, float width, float height);
}
