#include "ui/rml/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

void RmlUiController::HandleMouseDown(Rml::Element* target, Rml::Event& event) {
    if (target) {
        std::string tag = target->GetTagName();
        std::string targetAction = Attribute(target, "data-action");
        if (tag == "button" || targetAction == "overlay-remove-widget" || targetAction == "overlay-remove-container") {
            return;
        }
    }

    auto isDragAction = [](const std::string& a) {
        return a == "dashboard-drag" || a == "overlay-toolbox-drag" ||
               a == "overlay-widget-drag" || a == "settings-drag" ||
               a == "overlay-drag" || a == "overlay-resize" ||
               a == "floating-card-drag" || a == "color-field" || a == "color-hue";
    };

    Rml::Element* actionElement = target;
    std::string action = Attribute(actionElement, "data-action");
    while (!isDragAction(action) && actionElement && actionElement->GetParentNode()) {
        actionElement = actionElement->GetParentNode();
        action = Attribute(actionElement, "data-action");
    }
    if (action == "dashboard-drag") {
        m_drag = {};
        m_drag.kind = DragKind::DashboardWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        if (auto* root = Root("dashboard-root")) {
            const std::string draggedDomId = WidgetDomId(m_drag.widget);
            m_drag.element = root->QuerySelector(("[data-widget='" + draggedDomId + "']").c_str());
            if (m_drag.element) m_drag.element->SetClass("dragging", true);
        }
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-toolbox-drag") {
        m_drag = {};
        m_drag.kind = DragKind::OverlayToolboxWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-widget-drag") {
        m_drag = {};
        m_drag.kind = DragKind::OverlayWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.sourceContainerId = Attribute(actionElement, "data-container");
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_systemInterface.LockCursor("move");
    } else if (action == "settings-drag") {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        const float logicalWidth = static_cast<float>(m_width) / std::max(rmlScale, 0.01f);
        const bool compactViewport = logicalWidth <= 850.0f;
        const float settingsWidth = compactViewport ? static_cast<float>(m_width) * 0.96f
                                                    : std::min(900.0f * rmlScale, static_cast<float>(m_width) * 0.94f);
        const float settingsHeight = compactViewport ? static_cast<float>(m_height) * 0.94f
                                                     : std::min(650.0f * rmlScale, static_cast<float>(m_height) * 0.92f);
        if (!m_settingsPositioned) {
            m_settingsX = std::max(0.0f, (static_cast<float>(m_width) - settingsWidth) * 0.5f);
            m_settingsY = std::max(0.0f, (static_cast<float>(m_height) - settingsHeight) * 0.5f);
        }
        m_settingsPositioned = true;
        m_drag = {};
        m_drag.kind = DragKind::SettingsMove;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_drag.startX = m_settingsX;
        m_drag.startY = m_settingsY;
        m_drag.startW = settingsWidth;
        m_drag.startH = settingsHeight;
        if (auto* root = Root("settings-root")) m_drag.element = root->QuerySelector(".settings-window");
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-drag" || action == "overlay-resize") {
        const std::string id = Attribute(actionElement, "data-container");
        auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(), [&](const auto& c) { return c.id == id; });
        if (it == m_config.overlay_layout.containers.end()) return;
        m_drag = {};
        m_drag.kind = action == "overlay-drag" ? DragKind::OverlayMove : DragKind::OverlayResize;
        m_drag.containerId = id;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_drag.startX = it->x;
        m_drag.startY = it->y;
        m_drag.allowDock = m_state && m_state->ui.dashboardLayoutEditMode.load();
        const auto [resolvedWidth, resolvedHeight] = OverlayContainerSize(*it, m_dpiScale, &m_config);
        m_drag.startW = resolvedWidth;
        m_drag.startH = resolvedHeight;
        if (auto* root = Root("overlay-root")) {
            // The overlay DOM is deliberately held stable for the duration of a
            // drag, so these pointers remain valid until mouse-up. Resolve them
            // once instead of running selectors for every WM_MOUSEMOVE.
            m_drag.element = root->QuerySelector(("[data-container='" + id + "']").c_str());
            m_drag.guideX = m_document ? m_document->GetElementById("overlay-snap-x") : nullptr;
            m_drag.guideY = m_document ? m_document->GetElementById("overlay-snap-y") : nullptr;
            if (m_drag.element) {
                // Outside edit mode a container auto-fits its visible widgets, so
                // use its actual rendered bounds instead of the saved/default size.
                const float renderedWidth = m_drag.element->GetOffsetWidth();
                const float renderedHeight = m_drag.element->GetOffsetHeight();
                if (renderedWidth > 1.0f) m_drag.startW = renderedWidth;
                if (renderedHeight > 1.0f) m_drag.startH = renderedHeight;
            }

            m_drag.snapRects.reserve(m_config.overlay_layout.containers.size());
            for (const auto& other : m_config.overlay_layout.containers) {
                if (other.id == id) continue;
                auto [ow, oh] = OverlayContainerSize(other, m_dpiScale, &m_config);
                if (auto* element = root->QuerySelector(("[data-container='" + other.id + "']").c_str())) {
                    if (element->GetOffsetWidth() > 1.0f) ow = element->GetOffsetWidth();
                    if (element->GetOffsetHeight() > 1.0f) oh = element->GetOffsetHeight();
                }
                m_drag.snapRects.push_back({other.x, other.y, ow, oh});
            }
        }
        m_systemInterface.LockCursor(action == "overlay-resize" ? "resize" : "move");
    } else if (action == "floating-card-drag") {
        const std::string card = Attribute(actionElement, "data-card");
        if (card.empty()) return;
        m_drag = {};
        m_drag.kind = DragKind::FloatingCard;
        m_drag.containerId = card;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        // Centered cards have no stored position yet, so seed the drag from where
        // the card is actually drawn.
        m_drag.startX = actionElement->GetAbsoluteLeft();
        m_drag.startY = actionElement->GetAbsoluteTop();
        m_drag.startW = actionElement->GetOffsetWidth();
        m_drag.startH = actionElement->GetOffsetHeight();
        m_drag.element = actionElement;
        m_systemInterface.LockCursor("move");
    } else if (action == "color-field" || action == "color-hue") {
        m_drag = {};
        m_drag.kind = action == "color-field" ? DragKind::ColorField : DragKind::ColorHue;
        m_colorPickDirty = false;
        // Capture the picker rect once so pointer math stays stable while only
        // the existing marker/preview controls are updated during the drag.
        m_drag.startX = actionElement->GetAbsoluteLeft();
        m_drag.startY = actionElement->GetAbsoluteTop();
        m_drag.startW = std::max(actionElement->GetClientWidth(), 1.0f);
        m_drag.startH = std::max(actionElement->GetClientHeight(), 1.0f);
        m_systemInterface.LockCursor("cross");
        ApplyColorPick(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
    }
}

void RmlUiController::HandleMouseMove(Rml::Event& event) {
    m_lastPointerX = static_cast<int>(std::lround(event.GetParameter<float>("mouse_x", 0.0f)));
    m_lastPointerY = static_cast<int>(std::lround(event.GetParameter<float>("mouse_y", 0.0f)));
    m_hasPointerPosition = true;
    if (m_drag.kind == DragKind::SettingsMove) {
        const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
        const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
        const float dx = mouseX - m_drag.startMouseX;
        const float dy = mouseY - m_drag.startMouseY;
        m_settingsX = std::clamp(m_drag.startX + dx, 0.0f, std::max(0.0f, static_cast<float>(m_width) - m_drag.startW));
        m_settingsY = std::clamp(m_drag.startY + dy, 0.0f, std::max(0.0f, static_cast<float>(m_height) - m_drag.startH));

        if (m_drag.element) {
            const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
            m_drag.element->SetProperty("left", std::to_string(m_settingsX / std::max(rmlScale, 0.01f)) + "dp");
            m_drag.element->SetProperty("top", std::to_string(m_settingsY / std::max(rmlScale, 0.01f)) + "dp");
        }
        return;
    }
    if (m_drag.kind == DragKind::ColorField || m_drag.kind == DragKind::ColorHue) {
        ApplyColorPick(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
        return;
    }
    if (m_drag.kind == DragKind::FloatingCard) {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        const float dx = event.GetParameter<float>("mouse_x", 0.0f) - m_drag.startMouseX;
        const float dy = event.GetParameter<float>("mouse_y", 0.0f) - m_drag.startMouseY;
        const float x = std::clamp(m_drag.startX + dx, 0.0f, std::max(0.0f, static_cast<float>(m_width) - m_drag.startW));
        const float y = std::clamp(m_drag.startY + dy, 0.0f, std::max(0.0f, static_cast<float>(m_height) - m_drag.startH));
        if (m_drag.containerId == "session-view") {
            m_config.session_view_x = x;
            m_config.session_view_y = y;
        } else {
            m_config.match_summary_x = x;
            m_config.match_summary_y = y;
        }
        if (m_drag.element) {
            m_drag.element->SetProperty("left", std::to_string(x / std::max(rmlScale, 0.01f)) + "dp");
            m_drag.element->SetProperty("top", std::to_string(y / std::max(rmlScale, 0.01f)) + "dp");
            m_drag.element->SetProperty("margin-left", "0");
        }
        return;
    }
    if (m_drag.kind == DragKind::DashboardWidget) {
        const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
        const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
        if (auto* root = Root("dashboard-root")) {
            Rml::Element* hovered = m_context ? m_context->GetElementAtPoint(Rml::Vector2f(mouseX, mouseY)) : nullptr;
            Rml::Element* targetSlot = nullptr;
            while (hovered && hovered != root) {
                if (hovered->HasAttribute("data-drop-index")) {
                    targetSlot = hovered;
                    break;
                }
                if (hovered->HasAttribute("data-widget") && hovered->HasAttribute("data-order")) {
                    targetSlot = hovered;
                    break;
                }
                hovered = hovered->GetParentNode();
            }
            if (targetSlot != m_drag.dropTarget) {
                if (m_drag.dropTarget) m_drag.dropTarget->SetClass("drag-target", false);
                if (targetSlot) targetSlot->SetClass("drag-target", true);
                m_drag.dropTarget = targetSlot;
            }
        }
        return;
    }
    if (m_drag.kind != DragKind::OverlayMove && m_drag.kind != DragKind::OverlayResize) return;
    const float dpi = SanitizedScale(m_dpiScale);
    const float rmlScale = dpi * SanitizedUiScale(m_config.ui_scale);
    const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
    const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
    const float dx = mouseX - m_drag.startMouseX;
    const float dy = mouseY - m_drag.startMouseY;
    const float screenWidth = static_cast<float>(m_width);
    const float screenHeight = static_cast<float>(m_height);
    const float moveMargin = 0.0f;
    const float resizeMargin = 24.0f * dpi;
    const float snap = 12.0f * dpi;

    auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(), [&](const auto& c) { return c.id == m_drag.containerId; });
    if (it == m_config.overlay_layout.containers.end()) return;

    struct SnapResult {
        float value;
        float guide;
        bool snapped;
    };
    const auto considerSnap = [&](float source, float candidate, float guide, SnapResult& result, float& bestDistance) {
        const float distance = std::abs(source - candidate);
        if (distance < bestDistance) {
            bestDistance = distance;
            result = {candidate, guide, true};
        }
    };

    SnapResult xSnap{0.0f, 0.0f, false};
    SnapResult ySnap{0.0f, 0.0f, false};
    if (m_drag.kind == DragKind::OverlayMove) {
        const float x = std::max(0.0f, m_drag.startX + dx);
        const float y = std::max(0.0f, m_drag.startY + dy);
        xSnap = {x, 0.0f, false};
        ySnap = {y, 0.0f, false};
        float bestX = snap;
        float bestY = snap;
        considerSnap(x, moveMargin, moveMargin, xSnap, bestX);
        considerSnap(x, screenWidth - moveMargin - m_drag.startW, screenWidth - moveMargin, xSnap, bestX);
        considerSnap(y, moveMargin, moveMargin, ySnap, bestY);
        considerSnap(y, screenHeight - moveMargin - m_drag.startH, screenHeight - moveMargin, ySnap, bestY);

        // Neighbour bounds were resolved once on mouse-down. Check candidates
        // directly so mouse motion does not allocate temporary vectors.
        for (const auto& other : m_drag.snapRects) {
            considerSnap(x, other.x, other.x, xSnap, bestX);
            considerSnap(x, other.x + other.w, other.x + other.w, xSnap, bestX);
            considerSnap(x, other.x - m_drag.startW, other.x, xSnap, bestX);
            considerSnap(x, other.x + other.w - m_drag.startW, other.x + other.w, xSnap, bestX);
            considerSnap(y, other.y, other.y, ySnap, bestY);
            considerSnap(y, other.y + other.h, other.y + other.h, ySnap, bestY);
            considerSnap(y, other.y - m_drag.startH, other.y, ySnap, bestY);
            considerSnap(y, other.y + other.h - m_drag.startH, other.y + other.h, ySnap, bestY);
        }
        it->x = std::clamp(xSnap.value, 0.0f, std::max(0.0f, screenWidth - m_drag.startW));
        it->y = std::clamp(ySnap.value, 0.0f, std::max(0.0f, screenHeight - m_drag.startH));
    } else {
        const auto [minWidth, minHeight] = OverlayContainerMinSize(*it, dpi, &m_config);
        float newWidth = std::max(minWidth, m_drag.startW + dx);
        float newHeight = std::max(minHeight, m_drag.startH + dy);
        for (const auto& other : m_drag.snapRects) {
            if (std::abs(newWidth - other.w) < snap) {
                newWidth = other.w;
                xSnap = {it->x + newWidth, it->x + newWidth, true};
            }
            if (std::abs(newHeight - other.h) < snap) {
                newHeight = other.h;
                ySnap = {it->y + newHeight, it->y + newHeight, true};
            }
        }

        const float right = it->x + newWidth;
        const float bottom = it->y + newHeight;
        SnapResult rightSnap{right, 0.0f, false};
        SnapResult bottomSnap{bottom, 0.0f, false};
        float bestRight = snap;
        float bestBottom = snap;
        considerSnap(right, screenWidth - resizeMargin, screenWidth - resizeMargin, rightSnap, bestRight);
        considerSnap(bottom, screenHeight - resizeMargin, screenHeight - resizeMargin, bottomSnap, bestBottom);
        for (const auto& other : m_drag.snapRects) {
            considerSnap(right, other.x, other.x, rightSnap, bestRight);
            considerSnap(right, other.x + other.w, other.x + other.w, rightSnap, bestRight);
            considerSnap(bottom, other.y, other.y, bottomSnap, bestBottom);
            considerSnap(bottom, other.y + other.h, other.y + other.h, bottomSnap, bestBottom);
        }
        if (rightSnap.snapped) xSnap = rightSnap;
        if (bottomSnap.snapped) ySnap = bottomSnap;
        const float snappedRight = rightSnap.snapped ? rightSnap.value : right;
        const float snappedBottom = bottomSnap.snapped ? bottomSnap.value : bottom;
        // Every value above is in rendered screen pixels; stored geometry is
        // design pixels at 100% text size, so divide the scale back out.
        const float uiScale = SanitizedUiScale(m_config.ui_scale);
        it->w = std::clamp(snappedRight - it->x, minWidth, std::max(minWidth, screenWidth - resizeMargin - it->x)) / uiScale;
        it->h = std::clamp(snappedBottom - it->y, minHeight, std::max(minHeight, screenHeight - resizeMargin - it->y)) / uiScale;
    }

    const auto toDp = [rmlScale](float pixels) { return pixels / std::max(rmlScale, 0.5f); };
    if (m_drag.element) {
        m_drag.element->SetProperty("left", std::to_string(toDp(it->x)) + "dp");
        m_drag.element->SetProperty("top", std::to_string(toDp(it->y)) + "dp");
        if (m_drag.kind == DragKind::OverlayResize) {
            const auto [resolvedWidth, resolvedHeight] = OverlayContainerSize(*it, m_dpiScale, &m_config);
            m_drag.element->SetProperty("width", std::to_string(toDp(resolvedWidth)) + "dp");
            m_drag.element->SetProperty("min-height", std::to_string(toDp(resolvedHeight)) + "dp");
        }
    }
    if (m_drag.guideX) {
        m_drag.guideX->SetProperty("display", xSnap.snapped ? "block" : "none");
        if (xSnap.snapped) m_drag.guideX->SetProperty("left", std::to_string(toDp(xSnap.guide)) + "dp");
    }
    if (m_drag.guideY) {
        m_drag.guideY->SetProperty("display", ySnap.snapped ? "block" : "none");
        if (ySnap.snapped) m_drag.guideY->SetProperty("top", std::to_string(toDp(ySnap.guide)) + "dp");
    }
}

void RmlUiController::HandleMouseUp(Rml::Event& event) {
    m_systemInterface.UnlockCursor();
    const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
    const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
    if (m_drag.guideX) m_drag.guideX->SetProperty("display", "none");
    if (m_drag.guideY) m_drag.guideY->SetProperty("display", "none");

    // Component sliders do not use our DragState, but they share the same
    // staged color model. Commit them once when the pointer is released.
    if (m_colorPickDirty && m_drag.kind != DragKind::ColorField && m_drag.kind != DragKind::ColorHue)
        CommitColorPick();

    if (m_drag.kind == DragKind::SettingsMove) {
        // Position is intentionally session-local; reopening Settings keeps the
        // user's last placement without changing the existing config format.
    } else if (m_drag.kind == DragKind::ColorField || m_drag.kind == DragKind::ColorHue) {
        CommitColorPick();
    } else if (m_drag.kind == DragKind::FloatingCard) {
        const float sessionX = m_config.session_view_x;
        const float sessionY = m_config.session_view_y;
        const float summaryX = m_config.match_summary_x;
        const float summaryY = m_config.match_summary_y;
        Config::Update([=](ConfigData& c) {
            c.session_view_x = sessionX;
            c.session_view_y = sessionY;
            c.match_summary_x = summaryX;
            c.match_summary_y = summaryY;
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::DashboardWidget) {
        Rml::Element* element = m_context ? m_context->GetElementAtPoint(Rml::Vector2f(mouseX, mouseY)) : nullptr;
        if (!element) element = event.GetTargetElement();
        DashboardLayout::Zone targetZone = DashboardLayout::Zone::Left;
        int targetIndex = -1;
        bool found = false;

        Rml::Element* cur = element;
        while (cur) {
            const std::string dropIndex = Attribute(cur, "data-drop-index");
            const std::string zone = Attribute(cur, "data-zone");
            if (!zone.empty() && !dropIndex.empty()) {
                targetZone = ZoneFromDom(zone);
                targetIndex = std::max(0, std::atoi(dropIndex.c_str()));
                found = true;
                break;
            }
            const std::string widgetAttr = Attribute(cur, "data-widget");
            const std::string orderAttr = Attribute(cur, "data-order");
            if (!zone.empty() && !widgetAttr.empty() && !orderAttr.empty()) {
                targetZone = ZoneFromDom(zone);
                const int order = std::atoi(orderAttr.c_str());
                const float top = cur->GetAbsoluteTop();
                const float height = cur->GetOffsetHeight();
                targetIndex = (mouseY > top + height * 0.5f) ? (order + 1) : order;
                found = true;
                break;
            }
            if (!zone.empty() && !found) {
                targetZone = ZoneFromDom(zone);
                found = true;
            }
            cur = cur->GetParentNode();
        }

        if (found) {
            MoveDashboardWidget(m_drag.widget, targetZone, targetIndex);
            Config::RequestSave();
        }
        RebuildDashboard();
    } else if (m_drag.kind == DragKind::OverlayToolboxWidget) {
        std::string targetId;
        bool insertAtTop = false;
        for (const auto& container : m_config.overlay_layout.containers) {
            const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
            if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                targetId = container.id;
                insertAtTop = mouseY < container.y + height * 0.5f;
                break;
            }
        }
        const auto widget = m_drag.widget;
        Config::Update([=](ConfigData& c) {
            for (const auto& container : c.overlay_layout.containers) {
                if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) return;
            }
            if (!targetId.empty()) {
                for (auto& target : c.overlay_layout.containers) {
                    if (target.id != targetId) continue;
                    if (insertAtTop)
                        target.widgets.insert(target.widgets.begin(), widget);
                    else
                        target.widgets.push_back(widget);
                    target.w = 0.0f;
                    target.h = 0.0f;
                    OverlayLayout::Sanitize(c.overlay_layout);
                    return;
                }
            }
            OverlayLayout::ContainerConfig detached;
            detached.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
            detached.x = std::max(0.0f, mouseX - 24.0f * SanitizedScale(m_dpiScale));
            detached.y = std::max(0.0f, mouseY - 24.0f * SanitizedScale(m_dpiScale));
            const auto [width, height] = OverlayWidgetDefaultSize(widget, m_dpiScale);
            detached.w = width;
            detached.h = height;
            detached.widgets = {widget};
            c.overlay_layout.containers.push_back(std::move(detached));
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::OverlayWidget) {
        std::string targetId;
        bool insertAtTop = false;
        for (const auto& container : m_config.overlay_layout.containers) {
            if (container.id == m_drag.sourceContainerId) continue;
            const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
            if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                targetId = container.id;
                insertAtTop = mouseY < container.y + height * 0.5f;
                break;
            }
        }

        const auto widget = m_drag.widget;
        const std::string sourceId = m_drag.sourceContainerId;
        Config::Update([=](ConfigData& c) {
            bool removed = false;
            for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                if (it->id != sourceId) continue;
                const auto widgetIt = std::find(it->widgets.begin(), it->widgets.end(), widget);
                if (widgetIt != it->widgets.end()) {
                    it->widgets.erase(widgetIt);
                    removed = true;
                }
                if (it->widgets.empty()) c.overlay_layout.containers.erase(it);
                break;
            }
            if (!removed) return;

            bool inserted = false;
            if (!targetId.empty()) {
                for (auto& target : c.overlay_layout.containers) {
                    if (target.id != targetId) continue;
                    if (insertAtTop)
                        target.widgets.insert(target.widgets.begin(), widget);
                    else
                        target.widgets.push_back(widget);
                    target.w = 0.0f;
                    target.h = 0.0f;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                OverlayLayout::ContainerConfig detached;
                detached.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
                detached.x = std::max(0.0f, mouseX - 24.0f * SanitizedScale(m_dpiScale));
                detached.y = std::max(0.0f, mouseY - 24.0f * SanitizedScale(m_dpiScale));
                const auto [width, height] = OverlayWidgetDefaultSize(widget, m_dpiScale);
                detached.w = width;
                detached.h = height;
                detached.widgets = {widget};
                c.overlay_layout.containers.push_back(std::move(detached));
            }
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::OverlayMove || m_drag.kind == DragKind::OverlayResize) {
        if (m_drag.kind == DragKind::OverlayMove && m_drag.allowDock) {
            std::string targetId;
            bool dockAtTop = false;
            for (const auto& container : m_config.overlay_layout.containers) {
                if (container.id == m_drag.containerId) continue;
                const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
                if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                    targetId = container.id;
                    // Match the legacy container docking preview: only the top
                    // quarter inserts above; the center and lower region stack
                    // the dragged container below the target.
                    dockAtTop = mouseY < container.y + height * 0.25f;
                    break;
                }
            }
            if (!targetId.empty()) {
                const std::string draggedId = m_drag.containerId;
                auto layout = m_config.overlay_layout;
                OverlayLayout::ContainerConfig dragged;
                bool found = false;
                for (auto it = layout.containers.begin(); it != layout.containers.end(); ++it) {
                    if (it->id == draggedId) {
                        dragged = *it;
                        layout.containers.erase(it);
                        found = true;
                        break;
                    }
                }
                if (found) {
                    for (auto& target : layout.containers) {
                        if (target.id != targetId) continue;
                        if (dockAtTop)
                            target.widgets.insert(target.widgets.begin(), dragged.widgets.begin(), dragged.widgets.end());
                        else
                            target.widgets.insert(target.widgets.end(), dragged.widgets.begin(), dragged.widgets.end());
                        target.w = 0.0f;
                        target.h = 0.0f;
                        break;
                    }
                    OverlayLayout::Sanitize(layout);
                    m_config.overlay_layout = std::move(layout);
                }
            }
        }
        const auto layout = m_config.overlay_layout;
        Config::Update([&](ConfigData& c) { c.overlay_layout = layout; });
        m_config = Config::Read();
        RebuildOverlay();
    }
    m_drag = {};
    m_pointerPressed = false;
}
