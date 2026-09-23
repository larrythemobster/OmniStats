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

RmlUiController::RmlUiController(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)), m_systemInterface(nullptr) {
    if (m_state) m_lastShowMenu = m_state->ui.showMenu.load();
}

RmlUiController::~RmlUiController() {
    Shutdown();
}

// OmniStats ships its own typefaces so the client matches the web brand and does
// not inherit whatever Segoe UI revision a machine happens to have. Faces are
// registered with an explicit family and weight: static Inter/JetBrains Mono files
// name themselves "Inter SemiBold" and friends, which would otherwise register as
// separate families.
bool RmlUiController::LoadBundledFonts() {
    struct BundledFont {
        const char* resource;
        const char* file;
        const char* family;
        int weight;
        bool required;
    };
    static constexpr BundledFont kFonts[] = {
        {"FONT_UI_REGULAR", "Inter-Regular.ttf", "Inter", 400, true},
        {"FONT_UI_SEMIBOLD", "Inter-SemiBold.ttf", "Inter", 600, false},
        {"FONT_UI_BOLD", "Inter-Bold.ttf", "Inter", 700, false},
        {"FONT_MONO_REGULAR", "JetBrainsMono-Regular.ttf", "JetBrains Mono", 400, false},
        {"FONT_MONO_BOLD", "JetBrainsMono-Bold.ttf", "JetBrains Mono", 700, false},
        {"FONT_DISPLAY_REGULAR", "RussoOne-Regular.ttf", "Russo One", 400, false},
    };

    m_fontBlobs.clear();
    for (const auto& font : kFonts) {
        // Packaged builds carry the faces as RCDATA; test and unpacked builds have
        // no application resources, so fall back to the source tree like
        // RmlFileInterface does for RML/RCSS.
        Rml::Span<const Rml::byte> data = EmbeddedResource(font.resource);
        if (data.empty()) {
            auto blob = ReadFontFile(font.file);
            if (!blob.empty()) {
                m_fontBlobs.push_back(std::move(blob));
                const auto& stored = m_fontBlobs.back();
                data = {stored.data(), stored.size()};
            }
        }
        const bool loaded = !data.empty() &&
                            Rml::LoadFontFace(data, font.family, Rml::Style::FontStyle::Normal,
                                              static_cast<Rml::Style::FontWeight>(font.weight));
        if (loaded) continue;
        if (font.required) {
            std::cerr << "[RmlUi] Failed to load font " << font.file
                      << "; refusing to start with an unreadable UI.\n";
            return false;
        }
        std::cerr << "[RmlUi] Warning: failed to load font " << font.file << ".\n";
    }

    // Player names are arbitrary user data. Inter covers Latin/Greek/Cyrillic, so
    // register system faces as fallbacks for everything else instead of drawing
    // missing-glyph boxes.
    for (const char* fallback : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msgothic.ttc"})
        Rml::LoadFontFace(fallback, true);

    return true;
}

bool RmlUiController::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, float dpiScale) {
    Shutdown();
    m_hwnd = hwnd;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_dpiScale = SanitizedScale(dpiScale);
    m_systemInterface.SetWindow(hwnd);

    if (!m_renderInterface.Initialize(device, context, m_width, m_height)) return false;
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
    if (!m_liveModel.Create(m_context)) {
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

    m_document = m_context->LoadDocument("res://main.rml");
    if (!m_document) {
        Shutdown();
        return false;
    }
    m_document->Show();

    // Keep an immutable copy of the document's packaged RCSS. Theme changes are
    // layered on top as a stylesheet, so newly created DOM automatically gets
    // the current colors without rescanning every matching element after each
    // SetInnerRML call.
    if (const auto* packagedStyle = m_document->GetStyleSheetContainer()) {
        if (auto emptyStyle = Rml::Factory::InstanceStyleSheetString("#__omnistats_theme_base__ { color: inherit; }"))
            m_baseStyleSheet = packagedStyle->CombineStyleSheetContainer(*emptyStyle);
    }

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
        m_liveModel.Reset();
    }
    m_baseStyleSheet.reset();
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
    m_renderDirty = false;
}

bool RmlUiController::ShouldRender() const {
    if (!m_context) return false;
    // Keep the last presented swap-chain image until application data/input
    // changes or RmlUi's previously scheduled update deadline arrives. This
    // preserves transitions, smooth scrolling and caret blinking without
    // forcing static UI through a full Update/Render/Present every frame.
    if (m_renderDirty) return true;
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
           m_config.second_monitor_mode || m_drag.kind != DragKind::None;
}

void RmlUiController::Update(const ConfigData& config, bool configChanged, uint64_t configRevision) {
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
                       !sameColor(config.themeGraphBaseline, m_config.themeGraphBaseline);

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
    if (themeChanged) {
        UpdateThemeProperties();
        RefreshThemeEditorControls();
    }

    const int64_t nowMs = SteadyNowMs();
    if (m_state && nowMs >= m_nextUpdateCheckPollMs) {
        ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
            m_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Periodic);
        // The launcher applies the one-hour success interval and a shorter retry
        // delay after failures. Poll the gate once per minute instead of every frame.
        m_nextUpdateCheckPollMs = nowMs + 60 * 1000;
    }

    RebuildVisibleUi(false, configChanged && !localConfigEcho);
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
    if (replayPointer && m_hasPointerPosition && m_context)
        m_context->ProcessMouseMove(m_lastPointerX, m_lastPointerY, 0);
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
    {
        std::shared_lock lock(m_state->game.mutex);
        const auto version = m_state->game.version.load();
        if (version != m_lastGameVersion) {
            m_lastGameVersion = version;
            m_snap.matchGuid = m_state->game.matchGuid;
            m_snap.arenaName = m_state->game.arenaName;
            m_snap.score[0] = m_state->game.score[0];
            m_snap.score[1] = m_state->game.score[1];
            m_snap.inMatch = m_state->game.inMatch.load();
            m_snap.inReplay = m_state->game.inReplay.load();
            m_snap.matchFinalized = m_state->game.matchFinalized;
            m_snap.myPrimaryId = m_state->game.myPrimaryId;
            m_snap.myTeam = m_state->game.myTeam;
            m_snap.currentMatch = m_state->game.currentMatch;
            m_snap.sessionTotals = m_state->game.sessionTotals;
            m_snap.roster = m_state->game.roster;
            m_snap.sessionGamemodes = m_state->game.sessionGamemodes;
            m_snap.lastMatchWasVoid = m_state->game.lastMatchWasVoid;
            m_snap.lastMatchVoidReason = m_state->game.lastMatchVoidReason;
            m_snap.matchSummaryScore[0] = m_state->game.matchSummaryScore[0];
            m_snap.matchSummaryScore[1] = m_state->game.matchSummaryScore[1];
            m_snap.matchSummaryMyTeam = m_state->game.matchSummaryMyTeam;
            m_snap.matchSummaryWinnerTeam = m_state->game.matchSummaryWinnerTeam;
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
}

void RmlUiController::RefreshAsyncData() {
    if (!m_dbManager || !m_state) return;
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

    // Do not replace DOM while a pointer target is active, but keep safe leaf
    // telemetry flowing. This preserves drag/click stability without freezing
    // live counters for the entire interaction. Structural versions remain
    // pending and are reconciled as soon as the interaction ends.
    if (m_drag.kind != DragKind::None || (m_pointerPressed && !force)) {
        RefreshLiveUi(false, false);
        return;
    }

    const bool showMenu = m_state && m_state->ui.showMenu.load(std::memory_order_relaxed);
    const bool showOverlay = m_state && m_state->ui.showOverlay.load(std::memory_order_relaxed);
    const bool showSession = m_state && m_state->ui.showSessionView.load(std::memory_order_relaxed);
    bool showSummary = m_state && m_state->ui.showMatchSummary.load(std::memory_order_relaxed);
    if (showSummary && m_state && (SteadyNowMs() - m_state->ui.matchSummaryStartMs.load(std::memory_order_relaxed)) >= 30000) {
        m_state->ui.showMatchSummary.store(false, std::memory_order_relaxed);
        showSummary = false;
    }
    const bool dashboardEdit = m_state && m_state->ui.dashboardLayoutEditMode.load(std::memory_order_relaxed);
    const bool showGraphView = m_state && m_state->ui.showGraphView.load(std::memory_order_relaxed);
    const bool h2hExpanded = m_state && m_state->ui.h2hExpanded.load(std::memory_order_relaxed);

    // Runtime state that changes the dashboard/overlay structure but is not part
    // of Config. Keep this intentionally tiny; telemetry versions do not belong
    // here and must never invalidate a large root. Hash it directly so the 10 Hz
    // reconciliation path does not allocate/build fingerprint strings.
    uint64_t runtimeStructuralHash = kFnvOffset;
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showMenu));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showOverlay));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showSession));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showSummary));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(dashboardEdit));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(showGraphView));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(h2hExpanded));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_snap.inMatch));
    HashAppend(runtimeStructuralHash, static_cast<uint64_t>(m_config.second_monitor_mode));
    if (m_state && m_config.second_monitor_mode) {
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
           << m_config.show_match_summary << '|' << m_config.show_running_indicator << '|'
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
           << m_config.show_account_wins_overlay << '|' << m_config.show_demo_tracker_overlay << '|'
           << m_config.show_previous_games_summary << '|' << m_config.previous_games_limit << '|'
           << m_config.show_streaks_stats << '|' << m_config.show_longest_loss_streak << '|'
           << m_config.show_gamemode_breakdown << '|' << m_config.show_gamemode_record_1v1 << '|'
           << m_config.show_gamemode_record_2v2 << '|' << m_config.show_gamemode_record_3v3 << '|'
           << m_config.gamemode_breakdown_scope << '|' << m_config.mmr_category << '|'
           << m_config.auto_switch_mmr_category << '|' << m_config.graph_follow_current_playlist << '|'
           << m_config.graph_mmr_category << '|' << m_config.show_extra_playlists << '|'
           << m_config.ui_scale << '|' << m_config.imperial_units << '|' << m_config.crossbar_display_mode << '|'
           << m_config.use_roman_numerals << '|' << m_config.key_cycle << '|' << m_config.key_expand << '|'
           << m_config.key_session << '|';
        fp << m_config.overlay_layout.version << '|' << m_config.overlay_layout.toolboxOpen << '|';
        for (const auto& container : m_config.overlay_layout.containers) {
            fp << container.id << ':' << container.x << ',' << container.y << ',' << container.w << ',' << container.h << '[';
            for (auto widget : container.widgets)
                fp << static_cast<int>(widget) << ',';
            fp << "];";
        }
        fp << '|' << m_config.dashboard_layout.version << '|' << m_config.dashboard_layout.leftColumnWeight << '|';
        for (const auto& widget : m_config.dashboard_layout.widgets)
            fp << static_cast<int>(widget.id) << ',' << static_cast<int>(widget.zone) << ',' << widget.order << ','
               << widget.height << ',' << widget.collapsed << ';';

        const std::string renderConfigFingerprint = fp.str();
        renderConfigChanged = force || m_lastRenderConfigFingerprint != renderConfigFingerprint;
        m_lastRenderConfigFingerprint = renderConfigFingerprint;
    }

    const bool modeChanged = m_lastSecondMonitor != m_config.second_monitor_mode;
    const bool structuralDirty = force || renderConfigChanged || modeChanged ||
                                 m_lastRuntimeStructuralHash != runtimeStructuralHash;

    bool rebuiltMainStructure = false;
    if (structuralDirty) {
        if (m_config.second_monitor_mode) {
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
        HashAppend(settingsHash, static_cast<uint64_t>(m_confirmReplayUploads));
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
            HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastKeyboardKeyPressed.load(std::memory_order_relaxed))));
            HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastControllerButtonPressed.load(std::memory_order_relaxed))));
            HashAppend(settingsHash, static_cast<uint64_t>(static_cast<int64_t>(m_state->ui.lastRawControllerButtonPressed.load(std::memory_order_relaxed))));
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
        if (m_settingsPage == SettingsPage::General || m_settingsPage == SettingsPage::Ranks) {
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
                    if (m_settingsPage == SettingsPage::Ranks) {
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
    const bool showLifetimeGraph = m_state->history.showLifetimeGraph.load(std::memory_order_relaxed);
    const bool rosterCategoryChanged = force || m_lastRosterMmrCategory != rosterCategory;
    const bool graphCategoryChanged = force || m_lastGraphMmrCategory != graphCategory;
    const bool graphOffsetChanged = force || m_lastGraphOffset != graphOffset;
    const bool lifetimeGraphChanged = force || m_lastShowLifetimeGraph != showLifetimeGraph;

    const bool structuralPending = gameChanged || historyChanged || dbChanged || rosterCategoryChanged ||
                                   graphCategoryChanged || graphOffsetChanged || lifetimeGraphChanged;
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
                for (Rml::Element* element : groupIt->second)
                    SetElementRml(element, rml, false);
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
                                                 DashboardLayout::WidgetId::GamemodeBreakdown})
                refreshWidget(id, true);
        }

        // The roster's high-frequency goals/saves/etc. are leaf updates below. Only
        // rank/identity/team changes alter the roster subtree. game.version is a
        // coarse telemetry version, so use a cheap order-independent hash here
        // instead of sorting IDs and constructing a large fingerprint string on
        // every telemetry tick.
        if (gameChanged || rosterCategoryChanged || m_liveDomNeedsPrime) {
            uint64_t rosterHash = kFnvOffset;
            HashAppend(rosterHash, m_snap.myPrimaryId);
            HashAppend(rosterHash, static_cast<uint64_t>(rosterCategory));
            HashAppend(rosterHash, static_cast<uint64_t>(m_snap.roster.size()));

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
                        SetElementRml(element, rml, false);
                    }
                }
            }
        }
    }

    if (leafGameChanged || primeStructure) {
        // Bound text views re-evaluate only for the variables marked dirty here.
        bool changed = m_liveModel.SetValues(ComputeLiveValues(m_snap, m_config));
        for (const auto& [id, player] : m_snap.roster)
            changed |= m_liveModel.SetPlayer(m_liveModel.PlayerSlot(id), ComputeLivePlayerStat(player));
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
    if (ToLower(path.filename().string()) != "defaultstatsapi.ini") {
        error = "Filename must be exactly DefaultStatsAPI.ini";
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
    case SettingsPage::General:
        return "General";
    case SettingsPage::Cards:
        return "Cards";
    case SettingsPage::Ranks:
        return "Ranks";
    case SettingsPage::Shortcuts:
        return "Shortcuts";
    case SettingsPage::Appearance:
        return "Appearance";
    case SettingsPage::Integrations:
        return "Services & API";
    case SettingsPage::Data:
        return "Data";
    case SettingsPage::Troubleshooting:
        return "Troubleshooting";
    }
    return "General";
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
    }
    return "live-roster";
}

std::string RmlUiController::PlaylistImageForName(const std::string& playlist) {
    const int index = PlaylistResourceIndex(playlist);
    return index >= 0 ? "res://images/Playlists/" + std::to_string(index) + ".png" : "";
}

void RmlUiController::RebuildToast() {
    if (m_statusMessage.empty() || (m_statusUntilMs && SteadyNowMs() >= m_statusUntilMs)) {
        m_statusMessage.clear();
        m_statusUntilMs = 0;
        SetRootRml("toast-root", "");
        return;
    }
    SetRootRml("toast-root", "<div class='toast " + std::string(m_statusError ? "error" : "success") + "'>" + Escape(m_statusMessage) + "</div>");
}

void RmlUiController::ShowToast(std::string message, bool error) {
    m_statusMessage = std::move(message);
    m_statusError = error;
    m_statusUntilMs = SteadyNowMs() + 5000;
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

void RmlUiController::DeleteLocalHistory() {
    std::string error;
    if (!m_dbManager || !m_dbManager->DeleteLocalMatchHistory(error)) {
        ShowToast("Delete failed: " + (error.empty() ? std::string("database is unavailable.") : error), true);
        return;
    }
    DeleteFileA((Storage::GetDataDirectory() + "matches.jsonl").c_str());
    DeleteFileA((Storage::GetDataDirectory() + "mmr_history.jsonl").c_str());
    if (m_state) {
        {
            std::unique_lock lock(m_state->game.mutex);
            m_state->game.myPrimaryId.clear();
            m_state->game.myTeam = -1;
            m_state->game.version++;
        }
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
    ShowToast("Deleted local history and identity.");
}

void RmlUiController::CheckStatsApi(bool repair, bool showToast) {
    if (!m_state) return;
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
    bool repaired = true;
    if (repair && !path.empty()) repaired = ExternalUpdaterLauncher::RepairStatsApiConfig(path, m_config.port);
    auto result = StatsApiConfig::VerifyConfig(path, m_config.port);
    if (repair && (!repaired || result.status != StatsApiConfig::Status::Valid)) {
        result.message = "Automatic repair did not complete. If an elevation prompt appeared, approve it, or manually set PacketSendRate=30 and Port=" + std::to_string(m_config.port) + ".";
    }
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        m_state->ui.statsApiResult = result;
    }
    m_state->ui.statsApiChecked.store(true);
    if (showToast) {
        ShowToast(StatsApiConfig::GetStatusMessage(result.status), result.status != StatsApiConfig::Status::Valid);
    }
}
