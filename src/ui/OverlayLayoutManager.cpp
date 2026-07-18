#include "OverlayLayoutManager.hpp"
#include "core/Config.hpp"
#include "ui/Formatting.hpp"
#include <algorithm>
#include <iostream>
#include <set>

namespace OverlayLayout {

    LayoutManager::LayoutManager(WidgetRenderFunc renderFunc)
        : m_renderWidget(renderFunc) {}

    LayoutManager::~LayoutManager() = default;

    void LayoutManager::Render(bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold, ImFont* fontMono, ID3D11ShaderResourceView* logoTexture) {
        // Read the current configuration layout
        ConfigData config = Config::Read();
        m_layout = config.overlay_layout;
        Sanitize(m_layout);

        if (!settingsOpen && !editMode) {
            m_draggedContainerId.clear();
            m_dockTargetId.clear();
            m_isDraggingWidgetOut = false;
            m_isDraggingNewWidget = false;
            m_isMovingContainer = false;
            m_lastMouseDown = false;
            m_activeContainerCanDock = false;
        }

        m_showSnapGuide = false;

        // 1. Render transparent background to capture drop events in Edit Mode
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags bgFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings;
        if (!editMode) {
            bgFlags |= ImGuiWindowFlags_NoInputs;
        }

        ImGui::Begin("OverlayBackground", nullptr, bgFlags);

        if (editMode && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("NEW_WIDGET")) {
                DashboardLayout::WidgetId wId = *(const DashboardLayout::WidgetId*)payload->Data;
                ImVec2 dropPos = ImGui::GetMousePos();
                m_isDraggingNewWidget = false;

                // Add new container at mouse position
                Config::Update([&](ConfigData& c) {
                    ContainerConfig newC;
                    newC.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(rand() % 1000);
                    newC.x = dropPos.x;
                    newC.y = dropPos.y;
                    newC.widgets = {wId};
                    ApplyDefaultSize(newC, dpiScale);
                    c.overlay_layout.containers.push_back(newC);
                    Sanitize(c.overlay_layout);
                },
                               true);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::End();

        if (editMode) {
            RenderToolboxToggle(dpiScale, fontBold, logoTexture);
            if (m_layout.toolboxOpen) {
                RenderToolbox(dpiScale, fontBold, fontSmall, fontSmallBold);
            }
        }

        // 3. Render all active layout window containers
        RenderContainers(settingsOpen, editMode, showOverlay, inMatch, expanded, dpiScale, fontBold, fontSmall, fontSmallBold);

        // 4. Render snap guide lines if active
        if ((settingsOpen || editMode) && m_showSnapGuide) {
            ImDrawList* foreground = ImGui::GetForegroundDrawList();
            foreground->AddLine(m_snapGuideStart, m_snapGuideEnd, IM_COL32(235, 140, 36, 200), 2.0f * dpiScale);
        }
    }

    void LayoutManager::RenderToolboxToggle(float dpiScale, ImFont* fontBold, ID3D11ShaderResourceView* logoTexture) {
        ConfigData config = Config::Read();
        ImGui::SetNextWindowPos(ImVec2(12.0f * dpiScale, 12.0f * dpiScale), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(config.themeBg.a);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * dpiScale, 6.0f * dpiScale));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Format::C(config.themeBg));
        if (ImGui::Begin("Overlay Toolbox Toggle", nullptr, flags)) {
            ImVec2 buttonSize(36.0f * dpiScale, 36.0f * dpiScale);
            bool clicked = ImGui::InvisibleButton("##OpenToolbox", buttonSize);
            ImVec2 min = ImGui::GetItemRectMin();
            ImVec2 max = ImGui::GetItemRectMax();
            bool hovered = ImGui::IsItemHovered();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(min, max, hovered ? IM_COL32(52, 74, 92, 230) : IM_COL32(38, 54, 68, 210), 6.0f * dpiScale);
            ImU32 layerColor = ImGui::GetColorU32(Format::C(config.themeAccent));
            float layerW = 18.0f * dpiScale;
            float layerH = 8.0f * dpiScale;
            ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
            for (int i = 0; i < 3; ++i) {
                float y = center.y - 9.0f * dpiScale + static_cast<float>(i) * 6.0f * dpiScale;
                ImVec2 a(center.x - layerW * 0.5f + static_cast<float>(i) * 2.0f * dpiScale, y);
                ImVec2 b(a.x + layerW, y + layerH);
                drawList->AddRect(a, b, layerColor, 2.0f * dpiScale, 0, 1.5f * dpiScale);
            }
            if (clicked) {
                bool toolboxOpen = !m_layout.toolboxOpen;
                Config::Update([toolboxOpen](ConfigData& c) { c.overlay_layout.toolboxOpen = toolboxOpen; }, true);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    void LayoutManager::RenderToolbox(float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold) {
        ImGui::SetNextWindowPos(ImVec2(12.0f * dpiScale, 48.0f * dpiScale), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(240.0f * dpiScale, ImGui::GetIO().DisplaySize.y - 96.0f * dpiScale), ImGuiCond_Always);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
        ConfigData config = Config::Read();

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * dpiScale, 12.0f * dpiScale));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Format::C(config.themeBg));
        if (ImGui::Begin("Overlay Toolbox", nullptr, flags)) {
            ImGui::PushFont(fontBold);
            ImGui::TextColored(Format::C(config.themeAccent), "TOOLBOX");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 24.0f * dpiScale);
            if (RenderCloseButton("##CloseToolbox", dpiScale)) {
                Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
            }
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::PushFont(fontSmall);
            ImGui::TextWrapped("Click to add to the center, or drag onto the overlay to place it.");
            ImGui::PopFont();
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            std::set<DashboardLayout::WidgetId> activeWidgets;
            for (const auto& c : m_layout.containers) {
                for (auto w : c.widgets) {
                    activeWidgets.insert(w);
                }
            }

            std::vector<DashboardLayout::WidgetId> allWidgets = {
                DashboardLayout::WidgetId::LiveRoster,
                DashboardLayout::WidgetId::LiveMatchStats,
                DashboardLayout::WidgetId::SessionStats,
                DashboardLayout::WidgetId::MmrGraph,
                DashboardLayout::WidgetId::StreaksStats,
                DashboardLayout::WidgetId::GamemodeBreakdown,
                DashboardLayout::WidgetId::LobbyRanks,
                DashboardLayout::WidgetId::DemoTracker,
                DashboardLayout::WidgetId::PreviousGames};

            for (auto wId : allWidgets) {
                bool isActive = activeWidgets.find(wId) != activeWidgets.end();
                std::string label = DashboardLayout::GetWidgetDisplayName(wId);

                if (isActive) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Format::C(config.themeDim));
                    ImGui::Button((label + " [Active]").c_str(), ImVec2(-1, 32.0f * dpiScale));
                    ImGui::PopStyleColor();
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.35f, 0.2f, 0.6f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.45f, 0.25f, 0.8f));
                    bool clicked = ImGui::Button(label.c_str(), ImVec2(-1, 32.0f * dpiScale));

                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                        m_isDraggingNewWidget = true;
                        m_draggedNewWidgetId = wId;
                    }
                    ImGui::PopStyleColor(2);

                    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                        m_isDraggingNewWidget = true;
                        m_draggedNewWidgetId = wId;
                        ImGui::SetDragDropPayload("NEW_WIDGET", &wId, sizeof(wId));
                        ImGui::Text("Spawning: %s", label.c_str());
                        ImGui::EndDragDropSource();
                    }

                    if (m_isDraggingNewWidget && m_draggedNewWidgetId == wId) {
                        ImGui::SetTooltip("Spawning: %s", label.c_str());
                    }

                    if (clicked) {
                        ImVec2 center = ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
                        Config::Update([&](ConfigData& c) {
                            ContainerConfig newC;
                            newC.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(rand() % 1000);
                            newC.x = center.x - 150.0f * dpiScale;
                            newC.y = center.y - 100.0f * dpiScale;
                            newC.widgets = {wId};
                            ApplyDefaultSize(newC, dpiScale);
                            c.overlay_layout.containers.push_back(newC);
                            Sanitize(c.overlay_layout);
                        },
                                       true);
                    }
                }
                ImGui::Spacing();
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    bool LayoutManager::IsWidgetVisible(const ContainerConfig& container, DashboardLayout::WidgetId widget, bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, const ConfigData& config) const {
        if (editMode) {
            return true;
        }

        if (settingsOpen) {
            switch (widget) {
            case DashboardLayout::WidgetId::LiveRoster:
            case DashboardLayout::WidgetId::LiveMatchStats:
            case DashboardLayout::WidgetId::SessionStats:
            case DashboardLayout::WidgetId::MmrGraph:
                return true;
            case DashboardLayout::WidgetId::StreaksStats:
                return config.show_streaks_stats;
            case DashboardLayout::WidgetId::GamemodeBreakdown:
                return config.show_gamemode_breakdown;
            case DashboardLayout::WidgetId::LobbyRanks:
                return config.show_lobby_ranks_overlay;
            case DashboardLayout::WidgetId::DemoTracker:
                return config.show_demo_tracker_overlay;
            case DashboardLayout::WidgetId::PreviousGames:
                return config.show_previous_games_summary;
            }
            return false;
        }

        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
        case DashboardLayout::WidgetId::MmrGraph:
            return showOverlay;
        case DashboardLayout::WidgetId::LiveMatchStats:
            return showOverlay && expanded;
        case DashboardLayout::WidgetId::SessionStats:
            return showOverlay || (config.show_session_card_in_game && inMatch);
        case DashboardLayout::WidgetId::StreaksStats:
            return showOverlay && config.show_streaks_stats;
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            return showOverlay && config.show_gamemode_breakdown;
        case DashboardLayout::WidgetId::LobbyRanks:
            return showOverlay && config.show_lobby_ranks_overlay;
        case DashboardLayout::WidgetId::DemoTracker:
            return config.show_demo_tracker_overlay && inMatch;
        case DashboardLayout::WidgetId::PreviousGames:
            return showOverlay && config.show_previous_games_summary;
        }
        return false;
    }

    ImVec2 LayoutManager::DefaultWidgetSize(DashboardLayout::WidgetId widget, float dpiScale, const ConfigData& config) const {
        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
            return ImVec2(410.0f * dpiScale, 260.0f * dpiScale);
        case DashboardLayout::WidgetId::LiveMatchStats:
            return ImVec2(340.0f * dpiScale, 210.0f * dpiScale);
        case DashboardLayout::WidgetId::SessionStats:
            return ImVec2(280.0f * dpiScale, 220.0f * dpiScale);
        case DashboardLayout::WidgetId::MmrGraph:
            return ImVec2(430.0f * dpiScale, 260.0f * dpiScale);
        case DashboardLayout::WidgetId::StreaksStats:
            return ImVec2(300.0f * dpiScale, 115.0f * dpiScale);
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            return ImVec2(340.0f * dpiScale, 210.0f * dpiScale);
        case DashboardLayout::WidgetId::LobbyRanks:
            return ImVec2(720.0f * dpiScale, 360.0f * dpiScale);
        case DashboardLayout::WidgetId::DemoTracker:
            return ImVec2(280.0f * dpiScale, 84.0f * dpiScale);
        case DashboardLayout::WidgetId::PreviousGames:
            return ImVec2(560.0f * dpiScale, 260.0f * dpiScale);
        }
        return ImVec2(320.0f * dpiScale, 200.0f * dpiScale);
    }

    ImVec2 LayoutManager::DefaultContainerSize(const ContainerConfig& container, float dpiScale, const ConfigData& config) const {
        ImVec2 result(320.0f * dpiScale, 200.0f * dpiScale);
        if (container.widgets.empty()) {
            return result;
        }

        result = ImVec2(0.0f, 0.0f);
        for (auto widget : container.widgets) {
            ImVec2 widgetSize = DefaultWidgetSize(widget, dpiScale, config);
            result.x = (std::max)(result.x, widgetSize.x);
            result.y += widgetSize.y;
        }
        result.y += static_cast<float>(container.widgets.size() - 1) * 42.0f * dpiScale;

        ImVec2 display = ImGui::GetIO().DisplaySize;
        const float margin = 24.0f * dpiScale;

        std::vector<DashboardLayout::WidgetId> visibleWidgets = container.widgets;
        ImVec2 minSize = MinContainerSize(container, visibleWidgets, dpiScale, config);

        result.x = std::clamp(result.x, minSize.x, (std::max)(minSize.x, display.x - margin * 2.0f));
        result.y = std::clamp(result.y, minSize.y, (std::max)(minSize.y, display.y - margin * 2.0f));
        return result;
    }

    ImVec2 LayoutManager::MinWidgetSize(DashboardLayout::WidgetId widget, float dpiScale, const ConfigData& config) const {
        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
            return ImVec2(410.0f * dpiScale, 120.0f * dpiScale);
        default:
            return ImVec2(220.0f * dpiScale, 120.0f * dpiScale);
        }
    }

    ImVec2 LayoutManager::MinContainerSize(const ContainerConfig& container, const std::vector<DashboardLayout::WidgetId>& visibleWidgets, float dpiScale, const ConfigData& config) const {
        if (visibleWidgets.empty()) {
            return ImVec2(220.0f * dpiScale, 120.0f * dpiScale);
        }

        ImVec2 result(0.0f, 0.0f);
        for (auto widget : visibleWidgets) {
            ImVec2 minSize = MinWidgetSize(widget, dpiScale, config);
            result.x = (std::max)(result.x, minSize.x);
            result.y += minSize.y;
        }
        result.y += static_cast<float>(visibleWidgets.size() - 1) * 42.0f * dpiScale;

        result.x = (std::max)(result.x, 220.0f * dpiScale);
        result.y = (std::max)(result.y, 120.0f * dpiScale);
        return result;
    }

    void LayoutManager::ApplyDefaultSize(ContainerConfig& container, float dpiScale) const {
        if (container.w > 1.0f && container.h > 1.0f) {
            return;
        }

        ConfigData config = Config::Read();
        ImVec2 defaultSize = DefaultContainerSize(container, dpiScale, config);
        if (container.w <= 1.0f) {
            container.w = defaultSize.x;
        }
        if (container.h <= 1.0f) {
            container.h = defaultSize.y;
        }
    }

    void LayoutManager::FitContainerToWidgets(ContainerConfig& container, float dpiScale) const {
        ConfigData config = Config::Read();
        ImVec2 size = DefaultContainerSize(container, dpiScale, config);
        container.w = size.x;
        container.h = size.y;
    }

    void LayoutManager::DetachDraggedWidget(const ImVec2& dropPos, float dpiScale) {
        DashboardLayout::WidgetId widgetId = m_draggedWidgetId;
        std::string sourceId = m_draggedWidgetSourceContainerId;
        std::string targetId;
        bool dockAtTop = false;

        for (const auto& other : m_layout.containers) {
            if (other.id == sourceId) continue;

            ImVec2 boundsMin(other.x, other.y);
            ImVec2 boundsMax(other.x + other.w, other.y + other.h);
            if (dropPos.x >= boundsMin.x && dropPos.x <= boundsMax.x && dropPos.y >= boundsMin.y && dropPos.y <= boundsMax.y) {
                targetId = other.id;
                dockAtTop = dropPos.y < other.y + other.h * 0.5f;
                break;
            }
        }

        Config::Update([&](ConfigData& c) {
            bool removed = false;
            bool removedSourceContainer = false;

            for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                bool sourceMatch = !sourceId.empty() && it->id == sourceId;
                auto widgetIt = std::find(it->widgets.begin(), it->widgets.end(), widgetId);
                if (sourceMatch || widgetIt != it->widgets.end()) {
                    if (widgetIt != it->widgets.end()) {
                        it->widgets.erase(widgetIt);
                        removed = true;
                    }
                    if (it->widgets.empty()) {
                        removedSourceContainer = true;
                        c.overlay_layout.containers.erase(it);
                    }
                    break;
                }
            }

            if (!removed) {
                return;
            }

            bool inserted = false;
            if (!targetId.empty()) {
                for (auto& target : c.overlay_layout.containers) {
                    if (target.id == targetId) {
                        if (dockAtTop) {
                            target.widgets.insert(target.widgets.begin(), widgetId);
                        } else {
                            target.widgets.push_back(widgetId);
                        }
                        FitContainerToWidgets(target, dpiScale);
                        inserted = true;
                        break;
                    }
                }
            }

            if (!inserted) {
                ContainerConfig newC;
                newC.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(rand() % 1000);
                newC.x = dropPos.x - 24.0f * dpiScale;
                newC.y = dropPos.y - 24.0f * dpiScale;
                newC.widgets = {widgetId};
                ApplyDefaultSize(newC, dpiScale);
                c.overlay_layout.containers.push_back(newC);
            }

            if (removedSourceContainer) {
                m_lastContainerSizes.erase(sourceId);
            }
            Sanitize(c.overlay_layout);
        },
                       true);
    }

    bool LayoutManager::RenderCloseButton(const char* id, float dpiScale) {
        ImVec2 size(20.0f * dpiScale, 22.0f * dpiScale);
        bool clicked = ImGui::InvisibleButton(id, size);
        ImVec2 min = ImGui::GetItemRectMin();
        ImVec2 max = ImGui::GetItemRectMax();
        bool hovered = ImGui::IsItemHovered();
        ImU32 bg = hovered ? IM_COL32(153, 51, 51, 204) : IM_COL32(102, 38, 38, 153);
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, bg, 4.0f * dpiScale);

        ImU32 xColor = IM_COL32(255, 230, 230, 255);
        float padX = 6.0f * dpiScale;
        float padY = 7.0f * dpiScale;
        drawList->AddLine(ImVec2(min.x + padX, min.y + padY), ImVec2(max.x - padX, max.y - padY), xColor, 1.8f * dpiScale);
        drawList->AddLine(ImVec2(max.x - padX, min.y + padY), ImVec2(min.x + padX, max.y - padY), xColor, 1.8f * dpiScale);
        return clicked;
    }

    void LayoutManager::RenderContainers(bool settingsOpen, bool editMode, bool showOverlay, bool inMatch, bool expanded, float dpiScale, ImFont* fontBold, ImFont* fontSmall, ImFont* fontSmallBold) {
        ConfigData config = Config::Read();

        // Store original containers vector to check drag neighbor alignments
        std::vector<ContainerConfig> origContainers = m_layout.containers;
        for (auto& orig : origContainers) {
            auto sizeIt = m_lastContainerSizes.find(orig.id);
            if (sizeIt != m_lastContainerSizes.end()) {
                orig.w = sizeIt->second.x;
                orig.h = sizeIt->second.y;
            }
        }
        std::vector<ContainerConfig> snapContainers;
        for (const auto& orig : origContainers) {
            bool allHidden = true;
            for (auto w : orig.widgets) {
                if (IsWidgetVisible(orig, w, settingsOpen, editMode, showOverlay, inMatch, expanded, config)) {
                    allHidden = false;
                    break;
                }
            }
            if (!allHidden) {
                snapContainers.push_back(orig);
            }
        }

        for (size_t cIdx = 0; cIdx < m_layout.containers.size(); ++cIdx) {
            ContainerConfig& c = m_layout.containers[cIdx];

            ApplyDefaultSize(c, dpiScale);

            std::vector<DashboardLayout::WidgetId> visibleWidgets;
            for (auto w : c.widgets) {
                if (IsWidgetVisible(c, w, settingsOpen, editMode, showOverlay, inMatch, expanded, config)) {
                    visibleWidgets.push_back(w);
                }
            }
            if (!editMode && visibleWidgets.empty()) {
                continue;
            }

            // Clamp sizes in memory and save to config once if adjusted
            ImVec2 minSize = MinContainerSize(c, visibleWidgets, dpiScale, config);
            bool sizeAdjusted = false;
            if (c.w < minSize.x) {
                c.w = minSize.x;
                sizeAdjusted = true;
            }
            if (c.h < minSize.y) {
                c.h = minSize.y;
                sizeAdjusted = true;
            }
            if (sizeAdjusted) {
                Config::Update([&](ConfigData& conf) {
                    for (auto& cont : conf.overlay_layout.containers) {
                        if (cont.id == c.id) {
                            cont.w = c.w;
                            cont.h = c.h;
                            break;
                        }
                    }
                },
                               true);
            }

            bool showWidgetHeaders = editMode && c.widgets.size() > 1;
            std::string title = (editMode && c.widgets.size() == 1) ? DashboardLayout::GetWidgetDisplayName(c.widgets.front()) : "OmniStats Window";
            std::string windowName = title + "###" + c.id;

            bool isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
            bool moveMode = settingsOpen || editMode;
            ImGuiCond posCond = ImGuiCond_Always;
            ImGuiCond sizeCond = (moveMode && isMouseDown) ? ImGuiCond_Appearing : ImGuiCond_Always;
            if (moveMode && m_isMovingContainer && m_draggedContainerId == c.id && isMouseDown) {
                ImVec2 mousePos = ImGui::GetMousePos();
                c.x = mousePos.x - m_containerDragOffset.x;
                c.y = mousePos.y - m_containerDragOffset.y;
                m_activeContainerCanDock = editMode;
                CheckSnapping(c, snapContainers, dpiScale);
            }
            ImVec2 windowPos(c.x, c.y);
            ImVec2 windowPivot(0.0f, 0.0f);
            if (!editMode && c.x + c.w > ImGui::GetIO().DisplaySize.x * 0.5f) {
                windowPos.x = c.x + c.w;
                windowPivot.x = 1.0f;
            }
            ImGui::SetNextWindowPos(windowPos, posCond, windowPivot);
            if (editMode) {
                ImGui::SetNextWindowSize(ImVec2(c.w, c.h), sizeCond);
                ImGui::SetNextWindowSizeConstraints(minSize, ImVec2(ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y));
            } else {
                ImGui::SetNextWindowSizeConstraints(ImVec2(c.w, -1.0f), ImVec2(c.w, ImGui::GetIO().DisplaySize.y));
            }
            ImGui::SetNextWindowBgAlpha(config.themeBg.a);

            ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
            if (!moveMode || settingsOpen) {
                flags |= ImGuiWindowFlags_NoMove;
            }
            if (editMode) {
                flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
            }

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * dpiScale, 16.0f * dpiScale));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Format::C(config.themeBg));

            bool isOpen = true;
            bool visible = ImGui::Begin(windowName.c_str(), editMode ? &isOpen : nullptr, flags);

            if (!isOpen) {
                // Closed container -> delete it
                Config::Update([&](ConfigData& conf) {
                    conf.overlay_layout.containers.erase(conf.overlay_layout.containers.begin() + cIdx);
                    Sanitize(conf.overlay_layout);
                },
                               true);
                ImGui::End();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                --cIdx;
                continue;
            }

            if (visible) {
                // Track dynamic drag changes
                ImVec2 currentPos = ImGui::GetWindowPos();
                ImVec2 currentSize = ImGui::GetWindowSize();

                if (editMode) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    float titleBarHeight = ImGui::GetFrameHeight();
                    bool overTitleBar = mousePos.x >= currentPos.x && mousePos.x <= currentPos.x + currentSize.x - 28.0f * dpiScale &&
                                        mousePos.y >= currentPos.y && mousePos.y <= currentPos.y + titleBarHeight;
                    if (!m_isMovingContainer && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overTitleBar) {
                        m_isMovingContainer = true;
                        m_draggedContainerId = c.id;
                        m_containerDragOffset = ImVec2(mousePos.x - currentPos.x, mousePos.y - currentPos.y);
                        m_activeContainerCanDock = true;
                    }

                    bool positionChanged = std::abs(currentPos.x - c.x) > 0.1f || std::abs(currentPos.y - c.y) > 0.1f;
                    bool sizeChanged = std::abs(currentSize.x - c.w) > 0.1f || std::abs(currentSize.y - c.h) > 0.1f;
                    bool resizeChanged = sizeChanged && !positionChanged;
                    float resizeRight = currentPos.x + currentSize.x;
                    float resizeBottom = currentPos.y + currentSize.y;

                    if (positionChanged || sizeChanged) {
                        if (positionChanged) {
                            c.x = currentPos.x;
                            c.y = currentPos.y;
                        }
                        if (resizeChanged) {
                            c.w = (std::max)(currentSize.x, minSize.x);
                            c.h = (std::max)(currentSize.y, minSize.y);
                        }

                        m_draggedContainerId = c.id;
                        m_activeContainerCanDock = positionChanged && !resizeChanged;

                        if (positionChanged) {
                            CheckSnapping(c, snapContainers, dpiScale);
                            if (resizeChanged) {
                                c.w = (std::max)(resizeRight - c.x, minSize.x);
                                c.h = (std::max)(resizeBottom - c.y, minSize.y);
                            }
                        }
                        if (resizeChanged) {
                            CheckResizeSnapping(c, snapContainers, dpiScale, minSize.x, minSize.y);
                        }

                        ImGui::SetWindowPos(windowName.c_str(), ImVec2(c.x, c.y));
                        ImGui::SetWindowSize(windowName.c_str(), ImVec2(c.w, c.h));
                    }
                } else if (settingsOpen) {
                    if (!m_isMovingContainer && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow)) {
                        ImVec2 mousePos = ImGui::GetMousePos();
                        m_isMovingContainer = true;
                        m_draggedContainerId = c.id;
                        m_containerDragOffset = ImVec2(mousePos.x - currentPos.x, mousePos.y - currentPos.y);
                        m_activeContainerCanDock = false;
                    }
                } else {
                    if (std::abs(currentPos.x - c.x) > 0.1f || std::abs(currentPos.y - c.y) > 0.1f) {
                        c.x = currentPos.x;
                        c.y = currentPos.y;
                    }
                }

                m_lastContainerSizes[c.id] = ImVec2(c.w, c.h);
                for (auto& orig : origContainers) {
                    if (orig.id == c.id) {
                        orig.w = c.w;
                        orig.h = c.h;
                        break;
                    }
                }

                // Render all widgets within the container
                bool renderedAny = false;
                for (size_t wIdx = 0; wIdx < c.widgets.size(); ++wIdx) {
                    DashboardLayout::WidgetId wId = c.widgets[wIdx];

                    if (!IsWidgetVisible(c, wId, settingsOpen, editMode, showOverlay, inMatch, expanded, config)) continue;

                    if (renderedAny) {
                        ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
                        ImGui::Separator();
                        ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
                    }
                    renderedAny = true;

                    // Render sub-widget title bar/handle in Edit Mode
                    if (editMode) {
                        if (showWidgetHeaders) {
                            ImGui::BeginGroup();

                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.4f));
                            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));

                            std::string widgetName = DashboardLayout::GetWidgetDisplayName(wId);
                            // Make widget drag handle fill window width minus close button size using negative width to prevent window expansion feedback loop
                            float handleWidth = -28.0f * dpiScale;

                            ImGui::Button((widgetName + "##DragHandle" + std::to_string(static_cast<int>(wId))).c_str(), ImVec2(handleWidth, 22.0f * dpiScale));

                            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
                                m_isDraggingWidgetOut = true;
                                m_draggedWidgetId = wId;
                                m_draggedWidgetSourceContainerId = c.id;
                            }

                            if (m_isDraggingWidgetOut && m_draggedWidgetId == wId) {
                                ImGui::SetTooltip("Detaching: %s", widgetName.c_str());
                            }

                            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                                m_isDraggingWidgetOut = true;
                                m_draggedWidgetId = wId;
                                m_draggedWidgetSourceContainerId = c.id;
                                ImGui::SetDragDropPayload("DND_WIDGET_OUT", &wId, sizeof(wId));
                                ImGui::Text("Detaching: %s", widgetName.c_str());
                                ImGui::EndDragDropSource();
                            }

                            ImGui::SameLine();
                            std::string closeId = "##CloseWidget" + std::to_string(static_cast<int>(wId));
                            bool removeWidget = RenderCloseButton(closeId.c_str(), dpiScale);
                            ImGui::PopStyleColor(2);

                            if (removeWidget) {
                                Config::Update([&](ConfigData& conf) {
                                    for (auto& cont : conf.overlay_layout.containers) {
                                        if (cont.id == c.id) {
                                            cont.widgets.erase(std::remove(cont.widgets.begin(), cont.widgets.end(), wId), cont.widgets.end());
                                            FitContainerToWidgets(cont, dpiScale);
                                            m_lastContainerSizes.erase(cont.id);
                                            break;
                                        }
                                    }
                                    Sanitize(conf.overlay_layout);
                                },
                                               true);
                            }

                            ImGui::Dummy(ImVec2(0, 4.0f * dpiScale));
                            m_renderWidget(wId, c.id.c_str());
                            ImGui::EndGroup();
                        } else {
                            m_renderWidget(wId, c.id.c_str());
                        }
                    } else {
                        // Normal layout mode, just render contents directly
                        m_renderWidget(wId, c.id.c_str());
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // 4. Update the saved layout position coordinates back to Config on drag release
        if (settingsOpen || editMode) {
            bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);

            if (editMode && !mouseDown && m_lastMouseDown && m_isDraggingNewWidget) {
                DashboardLayout::WidgetId wId = m_draggedNewWidgetId;
                ImVec2 dropPos = ImGui::GetMousePos();
                Config::Update([&](ConfigData& c) {
                    ContainerConfig newC;
                    newC.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(rand() % 1000);
                    newC.x = dropPos.x - 24.0f * dpiScale;
                    newC.y = dropPos.y - 24.0f * dpiScale;
                    newC.widgets = {wId};
                    ApplyDefaultSize(newC, dpiScale);
                    c.overlay_layout.containers.push_back(newC);
                    Sanitize(c.overlay_layout);
                },
                               true);
                m_isDraggingNewWidget = false;
                m_lastMouseDown = mouseDown;
                return;
            }

            if (editMode && !mouseDown && m_lastMouseDown && m_isDraggingWidgetOut) {
                DetachDraggedWidget(ImGui::GetMousePos(), dpiScale);
                m_dockTargetId = "";
                m_draggedContainerId.clear();
                m_isDraggingWidgetOut = false;
                m_activeContainerCanDock = false;
                m_lastMouseDown = mouseDown;
                return;
            }

            if (!m_draggedContainerId.empty()) {
                // Continuous positions sync in memory
                Config::Update([&](ConfigData& c) {
                    for (auto& cont : c.overlay_layout.containers) {
                        if (cont.id == m_draggedContainerId) {
                            for (const auto& liveC : m_layout.containers) {
                                if (liveC.id == m_draggedContainerId) {
                                    cont.x = liveC.x;
                                    cont.y = liveC.y;
                                    cont.w = liveC.w;
                                    cont.h = liveC.h;
                                    break;
                                }
                            }
                            break;
                        }
                    }
                },
                               false); // saveToDisk = false to avoid continuous file-writes

                // Handle dock stacked previews
                if (editMode && m_activeContainerCanDock) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    for (const auto& liveC : m_layout.containers) {
                        if (liveC.id == m_draggedContainerId) {
                            HandleDockingPreview(liveC, mousePos, dpiScale);
                            break;
                        }
                    }
                }
            }

            // Drag release -> Save state permanently to config.json
            if (!mouseDown && m_lastMouseDown) {
                if (editMode && m_activeContainerCanDock && !m_dockTargetId.empty() && !m_draggedContainerId.empty()) {
                    // Dock merge containers
                    Config::Update([&](ConfigData& c) {
                        ContainerConfig draggedCont;
                        bool foundDragged = false;
                        for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                            if (it->id == m_draggedContainerId) {
                                draggedCont = *it;
                                c.overlay_layout.containers.erase(it);
                                foundDragged = true;
                                break;
                            }
                        }

                        if (foundDragged) {
                            for (auto& target : c.overlay_layout.containers) {
                                if (target.id == m_dockTargetId) {
                                    if (m_dockAtTop) {
                                        target.widgets.insert(target.widgets.begin(), draggedCont.widgets.begin(), draggedCont.widgets.end());
                                    } else {
                                        target.widgets.insert(target.widgets.end(), draggedCont.widgets.begin(), draggedCont.widgets.end());
                                    }
                                    FitContainerToWidgets(target, dpiScale);
                                    m_lastContainerSizes.erase(target.id);
                                    break;
                                }
                            }
                        }
                        if (foundDragged) {
                            m_lastContainerSizes.erase(draggedCont.id);
                        }
                        Sanitize(c.overlay_layout);
                    },
                                   true);
                } else if (!m_draggedContainerId.empty()) {
                    // Regular window drag release -> commit coords to disk
                    Config::Update([](ConfigData&) {}, true);
                }
                m_dockTargetId = "";
                m_draggedContainerId.clear();
                m_isDraggingWidgetOut = false;
                m_isMovingContainer = false;
                m_activeContainerCanDock = false;
            }
            m_lastMouseDown = mouseDown;
        }
    }

    void LayoutManager::CheckSnapping(ContainerConfig& c, const std::vector<ContainerConfig>& others, float dpiScale) {
        const float snapThreshold = 12.0f * dpiScale;
        const float margin = 0.0f;
        const float screenW = ImGui::GetIO().DisplaySize.x;
        const float screenH = ImGui::GetIO().DisplaySize.y;

        // Screen border horizontal boundaries
        if (std::abs(c.x - margin) < snapThreshold) {
            c.x = margin;
            m_snapGuideStart = ImVec2(margin, 0);
            m_snapGuideEnd = ImVec2(margin, screenH);
            m_showSnapGuide = true;
        } else if (std::abs((c.x + c.w) - (screenW - margin)) < snapThreshold) {
            c.x = screenW - margin - c.w;
            m_snapGuideStart = ImVec2(screenW - margin, 0);
            m_snapGuideEnd = ImVec2(screenW - margin, screenH);
            m_showSnapGuide = true;
        }

        // Screen border vertical boundaries
        if (std::abs(c.y - margin) < snapThreshold) {
            c.y = margin;
            m_snapGuideStart = ImVec2(0, margin);
            m_snapGuideEnd = ImVec2(screenW, margin);
            m_showSnapGuide = true;
        } else if (std::abs((c.y + c.h) - (screenH - margin)) < snapThreshold) {
            c.y = screenH - margin - c.h;
            m_snapGuideStart = ImVec2(0, screenH - margin);
            m_snapGuideEnd = ImVec2(screenW, screenH - margin);
            m_showSnapGuide = true;
        }

        // Snapping relative to other active windows
        for (const auto& other : others) {
            if (other.id == c.id) continue;

            // X coordinate alignments
            if (std::abs(c.x - other.x) < snapThreshold) {
                c.x = other.x;
                m_snapGuideStart = ImVec2(other.x, 0);
                m_snapGuideEnd = ImVec2(other.x, screenH);
                m_showSnapGuide = true;
            } else if (std::abs(c.x - (other.x + other.w)) < snapThreshold) {
                c.x = other.x + other.w;
                m_snapGuideStart = ImVec2(other.x + other.w, 0);
                m_snapGuideEnd = ImVec2(other.x + other.w, screenH);
                m_showSnapGuide = true;
            } else if (std::abs((c.x + c.w) - other.x) < snapThreshold) {
                c.x = other.x - c.w;
                m_snapGuideStart = ImVec2(other.x, 0);
                m_snapGuideEnd = ImVec2(other.x, screenH);
                m_showSnapGuide = true;
            } else if (std::abs((c.x + c.w) - (other.x + other.w)) < snapThreshold) {
                c.x = other.x + other.w - c.w;
                m_snapGuideStart = ImVec2(other.x + other.w, 0);
                m_snapGuideEnd = ImVec2(other.x + other.w, screenH);
                m_showSnapGuide = true;
            }

            // Y coordinate alignments
            if (std::abs(c.y - other.y) < snapThreshold) {
                c.y = other.y;
                m_snapGuideStart = ImVec2(0, other.y);
                m_snapGuideEnd = ImVec2(screenW, other.y);
                m_showSnapGuide = true;
            } else if (std::abs(c.y - (other.y + other.h)) < snapThreshold) {
                c.y = other.y + other.h;
                m_snapGuideStart = ImVec2(0, other.y + other.h);
                m_snapGuideEnd = ImVec2(screenW, other.y + other.h);
                m_showSnapGuide = true;
            } else if (std::abs((c.y + c.h) - other.y) < snapThreshold) {
                c.y = other.y - c.h;
                m_snapGuideStart = ImVec2(0, other.y);
                m_snapGuideEnd = ImVec2(screenW, other.y);
                m_showSnapGuide = true;
            } else if (std::abs((c.y + c.h) - (other.y + other.h)) < snapThreshold) {
                c.y = other.y + other.h - c.h;
                m_snapGuideStart = ImVec2(0, other.y + other.h);
                m_snapGuideEnd = ImVec2(screenW, other.y + other.h);
                m_showSnapGuide = true;
            }
        }
    }

    void LayoutManager::CheckResizeSnapping(ContainerConfig& c, const std::vector<ContainerConfig>& others, float dpiScale, float minW, float minH) {
        const float snapThreshold = 12.0f * dpiScale;
        const float margin = 24.0f * dpiScale;
        const float screenW = ImGui::GetIO().DisplaySize.x;
        const float screenH = ImGui::GetIO().DisplaySize.y;

        float right = c.x + c.w;
        float bottom = c.y + c.h;

        if (std::abs(right - (screenW - margin)) < snapThreshold) {
            c.w = screenW - margin - c.x;
            m_snapGuideStart = ImVec2(screenW - margin, 0);
            m_snapGuideEnd = ImVec2(screenW - margin, screenH);
            m_showSnapGuide = true;
        }
        if (std::abs(bottom - (screenH - margin)) < snapThreshold) {
            c.h = screenH - margin - c.y;
            m_snapGuideStart = ImVec2(0, screenH - margin);
            m_snapGuideEnd = ImVec2(screenW, screenH - margin);
            m_showSnapGuide = true;
        }

        for (const auto& other : others) {
            if (other.id == c.id) continue;

            if (std::abs(c.w - other.w) < snapThreshold) {
                c.w = other.w;
                m_snapGuideStart = ImVec2(c.x + c.w, 0);
                m_snapGuideEnd = ImVec2(c.x + c.w, screenH);
                m_showSnapGuide = true;
            } else if (std::abs((c.x + c.w) - other.x) < snapThreshold) {
                c.w = other.x - c.x;
                m_snapGuideStart = ImVec2(other.x, 0);
                m_snapGuideEnd = ImVec2(other.x, screenH);
                m_showSnapGuide = true;
            } else if (std::abs((c.x + c.w) - (other.x + other.w)) < snapThreshold) {
                c.w = other.x + other.w - c.x;
                m_snapGuideStart = ImVec2(other.x + other.w, 0);
                m_snapGuideEnd = ImVec2(other.x + other.w, screenH);
                m_showSnapGuide = true;
            }

            if (std::abs(c.h - other.h) < snapThreshold) {
                c.h = other.h;
                m_snapGuideStart = ImVec2(0, c.y + c.h);
                m_snapGuideEnd = ImVec2(screenW, c.y + c.h);
                m_showSnapGuide = true;
            } else if (std::abs((c.y + c.h) - other.y) < snapThreshold) {
                c.h = other.y - c.y;
                m_snapGuideStart = ImVec2(0, other.y);
                m_snapGuideEnd = ImVec2(screenW, other.y);
                m_showSnapGuide = true;
            } else if (std::abs((c.y + c.h) - (other.y + other.h)) < snapThreshold) {
                c.h = other.y + other.h - c.y;
                m_snapGuideStart = ImVec2(0, other.y + other.h);
                m_snapGuideEnd = ImVec2(screenW, other.y + other.h);
                m_showSnapGuide = true;
            }
        }

        c.w = std::clamp(c.w, minW, (std::max)(minW, screenW - margin - c.x));
        c.h = std::clamp(c.h, minH, (std::max)(minH, screenH - margin - c.y));
    }

    void LayoutManager::HandleDockingPreview(const ContainerConfig& dragged, const ImVec2& mousePos, float dpiScale) {
        m_dockTargetId = "";

        for (const auto& other : m_layout.containers) {
            if (other.id == dragged.id) continue;

            // Check if mouse falls inside the bounds of this target container
            ImVec2 min = ImVec2(other.x, other.y);
            ImVec2 max = ImVec2(other.x + other.w, other.y + other.h);

            if (mousePos.x >= min.x && mousePos.x <= max.x && mousePos.y >= min.y && mousePos.y <= max.y) {
                m_dockTargetId = other.id;

                // Check top 25% height or bottom 25% height
                float topHeight = other.h * 0.25f;
                float bottomHeight = other.h * 0.25f;

                ImDrawList* drawList = ImGui::GetForegroundDrawList();

                if (mousePos.y < min.y + topHeight) {
                    // Top stack combine preview (blue semi-transparent box)
                    m_dockAtTop = true;
                    ImVec2 previewMax = ImVec2(max.x, min.y + topHeight);
                    drawList->AddRectFilled(min, previewMax, IM_COL32(59, 158, 255, 60), 4.0f * dpiScale);
                    drawList->AddRect(min, previewMax, IM_COL32(59, 158, 255, 180), 4.0f * dpiScale, 0, 2.0f * dpiScale);
                } else if (mousePos.y > max.y - bottomHeight) {
                    // Bottom stack combine preview
                    m_dockAtTop = false;
                    ImVec2 previewMin = ImVec2(min.x, max.y - bottomHeight);
                    drawList->AddRectFilled(previewMin, max, IM_COL32(59, 158, 255, 60), 4.0f * dpiScale);
                    drawList->AddRect(previewMin, max, IM_COL32(59, 158, 255, 180), 4.0f * dpiScale, 0, 2.0f * dpiScale);
                } else {
                    // Default to bottom if mouse is in center area
                    m_dockAtTop = false;
                }
                break;
            }
        }
    }

} // namespace OverlayLayout
