#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include "imgui.h"
#include "ui/RenderContext.hpp"
#include "core/StatsScope.hpp"
#include "core/DashboardLayoutConfig.hpp"

struct DashRenderFuncs {
    std::function<void(int teamNum, const char* label, ImColor color, const char* tableId)> playerRoster;
    std::function<void(const char* tableIdPrefix)> liveMatchStats;
    std::function<void(const char* tableId)> streaksStatsTable;
    std::function<void(const char* tableId, GamemodeBreakdownScope scope)> gamemodeBreakdownTable;
    std::function<void(const char* tableId, bool isDashboard, bool showStreak)> sessionStatsTable;
    std::function<void(const char* tableId)> lobbyRanksTable;
    std::function<void(const char* tableId)> demoTrackerTable;
};

class DashboardPanel {
public:
    explicit DashboardPanel(RenderContext ctx, HWND hwnd, DashRenderFuncs funcs);
    ~DashboardPanel();
    void Render();
private:
    void RenderCustomTitleBar();
    
    void RenderDashboardZones(const DashboardLayout::LayoutConfig& layout);
    void RenderWidget(DashboardLayout::WidgetId id, const char* idSuffix);
    
    void RenderLiveRosterWidget(const char* idSuffix);
    void RenderLiveMatchStatsWidget(const char* idSuffix);
    void RenderSessionStatsWidget(const char* idSuffix);
    void RenderMmrGraphWidget(const char* idSuffix);
    void RenderStreaksStatsWidget(const char* idSuffix);
    void RenderGamemodeBreakdownWidget(const char* idSuffix);
    void RenderLobbyRanksWidget(const char* idSuffix);
    void RenderDemoTrackerWidget(const char* idSuffix);
    void RenderPreviousGamesWidget(const char* idSuffix);

    RenderContext ctx;
    HWND m_hwnd;
    DashRenderFuncs funcs;
    int m_draggedDashboardWidget = -1;
};
