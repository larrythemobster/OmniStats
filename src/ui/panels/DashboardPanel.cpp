#include "DashboardPanel.hpp"
#include "ui/Overlay.hpp"
#include "ui/RenderContext.hpp"
#include "ui/Formatting.hpp"
#include "ui/RenderHelpers.hpp"
#include "ui/widgets/MmrGraphWidget.hpp"
#include "core/Config.hpp"
#include "database/DatabaseManager.hpp"
#include "core/AppVersion.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <ctime>
#include <cstdint>

namespace {

    bool IsPointInRect(const ImVec2& point, const ImVec2& min, const ImVec2& max) {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }

    void MoveDashboardWidget(DashboardLayout::WidgetId droppedId, DashboardLayout::Zone zone, int insertIndex) {
        Config::Update([droppedId, zone, insertIndex](ConfigData& c) {
            DashboardLayout::Sanitize(c.dashboard_layout);

            std::vector<DashboardLayout::WidgetId> zoneIds;
            int currentIndex = -1;
            int sourceIndex = 0;
            for (const auto& w : c.dashboard_layout.widgets) {
                if (w.zone == zone) {
                    if (w.id == droppedId)
                        currentIndex = sourceIndex;
                    else
                        zoneIds.push_back(w.id);
                    sourceIndex++;
                }
            }

            int targetIndex = insertIndex;
            if (currentIndex >= 0 && currentIndex < targetIndex) targetIndex--;
            targetIndex = std::clamp(targetIndex, 0, static_cast<int>(zoneIds.size()));
            zoneIds.insert(zoneIds.begin() + targetIndex, droppedId);

            for (auto& w : c.dashboard_layout.widgets) {
                if (w.id == droppedId) {
                    w.zone = zone;
                    break;
                }
            }

            for (int i = 0; i < static_cast<int>(zoneIds.size()); ++i) {
                for (auto& w : c.dashboard_layout.widgets) {
                    if (w.id == zoneIds[i]) {
                        w.order = i;
                        break;
                    }
                }
            }

            DashboardLayout::Sanitize(c.dashboard_layout);
        });
    }

    std::string FormatRelativeMatchTime(int64_t endedAtUnix) {
        if (endedAtUnix <= 0) return "--";

        int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t diff = now - endedAtUnix;
        if (diff < 0) diff = 0;

        if (diff < 60) return "now";
        if (diff < 3600) return std::to_string(diff / 60) + "m ago";
        if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
        return std::to_string(diff / 86400) + "d ago";
    }

} // namespace

DashboardPanel::DashboardPanel(RenderContext context, HWND hwnd, DashRenderFuncs f)
    : ctx(std::move(context)), m_hwnd(hwnd), funcs(std::move(f)) {}

DashboardPanel::~DashboardPanel() = default;

void DashboardPanel::Render() {
    ImGuiIO& io = ImGui::GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;

    if (ctx.config.second_monitor_mode) {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowBgAlpha(ctx.config.themeBg.a);
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    } else {
        float dashW = fminf(1340.0f * ctx.dpiScale, io.DisplaySize.x - 40.0f * ctx.dpiScale);
        float dashH = fminf(820.0f * ctx.dpiScale, io.DisplaySize.y - 40.0f * ctx.dpiScale);
        ImGui::SetNextWindowPos(ImVec2((io.DisplaySize.x - dashW) * 0.5f, (io.DisplaySize.y - dashH) * 0.5f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(dashW, dashH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(ctx.config.themeBg.a);
    }

    float rounding = ctx.config.second_monitor_mode ? 0.0f : 12.0f * ctx.dpiScale;
    if (ctx.config.second_monitor_mode) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * ctx.dpiScale, 24.0f * ctx.dpiScale));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, rounding);

    if (ImGui::Begin("OmniStats Unified Dashboard", nullptr, flags)) {
        bool isWindowed = ctx.config.second_monitor_mode;

        // Prompt user for update if available and auto-update is disabled
        if (ctx.state.ui.updateAvailable.load() &&
            !ctx.config.enable_auto_updates &&
            !ctx.state.ui.updatePromptShown.load()) {
            ImGui::OpenPopup("Update Available Dialog");
            ctx.state.ui.updatePromptShown.store(true);
        }

        if (ImGui::BeginPopupModal("Update Available Dialog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            std::string ver;
            {
                std::lock_guard<std::mutex> lock(ctx.state.ui.updateMutex);
                ver = ctx.state.ui.updateAvailableVersion;
            }

            if (ver.empty()) {
                ImGui::Text("A new version of OmniStats is available.");
            } else {
                ImGui::Text("A new version of OmniStats is available: v%s", ver.c_str());
            }
            ImGui::Text("Would you like to update and restart the application now?");
            ImGui::Separator();

            if (ImGui::Button("Yes, Update Now", ImVec2(120, 0))) {
                if (!ctx.state.ui.updateDownloading.load()) {
                    ExternalUpdaterLauncher::StartInteractiveUpdate(ctx.state.shared_from_this());
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Remind Me Later", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        if (isWindowed) {
            RenderCustomTitleBar();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * ctx.dpiScale, 8.0f * ctx.dpiScale));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            ImGui::BeginChild("DashboardContents", ImVec2(0, -44.0f * ctx.dpiScale), ImGuiChildFlags_AlwaysUseWindowPadding);
        } else {
            ImGui::PushFont(ctx.fontBold);
            ImGui::TextColored(Format::C(ctx.config.themeAccent), "OMNISTATS UNIFIED DASHBOARD");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushFont(ctx.fontSmallBold);
            ImGui::TextColored(Format::C(ctx.config.themeMuted), " v%s", AppVersion::Current);
            ImGui::PopFont();

            if (ctx.state.ui.updateAvailable.load()) {
                ImGui::SameLine();
                std::string ver;
                {
                    std::lock_guard<std::mutex> lock(ctx.state.ui.updateMutex);
                    ver = ctx.state.ui.updateAvailableVersion;
                }

                ImGui::PushStyleColor(ImGuiCol_Button, Format::C(ctx.config.themeWin));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Format::C(ctx.config.themeWin).x * 1.1f, Format::C(ctx.config.themeWin).y * 1.1f, Format::C(ctx.config.themeWin).z * 1.1f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * ctx.dpiScale);
                ImGui::PushFont(ctx.fontSmallBold);

                std::string btnLabel;
                if (ctx.state.ui.updateDownloading.load()) {
                    btnLabel = ver.empty() ? "Starting updater..." : "Starting v" + ver + "...";
                } else if (ctx.state.ui.updateDownloadFailed.load()) {
                    btnLabel = ver.empty() ? "Retry Update" : "Retry Update v" + ver;
                } else {
                    btnLabel = ver.empty() ? "Update Available" : "Update Available: v" + ver;
                }

                if (ImGui::Button(btnLabel.c_str())) {
                    if (!ctx.state.ui.updateDownloading.load()) {
                        ExternalUpdaterLauncher::StartInteractiveUpdate(ctx.state.shared_from_this());
                    }
                }
                ImGui::PopFont();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);

                if (ImGui::IsItemHovered()) {
                    if (ctx.state.ui.updateDownloading.load()) {
                        ImGui::SetTooltip("Updater is preparing the update...");
                    } else {
                        ImGui::SetTooltip("Click to launch the updater and restart.");
                    }
                }
            }

            std::string statusStr = ctx.snap->inMatch ? "ACTIVE MATCH: " + ctx.snap->arenaName : "WAITING IN LOBBY...";
            ImVec4 statusColor = ctx.snap->inMatch ? Format::C(ctx.config.themeWin) : Format::C(ctx.config.themeMuted);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(statusStr.c_str()).x);
            ImGui::PushFont(ctx.fontBold);
            ImGui::TextColored(statusColor, "%s", statusStr.c_str());
            ImGui::PopFont();
            ImGui::Separator();
            ImGui::Spacing();
        }

        DashboardLayout::LayoutConfig layout = ctx.config.dashboard_layout;
        DashboardLayout::Sanitize(layout);

        for (auto& w : layout.widgets) {
            if (w.id == DashboardLayout::WidgetId::LobbyRanks && !ctx.config.show_lobby_ranks_overlay) {
                w.zone = DashboardLayout::Zone::Hidden;
            } else if (w.id == DashboardLayout::WidgetId::DemoTracker && !ctx.config.show_demo_tracker_overlay) {
                w.zone = DashboardLayout::Zone::Hidden;
            } else if (w.id == DashboardLayout::WidgetId::PreviousGames && !ctx.config.show_previous_games_summary) {
                w.zone = DashboardLayout::Zone::Hidden;
            }
        }

        bool anyVisible = false;
        for (const auto& w : layout.widgets) {
            if (w.zone != DashboardLayout::Zone::Hidden) {
                anyVisible = true;
                break;
            }
        }

        if (anyVisible) {
            RenderDashboardZones(layout);
        } else {
            ImGui::Spacing();
            ImGui::PushFont(ctx.fontBold);
            ImGui::TextColored(Format::C(ctx.config.themeMuted), "All dashboard panels have been toggled off.");
            ImGui::TextColored(Format::C(ctx.config.themeDim), "Press F5 (settings interact bind) and re-enable panels in standard settings window.");
            ImGui::PopFont();
            ImGui::Spacing();
        }

        if (isWindowed) {
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();

            ImVec2 windowPos = ImGui::GetWindowPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddLine(
                ImVec2(windowPos.x + 24.0f * ctx.dpiScale, windowPos.y + io.DisplaySize.y - 44.0f * ctx.dpiScale),
                ImVec2(windowPos.x + io.DisplaySize.x - 24.0f * ctx.dpiScale, windowPos.y + io.DisplaySize.y - 44.0f * ctx.dpiScale),
                ImGui::GetColorU32(Format::C(ctx.config.themeMuted)));
            ImGui::SetCursorPos(ImVec2(24.0f * ctx.dpiScale, io.DisplaySize.y - 32.0f * ctx.dpiScale));
            if (ctx.state.ui.showMenu) {
                ImGui::PushFont(ctx.fontBold);
                ImGui::TextColored(Format::C(ctx.config.themeWin), "SETTINGS ACTIVE: Adjust your preferences. Press ESC or F5 to close settings.");
                ImGui::PopFont();
            } else {
                ImGui::PushFont(ctx.fontSmallBold);
                ImGui::TextColored(Format::C(ctx.config.themeMuted), "DASHBOARD ACTIVE: Press F5 to open settings.");
                ImGui::PopFont();
            }
        } else {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (ctx.state.ui.showMenu) {
                ImGui::PushFont(ctx.fontBold);
                ImGui::TextColored(Format::C(ctx.config.themeWin), "SETTINGS ACTIVE: Click anywhere to adjust preferences. Press ESC or F5 to resume play.");
                ImGui::PopFont();
            } else {
                ImGui::PushFont(ctx.fontSmallBold);
                ImGui::TextColored(Format::C(ctx.config.themeMuted), "DASHBOARD LOCKED: Clicks bypass overlay to game window. Press F5 to interact with settings.");
                ImGui::PopFont();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DashboardPanel::RenderCustomTitleBar() {
    ImGuiIO& io = ImGui::GetIO();
    float barHeight = 40.0f * ctx.dpiScale;
    ImVec2 windowPos = ImGui::GetWindowPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + io.DisplaySize.x, windowPos.y + barHeight), ImColor(10, 10, 10, 255));
    drawList->AddLine(ImVec2(windowPos.x, windowPos.y + barHeight - 1.0f), ImVec2(windowPos.x + io.DisplaySize.x, windowPos.y + barHeight - 1.0f), ImColor(30, 30, 30, 255));

    float textYOffset = (barHeight - ImGui::GetTextLineHeight()) * 0.5f;
    ImVec2 iconPos = ImVec2(windowPos.x + 14.0f * ctx.dpiScale, windowPos.y + textYOffset + 2.0f * ctx.dpiScale);
    drawList->AddRectFilled(iconPos, ImVec2(iconPos.x + 10.0f * ctx.dpiScale, iconPos.y + 10.0f * ctx.dpiScale), ImGui::GetColorU32(Format::C(ctx.config.themeAccent)), 2.0f);
    drawList->AddRect(iconPos, ImVec2(iconPos.x + 10.0f * ctx.dpiScale, iconPos.y + 10.0f * ctx.dpiScale), ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.4f)), 2.0f);

    ImGui::SetCursorPos(ImVec2(32.0f * ctx.dpiScale, textYOffset));
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "OMNISTATS UNIFIED DASHBOARD");
    ImGui::PopFont();

    ImGui::SameLine();
    ImGui::SetCursorPosY(textYOffset + 2.0f * ctx.dpiScale);
    ImGui::PushFont(ctx.fontSmallBold);
    ImGui::TextColored(Format::C(ctx.config.themeMuted), " v%s", AppVersion::Current);
    ImGui::PopFont();

    if (ctx.state.ui.updateAvailable.load()) {
        ImGui::SameLine();
        ImGui::SetCursorPosY(textYOffset);
        std::string ver;
        {
            std::lock_guard<std::mutex> lock(ctx.state.ui.updateMutex);
            ver = ctx.state.ui.updateAvailableVersion;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, Format::C(ctx.config.themeWin));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Format::C(ctx.config.themeWin).x * 1.1f, Format::C(ctx.config.themeWin).y * 1.1f, Format::C(ctx.config.themeWin).z * 1.1f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * ctx.dpiScale);
        ImGui::PushFont(ctx.fontSmallBold);

        std::string btnLabel;
        if (ctx.state.ui.updateDownloading.load()) {
            btnLabel = ver.empty() ? "Starting updater..." : "Starting v" + ver + "...";
        } else if (ctx.state.ui.updateDownloadFailed.load()) {
            btnLabel = ver.empty() ? "Retry Update" : "Retry Update v" + ver;
        } else {
            btnLabel = ver.empty() ? "Update Available" : "Update Available: v" + ver;
        }

        if (ImGui::Button(btnLabel.c_str())) {
            if (!ctx.state.ui.updateDownloading.load()) {
                ExternalUpdaterLauncher::StartInteractiveUpdate(ctx.state.shared_from_this());
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            if (ctx.state.ui.updateDownloading.load()) {
                ImGui::SetTooltip("Updater is preparing the update...");
            } else {
                ImGui::SetTooltip("Click to launch the updater and restart.");
            }
        }
    }

    std::string statusStr = ctx.snap->inMatch ? "ACTIVE MATCH: " + ctx.snap->arenaName : "WAITING IN LOBBY...";
    ImVec4 statusColor = ctx.snap->inMatch ? Format::C(ctx.config.themeWin) : Format::C(ctx.config.themeMuted);
    float btnWidth = 46.0f * ctx.dpiScale;
    float startX = io.DisplaySize.x - (btnWidth * 4.0f); // Reserve for 4 buttons
    float statusTextWidth = ImGui::CalcTextSize(statusStr.c_str()).x;

    ImGui::SameLine();
    ImGui::SetCursorPos(ImVec2(startX - statusTextWidth - 16.0f * ctx.dpiScale, textYOffset));
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(statusColor, "%s", statusStr.c_str());
    ImGui::PopFont();

    float btnHeight = barHeight - 4.0f * ctx.dpiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.2f, 0.2f, 0.4f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));

    ImGui::SetCursorPos(ImVec2(startX, 4.0f * ctx.dpiScale));
    bool editMode = ctx.state.ui.dashboardLayoutEditMode.load();
    const char* editLabel = editMode ? "SAVE" : "EDIT";
    ImGui::Button(editLabel, ImVec2(btnWidth, btnHeight));
    bool clicked = ImGui::IsItemClicked();
    bool hovered = ImGui::IsItemHovered();
    bool mouseDown = ImGui::IsMouseDown(0);

    if (hovered) {
        ImGui::SetTooltip("Click to toggle edit mode");
    }

    static bool lastMouseDown = false;
    bool triggered = false;

    if (clicked) {
        triggered = true;
    } else if (hovered && mouseDown && !lastMouseDown) {
        triggered = true;
    }
    lastMouseDown = mouseDown;

    if (triggered) {
        bool newMode = !editMode;
        ctx.state.ui.dashboardLayoutEditMode.store(newMode);
        if (!newMode) {
            Config::RequestSave();
        }
    }

    ImGui::SameLine(0, 0);
    if (ImGui::Button("_##Minimize", ImVec2(btnWidth, btnHeight))) {
        ShowWindow(m_hwnd, SW_MINIMIZE);
    }
    ImGui::SameLine(0, 0);
    bool isMax = IsZoomed(m_hwnd) != 0;
    const char* maxSymbol = isMax ? "[]" : "[ ]";
    if (ImGui::Button(maxSymbol, ImVec2(btnWidth, btnHeight))) {
        if (isMax)
            ShowWindow(m_hwnd, SW_RESTORE);
        else
            ShowWindow(m_hwnd, SW_MAXIMIZE);
    }
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.65f, 0.1f, 0.1f, 1.0f));
    if (ImGui::Button("X##Close", ImVec2(btnWidth, btnHeight))) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPos(ImVec2(0.0f, barHeight));
}

void DashboardPanel::RenderDashboardZones(const DashboardLayout::LayoutConfig& layout) {
    float availW = ImGui::GetContentRegionAvail().x;
    bool editMode = ctx.state.ui.dashboardLayoutEditMode.load();

    if (!editMode) {
        m_draggedDashboardWidget = -1;
    }

    bool draggingWidget = editMode && m_draggedDashboardWidget >= 0;
    ImVec2 mousePos = ImGui::GetMousePos();
    DashboardLayout::Zone targetZone = DashboardLayout::Zone::Hidden;
    int targetIndex = 0;
    float targetLineY = 0.0f;
    float targetLineMinX = 0.0f;
    float targetLineMaxX = 0.0f;

    auto drawDropLine = [&]() {
        if (!draggingWidget || targetZone == DashboardLayout::Zone::Hidden) return;
        ImDrawList* drawList = ImGui::GetForegroundDrawList();
        ImU32 color = ImGui::GetColorU32(Format::C(ctx.config.themeAccent));
        drawList->AddLine(ImVec2(targetLineMinX, targetLineY), ImVec2(targetLineMaxX, targetLineY), color, 3.0f * ctx.dpiScale);
        drawList->AddCircleFilled(ImVec2(targetLineMinX, targetLineY), 4.0f * ctx.dpiScale, color);
        drawList->AddCircleFilled(ImVec2(targetLineMaxX, targetLineY), 4.0f * ctx.dpiScale, color);
    };

    auto setColumnTarget = [&](DashboardLayout::Zone zone, const std::vector<std::pair<float, float>>& widgetRects, const ImVec2& min, const ImVec2& max) {
        if (!draggingWidget || !IsPointInRect(mousePos, min, max)) return;

        int insertIndex = static_cast<int>(widgetRects.size());
        float lineY = widgetRects.empty() ? min.y + 18.0f * ctx.dpiScale : widgetRects.back().second + 4.0f * ctx.dpiScale;
        for (int i = 0; i < static_cast<int>(widgetRects.size()); ++i) {
            float midY = (widgetRects[i].first + widgetRects[i].second) * 0.5f;
            if (mousePos.y < midY) {
                insertIndex = i;
                lineY = widgetRects[i].first - 4.0f * ctx.dpiScale;
                break;
            }
        }

        targetZone = zone;
        targetIndex = insertIndex;
        targetLineY = std::clamp(lineY, min.y + 8.0f * ctx.dpiScale, max.y - 8.0f * ctx.dpiScale);
        targetLineMinX = min.x + 8.0f * ctx.dpiScale;
        targetLineMaxX = max.x - 8.0f * ctx.dpiScale;
    };

    auto renderEmptyColumnTarget = [&](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.3f, 0.1f, 0.4f));
        ImGui::Button(label, ImVec2(-1, 64.0f * ctx.dpiScale));
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8.0f * ctx.dpiScale));
    };

    // Render Top Zone (Full Width)
    if (editMode) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.3f, 0.1f, 0.4f));
        ImGui::Button("::: Drag a card here for Top Zone :::", ImVec2(-1, 24.0f * ctx.dpiScale));
        ImVec2 topMin = ImGui::GetItemRectMin();
        ImVec2 topMax = ImGui::GetItemRectMax();
        ImGui::PopStyleColor();
        if (draggingWidget && IsPointInRect(mousePos, topMin, topMax)) {
            targetZone = DashboardLayout::Zone::Top;
            targetIndex = 0;
            targetLineY = (topMin.y + topMax.y) * 0.5f;
            targetLineMinX = topMin.x + 8.0f * ctx.dpiScale;
            targetLineMaxX = topMax.x - 8.0f * ctx.dpiScale;
        }
        ImGui::Dummy(ImVec2(0, 4.0f * ctx.dpiScale));
    }

    bool topAny = false;
    for (const auto& w : layout.widgets) {
        if (w.zone == DashboardLayout::Zone::Top) {
            RenderWidget(w.id, "Top");
            topAny = true;
        }
    }

    if (topAny) {
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));
    }

    float leftW = availW * layout.leftColumnWeight;
    float rightW = availW - leftW - ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("LeftZone", ImVec2(leftW, 0), false);
    ImVec2 leftMin = ImGui::GetWindowPos();
    ImVec2 leftMax = ImVec2(leftMin.x + ImGui::GetWindowSize().x, leftMin.y + ImGui::GetWindowSize().y);
    std::vector<std::pair<float, float>> leftRects;
    for (const auto& w : layout.widgets) {
        if (w.zone == DashboardLayout::Zone::Left) {
            float widgetStartY = ImGui::GetCursorScreenPos().y;
            RenderWidget(w.id, "Left");
            float widgetEndY = ImGui::GetCursorScreenPos().y;
            leftRects.push_back({widgetStartY, widgetEndY});
        }
    }
    if (editMode && leftRects.empty()) {
        renderEmptyColumnTarget("::: Drag a card here for Left Column :::");
    }
    if (editMode) {
        ImVec2 dropSize = ImGui::GetContentRegionAvail();
        dropSize.y = (std::max)(dropSize.y, 24.0f);
        ImGui::Dummy(dropSize);
        setColumnTarget(DashboardLayout::Zone::Left, leftRects, leftMin, leftMax);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("RightZone", ImVec2(rightW, 0), false);
    ImVec2 rightMin = ImGui::GetWindowPos();
    ImVec2 rightMax = ImVec2(rightMin.x + ImGui::GetWindowSize().x, rightMin.y + ImGui::GetWindowSize().y);
    std::vector<std::pair<float, float>> rightRects;
    for (const auto& w : layout.widgets) {
        if (w.zone == DashboardLayout::Zone::Right) {
            float widgetStartY = ImGui::GetCursorScreenPos().y;
            RenderWidget(w.id, "Right");
            float widgetEndY = ImGui::GetCursorScreenPos().y;
            rightRects.push_back({widgetStartY, widgetEndY});
        }
    }
    if (editMode && rightRects.empty()) {
        renderEmptyColumnTarget("::: Drag a card here for Right Column :::");
    }
    if (editMode) {
        ImVec2 dropSize = ImGui::GetContentRegionAvail();
        dropSize.y = (std::max)(dropSize.y, 24.0f);
        ImGui::Dummy(dropSize);
        setColumnTarget(DashboardLayout::Zone::Right, rightRects, rightMin, rightMax);
    }
    ImGui::EndChild();

    drawDropLine();

    if (draggingWidget && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (targetZone != DashboardLayout::Zone::Hidden) {
            MoveDashboardWidget(static_cast<DashboardLayout::WidgetId>(m_draggedDashboardWidget), targetZone, targetIndex);
        }
        m_draggedDashboardWidget = -1;
    }
}

void DashboardPanel::RenderWidget(DashboardLayout::WidgetId id, const char* idSuffix) {
    bool editMode = ctx.state.ui.dashboardLayoutEditMode.load();

    ImGui::BeginGroup();

    if (editMode) {
        // Draw a handle for dragging
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.6f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.8f));
        ImGui::Button((std::string("::: ") + DashboardLayout::GetWidgetDisplayName(id) + " :::").c_str(), ImVec2(-1, 24));
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            m_draggedDashboardWidget = static_cast<int>(id);
        }
        if (m_draggedDashboardWidget == static_cast<int>(id)) {
            ImGui::SetTooltip("Moving %s", DashboardLayout::GetWidgetDisplayName(id));
        }
        ImGui::Dummy(ImVec2(0, 4));
    }

    switch (id) {
    case DashboardLayout::WidgetId::LiveRoster:
        RenderLiveRosterWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::LiveMatchStats:
        RenderLiveMatchStatsWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::SessionStats:
        RenderSessionStatsWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::MmrGraph:
        RenderMmrGraphWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::StreaksStats:
        RenderStreaksStatsWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::GamemodeBreakdown:
        RenderGamemodeBreakdownWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::LobbyRanks:
        RenderLobbyRanksWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::DemoTracker:
        RenderDemoTrackerWidget(idSuffix);
        break;
    case DashboardLayout::WidgetId::PreviousGames:
        RenderPreviousGamesWidget(idSuffix);
        break;
    }

    ImGui::EndGroup();

    // Add a separator or space between widgets
    ImGui::Dummy(ImVec2(0, 16));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));
}

void DashboardPanel::RenderLiveRosterWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "LIVE MATCH & ROSTER");
    ImGui::PopFont();

    float baseComboW = 120.0f * ctx.dpiScale;
    float comboW = baseComboW;
    float avail = ImGui::GetContentRegionAvail().x;
    bool pushedSmall = false;
    if (avail < 200.0f * ctx.dpiScale) {
        float scale = (std::max)(0.6f, avail / (200.0f * ctx.dpiScale));
        comboW = baseComboW * scale;
        ImGui::PushFont(ctx.fontSmall);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * ctx.dpiScale * scale, 2.0f * ctx.dpiScale * scale));
        pushedSmall = true;
    }
    ImGui::SameLine();
    float curX = ImGui::GetCursorPosX();
    float posCombo = curX + ImGui::GetContentRegionAvail().x - comboW;
    float minAllowed = curX + 8.0f * ctx.dpiScale;
    if (posCombo < minAllowed) posCombo = minAllowed;
    ImGui::SetCursorPosX(posCombo);
    ImGui::SetNextItemWidth(comboW);
    std::vector<std::pair<const char*, MmrCategory>> rosterCategories = {
        {"Best", MmrCategory::Best}, {"1v1", MmrCategory::OneVOne}, {"2v2", MmrCategory::TwoVTwo}, {"3v3", MmrCategory::ThreeVThree}, {"Casual", MmrCategory::Casual}, {"Tournament", MmrCategory::Tourny}};
    if (ctx.config.show_extra_playlists) {
        rosterCategories.insert(rosterCategories.begin() + 4, {{"Hoops", MmrCategory::Hoops}, {"Rumble", MmrCategory::Rumble}, {"Dropshot", MmrCategory::Dropshot}, {"Snow Day", MmrCategory::SnowDay}});
    }
    std::vector<const char*> rosterLabels;
    for (const auto& category : rosterCategories)
        rosterLabels.push_back(category.first);
    int currentRosterCat = 0;
    MmrCategory loadedRosterCat = ctx.state.ui.rosterMmrCategory.load();
    for (int i = 0; i < static_cast<int>(rosterCategories.size()); ++i) {
        if (rosterCategories[i].second == loadedRosterCat) {
            currentRosterCat = i;
            break;
        }
    }
    if (ImGui::Combo("##RosterCatCombo", &currentRosterCat, rosterLabels.data(), static_cast<int>(rosterLabels.size()))) {
        ctx.state.ui.rosterMmrCategory.store(rosterCategories[currentRosterCat].second);
    }
    if (pushedSmall) {
        ImGui::PopStyleVar();
        ImGui::PopFont();
    }

    ImGui::Separator();
    ImGui::Spacing();

    if (ctx.snap->roster.empty()) {
        ImGui::TextColored(Format::C(ctx.config.themeMuted), "No active match connected.");
    } else {
        if (funcs.playerRoster) funcs.playerRoster(0, "BLUE", ImColor(59, 158, 255), "BLUE_Dash");
        ImGui::Dummy(ImVec2(0, 8));
        if (funcs.playerRoster) funcs.playerRoster(1, "ORANGE", ImColor(255, 122, 41), "ORANGE_Dash");
    }
}

void DashboardPanel::RenderLiveMatchStatsWidget(const char* idSuffix) {
    ImGui::TextColored(Format::C(ctx.config.themeText), "LIVE MATCH STATS");
    ImGui::SameLine();
    ImGui::PushFont(ctx.fontSmall);
    float posX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Left = Lobby Total  |  Right = You").x;
    if (posX > ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(posX);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(Format::C(ctx.config.themeMuted), "Left = Lobby Total  |  Right = You");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    if (funcs.liveMatchStats) funcs.liveMatchStats("DashLiveTbl");
}

void DashboardPanel::RenderSessionStatsWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "SESSION STATS");
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    if (funcs.sessionStatsTable) funcs.sessionStatsTable("DashboardSessionStatsTbl", true, true);
}

void DashboardPanel::RenderPreviousGamesWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "PREVIOUS GAMES");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::PushFont(ctx.fontSmall);
    ImGui::TextColored(Format::C(ctx.config.themeMuted), "last %d saved games", ctx.config.previous_games_limit);
    ImGui::PopFont();
    ImGui::Separator();
    ImGui::Spacing();

    const auto& matches = ctx.snap->recentSavedMatches;
    if (!ctx.snap->recentSavedMatchesLoaded) {
        ImGui::TextColored(Format::C(ctx.config.themeMuted), "Loading saved match history...");
        ImGui::Dummy(ImVec2(0, 6.0f * ctx.dpiScale));
    } else if (matches.empty()) {
        ImGui::TextColored(Format::C(ctx.config.themeMuted), "No saved games in local history.");
        ImGui::Dummy(ImVec2(0, 6.0f * ctx.dpiScale));
    } else {
        const size_t displayCount = matches.size();
        const bool twoColumns = ImGui::GetContentRegionAvail().x >= 720.0f * ctx.dpiScale && displayCount > 10;
        const int columnSets = twoColumns ? 2 : 1;
        const int rowsPerColumn = twoColumns ? static_cast<int>((displayCount + 1) / 2) : static_cast<int>(displayCount);
        const int tableColumns = columnSets * 4 + (columnSets - 1);
        std::string tableId = std::string("PreviousGames_") + idSuffix;

        if (ImGui::BeginTable(tableId.c_str(), tableColumns, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            for (int set = 0; set < columnSets; ++set) {
                if (set > 0) ImGui::TableSetupColumn("##Gap", ImGuiTableColumnFlags_WidthFixed, 14.0f * ctx.dpiScale);
                ImGui::TableSetupColumn((std::string("Playlist##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn((std::string("Score##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 52.0f * ctx.dpiScale);
                ImGui::TableSetupColumn((std::string("MMR##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 56.0f * ctx.dpiScale);
                ImGui::TableSetupColumn((std::string("Time##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 78.0f * ctx.dpiScale);
            }
            ImGui::TableHeadersRow();

            ImGui::PushFont(ctx.fontMono);
            for (int row = 0; row < rowsPerColumn; ++row) {
                ImGui::TableNextRow();
                for (int set = 0; set < columnSets; ++set) {
                    if (set > 0) {
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("");
                    }

                    const size_t matchIndex = static_cast<size_t>(set * rowsPerColumn + row);
                    if (matchIndex >= displayCount) {
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("");
                        continue;
                    }

                    const auto& match = matches[matchIndex];
                    const ImVec4 rowColor = Format::C(match.win ? ctx.config.themeWin : ctx.config.themeLoss);
                    const std::string playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
                    const std::string score = std::to_string(match.ourScore) + "-" + std::to_string(match.theirScore);
                    const std::string time = FormatRelativeMatchTime(match.endedAtUnix);

                    ImGui::TableNextColumn();
                    ImGui::TextColored(rowColor, "%s", playlist.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextColored(rowColor, "%s", score.c_str());
                    ImGui::TableNextColumn();
                    if (match.mmr > 0)
                        ImGui::TextColored(rowColor, "%d", match.mmr);
                    else
                        ImGui::TextColored(Format::C(ctx.config.themeMuted), "--");
                    ImGui::TableNextColumn();
                    ImGui::TextColored(rowColor, "%s", time.c_str());
                }
            }
            ImGui::PopFont();
            ImGui::EndTable();
        }
    }

    ImGui::Dummy(ImVec2(0, 4.0f * ctx.dpiScale));
    ImGui::PushFont(ctx.fontSmallBold);
    ImGui::TextColored(Format::C(ctx.config.themeMuted), "Current session: W:%d L:%d", ctx.snap->sessionTotals.wins, ctx.snap->sessionTotals.losses);
    ImGui::PopFont();
}

void DashboardPanel::RenderMmrGraphWidget(const char* idSuffix) {
    std::string playlist = MmrCategoryToString(ctx.state.ui.graphMmrCategory.load());
    std::string playlistUpper = playlist;
    std::transform(playlistUpper.begin(), playlistUpper.end(), playlistUpper.begin(), ::toupper);

    auto& t = ctx.config;
    ImColor colorBg = Format::C(t.themeBg);
    ImColor colorText = Format::C(t.themeText);
    ImColor colorAccent = Format::C(t.themeAccent);
    ImColor colorDim = Format::C(t.themeDim);
    ImColor colorMuted = Format::C(t.themeMuted);
    ImColor colorWin = Format::C(t.themeWin);
    ImColor colorLoss = Format::C(t.themeLoss);
    ImColor colorGraphLine = Format::C(t.themeGraphLine);
    ImColor colorGraphBaseline = Format::C(t.themeGraphBaseline);

    auto [currentMmr, initialMmr, delta] = RenderHelper::ComputeMmrDelta(*ctx.snap, playlist);

    std::string displayPlaylist = ctx.snap->showLifetimeGraph ? "LIFETIME \xC2\xB7 " + playlistUpper : playlistUpper;

    std::string titleStr = std::string("MMR \xC2\xB7 ") + displayPlaylist;
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeText), "%s", titleStr.c_str());
    ImGui::PopFont();

    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;

    float baseComboW_graph = 120.0f * ctx.dpiScale;
    float checkboxLabelW = ImGui::CalcTextSize("Lifetime").x;
    float checkboxBoxW = ImGui::GetFrameHeight();
    float baseCheckboxTotal = checkboxBoxW + 6.0f * ctx.dpiScale + checkboxLabelW;
    float badgeReserve = 44.0f * ctx.dpiScale;
    float gap = 8.0f * ctx.dpiScale;

    float controlsWanted = baseComboW_graph + gap + baseCheckboxTotal;
    float maxAvailable = avail - badgeReserve - gap;
    float scale = 1.0f;
    const float minScale = 0.5f;
    if (controlsWanted > maxAvailable && maxAvailable > 0.0f) {
        scale = (std::max)(minScale, maxAvailable / controlsWanted);
    }

    bool pushedSmallGraph = (scale < 1.0f - 1e-6f);
    if (pushedSmallGraph) {
        ImGui::PushFont(ctx.fontSmall);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * ctx.dpiScale * scale, 2.0f * ctx.dpiScale * scale));
    }

    float comboW_graph = baseComboW_graph * scale;
    float checkboxTotal = baseCheckboxTotal * scale;

    float controlsTotal = comboW_graph + gap + checkboxTotal;
    float posX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - (badgeReserve + controlsTotal);
    float minXAllowed = ImGui::GetCursorPosX() + 8.0f * ctx.dpiScale;
    if (posX < minXAllowed) posX = minXAllowed;

    ImGui::SetCursorPosX(posX);
    ImGui::SetNextItemWidth(comboW_graph);

    std::vector<std::pair<const char*, MmrCategory>> graphCategories = {
        {"1v1", MmrCategory::OneVOne}, {"2v2", MmrCategory::TwoVTwo}, {"3v3", MmrCategory::ThreeVThree}, {"Casual", MmrCategory::Casual}, {"Tournament", MmrCategory::Tourny}};
    if (ctx.config.show_extra_playlists) {
        graphCategories.insert(graphCategories.begin() + 3, {{"Hoops", MmrCategory::Hoops}, {"Rumble", MmrCategory::Rumble}, {"Dropshot", MmrCategory::Dropshot}, {"Snow Day", MmrCategory::SnowDay}});
    }
    std::vector<const char*> graphLabels;
    for (const auto& category : graphCategories)
        graphLabels.push_back(category.first);
    int currentGraphCat = 1;
    MmrCategory loadedGraphCat = ctx.state.ui.graphMmrCategory.load();
    for (int i = 0; i < static_cast<int>(graphCategories.size()); ++i) {
        if (graphCategories[i].second == loadedGraphCat) {
            currentGraphCat = i;
            break;
        }
    }

    if (ImGui::Combo("##GraphCatCombo", &currentGraphCat, graphLabels.data(), static_cast<int>(graphLabels.size()))) {
        MmrCategory selectedGraphCat = graphCategories[currentGraphCat].second;
        ctx.state.ui.graphMmrCategory.store(selectedGraphCat);
        if (ctx.state.history.showLifetimeGraph.load() && ctx.db && !ctx.snap->myPrimaryId.empty()) {
            ctx.db->AsyncGetLifetimeMmrHistory(ctx.snap->myPrimaryId, MmrCategoryToString(selectedGraphCat));
        }
    }

    ImGui::SameLine(0.0f, gap);
    bool showLifetime = ctx.snap->showLifetimeGraph;
    if (ImGui::Checkbox("Lifetime##Dash", &showLifetime)) {
        ctx.state.history.showLifetimeGraph = showLifetime;
    }

    if (pushedSmallGraph) {
        ImGui::PopStyleVar();
        ImGui::PopFont();
    }

    ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
    ImVec2 badgePos = ImVec2(cursorScreen.x + ImGui::GetContentRegionAvail().x - 44.0f * ctx.dpiScale, cursorScreen.y - 24.0f * ctx.dpiScale);
    Widgets::RenderMmrDeltaBadge(badgePos, delta, colorWin, colorLoss, colorMuted, ctx.fontSmallBold);

    const auto& history = ctx.snap->showLifetimeGraph ? ctx.snap->lifetimeMmrY : (ctx.snap->playlistHistoryY.count(playlist) ? ctx.snap->playlistHistoryY.at(playlist) : std::vector<float>{});
    if (history.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 25.0f));
        ImGui::PushFont(ctx.fontRegular);
        if (ctx.snap->showLifetimeGraph) {
            ImGui::TextColored(Format::C(ctx.config.themeDim), "No lifetime MMR history in database yet.");
            ImGui::TextColored(Format::C(ctx.config.themeMuted), "Play matches to populate database records!");
        } else {
            ImGui::TextColored(Format::C(ctx.config.themeDim), "No MMR data for %s this session.", playlistUpper.c_str());
            ImGui::TextColored(Format::C(ctx.config.themeMuted), "Play a match to start plotting!");
        }
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0.0f, 105.0f));
    } else {
        Widgets::MmrGraphParams params{
            .history = history,
            .currentMmr = currentMmr,
            .initialMmr = initialMmr,
            .plotHeight = 180.0f,
            .colorWin = colorWin,
            .colorLoss = colorLoss,
            .colorMuted = colorMuted,
            .colorDim = colorDim,
            .colorText = colorText,
            .colorGraphLine = colorGraphLine,
            .colorGraphBaseline = colorGraphBaseline,
            .fontSmall = ctx.fontSmall,
            .fontSmallBold = ctx.fontSmallBold,
            .fontBold = ctx.fontBold};
        Widgets::RenderMmrGraph(params);
    }
}

void DashboardPanel::RenderStreaksStatsWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "STREAKS & STATS");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (funcs.streaksStatsTable) {
        std::string tableId = std::string("DashboardStreakStats_") + idSuffix;
        funcs.streaksStatsTable(tableId.c_str());
    }
}

void DashboardPanel::RenderGamemodeBreakdownWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "GAMEMODE BREAKDOWN");
    ImGui::PopFont();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f * ctx.dpiScale);
    ImGui::SetNextItemWidth(120.0f * ctx.dpiScale);
    const char* scopes[] = {"Current Session", "All-Time"};
    GamemodeBreakdownScope currentScope = ScopeFromConfigString(ctx.config.gamemode_breakdown_scope);
    int currentScopeIdx = (currentScope == GamemodeBreakdownScope::AllTime) ? 1 : 0;

    ImGui::PushFont(ctx.fontSmall);
    if (ImGui::Combo("##GamemodeScopeCombo", &currentScopeIdx, scopes, 2)) {
        std::string newScopeStr = (currentScopeIdx == 1) ? "all_time" : "current_session";
        Config::Update([&newScopeStr](ConfigData& c) {
            c.gamemode_breakdown_scope = newScopeStr;
        });
    }
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 4));

    if (funcs.gamemodeBreakdownTable) {
        std::string tableId = std::string("DashboardGamemodeStats_") + idSuffix;
        funcs.gamemodeBreakdownTable(tableId.c_str(), ScopeFromConfigString(ctx.config.gamemode_breakdown_scope));
    }
}

void DashboardPanel::RenderLobbyRanksWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "LOBBY RANKS");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (funcs.lobbyRanksTable) {
        std::string tableId = std::string("DashboardLobbyRanks_") + idSuffix;
        funcs.lobbyRanksTable(tableId.c_str());
    }
}

void DashboardPanel::RenderDemoTrackerWidget(const char* idSuffix) {
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextColored(Format::C(ctx.config.themeAccent), "DEMOLITION TRACKER");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));

    if (funcs.demoTrackerTable) {
        std::string tableId = std::string("DashboardDemoTracker_") + idSuffix;
        funcs.demoTrackerTable(tableId.c_str());
    }
}
