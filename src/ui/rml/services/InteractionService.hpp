#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <string>
#include <vector>
#include "ui/config/DashboardLayoutConfig.hpp"
#include "ui/config/OverlayLayoutConfig.hpp"

enum class DragKind {
    None,
    DashboardWidget,
    OverlayMove,
    OverlayResize,
    OverlayWidget,
    OverlayToolboxWidget,
    SettingsMove,
    FloatingCardMove,
    ColorField,
    ColorHue
};

struct DragSnapRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    std::string id;
};

struct DragState {
    DragKind kind = DragKind::None;
    DashboardLayout::WidgetId widget = DashboardLayout::WidgetId::LiveRoster;
    std::string containerId;
    std::string sourceContainerId;
    float startMouseX = 0.0f;
    float startMouseY = 0.0f;
    float startX = 0.0f;
    float startY = 0.0f;
    float startW = 0.0f;
    float startH = 0.0f;
    bool allowDock = false;
    Rml::Element* element = nullptr;
    Rml::Element* guideX = nullptr;
    Rml::Element* guideY = nullptr;
    Rml::Element* dropTarget = nullptr;
    Rml::Element* dockGuide = nullptr;
    Rml::Element* dockTarget = nullptr;
    std::vector<DragSnapRect> snapRects;
};

class InteractionService {
  public:
    InteractionService() = default;

    const DragState& Drag() const noexcept {
        return m_drag;
    }
    DragState& Drag() noexcept {
        return m_drag;
    }
    bool IsDragging() const noexcept {
        return m_drag.kind != DragKind::None;
    }
    void ClearDrag() noexcept {
        m_drag = {};
    }

  private:
    DragState m_drag;
};
