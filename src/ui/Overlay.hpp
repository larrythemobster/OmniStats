#pragma once
#include <windows.h>
#include <d3d11.h>
#include <memory>
#include <string>
#include <thread>
#include "core/SessionState.hpp"
#include "core/StatsScope.hpp"
#include "ui/RenderContext.hpp"
#include "ui/TrayIcon.hpp"
#include "ui/D3D11Device.hpp"
#include "ui/OverlayWindow.hpp"
#include "ui/panels/SettingsPanel.hpp"
#include "ui/panels/DashboardPanel.hpp"
#include "ui/OverlayLayoutManager.hpp"

#include "imgui.h"
#include "core/Config.hpp"
#include <vector>
#include <unordered_map>

class DatabaseManager;
class RankIconAssets;

struct RenderSnapshot {
    // Game state
    std::string matchGuid;
    std::string arenaName;
    int score[2] = {0, 0};
    bool inMatch = false;
    bool inReplay = false;
    int maxPlayersSeen = 0;
    std::string myPrimaryId;
    int myTeam = -1;
    MatchStats currentMatch;
    SessionTotals sessionTotals;
    std::unordered_map<std::string, PlayerData> roster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    int matchSummaryScore[2] = {0, 0};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;

    // History state
    float initialMmr = -1.0f;
    std::vector<float> mmrHistoryX;
    std::vector<float> mmrHistoryY;
    std::map<std::string, int> playlistInitialMmr;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::vector<float> lifetimeMmrX;
    std::vector<float> lifetimeMmrY;
    std::vector<SessionMatchSummary> recentSavedMatches;
    bool recentSavedMatchesLoaded = false;
    bool showLifetimeGraph = false;
};

enum class RecordFormat {
    Full,
    Abbreviated,
    Short
};

class Overlay {
public:
    friend LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    friend class HeadlessOverlayTest;
    Overlay(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager = nullptr);
    ~Overlay();

    bool Initialize();
    void RunLoop();
    void Shutdown();
    void UpdateWindowStyle() { if (m_window) { ConfigData c = Config::Read(); m_window->UpdateStyle(c.second_monitor_mode, m_state->ui.showMenu); } }
    void ResizeSwapChain(int width, int height);
    void RenderUI();

private:
    void RenderSessionCard();
    void RenderPreviousGamesOverlay();
    void RenderDemoTrackerOverlay();
    void RenderMatchSummary();
    void RenderSessionView();
    void ApplyTheme();
    bool LoadFonts();
    bool RebuildFontsForCurrentScale();
    void RefreshPanelContexts();
    void UpdateWindowPosition(bool resetSecondMonitorPlacement = true);
    void SaveSecondMonitorWindowBounds();
    void HandleDpiChanged(UINT dpi, const RECT* suggestedRect);
    bool RecreateD3DDevice();
    bool HandleDeviceLost(const char* reason, HRESULT hr);
    RECT GetSecondaryMonitorRect() const { return m_window ? m_window->GetSecondaryMonitorRect() : RECT{}; }

    // Shared state for panels and render helpers
    RenderContext MakeCtx();
    DashRenderFuncs MakeDashFuncs();

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    RenderSnapshot m_snap;
    ConfigData m_frameConfig;
    uint64_t m_lastGameVersion = 0;
    uint64_t m_lastHistoryVersion = 0;

    // Track the last primary id we requested DB updates for to avoid repeated async calls
    std::string m_lastDbFetchPrimaryId;
    std::string m_lastRecentMatchHistoryPrimaryId;
    int m_lastRecentMatchHistoryLimit = 0;

    std::string m_pendingBallchasingToken;
    ImFont* fontRegular = nullptr;
    ImFont* fontBold = nullptr;
    ImFont* fontSmall = nullptr;
    ImFont* fontSmallBold = nullptr;
    ImFont* fontMono = nullptr;
    ImFont* lobbyFontSmall = nullptr;

    // Win32 Window
    std::unique_ptr<OverlayWindow> m_window;
    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    std::string m_imguiIniPath;
    std::unique_ptr<TrayIcon> m_trayIcon;
    float m_loadedFontScale = 0.0f;
    bool m_fontReloadPending = false;
    bool m_rebuildingFonts = false;

    // DirectX 11
    std::unique_ptr<D3D11Device> m_d3d11;
    std::unique_ptr<RankIconAssets> m_rankIcons;
    bool m_imguiDx11Initialized = false;

    // Panels
    std::unique_ptr<SettingsPanel> m_settingsPanel;
    std::unique_ptr<DashboardPanel> m_dashboardPanel;
    std::unique_ptr<OverlayLayout::LayoutManager> m_layoutManager;

    // Shared Reusable UI Modular Renderers (still Overlay:: methods, called via DashRenderFuncs)
    void RenderWidgetContent(DashboardLayout::WidgetId id, const char* suffix);
    void RenderRecord(int winsWith, int lossesWith, int winsAgainst, int lossesAgainst, RecordFormat fmt = RecordFormat::Full) const;
    void RenderStatSection(const std::string& title, const std::string& tableId, const std::vector<std::pair<std::string, std::string>>& rows, bool compact = false);
    void RenderPlayerRoster(int teamNum, const char* label, ImColor color, const char* tableId);
    void RenderLiveMatchStats(const char* tableIdPrefix);
    void RenderStreaksStatsTable(const char* tableId);
    void RenderGamemodeBreakdownTable(const char* tableId, GamemodeBreakdownScope scope);
    void RenderSessionStatsTable(const char* tableId, bool isDashboard, bool showStreak = true);
    void RenderDemoTrackerTable(const char* tableId);
    void RenderLobbyRanksTable(const char* tableId);
    void RenderLobbyRanksOverlay();
};
