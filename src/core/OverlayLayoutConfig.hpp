#pragma once
#include <string>
#include <vector>
#include "core/DashboardLayoutConfig.hpp"

namespace OverlayLayout {

struct ContainerConfig {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    std::vector<DashboardLayout::WidgetId> widgets;
};

struct LayoutConfig {
    int version = 1;
    bool toolboxOpen = false;
    std::vector<ContainerConfig> containers;
};

LayoutConfig DefaultOverlayLayout();
void Sanitize(LayoutConfig& layout);

} // namespace OverlayLayout
