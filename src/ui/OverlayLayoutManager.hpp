#pragma once
#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <functional>
#include <unordered_map>
#include "imgui.h"
#include "core/OverlayLayoutConfig.hpp"
#include "core/DashboardLayoutConfig.hpp"

struct ConfigData;

namespace OverlayLayout {

class LayoutManager {
public:
    using WidgetRenderFunc = std::function<void(DashboardLayout::WidgetId, const char* suffix)>;

    LayoutManager(WidgetRenderFunc renderFunc);
    ~LayoutManager();
    
    // Renders the entire modular overlay system (Toolbox sidebar, active windows, snapping lines, etc.)
    void Render(bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold, ImFont* fontMono, ID3D11ShaderResourceView* logoTexture);
    
private:
    void RenderToolboxToggle(float dpiScale, ImFont* fontBold, ID3D11ShaderResourceView* logoTexture);
    void RenderToolbox(float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold);
    void RenderContainers(bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold);
    bool IsWidgetVisible(const ContainerConfig& container, DashboardLayout::WidgetId widget, bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, const ConfigData& config) const;
    ImVec2 DefaultWidgetSize(DashboardLayout::WidgetId widget, float dpiScale, const ConfigData& config) const;
    ImVec2 DefaultContainerSize(const ContainerConfig& container, float dpiScale, const ConfigData& config) const;
    ImVec2 MinWidgetSize(DashboardLayout::WidgetId widget, float dpiScale, const ConfigData& config) const;
    ImVec2 MinContainerSize(const ContainerConfig& container, const std::vector<DashboardLayout::WidgetId>& visibleWidgets, float dpiScale, const ConfigData& config) const;
    void ApplyDefaultSize(ContainerConfig& container, float dpiScale) const;
    void FitContainerToWidgets(ContainerConfig& container, float dpiScale) const;
    void DetachDraggedWidget(const ImVec2& dropPos, float dpiScale);
    bool RenderCloseButton(const char* id, float dpiScale);
    
    // Snapping logic
    void CheckSnapping(ContainerConfig& c, const std::vector<ContainerConfig>& others, float dpiScale);
    void CheckResizeSnapping(ContainerConfig& c, const std::vector<ContainerConfig>& others, float dpiScale, float minW, float minH);
    
    // Docking logic
    void HandleDockingPreview(const ContainerConfig& dragged, const ImVec2& mousePos, float dpiScale);
    
    WidgetRenderFunc m_renderWidget;
    LayoutConfig m_layout;
    std::unordered_map<std::string, ImVec2> m_lastContainerSizes;
    
    // Snapping / Docking interaction state
    std::string m_dockTargetId;
    std::string m_draggedContainerId;
    ImVec2 m_containerDragOffset = ImVec2(0, 0);
    bool m_isMovingContainer = false;
    bool m_dockAtTop = false;
    bool m_isDockPreviewActive = false;
    bool m_lastMouseDown = false;
    bool m_activeContainerCanDock = false;
    ImVec2 m_snapGuideStart = ImVec2(0, 0);
    ImVec2 m_snapGuideEnd = ImVec2(0, 0);
    bool m_showSnapGuide = false;

    // Detach / Drag widget state
    DashboardLayout::WidgetId m_draggedWidgetId = DashboardLayout::WidgetId::LiveRoster;
    std::string m_draggedWidgetSourceContainerId = "";
    bool m_isDraggingWidgetOut = false;
    DashboardLayout::WidgetId m_draggedNewWidgetId = DashboardLayout::WidgetId::LiveRoster;
    bool m_isDraggingNewWidget = false;
};

} // namespace OverlayLayout
