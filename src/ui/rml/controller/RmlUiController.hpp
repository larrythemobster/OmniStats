#pragma once

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>
#include <windows.h>
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <functional>
#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <span>
#include <thread>

#include "core/config/Config.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "core/state/SessionState.hpp"
#include "core/game/RankProgress.hpp"
#include "core/game/StatsScope.hpp"
#include "ui/rml/backend/RmlFileInterface.hpp"
#include "ui/rml/backend/RmlRenderInterfaceD3D11.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "ui/rml/controller/RmlLiveModel.hpp"
#include "ui/rml/views/InsightsView.hpp"
#include "ui/rml/views/HistoryView.hpp"
#include "ui/rml/views/OnboardingView.hpp"
#include "ui/rml/views/AccountsView.hpp"
#include "ui/rml/views/PlayerNoteView.hpp"
#include "ui/rml/backend/RmlSystemInterfaceWin32.hpp"

class DatabaseManager;
struct DbMergeResult;
class AccountManagerService;
namespace Rml {
    class Context;
    class Element;
    class ElementDocument;
    class ElementInstancer;
    class Event;
    class StyleSheetContainer;
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
    float lastGoalSpeed = 0.0f;
    float lastGoalSpeedSelf = 0.0f;
    SessionTotals sessionTotals;
    std::unordered_map<std::string, PlayerData> roster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    int matchSummaryScore[2] = {0, 0};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;
    float matchSummaryOvertimeSeconds = 0.0f;

    float initialMmr = -1.0f;
    std::map<std::string, int> playlistInitialMmr;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, std::vector<bool>> playlistHistoryEstimated;
    std::vector<int64_t> lifetimeMmrX;
    std::vector<float> lifetimeMmrY;
    std::vector<SessionMatchSummary> recentSavedMatches;
    bool recentSavedMatchesLoaded = false;
    bool showLifetimeGraph = false;

    // Identity key -> single-line preview, refreshed when SessionState::notes changes.
    std::unordered_map<std::string, std::string> notePreviews;
    std::unordered_map<std::string, std::vector<std::string>> noteTags;
    bool notesLoaded = false;
    uint64_t notesVersion = 0;
};

class RmlUiController final : public Rml::EventListener {
    friend class RmlUiControllerStateTest;
    friend class AccountsViewTest;

  public:
    RmlUiController(std::shared_ptr<SessionState> state,
                    std::shared_ptr<DatabaseManager> dbManager,
                    std::shared_ptr<AccountManagerService> accounts = nullptr);
    ~RmlUiController() override;

    bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, float dpiScale);
    void Shutdown();
    void Resize(int width, int height, float dpiScale);
    void SetDpiScale(float dpiScale);
    bool ProcessWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool ApplyMouseCursor();
    void Update(const ConfigData& config, bool configChanged = true, uint64_t configRevision = 0);
    void Render();
    bool ShouldRender() const;
    void RequestRender();
    bool HasPendingInteractionUpdate() const noexcept {
        return m_interactionUpdatePending;
    }
    std::optional<int64_t> NextVisibilityWakeMs() const noexcept {
        return m_nextVisibilityWakeMs;
    }
    bool WantsInteraction() const;
    // Insights and onboarding are modal documents that must be seen even when
    // the overlay would otherwise hide (Rocket League not focused or closed).
    bool WantsAttention() const;
    bool IsOnboardingVisible() const noexcept {
        return m_onboardingVisible;
    }
    void OpenInsights();
    void OpenHistory();
    void OpenAccounts();

    void ProcessEvent(Rml::Event& event) override;
    static const char* DemoKdClass(int demos, int demoed);
    static std::string FormatDemoKd(int demos, int demoed);
    static std::string CssColor(const ColorRGBA& color);

  private:
    enum class SettingsPage {
        Display,
        Reminders,
        Appearance,
        Controls,
        Accounts,
        DataSupport,
        Count
    };

    enum class BindCaptureTarget {
        None,
        KeyOverlay,
        KeyCycle,
        KeyExpand,
        KeySession,
        KeyMenu,
        KeySaveReplay,
        KeyGraphPanLeft,
        KeyGraphPanRight,
        KeyLifetimeGraph,
        GamepadOverlay,
        GamepadCycle,
        GamepadExpand,
        GamepadSession,
        GamepadMenu,
        GamepadGraphPanLeft,
        GamepadGraphPanRight
    };

    enum class DragKind { None,
                          DashboardWidget,
                          OverlayMove,
                          OverlayResize,
                          OverlayWidget,
                          OverlayToolboxWidget,
                          SettingsMove,
                          FloatingCard,
                          ColorField,
                          ColorHue };

    struct DragSnapRect {
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
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

    void SnapshotState();
    bool LoadBundledFonts();
    // Rml family name to render `choice` with: bundled families as-is, installed
    // families loaded once under a private alias, or `defaultFamily` when the
    // choice is empty or cannot be loaded.
    std::string ResolveFontFamily(const std::string& choice, std::string_view defaultFamily);
    // font-family overrides for the configured roles; empty when all are default.
    std::string FontOverrideCss();
    const std::vector<std::string>& SystemFontFamilies();
    bool FontsDifferFromApplied(const ConfigData& config) const;
    void RefreshAsyncData();
    void UpdateInputCapture();
    void UpdateThemeProperties();
    void CaptureBaseStyleSheet();
    void ReloadUiResources();
    void RebuildVisibleUi(bool force = false, bool configChanged = false);
    void RefreshLiveUi(bool force = false, bool allowStructural = true);
    void SetElementRml(Rml::Element* element, const std::string& rml, bool replayPointer = false);
    void SetElementText(Rml::Element* element, const std::string& text);
    void RebuildOverlay();
    void RebuildDashboard();
    void RebuildSettings();
    void RebuildToast();

    std::string RenderReleaseBanners() const;
    std::string RenderWidget(DashboardLayout::WidgetId id, bool dashboard);
    std::string RenderPlayerRoster(int team, const char* label);
    std::string RenderLiveMatchStats();
    std::string RenderSessionStats(bool compact = false, bool includeStreak = true);
    std::string RenderStreaksStats();
    std::string RenderGamemodeBreakdown(GamemodeBreakdownScope scope);
    struct GraphSeriesData {
        std::string playlist;
        std::span<const float> ratings;
        std::span<const int64_t> times;
        const std::vector<bool>* estimated = nullptr;
        float baseline = -1.0f;
    };

    GraphSeriesData ResolveGraphSeries() const;
    std::string RenderMmrGraph(bool showCategoryBadge, bool showPanControls);
    // Visible-range label and pan buttons; empty while every match fits.
    std::string RenderGraphPanControls() const;
    std::string RenderGraphPanControls(const RmlUiDetail::GraphWindow& window) const;
    // Progress for `mmr` in `playlist`; the player's fetched division is used
    // only when it was reported for that same rating.
    RankProgress::Progress RankProgressFor(const PlayerData* player, const std::string& playlist, int mmr) const;
    std::string RenderRankProgress(const RankProgress::Progress& progress) const;
    // Rank Progress widget for the roster playlist: the bare bar on the overlay,
    // or with a playlist badge and an empty-state line when `card`.
    std::string RenderSelfRankProgress(bool card) const;
    std::string RenderDemoTracker();
    std::string RenderPreviousGames(bool includeHeading = true);
    std::string RenderLobbyRanks();
    bool RankSourceAvailable() const;
    std::string RenderRankSourceHint(bool interactive) const;
    void OpenStatsApiFixIfNeeded();
    void RebuildStatsApiPrompt();
    enum class StatsApiPrompt { Hidden,
                                Asking,
                                Repairing,
                                Repaired,
                                Failed,
                                NotFound };
    void SetStatsApiPrompt(StatsApiPrompt state);
    std::string RenderMatchSummary();
    std::string RenderSessionView();
    std::string FloatingCardClass() const;
    std::string FloatingCardStyle(float x, float y, float widthDp) const;
    std::string RenderOverlayContainer(const OverlayLayout::ContainerConfig& container, bool editMode);
    // Overlay key hints (cycle / expand / session) that close the roster block.
    std::string RenderRosterFooter() const;
    std::string RenderOverlayToolbox(bool editMode);

    std::string RenderSettingsGeneral();
    std::string RenderSettingsCards();
    std::string RenderSettingsReminders();
    std::string RenderSettingsRanks();
    std::string RenderSettingsShortcuts();
    std::string RenderSettingsAppearance();
    std::string RenderSettingsIntegrations();
    std::string RenderSettingsData();
    std::string RenderSettingsTroubleshooting();
    std::vector<RmlUiDetail::SelectOption> IdentityOptions() const;

    void HandleClick(Rml::Element* target);
    void HandleChange(Rml::Element* target, Rml::Event& event);
    void HandleInput(Rml::Element* target);
    void HandleMouseDown(Rml::Element* target, Rml::Event& event);
    void ApplyColorPick(float mouseX, float mouseY);
    void RefreshColorPickPreview(bool updateFieldGradient);
    void RefreshThemeEditorControls(std::string_view preserveHexKey = {});
    void CommitColorPick();
    void HandleMouseMove(Rml::Event& event);
    void UpdateOverlayDockHover(float mouseX, float mouseY, const std::string& excludeId);
    void HandleMouseUp(Rml::Event& event);
    void PanGraph(int direction);
    void BeginBindCapture(BindCaptureTarget target);
    void FinishBindCapture();
    void ClearBind(BindCaptureTarget target);
    void MoveDashboardWidget(DashboardLayout::WidgetId widget, DashboardLayout::Zone zone, int order = -1);
    void DeleteLocalHistory();
    void FinishDatabaseMerge(const DbMergeResult& result);
    bool CheckStatsApi(bool repair, bool showToast = true);
    void BrowseStatsApiPath();
    // Runs `work` off the render thread; the returned callback runs on the render thread during Update().
    bool StartBackgroundTask(std::string startedToast, std::function<std::function<void()>(std::stop_token)> work);
    void PublishBackgroundTaskCompletion();
    void ShowToast(std::string message, bool error = false, bool compact = false);
    void RefreshTournaments(bool force = false);

    static bool ValidateStatsApiPath(std::string input, std::string& normalized, std::string& error);
    static const char* SettingsPageName(SettingsPage page);
    static const char* ZoneName(DashboardLayout::Zone zone);
    static std::string WidgetDomId(DashboardLayout::WidgetId id);
    static std::string PlaylistImageForName(const std::string& playlist);
    std::string RenderRankBadge(const std::string& tier, bool fetched, const std::string& tooltip = {}) const;
    const std::string* NotePreviewFor(const std::string& primaryId) const;
    // SCOUT badge (with preview tooltip) when a note or tag exists, otherwise a hover-only
    // "Scout"; nothing for players without a stable account identity.
    std::string RenderNoteControl(const std::string& primaryId, const std::string& name) const;
    // Up to two private tag chips plus a "+N" overflow chip for a roster row.
    std::string RenderTagChips(const std::string& primaryId) const;
    // The roster's "best" rank: the reported tier, else the tier implied by the player's best MMR.
    std::string BestRankTier(const PlayerData& player) const;

    Rml::Element* Root(const char* id) const;
    void SetRootRml(const char* id, const std::string& rml);
    std::string ControlValue(Rml::Element* target) const;
    bool EventChecked(Rml::Event& event, Rml::Element* target) const;

    // Insights and onboarding documents (RmlUiViews.cpp).
    void LoadViewDocuments();
    void CloseViewDocuments();
    void ShowInsights(InsightsView::Tab tab, bool endedSession);
    void HideInsights();
    void RefreshInsights(bool force);
    void ShowHistory(int64_t detailMatchId = 0, const std::string& detailMatchGuid = "");
    void HideHistory();
    void RefreshHistory(bool force);
    void ShowAccounts();
    void HideAccounts();
    void RefreshAccounts(bool force);
    void RefreshAccountRow(const std::string& primaryId);
    void ShowPlayerNote(const std::string& primaryId, const std::string& name, const std::string& context);
    void HidePlayerNote();
    // Closes immediately, or asks to confirm when the draft has unsaved edits.
    void RequestClosePlayerNote();
    void RefreshPlayerNote();
    void SnapshotNotes();
    void TriggerHistoryQuery(bool append);
    void OpenHistoryDetail(int64_t matchId, const std::string& matchGuid = "");
    void ExportRecapPng();
    void ShowOnboarding();
    void FinishOnboarding();
    void RefreshOnboarding();
    void CheckAccountLinkPrompt();
    bool HandleViewAction(const std::string& action, Rml::Element* target);

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<AccountManagerService> m_accountsService;
    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    // ui_scale currently pushed into the RmlUi context.
    float m_appliedUiScale = 1.0f;
    int m_width = 1;
    int m_height = 1;

    RmlFileInterface m_fileInterface;
    RmlSystemInterfaceWin32 m_systemInterface;
    RmlRenderInterfaceD3D11 m_renderInterface;
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_document = nullptr;
    Rml::SharedPtr<Rml::StyleSheetContainer> m_baseStyleSheet;
    bool m_rmlInitialized = false;
    // Faces loaded from disk must outlive Rml::Shutdown; RCDATA-backed faces
    // point into the module image and need no storage.
    std::vector<std::vector<unsigned char>> m_fontBlobs;
    // Installed family name -> registered Rml alias; empty when loading failed.
    std::map<std::string, std::string> m_systemFontAliases;
    std::vector<std::string> m_systemFontFamilies;
    bool m_systemFontFamiliesLoaded = false;
    // Font choices behind the stylesheet currently applied to the documents.
    std::string m_appliedFontUi;
    std::string m_appliedFontMono;
    std::string m_appliedFontDisplay;
    bool m_rmlInterfacesInstalled = false;
    std::unique_ptr<Rml::ElementInstancer> m_graphLineInstancer;
    ID3D11DeviceContext* m_d3dContext = nullptr;
    InsightsView m_insights;
    OnboardingView m_onboarding;
    HistoryView m_history;
    Rml::ElementDocument* m_insightsDoc = nullptr;
    Rml::ElementDocument* m_onboardingDoc = nullptr;
    Rml::ElementDocument* m_historyDoc = nullptr;
    AccountsView m_accounts;
    Rml::ElementDocument* m_accountsDoc = nullptr;
    bool m_accountsVisible = false;
    PlayerNoteView m_playerNote;
    Rml::ElementDocument* m_playerNoteDoc = nullptr;
    bool m_playerNoteVisible = false;
    uint64_t m_nextPlayerNoteRequestId = 0;
    uint64_t m_nextPlayerTagRequestId = 0;
    uint64_t m_nextScoutRequestId = 0;
    uint64_t m_lastScoutViewVersion = std::numeric_limits<uint64_t>::max();
    bool m_playerNotesRequested = false;
    uint64_t m_lastViewNotesVersion = std::numeric_limits<uint64_t>::max();
    uint64_t m_lastAccountsVersion = std::numeric_limits<uint64_t>::max();
    std::chrono::steady_clock::time_point m_lastAccountsRelativeRender{};
    std::string m_lastAccountsActive;
    bool m_insightsVisible = false;
    bool m_onboardingVisible = false;
    bool m_historyVisible = false;
    // True while the recap tab shows a session captured when it ended rather
    // than the live one.
    bool m_insightsShowsEndedSession = false;
    uint64_t m_lastInsightsVersion = std::numeric_limits<uint64_t>::max();
    uint64_t m_lastRecapGameVersion = std::numeric_limits<uint64_t>::max();
    uint64_t m_lastHistoryViewVersion = std::numeric_limits<uint64_t>::max();
    std::string m_lastOnboardingIdentityRml;
    bool m_recapCapturePending = false;

    ConfigData m_config;
    RmlRenderSnapshot m_snap;
    uint64_t m_lastGameVersion = 0;
    uint64_t m_lastHistoryVersion = 0;
    uint64_t m_lastRenderedGameVersion = 0;
    uint64_t m_lastLeafGameVersion = 0;
    uint64_t m_lastRenderedHistoryVersion = 0;
    uint64_t m_lastRenderedDbStatsVersion = 0;
    uint64_t m_lastAccountStatusVersion = 0;
    bool m_lastShowMenu = false;
    bool m_lastShowOverlay = false;
    bool m_lastShowSessionView = false;
    bool m_lastShowMatchSummary = false;
    bool m_lastSecondMonitor = false;
    bool m_lastDashboardEditMode = false;
    bool m_lastShowGraphView = false;
    bool m_lastH2hExpanded = false;
    bool m_lastShowLifetimeGraph = false;
    bool m_lastInMatch = false;
    int m_lastGraphOffset = 0;
    MmrCategory m_lastRosterMmrCategory = MmrCategory::Best;
    MmrCategory m_lastGraphMmrCategory = MmrCategory::Best;
    std::string m_lastConfigFingerprint;
    uint64_t m_lastConfigHash = 0;
    uint64_t m_lastRuntimeStructuralHash = 0;
    std::string m_lastRenderConfigFingerprint;
    std::string m_lastSettingsFingerprint;
    uint64_t m_lastSettingsHash = 0;
    uint64_t m_lastSettingsRosterHash = 0;
    bool m_hasSettingsRosterHash = false;
    uint64_t m_lastSettingsRosterGameVersion = std::numeric_limits<uint64_t>::max();
    SettingsPage m_lastSettingsRosterPage = SettingsPage::Display;
    std::unordered_map<std::string, std::string> m_lastLiveWidgetRml;
    RmlLiveModel m_liveModel;
    bool m_liveDomNeedsPrime = true;
    uint64_t m_lastRosterStructureHash = 0;
    bool m_hasRosterStructureHash = false;
    uint64_t m_lastRenderedNotesVersion = 0;
    bool m_lastLiveMatchHadDemoedRow = false;
    bool m_lastLiveMatchHadOwnGoalsRow = false;
    std::string m_lastSessionViewRml;
    uint64_t m_lastGamemodeBreakdownHash = 0;
    bool m_hasGamemodeBreakdownHash = false;

    std::string m_lastDbFetchPrimaryId;
    std::string m_lastLifetimeHistoryPrimaryId;
    MmrCategory m_lastLifetimeHistoryCategory = MmrCategory::Best;
    std::string m_lastRecentMatchHistoryPrimaryId;
    int m_lastRecentMatchHistoryLimit = 0;

    bool m_detailedStatsSummaryRequested = false;
    SettingsPage m_settingsPage = SettingsPage::Display;
    BindCaptureTarget m_bindCaptureTarget = BindCaptureTarget::None;
    bool m_showBallchasingToken = false;
    std::string m_editColorKey;
    // Hue is tracked separately from the edited RGBA: dragging saturation or
    // value through gray would otherwise lose the hue the user picked.
    float m_editColorHue = 0.0f;
    bool m_colorPickDirty = false;
    // The SV texture only needs to change when the visible hue bucket changes.
    // Avoid rebinding/regenerating a procedural texture for every mouse pixel.
    int m_colorPickerGradientHue = -1;
    bool m_showCardDetails = false;
    bool m_showAdvancedAppearance = false;
    bool m_showRankDetails = false;
    bool m_showTroubleshooting = false;
    bool m_confirmReplayUploads = false;
    bool m_confirmDeleteHistory = false;
    bool m_showUpdatePrompt = false;
    bool m_accountPromptPending = false;
    StatsApiPrompt m_statsApiPrompt = StatsApiPrompt::Hidden;
    DragState m_drag;
    bool m_pointerPressed = false;
    std::string m_pendingBallchasingToken;
    std::string m_statsApiPathError;
    std::string m_statusMessage;
    bool m_statusError = false;
    int64_t m_statusUntilMs = 0;
    bool m_statusCompact = false;
    uint64_t m_tournamentVersion = 0;
    int64_t m_tournamentMinute = -1;
    Tournaments::ReminderTracker m_tournamentReminder;
    bool m_rebuildingUi = false;
    bool m_renderDirty = true;
    bool m_interactionUpdatePending = false;
    std::chrono::steady_clock::time_point m_nextRmlUpdateAt = std::chrono::steady_clock::time_point::max();
    std::optional<int64_t> m_nextVisibilityWakeMs;
    bool m_deferredSelectRebuild = false;
    uint64_t m_lastLocalConfigRevision = 0;

    // Replayed after an overlay DOM rebuild so RmlUi immediately resolves the
    // new hover chain instead of briefly falling back to the arrow cursor.
    bool m_hasPointerPosition = false;
    int m_lastPointerX = 0;
    int m_lastPointerY = 0;

    // Settings is a movable native-style panel. Coordinates are kept in context
    // pixels so dragging remains stable regardless of RmlUi dp scaling.
    bool m_settingsPositioned = false;
    float m_settingsX = 0.0f;
    float m_settingsY = 0.0f;
    int64_t m_nextUpdateCheckPollMs = 0;

    std::mutex m_backgroundTaskMutex;
    std::function<void()> m_backgroundTaskCompletion;
    std::atomic<bool> m_backgroundTaskRunning{false};
    // Declared last so it stops and joins before the members it writes are destroyed.
    std::jthread m_backgroundTask;
};
