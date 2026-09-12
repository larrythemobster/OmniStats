#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>
#include <windows.h>
#include <d3d11.h>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include "core/StatsScope.hpp"
#include "ui/rml/RmlFileInterface.hpp"
#include "ui/rml/RmlRenderInterfaceD3D11.hpp"
#include "ui/rml/RmlSystemInterfaceWin32.hpp"

class DatabaseManager;
namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class ElementInstancer;
    class Event;
}

struct RmlRenderSnapshot {
    std::string matchGuid;
    std::string arenaName;
    int score[2] = {0, 0};
    bool inMatch = false;
    bool inReplay = false;
    bool matchFinalized = false;
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

    float initialMmr = -1.0f;
    std::vector<float> mmrHistoryX;
    std::vector<float> mmrHistoryY;
    std::map<std::string, int> playlistInitialMmr;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, std::vector<bool>> playlistHistoryEstimated;
    std::vector<float> lifetimeMmrX;
    std::vector<float> lifetimeMmrY;
    std::vector<SessionMatchSummary> recentSavedMatches;
    bool recentSavedMatchesLoaded = false;
    bool showLifetimeGraph = false;
};

class RmlUiController final : public Rml::EventListener {
    friend class RmlUiControllerStateTest;

  public:
    RmlUiController(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager);
    ~RmlUiController() override;

    bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, float dpiScale);
    void Shutdown();
    void Resize(int width, int height, float dpiScale);
    void SetDpiScale(float dpiScale);
    bool ProcessWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ApplyMouseCursor();
    void Update(const ConfigData& config);
    void Render();
    bool WantsInteraction() const;

    void ProcessEvent(Rml::Event& event) override;

  private:
    enum class SettingsPage {
        General,
        Cards,
        Ranks,
        Shortcuts,
        Appearance,
        Integrations,
        Data,
        Troubleshooting
    };

    enum class BindCaptureTarget {
        None,
        KeyOverlay,
        KeyCycle,
        KeyExpand,
        KeySession,
        KeyMenu,
        KeySaveReplay,
        GamepadOverlay,
        GamepadCycle,
        GamepadExpand,
        GamepadSession,
        GamepadMenu
    };

    enum class DragKind { None,
                          DashboardWidget,
                          OverlayMove,
                          OverlayResize,
                          OverlayWidget,
                          OverlayToolboxWidget,
                          SettingsMove };

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
    };

    void SnapshotState();
    void RefreshAsyncData();
    void UpdateInputCapture();
    void UpdateThemeProperties();
    void RebuildVisibleUi(bool force = false);
    void RebuildOverlay();
    void RebuildDashboard();
    void RebuildSettings();
    void RebuildToast();

    std::string RenderWidget(DashboardLayout::WidgetId id, bool dashboard);
    std::string RenderPlayerRoster(int team, const char* label);
    std::string RenderLiveMatchStats();
    std::string RenderSessionStats(bool compact = false, bool includeStreak = true);
    std::string RenderStreaksStats();
    std::string RenderGamemodeBreakdown(GamemodeBreakdownScope scope);
    std::string RenderMmrGraph();
    std::string RenderLobbyRanks();
    std::string RenderDemoTracker();
    std::string RenderPreviousGames();
    std::string RenderMatchSummary();
    std::string RenderSessionView();
    std::string RenderOverlayContainer(const OverlayLayout::ContainerConfig& container, bool editMode);
    std::string RenderOverlayToolbox(bool editMode);

    std::string RenderSettingsGeneral();
    std::string RenderSettingsCards();
    std::string RenderSettingsRanks();
    std::string RenderSettingsShortcuts();
    std::string RenderSettingsAppearance();
    std::string RenderSettingsIntegrations();
    std::string RenderSettingsData();
    std::string RenderSettingsTroubleshooting();

    void HandleClick(Rml::Element* target);
    void HandleChange(Rml::Element* target, Rml::Event& event);
    void HandleInput(Rml::Element* target);
    void HandleMouseDown(Rml::Element* target, Rml::Event& event);
    void HandleMouseMove(Rml::Event& event);
    void HandleMouseUp(Rml::Event& event);
    void BeginBindCapture(BindCaptureTarget target);
    void FinishBindCapture();
    void ClearBind(BindCaptureTarget target);
    void MoveDashboardWidget(DashboardLayout::WidgetId widget, DashboardLayout::Zone zone, int order = -1);
    void DeleteLocalHistory();
    void CheckStatsApi(bool repair);
    void ShowToast(std::string message, bool error = false);

    static std::string Escape(std::string_view text);
    static std::string CssColor(const ColorRGBA& color);
    static std::string BoolAttr(bool value);
    static std::string Checked(bool value);
    static std::string Selected(bool value);
    static std::string FormatNumber(float value, int precision = 0);
    static std::string FormatRecord(int wins, int losses);
    static std::string FormatClock(int64_t unixSeconds);
    static std::string FormatDemoKd(int demos, int demoed);
    static bool ValidateStatsApiPath(std::string input, std::string& normalized, std::string& error);
    static const char* SettingsPageName(SettingsPage page);
    static const char* ZoneName(DashboardLayout::Zone zone);
    static std::string WidgetDomId(DashboardLayout::WidgetId id);
    static std::string PlaylistImageForName(const std::string& playlist);
    std::string RenderRankBadge(const std::string& tier, bool fetched, const std::string& tooltip = {}) const;

    Rml::Element* Root(const char* id) const;
    void SetRootRml(const char* id, const std::string& rml);
    std::string ControlValue(Rml::Element* target) const;
    bool EventChecked(Rml::Event& event, Rml::Element* target) const;

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    int m_width = 1;
    int m_height = 1;

    RmlFileInterface m_fileInterface;
    RmlSystemInterfaceWin32 m_systemInterface;
    RmlRenderInterfaceD3D11 m_renderInterface;
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_document = nullptr;
    bool m_rmlInitialized = false;
    bool m_rmlInterfacesInstalled = false;
    std::unique_ptr<Rml::ElementInstancer> m_graphLineInstancer;

    ConfigData m_config;
    RmlRenderSnapshot m_snap;
    uint64_t m_lastGameVersion = 0;
    uint64_t m_lastHistoryVersion = 0;
    uint64_t m_lastRenderedGameVersion = 0;
    uint64_t m_lastRenderedHistoryVersion = 0;
    uint64_t m_lastRenderedDbStatsVersion = 0;
    bool m_lastShowMenu = false;
    bool m_lastShowOverlay = false;
    bool m_lastShowSessionView = false;
    bool m_lastShowMatchSummary = false;
    bool m_lastSecondMonitor = false;
    bool m_lastDashboardEditMode = false;
    bool m_lastShowGraphView = false;
    bool m_lastH2hExpanded = false;
    bool m_lastShowLifetimeGraph = false;
    MmrCategory m_lastRosterMmrCategory = MmrCategory::Best;
    MmrCategory m_lastGraphMmrCategory = MmrCategory::Best;
    std::string m_lastConfigFingerprint;
    std::string m_lastSettingsFingerprint;

    std::string m_lastDbFetchPrimaryId;
    std::string m_lastLifetimeHistoryPrimaryId;
    MmrCategory m_lastLifetimeHistoryCategory = MmrCategory::Best;
    std::string m_lastRecentMatchHistoryPrimaryId;
    int m_lastRecentMatchHistoryLimit = 0;

    SettingsPage m_settingsPage = SettingsPage::General;
    BindCaptureTarget m_bindCaptureTarget = BindCaptureTarget::None;
    bool m_showBallchasingToken = false;
    std::string m_editColorKey;
    bool m_confirmReplayUploads = false;
    bool m_confirmDeleteHistory = false;
    bool m_showUpdatePrompt = false;
    DragState m_drag;
    std::string m_pendingBallchasingToken;
    std::string m_statsApiPathError;
    std::string m_statusMessage;
    bool m_statusError = false;
    int64_t m_statusUntilMs = 0;
    bool m_rebuildingUi = false;

    // Settings is a movable native-style panel. Coordinates are kept in context
    // pixels so dragging remains stable regardless of RmlUi dp scaling.
    bool m_settingsPositioned = false;
    float m_settingsX = 0.0f;
    float m_settingsY = 0.0f;
};
