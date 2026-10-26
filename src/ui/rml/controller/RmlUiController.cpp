#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/widgets/TournamentWidget.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"
#include <mmsystem.h>
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "core/config/StatsApiIni.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/api/AccountClient.hpp"
#include "network/api/RemoteConfig.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

RmlUiController::RmlUiController(std::shared_ptr<SessionState> state,
                                 std::shared_ptr<DatabaseManager> dbManager,
                                 std::shared_ptr<AccountManagerService> accounts,
                                 std::shared_ptr<RemoteConfig> remoteConfig,
                                 std::shared_ptr<Tournaments::TournamentScheduleService> tournamentService,
                                 std::shared_ptr<TelemetryManager> telemetry)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)), m_accountsService(std::move(accounts)),
      m_remoteConfig(std::move(remoteConfig)), m_tournamentService(std::move(tournamentService)), m_telemetry(std::move(telemetry)),
      m_systemInterface(nullptr),
      m_overlayPresenter(std::make_unique<OverlayPresenter>()),
      m_dashboardPresenter(std::make_unique<DashboardPresenter>()),
      m_settingsPresenter(std::make_unique<SettingsPresenter>()),
      m_historyPresenter(std::make_unique<HistoryPresenter>()),
      m_insightsPresenter(std::make_unique<InsightsPresenter>()),
      m_accountsPresenter(std::make_unique<AccountsPresenter>()),
      m_onboardingPresenter(std::make_unique<OnboardingPresenter>()),
      m_playerNotePresenter(std::make_unique<PlayerNotePresenter>()),
      m_backgroundTaskService(std::make_unique<BackgroundTaskService>()),
      m_inputCaptureService(std::make_unique<InputCaptureService>()),
      m_colorPickerService(std::make_unique<ColorPickerService>()),
      m_fontService(std::make_unique<FontService>()),
      m_interactionService(std::make_unique<InteractionService>()) {
    if (m_state) m_lastShowMenu = m_state->ui.showMenu.load();
}

RmlUiController::~RmlUiController() {
    Shutdown();
}

bool RmlUiController::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, float dpiScale) {
    Shutdown();
    m_hwnd = hwnd;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_dpiScale = SanitizedScale(dpiScale);
    m_systemInterface.SetWindow(hwnd);

    if (!m_renderInterface.Initialize(device, context, m_width, m_height)) return false;
    m_d3dContext = context;
    Rml::SetSystemInterface(&m_systemInterface);
    Rml::SetRenderInterface(&m_renderInterface);
    Rml::SetFileInterface(&m_fileInterface);
    m_rmlInterfacesInstalled = true;
    if (!Rml::Initialise()) {
        Shutdown();
        return false;
    }
    m_rmlInitialized = true;

    m_graphLineInstancer = std::make_unique<Rml::ElementInstancerGeneric<RmlMmrGraphLines>>();
    Rml::Factory::RegisterElementInstancer("mmrgraphlines", m_graphLineInstancer.get());

    m_context = Rml::CreateContext("omnistats", Rml::Vector2i(m_width, m_height));
    if (!m_context) {
        Shutdown();
        return false;
    }
    if (!m_liveModel.Create(m_context, [this](Rml::DataModelConstructor& c) {
            if (m_overlayPresenter) m_overlayPresenter->Bind(c);
        }) ||
        !m_insights.Create(m_context) || !m_history.Create(m_context) || !m_accounts.Create(m_context) || !m_playerNote.Create(m_context) || !m_onboarding.Create(m_context)) {
        Shutdown();
        return false;
    }

    // Resolve `dp` lengths and font sizes against the target monitor from the
    // first document layout instead of loading once at RmlUi's default 1.0
    // density and correcting it afterwards.
    m_config = Config::Read();
    SetDpiScale(m_dpiScale);

    if (!LoadBundledFonts()) {
        Shutdown();
        return false;
    }

    m_fileInterface.SetOverrideDirectory(RmlDevDirectory());
    m_document = m_context->LoadDocument("res://main.rml");
    if (!m_document) {
        Shutdown();
        return false;
    }
    m_document->Show();
    CaptureBaseStyleSheet();
    LoadViewDocuments();

    for (const char* event : {"click", "change", "input", "mousedown", "mousemove", "mouseup"}) {
        m_context->AddEventListener(event, this);
    }
    m_context->AddEventListener("blur", this, true);
    if (m_pendingBallchasingToken.empty()) m_pendingBallchasingToken = m_config.ballchasing_token;
    SnapshotState();
    RefreshAsyncData();
    UpdateThemeProperties();
    if (!m_config.second_monitor_mode) {
        SetRootRml("dashboard-root", "");
    } else {
        SetRootRml("overlay-root", "");
    }
    if (!m_config.onboarding_completed || !Config::HasCurrentConsent(m_config)) {
        ShowOnboarding();
    } else {
        CheckAccountLinkPrompt();
        OpenStatsApiFixIfNeeded();
    }
    RebuildVisibleUi(true);
    return true;
}

void RmlUiController::Shutdown() {
    if (m_context) {
        for (const char* event : {"click", "change", "input", "mousedown", "mousemove", "mouseup"}) {
            m_context->RemoveEventListener(event, this);
        }
        m_context->RemoveEventListener("blur", this, true);
        m_context = nullptr;
        m_document = nullptr;
        m_insightsDoc = nullptr;
        m_onboardingDoc = nullptr;
        m_historyDoc = nullptr;
        m_accountsDoc = nullptr;
        m_playerNoteDoc = nullptr;
        m_insightsVisible = false;
        m_onboardingVisible = false;
        m_historyVisible = false;
        m_accountsVisible = false;
        m_playerNoteVisible = false;
        m_liveModel.Reset();
        m_insights.Reset();
        m_history.Reset();
        m_accounts.Reset();
        m_playerNote.Reset();
        m_onboarding.Reset();
        if (m_overlayPresenter) m_overlayPresenter->Reset();
        if (m_historyPresenter) m_historyPresenter->Shutdown();
        if (m_insightsPresenter) m_insightsPresenter->Shutdown();
        if (m_accountsPresenter) m_accountsPresenter->Shutdown();
        if (m_onboardingPresenter) m_onboardingPresenter->Shutdown();
        if (m_playerNotePresenter) m_playerNotePresenter->Shutdown();
    }
    if (m_backgroundTaskService) {
        m_backgroundTaskService->Shutdown();
    }
    if (m_inputCaptureService && m_state) {
        m_inputCaptureService->FinishCapture(m_state);
    }
    if (m_colorPickerService) {
        m_colorPickerService->Close();
    }
    if (m_rmlInitialized) {
        Rml::Shutdown();
        m_rmlInitialized = false;
    }
    // RmlUi stores these interfaces as global raw pointers. Clear the pointers
    // installed by this controller even if Rml::Initialise() failed before
    // m_rmlInitialized became true. Headless/uninitialized controller instances
    // must not disturb interfaces owned by another live RmlUi host.
    if (m_rmlInterfacesInstalled) {
        Rml::SetFileInterface(nullptr);
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        m_rmlInterfacesInstalled = false;
    }

    // Factory registrations are non-owning; keep the instancer alive through
    // Rml::Shutdown(), then release it after the factory has torn down.
    m_graphLineInstancer.reset();
    m_renderInterface.Shutdown();
    m_d3dContext = nullptr;
    m_systemInterface.SetWindow(nullptr);
    m_hwnd = nullptr;
}

void RmlUiController::Resize(int width, int height, float dpiScale) {
    const bool sizeChanged = m_width != std::max(width, 1) || m_height != std::max(height, 1);
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_renderInterface.SetViewport(m_width, m_height);
    if (m_context) m_context->SetDimensions(Rml::Vector2i(m_width, m_height));
    SetDpiScale(dpiScale);
    // A window-mode switch changes the client size. Re-center Settings for the
    // new size instead of leaving it positioned (and clipped) for the old one.
    if (sizeChanged) {
        m_renderDirty = true;
        m_settingsPositioned = false;
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    }
}

void RmlUiController::SetDpiScale(float dpiScale) {
    const float previous = m_dpiScale;
    m_dpiScale = SanitizedScale(dpiScale);
    m_appliedUiScale = SanitizedUiScale(m_config.ui_scale);
    if (m_context) m_context->SetDensityIndependentPixelRatio(m_dpiScale * m_appliedUiScale);
    if (m_dpiScale != previous) m_renderDirty = true;
}

bool RmlUiController::ProcessWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Losing the button (focus change or capture loss) never delivers `mouseup`,
    // so release the rebuild hold here or live updates would stay frozen.
    if (message == WM_KILLFOCUS || message == WM_CAPTURECHANGED || message == WM_LBUTTONUP) m_pointerPressed = false;
    // Ctrl+Shift+R reloads RML/RCSS from the development resource directory.
    if (message == WM_KEYDOWN && wParam == 'R' && !m_fileInterface.OverrideDirectory().empty() &&
        (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        ReloadUiResources();
        return true;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE && m_playerNoteVisible && !m_onboardingVisible) {
        RequestClosePlayerNote();
        return true;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE && m_insightsVisible && !m_onboardingVisible) {
        HideInsights();
        return true;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE && m_historyVisible && !m_onboardingVisible) {
        if (m_history.IsDetailOpen()) {
            m_history.CloseDetail();
            m_renderDirty = true;
        } else {
            HideHistory();
        }
        return true;
    }
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE && m_accountsVisible && !m_onboardingVisible) {
        if (m_accounts.IsAddOpen()) {
            m_accounts.CloseAdd();
            m_renderDirty = true;
        } else if (m_accounts.IsImportOpen()) {
            m_accounts.CloseImport();
            m_renderDirty = true;
        } else if (m_accounts.IsDetailOpen()) {
            m_accounts.CloseDetail();
            m_renderDirty = true;
        } else {
            HideAccounts();
        }
        return true;
    }
    const bool handled = RmlInputWin32::ProcessWindowMessage(m_context, hwnd, message, wParam, lParam);
    // Hover/focus/scroll state can change RmlUi pseudo-classes without touching
    // application data, so any input consumed by RmlUi requests a new frame.
    if (handled || message == WM_KILLFOCUS || message == WM_SETFOCUS || message == WM_CAPTURECHANGED)
        m_renderDirty = true;
    return handled;
}

bool RmlUiController::ApplyMouseCursor() {
    return m_systemInterface.ApplyMouseCursor();
}

void RmlUiController::Render() {
    if (!m_context) return;
    if (m_nextVisibilityWakeMs.has_value() && SteadyNowMs() >= *m_nextVisibilityWakeMs) {
        RebuildVisibleUi(false, false);
    }
    m_context->Update();
    // GetNextUpdateDelay() is a delay value, not a continuously decreasing
    // timer. Convert it to an absolute deadline immediately after Update().
    // Without this, a finite request such as a caret blink in 0.5 seconds would
    // be observed as "0.5" forever and the context would never be updated
    // again unless some unrelated input/data dirtied the UI.
    const double nextDelay = m_context->GetNextUpdateDelay();
    const auto now = std::chrono::steady_clock::now();
    if (nextDelay <= 0.0) {
        m_nextRmlUpdateAt = now;
    } else if (std::isfinite(nextDelay)) {
        m_nextRmlUpdateAt = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double>(nextDelay));
    } else {
        m_nextRmlUpdateAt = std::chrono::steady_clock::time_point::max();
    }

    // RmlUi emits many small geometry draws. Bind the invariant DX11 pipeline
    // state once per UI frame instead of rebinding shaders/layout/blend/depth
    // for every geometry handle. Per-draw buffers, constants, scissor and
    // texture changes are still applied by the render interface.
    m_renderInterface.BeginFrame();
    m_context->Render();
    m_renderInterface.EndFrame();
    if (m_recapCapturePending) ExportRecapPng();
    m_renderDirty = false;
}

bool RmlUiController::ShouldRender() const {
    if (!m_context) return false;
    // Keep the last presented swap-chain image until application data/input
    // changes or RmlUi's previously scheduled update deadline arrives. This
    // preserves transitions, smooth scrolling and caret blinking without
    // forcing static UI through a full Update/Render/Present every frame.
    if (m_renderDirty) return true;
    if (m_nextVisibilityWakeMs.has_value() && SteadyNowMs() >= *m_nextVisibilityWakeMs) return true;
    return std::chrono::steady_clock::now() >= m_nextRmlUpdateAt;
}

void RmlUiController::RequestRender() {
    m_renderDirty = true;
}

bool RmlUiController::WantsInteraction() const {
    if (!m_state) return false;
    // Preserve the native overlay's click-through contract. In-game UI only
    // becomes interactive while Settings/layout editing is open; otherwise
    // mouse input must continue through to Rocket League. Second-monitor mode
    // is a normal interactive window. RmlUi hover state must not override this.
    return m_state->ui.showMenu.load() || m_state->ui.dashboardLayoutEditMode.load() ||
           m_config.second_monitor_mode || m_drag.kind != DragKind::None || WantsAttention();
}

void RmlUiController::Update(const ConfigData& config, bool configChanged, uint64_t configRevision) {
    m_interactionUpdatePending = false;
    // UI event handlers apply their own targeted/structural updates immediately.
    // The render loop observes that Config revision a few microseconds later; do
    // not treat the same revision as a second UI invalidation and rebuild again.
    const bool localConfigEcho = configChanged && configRevision != 0 && configRevision == m_lastLocalConfigRevision;
    bool scaleChanged = false;
    bool themeChanged = false;

    if (configChanged) {
        const auto sameColor = [](const ColorRGBA& a, const ColorRGBA& b) {
            return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
        };
        // Compare against the scale actually pushed into the RmlUi context, not the
        // previous config: a `<select>` change writes the new value into m_config and
        // defers the rest of its work, so a config-to-config comparison would never
        // see the change and the context would keep rendering at the old dp ratio
        // while all geometry was computed for the new one.
        scaleChanged = SanitizedUiScale(config.ui_scale) != m_appliedUiScale;
        themeChanged = !sameColor(config.themeBg, m_config.themeBg) ||
                       !sameColor(config.themeSettingsPanel, m_config.themeSettingsPanel) ||
                       !sameColor(config.themeTopbar, m_config.themeTopbar) ||
                       !sameColor(config.themeGraphPanel, m_config.themeGraphPanel) ||
                       !sameColor(config.themeText, m_config.themeText) ||
                       !sameColor(config.themeAccent, m_config.themeAccent) ||
                       !sameColor(config.themeWin, m_config.themeWin) ||
                       !sameColor(config.themeLoss, m_config.themeLoss) ||
                       !sameColor(config.themeDim, m_config.themeDim) ||
                       !sameColor(config.themeMuted, m_config.themeMuted) ||
                       !sameColor(config.themeGraphLine, m_config.themeGraphLine) ||
                       !sameColor(config.themeGraphBaseline, m_config.themeGraphBaseline) ||
                       !sameColor(config.themeScoutTag, m_config.themeScoutTag) ||
                       FontsDifferFromApplied(config);
        if (!m_config.account_link_prompt_shown) CheckAccountLinkPrompt();

        const auto activeDragKind = m_drag.kind;
        const std::string draggingId = m_drag.containerId;
        float inFlightX = 0.0f, inFlightY = 0.0f, inFlightW = 0.0f, inFlightH = 0.0f;
        bool hasInFlight = false;
        if (activeDragKind == DragKind::OverlayMove || activeDragKind == DragKind::OverlayResize) {
            auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(),
                                   [&](const auto& c) { return c.id == draggingId; });
            if (it != m_config.overlay_layout.containers.end()) {
                inFlightX = it->x;
                inFlightY = it->y;
                inFlightW = it->w;
                inFlightH = it->h;
                hasInFlight = true;
            }
        }
        // Positions are edited live in m_config while dragging; preserve those
        // in-flight values until the drag commits them to Config.
        const float inFlightSessionX = m_config.session_view_x;
        const float inFlightSessionY = m_config.session_view_y;
        const float inFlightSummaryX = m_config.match_summary_x;
        const float inFlightSummaryY = m_config.match_summary_y;

        m_config = config;

        if (hasInFlight) {
            for (auto& c : m_config.overlay_layout.containers) {
                if (c.id == draggingId) {
                    c.x = inFlightX;
                    c.y = inFlightY;
                    c.w = inFlightW;
                    c.h = inFlightH;
                    break;
                }
            }
        }
        if (activeDragKind == DragKind::FloatingCard) {
            m_config.session_view_x = inFlightSessionX;
            m_config.session_view_y = inFlightSessionY;
            m_config.match_summary_x = inFlightSummaryX;
            m_config.match_summary_y = inFlightSummaryY;
        }
    }

    const uint64_t previousGameVersion = m_lastGameVersion;
    const uint64_t previousHistoryVersion = m_lastHistoryVersion;
    SnapshotState();
    const bool stateChanged = previousGameVersion != m_lastGameVersion || previousHistoryVersion != m_lastHistoryVersion;
    if (configChanged || stateChanged) RefreshAsyncData();

    const bool settingsOpen = m_state && m_state->ui.showMenu.load();
    if (!m_lastShowMenu && settingsOpen) {
        ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
            m_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::SettingsOpened);
        CheckStatsApi(false, false);
        UpdateInputCapture();
    } else if (m_lastShowMenu && !settingsOpen) {
        // A range can be adjusted from the keyboard without producing a mouse-up.
        // Do not discard an in-flight color edit when Settings is closed.
        CommitColorPick();
        FinishBindCapture();
        m_showBallchasingToken = false;
        m_confirmReplayUploads = false;
        m_confirmDeleteHistory = false;
        m_editColorKey.clear();
    } else {
        UpdateInputCapture();
    }
    if (scaleChanged) SetDpiScale(m_dpiScale);
    PublishBackgroundTaskCompletion();
    if (themeChanged) {
        UpdateThemeProperties();
        RefreshThemeEditorControls();
    }

    const int64_t nowMs = SteadyNowMs();
    if (m_state && nowMs >= m_nextUpdateCheckPollMs && Config::HasCurrentConsent(m_config)) {
        ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
            m_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Periodic);
        // The launcher applies the one-hour success interval and a shorter retry
        // delay after failures. Poll the gate once per minute instead of every frame.
        m_nextUpdateCheckPollMs = nowMs + 60 * 1000;
    }
    RebuildVisibleUi(false, (configChanged && !localConfigEcho) || m_deferredSelectRebuild);
    if (m_state && m_state->ui.showSessionRecap.exchange(false)) ShowInsights(InsightsView::Tab::Recap, true);
    if (m_insightsVisible) RefreshInsights(false);
    if (m_historyVisible) RefreshHistory(false);
    if (m_accountsVisible) RefreshAccounts(false);
    RefreshPlayerNote();
    if (m_onboardingVisible) RefreshOnboarding();
}

Rml::Element* RmlUiController::Root(const char* id) const {
    return m_document ? m_document->GetElementById(id) : nullptr;
}

void RmlUiController::SetElementRml(Rml::Element* element, const std::string& rml, bool replayPointer) {
    if (!element) return;
    const bool prev = m_rebuildingUi;
    m_rebuildingUi = true;
    m_systemInterface.BeginCursorUpdate();
    element->SetInnerRML(rml);
    if (replayPointer && m_hasPointerPosition && m_context) {
        m_context->Update();
        m_context->ProcessMouseMove(m_lastPointerX, m_lastPointerY, 0);
    }
    m_systemInterface.EndCursorUpdate();
    m_rebuildingUi = prev;
    m_renderDirty = true;
}

void RmlUiController::SetElementText(Rml::Element* element, const std::string& text) {
    if (!element) return;
    // Updating the single text node avoids reparsing RML and replacing child
    // DOM/geometry for a value-only change.
    if (element->GetNumChildren() == 1) {
        if (auto* textElement = dynamic_cast<Rml::ElementText*>(element->GetFirstChild())) {
            textElement->SetText(text);
            m_renderDirty = true;
            return;
        }
    }
    SetElementRml(element, Escape(text), false);
}

void RmlUiController::SetRootRml(const char* id, const std::string& rml) {
    // Root replacement is now reserved for structural UI changes. Theme selector
    // work is intentionally not performed here; telemetry/data refreshes update
    // persistent child elements instead of recreating the large root tree.
    SetElementRml(Root(id), rml, true);
}

void RmlUiController::SnapshotState() {
    if (!m_state) return;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap && liveSnap->version != m_lastGameVersion) {
            m_lastGameVersion = liveSnap->version;
            m_snap.matchGuid = liveSnap->matchGuid;
            m_snap.arenaName = liveSnap->arenaName;
            m_snap.score[0] = liveSnap->score[0];
            m_snap.score[1] = liveSnap->score[1];
            m_snap.inMatch = liveSnap->inMatch;
            m_snap.inReplay = liveSnap->inReplay;
            m_snap.matchFinalized = liveSnap->matchFinalized;
            m_snap.myPrimaryId = liveSnap->myPrimaryId;
            m_snap.myTeam = liveSnap->myTeam;
            m_snap.currentMatch = liveSnap->currentMatch;
            m_snap.lastGoalSpeed = liveSnap->lastGoalSpeed;
            m_snap.lastGoalSpeedSelf = liveSnap->lastGoalSpeedSelf;
            m_snap.sessionTotals = liveSnap->sessionTotals;
            m_snap.roster = liveSnap->roster;
            m_snap.sessionGamemodes = liveSnap->sessionGamemodes;
            m_snap.lastMatchWasVoid = liveSnap->lastMatchWasVoid;
            m_snap.lastMatchVoidReason = liveSnap->lastMatchVoidReason;
            m_snap.matchSummaryScore[0] = liveSnap->matchSummaryScore[0];
            m_snap.matchSummaryScore[1] = liveSnap->matchSummaryScore[1];
            m_snap.matchSummaryMyTeam = liveSnap->matchSummaryMyTeam;
            m_snap.matchSummaryWinnerTeam = liveSnap->matchSummaryWinnerTeam;
            m_snap.matchSummaryOvertimeSeconds = liveSnap->matchSummaryOvertimeSeconds;
        }
    }
    {
        std::shared_lock lock(m_state->history.mutex);
        m_snap.showLifetimeGraph = m_state->history.showLifetimeGraph.load();
        const auto version = m_state->history.version.load();
        if (version != m_lastHistoryVersion) {
            m_lastHistoryVersion = version;
            m_snap.initialMmr = static_cast<float>(m_state->history.initialMmr);
            m_snap.playlistInitialMmr = m_state->history.playlistInitialMmr;
            m_snap.playlistHistoryY = m_state->history.playlistHistoryY;
            m_snap.playlistHistoryEstimated.clear();
            for (const auto& [playlist, history] : m_state->history.playlistHistoryY) {
                auto& flags = m_snap.playlistHistoryEstimated[playlist];
                flags.assign(history.size(), false);
                auto pointIt = m_state->history.playlistMatchPoints.find(playlist);
                if (pointIt == m_state->history.playlistMatchPoints.end()) continue;
                for (const auto& point : pointIt->second)
                    if (point.historyIndex < flags.size()) flags[point.historyIndex] = point.valueEstimated;
            }
            m_snap.lifetimeMmrX = m_state->history.lifetimeMmrX;
            m_snap.lifetimeMmrY = m_state->history.lifetimeMmrY;
            m_snap.recentSavedMatches = m_state->history.pendingRecentMatches;
            for (const auto& saved : m_state->history.recentSavedMatches) {
                const bool shadowed = !saved.matchGuid.empty() && std::any_of(m_state->history.pendingRecentMatches.begin(), m_state->history.pendingRecentMatches.end(), [&](const SessionMatchSummary& pending) {
                    return pending.matchGuid == saved.matchGuid;
                });
                if (!shadowed) m_snap.recentSavedMatches.push_back(saved);
            }
            std::stable_sort(m_snap.recentSavedMatches.begin(), m_snap.recentSavedMatches.end(), [](const auto& a, const auto& b) { return a.endedAtUnix > b.endedAtUnix; });
            m_snap.recentSavedMatchesLoaded = m_state->history.recentSavedMatchesLoaded || !m_state->history.pendingRecentMatches.empty();
        }
    }
    if (m_snap.myPrimaryId.empty() && !m_config.last_primary_id.empty()) m_snap.myPrimaryId = m_config.last_primary_id;
    SnapshotNotes();
}

void RmlUiController::RefreshAsyncData() {
    if (!m_dbManager || !m_state) return;
    if (!m_playerNotesRequested) {
        m_dbManager->AsyncLoadPlayerNotes();
        m_playerNotesRequested = true;
    }
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    if (!effectivePrimary.empty()) {
        const auto category = m_state->ui.graphMmrCategory.load();
        if (effectivePrimary != m_lastLifetimeHistoryPrimaryId || category != m_lastLifetimeHistoryCategory) {
            {
                std::unique_lock lock(m_state->history.mutex);
                m_state->history.lifetimeMmrX.clear();
                m_state->history.lifetimeMmrY.clear();
                m_state->history.version++;
            }
            m_snap.lifetimeMmrX.clear();
            m_snap.lifetimeMmrY.clear();
            m_dbManager->AsyncGetLifetimeMmrHistory(effectivePrimary, MmrCategoryToString(category));
            m_lastLifetimeHistoryPrimaryId = effectivePrimary;
            m_lastLifetimeHistoryCategory = category;
        }
        if (effectivePrimary != m_lastDbFetchPrimaryId) {
            m_dbManager->AsyncRefreshDbStats(effectivePrimary);
            m_lastDbFetchPrimaryId = effectivePrimary;
        }
    }
    const int recentLimit = std::clamp(m_config.previous_games_limit, 10, kPreviousGamesMaxLimit);
    if (effectivePrimary != m_lastRecentMatchHistoryPrimaryId || recentLimit != m_lastRecentMatchHistoryLimit) {
        m_dbManager->AsyncGetRecentMatchHistory(effectivePrimary, recentLimit);
        m_lastRecentMatchHistoryPrimaryId = effectivePrimary;
        m_lastRecentMatchHistoryLimit = recentLimit;
    }
    if (!m_detailedStatsSummaryRequested || m_state->ui.dbStatsDirty.load(std::memory_order_relaxed)) {
        m_dbManager->AsyncRefreshDetailedStatsSummary();
        m_detailedStatsSummaryRequested = true;
    }
}

void RmlUiController::RebuildVisibleUi(bool force, bool configChanged) {
    if (m_rebuildingUi) return;

    // A pointer release is not always delivered: switching out of second-monitor
    // mode makes the overlay click-through mid-click, and hiding the window
    // swallows WM_LBUTTONUP. Trust the physical button instead of waiting for a
    // message that will never arrive, or the hold below never lifts.
    if (m_pointerPressed && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) m_pointerPressed = false;

    // The toast is purely time-based and lives in its own leaf root, so expiring
    // it is safe during an interaction and must not be gated behind the hold.
    if (m_statusUntilMs && SteadyNowMs() >= m_statusUntilMs) {
        RebuildToast();
    }

    {
        const AccountStatusSnapshot accountStatus = AccountClient::Instance().GetStatus();
        if (accountStatus.version != m_lastAccountStatusVersion) {
            m_lastAccountStatusVersion = accountStatus.version;
            std::string signedOutReason;
            if (AccountClient::Instance().ConsumeSignedOutNotification(signedOutReason)) {
                ShowToast(signedOutReason, true);
            }
            if (m_onboardingVisible) RefreshOnboarding();
            if (m_accountPromptPending) CheckAccountLinkPrompt();
        } else if (m_accountPromptPending) {
            CheckAccountLinkPrompt();
        }
    }
    // Do not replace DOM while a pointer target is active, but keep safe leaf
    // telemetry flowing. This preserves drag/click stability without freezing
    // live counters for the entire interaction. Structural versions remain
    // pending and are reconciled as soon as the interaction ends.
    if (m_drag.kind != DragKind::None || (m_pointerPressed && !force)) {
        m_nextVisibilityWakeMs.reset();
        RefreshLiveUi(false, false);
        return;
    }
    m_deferredSelectRebuild = false;

    const int64_t nowMs = SteadyNowMs();
    const bool showMenu = m_state && m_state->ui.showMenu.load(std::memory_order_relaxed);
    const bool showOverlay = m_state && m_state->ui.showOverlay.load(std::memory_order_relaxed);
    const bool showSession = m_state && m_state->ui.showSessionView.load(std::memory_order_relaxed);
    bool showSummary = m_state && m_state->ui.showMatchSummary.load(std::memory_order_relaxed);
    int64_t summaryEndMs = 0;
    if (showSummary && m_state) {
        const int64_t summaryStartMs = EffectiveMatchSummaryStartMs(*m_state);
        summaryEndMs = summaryStartMs + MatchSummaryDurationMs(m_config);
        if (nowMs >= summaryEndMs) {
            m_state->ui.showMatchSummary.store(false, std::memory_order_relaxed);
            showSummary = false;
            summaryEndMs = 0;
        }
    }
    const bool dashboardEdit = m_state && m_state->ui.dashboardLayoutEditMode.load(std::memory_order_relaxed);
    const bool editMode = dashboardEdit;
    const bool showGraphView = m_state && m_state->ui.showGraphView.load(std::memory_order_relaxed);
    const bool h2hExpanded = m_state && m_state->ui.h2hExpanded.load(std::memory_order_relaxed);
    const OverlayLayout::VisibilityState visState =
        m_state ? OverlayLayout::SnapshotVisibilityState(*m_state, editMode)
                : OverlayLayout::VisibilityState{.editMode = editMode};

    m_nextVisibilityWakeMs.reset();
    if (!m_config.second_monitor_mode && !showMenu && !editMode) {
        m_nextVisibilityWakeMs = OverlayLayout::NextVisibilityChangeMs(m_config.overlay_layout, visState, nowMs);
        if (showSummary && m_config.show_match_summary && summaryEndMs > nowMs) {
            if (!m_nextVisibilityWakeMs.has_value() || summaryEndMs < *m_nextVisibilityWakeMs) {
                m_nextVisibilityWakeMs = summaryEndMs;
            }
        }
    }

    // Runtime state that changes the dashboard/overlay structure but is not part
    // of Config. Keep this intentionally tiny; telemetry versions do not belong
    // here and must never invalidate a large root. Hash it directly so the 10 Hz
    // reconciliation path does not allocate/build fingerprint strings.
    uint64_t runtimeStructuralHash = kFnvOffset;
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showMenu));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showOverlay));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showSession));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showSummary));
    if (showSummary && m_state) {
        HashAppend(runtimeStructuralHash, m_state->ui.replayUploadVersion.load(std::memory_order_relaxed));
    }
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(dashboardEdit));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showGraphView));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(h2hExpanded));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_snap.inMatch));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_config.second_monitor_mode));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(RankSourceAvailable(m_config)));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_config.show_tournaments && AccountClient::Instance().IsSignedIn()));
    if (!m_config.second_monitor_mode) {
        for (const auto& container : m_config.overlay_layout.containers) {
            for (auto widget : container.widgets) {
                const auto vis = OverlayLayout::GetWidgetVisibility(m_config.overlay_layout, widget);
                const bool wVis = OverlayLayout::IsVisibilityActive(vis, visState, nowMs);
                HashAppend(runtimeStructuralHash, static_cast<uint64_t>(wVis));
            }
        }
    }
    if (m_state && m_config.second_monitor_mode) {
        HashAppend(runtimeStructuralHash, m_remoteConfig ? m_remoteConfig->Version() : 0);
        const bool updateAvailable = m_state->ui.updateAvailable.load(std::memory_order_relaxed);
        HashAppend(runtimeStructuralHash, static_cast<uint64_t>(updateAvailable));
        HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_state->ui.updateDownloading.load(std::memory_order_relaxed)));
        HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_state->ui.updateDownloadFailed.load(std::memory_order_relaxed)));
        if (updateAvailable) {
            std::lock_guard lock(m_state->ui.updateMutex);
            HashAppend(runtimeStructuralHash, m_state->ui.updateAvailableVersion);
        }
    }

    // Keep an externally-observable value for tests/diagnostics, but materialize
    // its string only when the underlying hash changes.
    uint64_t invalidationHash = runtimeStructuralHash;
    if (m_state) {
        HashAppend(invalidationHash, static_cast<uint64_t>(m_state->ui.statsApiChecked.load(std::memory_order_relaxed)));
        std::lock_guard lock(m_state->ui.statsApiMutex);
        const auto& result = m_state->ui.statsApiResult;
        HashAppend(invalidationHash, static_cast<uint64_t>(result.status));
        HashAppend(invalidationHash, result.path);
        HashAppend(invalidationHash, result.message);
        HashAppend(invalidationHash, static_cast<uint64_t>(static_cast<int64_t>(result.expectedPort)));
        HashAppend(invalidationHash, static_cast<uint64_t>(static_cast<int64_t>(result.actualPort)));
        HashAppend(invalidationHash, static_cast<uint64_t>(std::bit_cast<uint32_t>(result.packetSendRate)));
        HashAppend(invalidationHash, static_cast<uint64_t>(result.rlRunning));
    }
    if (m_lastConfigHash != invalidationHash) {
        m_lastConfigHash = invalidationHash;
        m_lastConfigFingerprint = std::to_string(invalidationHash);
    }

    // Config is revision-gated by Overlay. Build the larger render-config
    // fingerprint only when a config commit actually occurred. Theme colors,
    // vsync and the FPS cap are intentionally excluded: they do not change DOM
    // structure and must not recreate the overlay/dashboard roots.
    bool renderConfigChanged = force;
    if (force || configChanged) {
        std::ostringstream fp;
        fp << std::setprecision(9)
           << m_config.require_rl_focus << '|' << m_config.second_monitor_mode << '|'
           << m_config.second_monitor_show_roster << '|' << m_config.second_monitor_show_session << '|'
           << m_config.show_match_summary << '|' << m_config.match_summary_seconds << '|' << m_config.show_running_indicator << '|'
           << m_config.enable_auto_updates << '|'
           << m_config.last_primary_id << '|'
           << m_config.show_session_record << '|' << m_config.show_session_goals << '|'
           << m_config.show_session_saves << '|' << m_config.show_session_demos << '|'
           << m_config.show_session_assists << '|' << m_config.show_session_goal_participation << '|'
           << m_config.show_session_mmr_change << '|' << m_config.show_session_boost << '|'
           << m_config.use_rank_icons << '|' << m_config.show_lobby_ranks_overlay << '|'
           << m_config.show_lobby_rank_1v1 << '|' << m_config.show_lobby_rank_2v2 << '|'
           << m_config.show_lobby_rank_3v3 << '|' << m_config.show_lobby_rank_casual << '|'
           << m_config.show_lobby_rank_tourny << '|' << m_config.show_lobby_rank_hoops << '|'
           << m_config.show_lobby_rank_rumble << '|' << m_config.show_lobby_rank_dropshot << '|'
           << m_config.show_lobby_rank_snowday << '|' << m_config.show_lobby_rank_heatseeker << '|'
           << m_config.show_account_wins_overlay << '|' << m_config.show_demo_tracker_overlay << '|' << m_config.show_rank_progress << '|'
           << m_config.show_previous_games_summary << '|' << m_config.previous_games_limit << '|'
           << m_config.show_tournaments << '|' << m_config.tournament_region << '|'
           << m_config.tournament_reminder_minutes << '|' << m_config.tournament_reminder_standard_only << '|'
           << m_config.show_streaks_stats << '|' << m_config.show_longest_loss_streak << '|'
           << m_config.show_gamemode_breakdown << '|' << m_config.show_gamemode_record_1v1 << '|'
           << m_config.show_gamemode_record_2v2 << '|' << m_config.show_gamemode_record_3v3 << '|'
           << m_config.gamemode_breakdown_scope << '|' << m_config.mmr_category << '|'
           << m_config.auto_switch_mmr_category << '|' << m_config.graph_follow_current_playlist << '|'
           << m_config.graph_mmr_category << '|' << m_config.rank_progress_follow_current_playlist << '|'
           << m_config.rank_progress_mmr_category << '|' << m_config.show_extra_playlists << '|'
           << m_config.ui_scale << '|' << m_config.imperial_units << '|' << m_config.crossbar_display_mode << '|'
           << m_config.use_roman_numerals << '|' << m_config.key_cycle << '|' << m_config.key_expand << '|'
           << m_config.key_session << '|';
        fp << m_config.overlay_layout.version << '|' << m_config.overlay_layout.toolboxOpen << '|';
        for (auto widget : m_config.overlay_layout.disabledWidgets)
            fp << "disabled:" << static_cast<int>(widget) << ';';
        for (const auto& container : m_config.overlay_layout.containers) {
            fp << container.id << ':' << container.x << ',' << container.y << ',' << container.w << ',' << container.h
               << ',' << static_cast<int>(container.visibility.mode) << ',' << static_cast<int>(container.visibility.event)
               << ',' << container.visibility.seconds << ',' << container.visibility.alsoWhileKeyHeld
               << ',' << container.visibility.hideDuringReplay << '[';
            for (auto widget : container.widgets)
                fp << static_cast<int>(widget) << ',';
            fp << "];";
        }
        fp << '|' << m_config.dashboard_layout.version << '|' << m_config.dashboard_layout.leftColumnWeight << '|';
        for (const auto& widget : m_config.dashboard_layout.widgets)
            fp << static_cast<int>(widget.id) << ',' << static_cast<int>(widget.zone) << ',' << widget.order << ','
               << widget.height << ',' << widget.collapsed << ',' << widget.enabled << ';';

        const std::string renderConfigFingerprint = fp.str();
        renderConfigChanged = force || m_lastRenderConfigFingerprint != renderConfigFingerprint;
        m_lastRenderConfigFingerprint = renderConfigFingerprint;
    }

    const bool modeChanged = m_lastSecondMonitor != m_config.second_monitor_mode;
    const bool structuralDirty = force || renderConfigChanged || modeChanged ||
                                 m_lastRuntimeStructuralHash != runtimeStructuralHash;

    bool rebuiltMainStructure = false;
    if (structuralDirty) {
        if (m_onboardingVisible) {
            SetRootRml("dashboard-root", "");
            SetRootRml("overlay-root", "");
        } else if (m_config.second_monitor_mode) {
            if (modeChanged || force) SetRootRml("overlay-root", "");
            RebuildDashboard();
        } else {
            if (modeChanged || force) SetRootRml("dashboard-root", "");
            RebuildOverlay();
        }
        RebuildToast();
        rebuiltMainStructure = true;

        m_lastShowOverlay = showOverlay;
        m_lastShowSessionView = showSession;
        m_lastShowMatchSummary = showSummary;
        m_lastSecondMonitor = m_config.second_monitor_mode;
        m_lastDashboardEditMode = dashboardEdit;
        m_lastShowGraphView = showGraphView;
        m_lastH2hExpanded = h2hExpanded;
        m_lastInMatch = m_snap.inMatch;
        m_lastRuntimeStructuralHash = runtimeStructuralHash;
    }

    // Settings has its own narrow invalidation path. It is never tied to
    // game.version, match counters, or the dashboard render cadence.
    if (showMenu) {
        uint64_t settingsHash = kFnvOffset;
        HashAppend(settingsHash, static_cast<uint64_t>(m_settingsPage));
        HashAppend(settingsHash, static_cast<uint64_t>(m_bindCaptureTarget));
        HashAppend(settingsHash, static_cast<uint64_t>(m_showBallchasingToken));
        HashAppend(settingsHash, AccountClient::Instance().GetStatus().version);
        HashAppend(settingsHash, static_cast<uint64_t>(m_onboarding.ShowFallback()));
        HashAppend(settingsHash, static_cast<uint64_t>(m_confirmReplayUploads));
        HashAppend(settingsHash, m_remoteConfig ? m_remoteConfig->Version() : 0);
        HashAppend(settingsHash, (m_tournamentService && m_tournamentService->Snapshot()) ? m_tournamentService->Snapshot()->version : 0);
        HashAppend(settingsHash, static_cast<uint64_t>(m_confirmDeleteHistory));
        HashAppend(settingsHash, m_editColorKey);
        HashAppend(settingsHash, m_statsApiPathError);
        if (m_state) {
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.updateChecked.load(std::memory_order_relaxed)));
            const bool updateAvailable = m_state->ui.updateAvailable.load(std::memory_order_relaxed);
            HashAppend(settingsHash, static_cast<uint64_t>(updateAvailable));
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.updateDownloading.load(std::memory_order_relaxed)));
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.updateDownloadFailed.load(std::memory_order_relaxed)));
            const bool controllerConnected = m_state->ui.controllerConnected.load(std::memory_order_relaxed);
            HashAppend(settingsHash, static_cast<uint64_t>(controllerConnected));
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.controllerIsGameController.load(std::memory_order_relaxed)));
            const bool capturing = m_bindCaptureTarget != BindCaptureTarget::None;
            if (capturing)
                HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastKeyboardKeyPressed.load(std::memory_order_relaxed))));
            if (capturing || (controllerConnected && m_settingsPage == SettingsPage::Controls)) {
                HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastControllerButtonPressed.load(std::memory_order_relaxed))));
                HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastRawControllerButtonPressed.load(std::memory_order_relaxed))));
            }
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.statsApiChecked.load(std::memory_order_relaxed)));
            if (updateAvailable) {
                std::lock_guard lock(m_state->ui.updateMutex);
                HashAppend(settingsHash, m_state->ui.updateAvailableVersion);
            }
            if (controllerConnected) {
                std::lock_guard lock(m_state->ui.controllerDebugMutex);
                HashAppend(settingsHash, m_state->ui.controllerDebugName);
            }
            {
                std::lock_guard lock(m_state->ui.statsApiMutex);
                const auto& result = m_state->ui.statsApiResult;
                HashAppend(settingsHash, static_cast<uint64_t>(result.status));
                HashAppend(settingsHash, result.path);
                HashAppend(settingsHash, result.message);
                HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(result.expectedPort)));
                HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(result.actualPort)));
                HashAppend(settingsHash, static_cast<uint64_t>(std::bit_cast<uint32_t>(result.packetSendRate)));
                HashAppend(settingsHash, static_cast<uint64_t>(result.rlRunning));
            }
        }
        if (m_settingsPage == SettingsPage::DataSupport && m_state) {
            HashAppend(settingsHash, m_state->ui.dbStatsVersion.load(std::memory_order_relaxed));
        }
        if (m_settingsPage == SettingsPage::Accounts && m_state) {
            HashAppend(settingsHash, m_state->ui.replayUploadVersion.load(std::memory_order_relaxed));
            HashAppend(settingsHash, static_cast<uint64_t>(m_state->ui.ballchasingTokenRejected.load(std::memory_order_relaxed)));
        }
        if (m_settingsPage == SettingsPage::Accounts) {
            // game.version also advances for ordinary match counters. Hash only
            // the identity/rank fields rendered by these settings pages, without
            // sorting IDs or constructing a large temporary fingerprint string.
            if (force || configChanged || !m_hasSettingsRosterHash ||
                m_lastSettingsRosterGameVersion != m_lastGameVersion ||
                m_lastSettingsRosterPage != m_settingsPage) {
                uint64_t rosterHash = kFnvOffset;
                HashAppend(rosterHash, static_cast<uint64_t>(m_settingsPage));
                HashAppend(rosterHash, static_cast<uint64_t>(m_snap.roster.size()));
                uint64_t membersHash = 0;
                for (const auto& [id, player] : m_snap.roster) {
                    uint64_t playerHash = kFnvOffset;
                    HashAppend(playerHash, id);
                    HashAppend(playerHash, player.name);
                    HashAppend(playerHash, static_cast<uint64_t>(player.fetched));
                    HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(player.mmr)));
                    HashAppend(playerHash, player.rankTier);
                    if (m_showRankDetails) {
                        for (auto category : MmrCategories(false, m_config.show_extra_playlists)) {
                            const std::string key = MmrCategoryToString(category);
                            HashAppend(playerHash, key);
                            const auto mmrIt = player.playlists.find(key);
                            const auto tierIt = player.playlistTiers.find(key);
                            const auto matchesIt = player.playlistMatches.find(key);
                            HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(mmrIt == player.playlists.end() ? 0 : mmrIt->second)));
                            if (tierIt != player.playlistTiers.end())
                                HashAppend(playerHash, tierIt->second);
                            else
                                HashAppend(playerHash, std::string_view{});
                            HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(matchesIt == player.playlistMatches.end() ? 0 : matchesIt->second)));
                        }
                    }
                    membersHash ^= AvalancheHash(playerHash);
                }
                HashAppend(rosterHash, membersHash);
                m_lastSettingsRosterHash = rosterHash;
                m_hasSettingsRosterHash = true;
                m_lastSettingsRosterGameVersion = m_lastGameVersion;
                m_lastSettingsRosterPage = m_settingsPage;
            }
            HashAppend(settingsHash, m_lastSettingsRosterHash);
        }
        const bool settingsDirty = force || configChanged || !m_lastShowMenu || m_lastSettingsHash != settingsHash;
        if (settingsDirty) {
            RebuildSettings();
            m_lastSettingsHash = settingsHash;
            m_lastSettingsFingerprint = std::to_string(settingsHash);
        }
    } else if (m_lastShowMenu) {
        SetRootRml("settings-root", "");
        m_lastSettingsFingerprint.clear();
        m_lastSettingsHash = 0;
        m_hasSettingsRosterHash = false;
        m_lastSettingsRosterGameVersion = std::numeric_limits<uint64_t>::max();
    }

    m_lastShowMenu = showMenu;

    if (rebuiltMainStructure) m_liveDomNeedsPrime = true;

    RefreshLiveUi(false);
    RefreshTournaments(rebuiltMainStructure || renderConfigChanged);
}

void RmlUiController::RefreshLiveUi(bool force, bool allowStructural) {
    auto* app = Root("app");
    if (!app || !m_state) return;

    const uint64_t dbStatsVersion = m_state->ui.dbStatsVersion.load(std::memory_order_relaxed);
    const bool gameChanged = force || m_lastRenderedGameVersion != m_lastGameVersion;
    const bool leafGameChanged = force || m_lastLeafGameVersion != m_lastGameVersion;
    const bool historyChanged = force || m_lastRenderedHistoryVersion != m_lastHistoryVersion;
    const bool dbChanged = force || m_lastRenderedDbStatsVersion != dbStatsVersion;
    const int graphOffset = m_state->ui.graphOffset.load(std::memory_order_relaxed);
    const MmrCategory rosterCategory = m_state->ui.rosterMmrCategory.load(std::memory_order_relaxed);
    const MmrCategory graphCategory = m_state->ui.graphMmrCategory.load(std::memory_order_relaxed);
    const MmrCategory rankProgressCategory = m_state ? m_state->ui.rankProgressMmrCategory.load(std::memory_order_relaxed) : MmrCategory::TwoVTwo;
    const bool showLifetimeGraph = m_state->history.showLifetimeGraph.load(std::memory_order_relaxed);
    const bool rosterCategoryChanged = force || m_lastRosterMmrCategory != rosterCategory;
    const bool graphCategoryChanged = force || m_lastGraphMmrCategory != graphCategory;
    const bool rankProgressCategoryChanged = force || m_lastRankProgressMmrCategory != rankProgressCategory;
    const bool graphOffsetChanged = force || m_lastGraphOffset != graphOffset;
    const bool lifetimeGraphChanged = force || m_lastShowLifetimeGraph != showLifetimeGraph;
    const bool notesChanged = force || m_lastRenderedNotesVersion != m_snap.notesVersion;

    const bool structuralPending = gameChanged || historyChanged || dbChanged || rosterCategoryChanged ||
                                   graphCategoryChanged || rankProgressCategoryChanged || graphOffsetChanged || lifetimeGraphChanged || notesChanged;
    const bool primeStructure = allowStructural && m_liveDomNeedsPrime;
    if (!leafGameChanged && !primeStructure && (!allowStructural || !structuralPending)) return;

    if (allowStructural) {
        // Resolve widget wrappers lazily. The ordinary telemetry path updates leaf
        // values and never needs this scan unless a widget's structure/data source
        // truly changed.
        bool widgetGroupsReady = false;
        std::unordered_map<std::string, std::vector<Rml::Element*>> widgetGroups;
        const auto ensureWidgetGroups = [&]() {
            if (widgetGroupsReady) return;
            widgetGroupsReady = true;
            Rml::ElementList liveWidgets;
            app->GetElementsByClassName(liveWidgets, "live-widget");
            for (Rml::Element* element : liveWidgets) {
                const std::string id = Attribute(element, "data-live-widget");
                const std::string surface = Attribute(element, "data-live-surface");
                if (!id.empty()) widgetGroups[surface + ":" + id].push_back(element);
            }
        };

        const auto refreshWidget = [&](DashboardLayout::WidgetId id, bool primeOnly = false) {
            ensureWidgetGroups();
            const std::string domId = WidgetDomId(id);
            for (const char* surfaceName : {"overlay", "dashboard"}) {
                const std::string groupKey = std::string(surfaceName) + ":" + domId;
                auto groupIt = widgetGroups.find(groupKey);
                if (groupIt == widgetGroups.end() || groupIt->second.empty()) continue;

                const bool dashboard = std::string_view(surfaceName) == "dashboard";
                const std::string rml = RenderWidget(id, dashboard);
                const std::string cacheKey = "widget:" + groupKey;
                auto cacheIt = m_lastLiveWidgetRml.find(cacheKey);
                if (primeOnly || cacheIt == m_lastLiveWidgetRml.end()) {
                    // A newly built root already contains the current data. Prime the
                    // cache without immediately compiling identical geometry again.
                    m_lastLiveWidgetRml[cacheKey] = rml;
                    continue;
                }
                if (!force && cacheIt->second == rml) continue;
                cacheIt->second = rml;
                const bool replay = (id == DashboardLayout::WidgetId::MmrGraph);
                for (Rml::Element* element : groupIt->second)
                    SetElementRml(element, rml, replay);
            }
        };

        // After startup or a mode/layout rebuild, prime the content caches from the
        // DOM that was just rendered. This guarantees the *next* data change is
        // compared against the data actually on screen instead of being mistaken
        // for the initial cache population.
        if (m_liveDomNeedsPrime) {
            for (DashboardLayout::WidgetId id : {DashboardLayout::WidgetId::LiveRoster,
                                                 DashboardLayout::WidgetId::LobbyRanks,
                                                 DashboardLayout::WidgetId::LiveMatchStats,
                                                 DashboardLayout::WidgetId::SessionStats,
                                                 DashboardLayout::WidgetId::DemoTracker,
                                                 DashboardLayout::WidgetId::MmrGraph,
                                                 DashboardLayout::WidgetId::PreviousGames,
                                                 DashboardLayout::WidgetId::StreaksStats,
                                                 DashboardLayout::WidgetId::GamemodeBreakdown,
                                                 DashboardLayout::WidgetId::RankProgress})
                refreshWidget(id, true);
        }

        // The roster's high-frequency goals/saves/etc. are leaf updates below. Only
        // rank/identity/team changes alter the roster subtree. game.version is a
        // coarse telemetry version, so use a cheap order-independent hash here
        // instead of sorting IDs and constructing a large fingerprint string on
        // every telemetry tick.
        if (gameChanged || rosterCategoryChanged || notesChanged || m_liveDomNeedsPrime) {
            uint64_t rosterHash = kFnvOffset;
            HashAppend(rosterHash, m_snap.myPrimaryId);
            HashAppend(rosterHash, static_cast<uint64_t>(rosterCategory));
            HashAppend(rosterHash, static_cast<uint64_t>(m_snap.roster.size()));
            HashAppend(rosterHash, m_snap.notesVersion);

            const std::string category = MmrCategoryToString(rosterCategory);
            uint64_t membersHash = 0;
            for (const auto& [id, p] : m_snap.roster) {
                int selectedMmr = p.mmr;
                const std::string* selectedTier = &p.rankTier;
                int selectedMatches = 0;
                if (category != "best") {
                    if (auto it = p.playlists.find(category); it != p.playlists.end()) selectedMmr = it->second;
                    if (auto it = p.playlistTiers.find(category); it != p.playlistTiers.end()) selectedTier = &it->second;
                    if (auto it = p.playlistMatches.find(category); it != p.playlistMatches.end()) selectedMatches = it->second;
                }

                uint64_t playerHash = kFnvOffset;
                HashAppend(playerHash, id);
                HashAppend(playerHash, p.name);
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.team)));
                HashAppend(playerHash, static_cast<uint64_t>(p.fetched));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(selectedMmr)));
                HashAppend(playerHash, *selectedTier);
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(selectedMatches)));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.totalWins)));

                // LobbyRanks renders all fetched playlist columns, not only the
                // currently selected roster category. Fold all rank payload maps
                // into the structural hash so an async rank result refreshes the
                // table without tying the whole root to game.version.
                uint64_t playlistHash = 0;
                for (const auto& [key, value] : p.playlists) {
                    uint64_t entryHash = kFnvOffset;
                    HashAppend(entryHash, key);
                    HashAppend(entryHash, static_cast<uint64_t>(static_cast<int64_t>(value)));
                    playlistHash ^= AvalancheHash(entryHash);
                }
                HashAppend(playerHash, playlistHash);
                uint64_t tierHash = 0;
                for (const auto& [key, value] : p.playlistTiers) {
                    uint64_t entryHash = kFnvOffset;
                    HashAppend(entryHash, key);
                    HashAppend(entryHash, value);
                    tierHash ^= AvalancheHash(entryHash);
                }
                HashAppend(playerHash, tierHash);
                uint64_t matchesHash = 0;
                for (const auto& [key, value] : p.playlistMatches) {
                    uint64_t entryHash = kFnvOffset;
                    HashAppend(entryHash, key);
                    HashAppend(entryHash, static_cast<uint64_t>(static_cast<int64_t>(value)));
                    matchesHash ^= AvalancheHash(entryHash);
                }
                HashAppend(playerHash, matchesHash);

                HashAppend(playerHash, static_cast<uint64_t>(p.hasLifetimeData));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.lifetimeWinsWith)));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.lifetimeLossesWith)));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.lifetimeWinsAgainst)));
                HashAppend(playerHash, static_cast<uint64_t>(static_cast<int64_t>(p.lifetimeLossesAgainst)));
                membersHash ^= AvalancheHash(playerHash);
            }
            HashAppend(rosterHash, membersHash);

            if (m_liveDomNeedsPrime || !m_hasRosterStructureHash) {
                // A freshly built root already contains this exact roster.
                m_lastRosterStructureHash = rosterHash;
                m_hasRosterStructureHash = true;
            } else if (m_lastRosterStructureHash != rosterHash) {
                m_lastRosterStructureHash = rosterHash;
                refreshWidget(DashboardLayout::WidgetId::LiveRoster);
                refreshWidget(DashboardLayout::WidgetId::LobbyRanks);
                refreshWidget(DashboardLayout::WidgetId::RankProgress);
            }
        }

        // Live Match Stats only changes structure when optional rows appear. All
        // ordinary counter/speed changes stay on the existing DOM nodes.
        if (gameChanged || m_liveDomNeedsPrime) {
            const bool hasDemoed = m_snap.currentMatch.demoedSelf > 0;
            const bool hasOwnGoals = m_snap.currentMatch.ownGoals > 0;
            if (!m_liveDomNeedsPrime && (hasDemoed != m_lastLiveMatchHadDemoedRow || hasOwnGoals != m_lastLiveMatchHadOwnGoalsRow))
                refreshWidget(DashboardLayout::WidgetId::LiveMatchStats);
            m_lastLiveMatchHadDemoedRow = hasDemoed;
            m_lastLiveMatchHadOwnGoalsRow = hasOwnGoals;
        }

        if (historyChanged || graphCategoryChanged || graphOffsetChanged || lifetimeGraphChanged)
            refreshWidget(DashboardLayout::WidgetId::MmrGraph);
        if (rankProgressCategoryChanged)
            refreshWidget(DashboardLayout::WidgetId::RankProgress);
        if (historyChanged) refreshWidget(DashboardLayout::WidgetId::PreviousGames);
        if (dbChanged) refreshWidget(DashboardLayout::WidgetId::StreaksStats);
        if (dbChanged || gameChanged || m_liveDomNeedsPrime) {
            uint64_t breakdownHash = kFnvOffset;
            HashAppend(breakdownHash, static_cast<uint64_t>(ScopeFromConfigString(m_config.gamemode_breakdown_scope)));
            HashAppend(breakdownHash, dbStatsVersion);
            for (const char* mode : {"1v1", "2v2", "3v3"}) {
                HashAppend(breakdownHash, mode);
                if (auto it = m_snap.sessionGamemodes.find(mode); it != m_snap.sessionGamemodes.end()) {
                    HashAppend(breakdownHash, static_cast<uint64_t>(static_cast<int64_t>(it->second.wins)));
                    HashAppend(breakdownHash, static_cast<uint64_t>(static_cast<int64_t>(it->second.losses)));
                    HashAppend(breakdownHash, static_cast<uint64_t>(static_cast<int64_t>(it->second.total)));
                }
            }
            if (m_liveDomNeedsPrime || !m_hasGamemodeBreakdownHash) {
                m_lastGamemodeBreakdownHash = breakdownHash;
                m_hasGamemodeBreakdownHash = true;
            } else if (m_lastGamemodeBreakdownHash != breakdownHash) {
                m_lastGamemodeBreakdownHash = breakdownHash;
                refreshWidget(DashboardLayout::WidgetId::GamemodeBreakdown);
            }
        }

        // A graph shown inside the floating session card is not one of the normal
        // layout widgets. It changes only on graph/history navigation, never on each
        // telemetry packet.
        const bool sessionViewVisible = m_state->ui.showSessionView.load(std::memory_order_relaxed);
        const bool sessionGraphVisible = m_state->ui.showGraphView.load(std::memory_order_relaxed);
        const bool sessionViewStructureChanged = graphCategoryChanged ||
                                                 (sessionGraphVisible && (historyChanged || graphOffsetChanged || lifetimeGraphChanged));
        if (sessionViewVisible && (m_liveDomNeedsPrime || sessionViewStructureChanged)) {
            if (auto* overlayRoot = Root("overlay-root")) {
                Rml::ElementList specials;
                overlayRoot->GetElementsByClassName(specials, "live-special");
                for (Rml::Element* element : specials) {
                    if (Attribute(element, "data-live-special") != "session-view") continue;
                    const std::string rml = RenderSessionView();
                    if (m_liveDomNeedsPrime || m_lastSessionViewRml.empty()) {
                        m_lastSessionViewRml = rml;
                    } else if (force || m_lastSessionViewRml != rml) {
                        m_lastSessionViewRml = rml;
                        SetElementRml(element, rml, sessionGraphVisible);
                    }
                }
            }
        }
    }

    if (leafGameChanged || primeStructure) {
        // Bound text views re-evaluate only for the variables marked dirty here.
        bool changed = m_liveModel.SetValues(ComputeLiveValues(m_snap, m_config));
        if (changed) m_renderDirty = true;
    }
    if (m_overlayPresenter) {
        bool changed = m_overlayPresenter->Update(m_snap, m_config, m_state, m_tournamentService.get());
        if (changed) m_renderDirty = true;
    }

    if (leafGameChanged || primeStructure) m_lastLeafGameVersion = m_lastGameVersion;
    if (allowStructural) {
        m_liveDomNeedsPrime = false;
        m_lastRenderedGameVersion = m_lastGameVersion;
        m_lastRenderedHistoryVersion = m_lastHistoryVersion;
        m_lastRenderedDbStatsVersion = dbStatsVersion;
        m_lastShowLifetimeGraph = showLifetimeGraph;
        m_lastGraphOffset = graphOffset;
        m_lastRosterMmrCategory = rosterCategory;
        m_lastGraphMmrCategory = graphCategory;
        m_lastRankProgressMmrCategory = rankProgressCategory;
        m_lastRenderedNotesVersion = m_snap.notesVersion;
    }
}

std::string RmlUiController::CssColor(const ColorRGBA& color) {
    auto byte = [](float v) {
        if (!std::isfinite(v)) return 0;
        return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
    };
    char buffer[10]{};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", byte(color.r), byte(color.g), byte(color.b), byte(color.a));
    return buffer;
}

std::string RmlUiController::FormatDemoKd(int demos, int demoed) {
    if (demoed <= 0) return demos > 0 ? FormatNumber(static_cast<float>(demos), 1) : "0.0";
    return FormatNumber(static_cast<float>(demos) / static_cast<float>(demoed), 2);
}

const char* RmlUiController::DemoKdClass(int demos, int demoed) {
    const float ratio = demoed <= 0 ? (demos > 0 ? static_cast<float>(demos) : 0.0f)
                                    : (static_cast<float>(demos) / static_cast<float>(demoed));
    if (ratio > 1.0f) return "win";
    if (ratio < 1.0f) return "loss";
    return "muted";
}

bool RmlUiController::ValidateStatsApiPath(std::string input, std::string& normalized, std::string& error) {
    const size_t first = input.find_first_not_of(" \t\r\n");
    const size_t last = input.find_last_not_of(" \t\r\n");
    normalized = (first == std::string::npos || last == std::string::npos)
                     ? std::string{}
                     : input.substr(first, last - first + 1);
    error.clear();
    if (normalized.empty()) return true;

    const std::filesystem::path path(normalized);
    if (!StatsApiIni::IsStatsApiFileName(path)) {
        error = "Choose TAStatsAPI.ini or DefaultStatsAPI.ini.";
        return false;
    }
    if (!std::filesystem::exists(path)) {
        error = "File does not exist.";
        return false;
    }
    if (!std::filesystem::exists(path.parent_path())) {
        error = "Parent directory does not exist.";
        return false;
    }
    return true;
}

const char* RmlUiController::SettingsPageName(SettingsPage page) {
    switch (page) {
    case SettingsPage::Display:
        return "Display";
    case SettingsPage::Reminders:
        return "Reminders";
    case SettingsPage::Appearance:
        return "Appearance";
    case SettingsPage::Controls:
        return "Controls";
    case SettingsPage::Accounts:
        return "Accounts & services";
    case SettingsPage::DataSupport:
        return "Data & support";
    case SettingsPage::Count:
        break;
    }
    return "Display";
}

const char* RmlUiController::ZoneName(DashboardLayout::Zone zone) {
    switch (zone) {
    case DashboardLayout::Zone::Left:
        return "left";
    case DashboardLayout::Zone::Right:
        return "right";
    case DashboardLayout::Zone::Bottom:
        return "bottom";
    case DashboardLayout::Zone::Hidden:
        return "hidden";
    case DashboardLayout::Zone::Top:
        return "top";
    }
    return "left";
}

std::string RmlUiController::WidgetDomId(DashboardLayout::WidgetId id) {
    switch (id) {
    case DashboardLayout::WidgetId::LiveRoster:
        return "live-roster";
    case DashboardLayout::WidgetId::LiveMatchStats:
        return "live-match";
    case DashboardLayout::WidgetId::SessionStats:
        return "session";
    case DashboardLayout::WidgetId::MmrGraph:
        return "mmr-graph";
    case DashboardLayout::WidgetId::StreaksStats:
        return "streaks";
    case DashboardLayout::WidgetId::GamemodeBreakdown:
        return "gamemodes";
    case DashboardLayout::WidgetId::LobbyRanks:
        return "lobby-ranks";
    case DashboardLayout::WidgetId::DemoTracker:
        return "demos";
    case DashboardLayout::WidgetId::PreviousGames:
        return "previous-games";
    case DashboardLayout::WidgetId::RankProgress:
        return "rank-progress";
    case DashboardLayout::WidgetId::Tournaments:
        return "tournaments";
    }
    return "live-roster";
}

std::string RmlUiController::PlaylistImageForName(const std::string& playlist) {
    const int index = PlaylistResourceIndex(playlist);
    return index >= 0 ? "res://images/Playlists/" + std::to_string(index) + ".png" : "";
}

void RmlUiController::RefreshTournaments(bool force) {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const auto snapshot = m_tournamentService ? m_tournamentService->Snapshot() : std::make_shared<Tournaments::ScheduleSnapshot>();
    const bool enabled = AccountClient::Instance().IsSignedIn();
    if (enabled && !m_onboardingVisible) {
        const auto* next = Tournaments::NextSlot(snapshot->slots, now, m_config.tournament_reminder_standard_only);
        if (next && m_tournamentReminder.ShouldFire(*next, now, m_config.tournament_reminder_minutes, m_config.tournament_reminder_standard_only)) {
            if (m_config.second_monitor_mode) {
                if (m_config.tournament_sound_cue) PlaySoundW(L"SystemNotification", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
            } else {
                ShowToast(next->title + (m_snap.inMatch ? " in " : " tournament starts in ") + TournamentUi::Countdown(next->startUnix - now), false, m_snap.inMatch);
            }
        }
    }
    if (enabled) {
        const int64_t wake = SteadyNowMs() + (60 - now % 60) * 1000;
        if (!m_nextVisibilityWakeMs || wake < *m_nextVisibilityWakeMs) m_nextVisibilityWakeMs = wake;
    }
    if (!force && m_tournamentVersion == snapshot->version && m_tournamentMinute == now / 60) return;
    m_tournamentVersion = snapshot->version;
    m_tournamentMinute = now / 60;
    if (auto* root = Root("tournament-topbar-root"))
        SetElementRml(root, enabled && m_config.show_tournaments ? TournamentUi::RenderTopbar(*snapshot, now, m_config.tournament_reminder_standard_only, m_config.tournament_reminder_minutes) : "", false);
    if (auto* app = Root("app")) {
        Rml::ElementList widgets;
        app->GetElementsByClassName(widgets, "live-widget");
        for (auto* widget : widgets) {
            if (Attribute(widget, "data-live-widget") == "tournaments")
                SetElementRml(widget, enabled ? TournamentUi::RenderWidget(*snapshot, now) : "", false);
        }
    }
}

void RmlUiController::RebuildToast() {
    if (m_statusMessage.empty() || (m_statusUntilMs && SteadyNowMs() >= m_statusUntilMs)) {
        m_statusMessage.clear();
        m_statusUntilMs = 0;
        SetRootRml("toast-root", "");
        return;
    }
    SetRootRml("toast-root", "<div class='toast " + std::string(m_statusCompact ? "tournament-compact" : m_statusError ? "error"
                                                                                                                       : "success") +
                                 "'>" + Escape(m_statusMessage) + "</div>");
}

void RmlUiController::ShowToast(std::string message, bool error, bool compact) {
    m_statusMessage = std::move(message);
    m_statusError = error;
    m_statusCompact = compact;
    m_statusUntilMs = SteadyNowMs() + (compact ? 3000 : 5000);
    RebuildToast();
}

std::string RmlUiController::ControlValue(Rml::Element* target) const {
    if (!target) return {};
    if (auto* control = dynamic_cast<Rml::ElementFormControl*>(target)) return control->GetValue().c_str();
    return Attribute(target, "value");
}

bool RmlUiController::EventChecked(Rml::Event& event, Rml::Element* target) const {
    return event.GetParameter<bool>("checked", target && target->HasAttribute("checked"));
}

void RmlUiController::FinishDatabaseMerge(const DbMergeResult& result) {
    if (!result.success) {
        ShowToast("Couldn't merge the database: " + (result.error.empty() ? std::string("unknown error.") : result.error), true);
        return;
    }
    const std::string alreadyThere = result.matchesSkipped > 0
                                         ? " " + std::to_string(result.matchesSkipped) + " were already in your history."
                                         : std::string();
    if (result.matchesImported > 0) {
        ShowToast("Merged " + std::to_string(result.matchesImported) +
                  (result.matchesImported == 1 ? " match." : " matches.") + alreadyThere);
    } else {
        ShowToast("No new matches to merge." + alreadyThere);
    }
    if (m_state) {
        {
            std::unique_lock lock(m_state->history.mutex);
            m_state->history.lifetimeMmrX.clear();
            m_state->history.lifetimeMmrY.clear();
            m_state->history.recentSavedMatches.clear();
            m_state->history.pendingRecentMatches.clear();
            m_state->history.recentSavedMatchesLoaded = false;
            m_state->history.version++;
        }
        {
            std::lock_guard lock(m_state->ui.dbStatsMutex);
            m_state->ui.cachedDbStats = {};
            m_state->ui.dbStatsDirty.store(true);
            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
        }
    }
    m_lastDbFetchPrimaryId.clear();
    m_lastLifetimeHistoryPrimaryId.clear();
    m_lastRecentMatchHistoryPrimaryId.clear();
    m_detailedStatsSummaryRequested = false;
    RefreshAsyncData();
    RebuildSettings();
    RebuildVisibleUi(true);
}

void RmlUiController::DeleteLocalHistory() {
    StartBackgroundTask("Deleting match history...", [this](std::stop_token /*stop*/) -> std::function<void()> {
        std::string error;
        bool ok = m_dbManager && m_dbManager->DeleteLocalMatchHistory(error);
        if (ok) {
            DeleteFileA((Storage::GetDataDirectory() + "matches.jsonl").c_str());
            DeleteFileA((Storage::GetDataDirectory() + "mmr_history.jsonl").c_str());
        }
        return [this, ok, error = std::move(error)]() {
            if (!ok) {
                ShowToast("Delete failed: " + (error.empty() ? std::string("database is unavailable.") : error), true);
                RebuildSettings();
                return;
            }
            if (m_state) {
                if (m_state->liveMatch) m_state->liveMatch->SetLocalPrimaryId("");
                {
                    std::unique_lock lock(m_state->history.mutex);
                    m_state->history.lifetimeMmrX.clear();
                    m_state->history.lifetimeMmrY.clear();
                    m_state->history.recentSavedMatches.clear();
                    m_state->history.pendingRecentMatches.clear();
                    m_state->history.recentSavedMatchesLoaded = true;
                    m_state->history.version++;
                }
                {
                    std::lock_guard lock(m_state->ui.dbStatsMutex);
                    m_state->ui.cachedDbStats = {};
                    m_state->ui.dbStatsDirty.store(true);
                    m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
                }
            }
            Config::Update([](ConfigData& c) { c.last_primary_id.clear(); c.known_primary_ids.clear(); });
            m_lastDbFetchPrimaryId.clear();
            m_lastLifetimeHistoryPrimaryId.clear();
            m_lastRecentMatchHistoryPrimaryId.clear();
            m_detailedStatsSummaryRequested = false;
            RefreshAsyncData();
            RebuildSettings();
            RebuildVisibleUi(true);
            ShowToast("Local match history deleted.", false);
        };
    });
    RebuildSettings();
}

bool RmlUiController::CheckStatsApi(bool repair, bool showToast) {
    if (!m_state) return false;
    StatsApiConfig::CheckResult oldResult;
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        oldResult = m_state->ui.statsApiResult;
    }
    std::string path = m_config.rocket_league_stats_api_config_path;
    if (path.empty()) {
        path = StatsApiConfig::DetectConfigPath();
        if (path.empty()) path = oldResult.path;
    }
    if (repair && !path.empty()) {
        // The elevated updater can wait on a UAC prompt indefinitely, so it never runs on the render thread.
        const bool started = StartBackgroundTask({}, [this, path, port = m_config.port, showToast](std::stop_token stop) -> std::function<void()> {
            const bool repaired = ExternalUpdaterLauncher::RepairStatsApiConfig(path, port, stop);
            auto result = StatsApiConfig::VerifyConfig(path, port);
            if (!repaired || result.status != StatsApiConfig::Status::Valid) {
                result.message = "Automatic repair did not complete. If an elevation prompt appeared, approve it, or manually set PacketSendRate=30 and Port=" + std::to_string(port) + ".";
            }
            return [this, result = std::move(result), showToast] {
                {
                    std::lock_guard lock(m_state->ui.statsApiMutex);
                    m_state->ui.statsApiResult = result;
                }
                m_state->ui.statsApiChecked.store(true);
                if (showToast) ShowToast(StatsApiConfig::GetStatusMessage(result.status), result.status != StatsApiConfig::Status::Valid);
                if (m_state->ui.showMenu.load()) RebuildSettings();
                if (m_onboardingVisible) RefreshOnboarding();
                if (m_statsApiPrompt == StatsApiPrompt::Repairing) {
                    SetStatsApiPrompt(result.status == StatsApiConfig::Status::Valid ? StatsApiPrompt::Repaired
                                                                                     : StatsApiPrompt::Failed);
                }
            };
        });
        if (started) {
            std::lock_guard lock(m_state->ui.statsApiMutex);
            m_state->ui.statsApiResult.message = "Repairing the Stats API config. Approve the Windows prompt if it appears.";
        }
        return started;
    }
    auto result = StatsApiConfig::VerifyConfig(path, m_config.port);
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        m_state->ui.statsApiResult = result;
    }
    m_state->ui.statsApiChecked.store(true);
    if (showToast) {
        ShowToast(StatsApiConfig::GetStatusMessage(result.status), result.status != StatsApiConfig::Status::Valid);
    }
    return true;
}

void RmlUiController::BrowseStatsApiPath() {
    const std::string selected = PromptForFile(m_hwnd, L"Select Rocket League's DefaultStatsAPI.ini (in TAGame\\Config)",
                                               L"Stats API config (*StatsAPI.ini)", L"*StatsAPI.ini");
    if (selected.empty()) return;
    std::string normalized;
    if (!ValidateStatsApiPath(selected, normalized, m_statsApiPathError)) {
        ShowToast(m_statsApiPathError, true);
        return;
    }
    m_statsApiPathError.clear();
    Config::Update([&](ConfigData& c) { c.rocket_league_stats_api_config_path = normalized; });
    m_config = Config::Read();
    CheckStatsApi(false);
}

bool RmlUiController::StartBackgroundTask(std::string startedToast, std::function<std::function<void()>(std::stop_token)> work) {
    if (!m_backgroundTaskService) return false;
    const bool started = m_backgroundTaskService->StartTask(
        std::move(startedToast),
        [this, work = std::move(work)](std::stop_token stop) mutable {
            auto completion = work(stop);
            m_backgroundTaskRunning.store(false, std::memory_order_relaxed);
            return completion;
        },
        [this](std::string msg, bool err) { ShowToast(std::move(msg), err); });
    m_backgroundTaskRunning.store(started && m_backgroundTaskService->IsRunning(), std::memory_order_relaxed);
    m_renderDirty = true;
    return started;
}

void RmlUiController::PublishBackgroundTaskCompletion() {
    if (m_backgroundTaskService) {
        m_backgroundTaskService->PublishCompletion();
        m_backgroundTaskRunning.store(m_backgroundTaskService->IsRunning());
    }
}
