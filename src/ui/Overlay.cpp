#include "Overlay.hpp"

#include <chrono>
#include <dwmapi.h>
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "platform/InputManager.hpp"
#include "ui/window/WindowUtils.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <timeapi.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

namespace {
    bool s_isQuitting = false;

    bool IsRmlInputMessage(UINT msg) {
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEMOVE:
        case WM_MOUSELEAVE:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
            return true;
        default:
            return false;
        }
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (InputManager::HandleWindowMessage(msg, wParam, lParam)) return 0;

    Overlay* overlay = nullptr;
    const LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (ptr != 0) overlay = reinterpret_cast<Overlay*>(ptr);

    if (overlay && overlay->m_rmlUi && IsRmlInputMessage(msg) &&
        overlay->m_rmlUi->ProcessWindowMessage(hWnd, msg, wParam, lParam)) {
        return 0;
    }

    switch (msg) {
    case WM_SETCURSOR:
        if (overlay && overlay->m_rmlUi && LOWORD(lParam) == HTCLIENT && overlay->m_rmlUi->ApplyMouseCursor()) {
            return TRUE;
        }
        break;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED && overlay) {
            const int width = static_cast<int>(LOWORD(lParam));
            const int height = static_cast<int>(HIWORD(lParam));
            if (width > 0 && height > 0) {
                overlay->m_pendingWidth.store(width, std::memory_order_relaxed);
                overlay->m_pendingHeight.store(height, std::memory_order_relaxed);
                overlay->m_resizePending.store(true, std::memory_order_release);
                overlay->SaveSecondMonitorWindowBounds();
            }
        }
        return 0;

    case WM_MOVE:
    case WM_EXITSIZEMOVE:
        if (overlay) overlay->SaveSecondMonitorWindowBounds();
        return 0;

    case WM_DISPLAYCHANGE:
        if (overlay) overlay->UpdateWindowPosition(false);
        return 0;

    case WM_DPICHANGED:
        if (overlay) overlay->HandleDpiChanged(LOWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        return 0;

    case WM_NCCALCSIZE:
        if (wParam && ((overlay && overlay->m_frameConfig.second_monitor_mode) ||
                       (!overlay && Config::Read().second_monitor_mode))) return 0;
        break;

    case WM_NCHITTEST:
        if (overlay && overlay->m_setupWindow) {
            const LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
            if (hit != HTCLIENT) return hit;
            POINT pt{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                     static_cast<int>(static_cast<short>(HIWORD(lParam)))};
            ScreenToClient(hWnd, &pt);
            RECT rect{};
            GetClientRect(hWnd, &rect);
            // Header doubles as the title bar; the Skip button on its right stays clickable.
            if (pt.y <= overlay->SetupWindowPixels(64.0f) &&
                pt.x < rect.right - overlay->SetupWindowPixels(150.0f)) return HTCAPTION;
            return HTCLIENT;
        }
        if (overlay && overlay->m_frameConfig.second_monitor_mode) {
            const LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
            if (hit == HTCLIENT) {
                POINT pt{static_cast<int>(static_cast<short>(LOWORD(lParam))),
                         static_cast<int>(static_cast<short>(HIWORD(lParam)))};
                ScreenToClient(hWnd, &pt);
                RECT rect{};
                GetClientRect(hWnd, &rect);
                const int width = rect.right - rect.left;
                const int height = rect.bottom - rect.top;
                if (!IsZoomed(hWnd)) {
                    const int borderSize = static_cast<int>(8.0f * overlay->m_dpiScale);
                    if (pt.x <= borderSize && pt.y <= borderSize) return HTTOPLEFT;
                    if (pt.x >= width - borderSize && pt.y <= borderSize) return HTTOPRIGHT;
                    if (pt.x <= borderSize && pt.y >= height - borderSize) return HTBOTTOMLEFT;
                    if (pt.x >= width - borderSize && pt.y >= height - borderSize) return HTBOTTOMRIGHT;
                    if (pt.x <= borderSize) return HTLEFT;
                    if (pt.x >= width - borderSize) return HTRIGHT;
                    if (pt.y <= borderSize) return HTTOP;
                    if (pt.y >= height - borderSize) return HTBOTTOM;
                }
                const int titleBarHeight = static_cast<int>(34.0f * overlay->m_dpiScale);
                const int controlsWidth = static_cast<int>(420.0f * overlay->m_dpiScale);
                if (pt.y >= 0 && pt.y <= titleBarHeight) {
                    if (pt.x >= width - controlsWidth) return HTCLIENT;
                    return HTCAPTION;
                }
            }
            return hit;
        }
        break;

    case WM_TOGGLE_MODE:
        if (overlay) {
            Config::Update([](ConfigData& c) { c.second_monitor_mode = !c.second_monitor_mode; });
            overlay->m_frameConfig = Config::Read();
            overlay->m_lastSecondMonitorMode = overlay->m_frameConfig.second_monitor_mode;
            overlay->UpdateWindowStyle();
            overlay->UpdateWindowPosition();
        }
        return 0;

    case WM_OPEN_INSIGHTS:
        if (overlay && overlay->m_rmlUi) overlay->m_rmlUi->OpenInsights();
        return 0;

    case WM_OPEN_HISTORY:
        if (overlay && overlay->m_rmlUi) overlay->m_rmlUi->OpenHistory();
        return 0;
    case WM_OPEN_ACCOUNTS:
        if (overlay && overlay->m_rmlUi) overlay->m_rmlUi->OpenAccounts();
        return 0;

    case WM_GETMINMAXINFO:
        if ((overlay && overlay->m_frameConfig.second_monitor_mode && !overlay->m_setupWindow) ||
            (!overlay && Config::Read().second_monitor_mode)) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            float dpiScale = overlay ? overlay->m_dpiScale : 1.0f;
            if (!overlay) {
                using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
                static HMODULE user32 = GetModuleHandleW(L"user32.dll");
                static auto getDpiForWindow = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                if (getDpiForWindow) dpiScale = static_cast<float>(getDpiForWindow(hWnd)) / 96.0f;
            }
            const SIZE minimum = GetSecondMonitorMinimumSize(dpiScale);
            mmi->ptMinTrackSize.x = minimum.cx;
            mmi->ptMinTrackSize.y = minimum.cy;
            return 0;
        }
        break;

    case WM_CLOSE:
    case WM_DESTROY:
        if (!s_isQuitting) {
            s_isQuitting = true;
            PostQuitMessage(0);
        }
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

Overlay::Overlay(std::shared_ptr<SessionState> state,
                 std::shared_ptr<DatabaseManager> dbManager,
                 std::shared_ptr<AccountManagerService> accounts,
                 std::shared_ptr<RemoteConfig> remoteConfig,
                 std::shared_ptr<Tournaments::TournamentScheduleService> tournamentService,
                 std::shared_ptr<TelemetryManager> telemetry,
                 std::shared_ptr<AccountClient> accountClient)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)), m_accounts(std::move(accounts)),
      m_accountClient(std::move(accountClient)), m_remoteConfig(std::move(remoteConfig)), m_tournamentService(std::move(tournamentService)), m_telemetry(std::move(telemetry)) {}

Overlay::~Overlay() {
    Shutdown();
}

bool Overlay::Initialize() {
    s_isQuitting = false;
    const HICON appIconBig = LoadAppIcon(32, 32);
    const HICON appIconSmall = LoadAppIcon(16, 16);

    m_window = std::make_unique<OverlayWindow>();
    if (!m_window->Create(appIconBig, appIconSmall, WndProc)) return false;

    m_hwnd = m_window->Handle();
    m_dpiScale = m_window->DpiScale();
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    m_trayIcon = std::make_unique<TrayIcon>(m_hwnd);
    if (!m_trayIcon->Initialize()) {
        std::cout << "[Tray] Tray icon failed to initialize. Continuing without tray icon.\n";
        m_trayIcon.reset();
    }

    m_frameConfig = Config::Read();
    m_lastSecondMonitorMode = m_frameConfig.second_monitor_mode;
    UpdateWindowStyle();
    UpdateWindowPosition();

    m_d3d11 = std::make_unique<D3D11Device>();
    if (!m_d3d11->Create(m_hwnd)) return false;
    UpdateWindowStyle();

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = std::max(client.right - client.left, 1L);
    const int height = std::max(client.bottom - client.top, 1L);

    m_rmlUi = std::make_unique<RmlUiController>(m_state, m_dbManager, m_accounts, m_remoteConfig, m_tournamentService, m_telemetry, m_accountClient);
    if (!m_rmlUi->Initialize(m_hwnd, m_d3d11->Device(), m_d3d11->Context(), width, height, m_dpiScale)) {
        std::cout << "[RmlUi] Failed to initialize UI.\n";
        return false;
    }
    SyncSetupWindow();
    m_foreground = std::make_unique<ForegroundDetector>();
    m_foreground->Initialize(m_hwnd);

    timeBeginPeriod(1);
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    return true;
}

void Overlay::UpdateWindowStyle() {
    if (!m_window) return;
    const bool compositionEnabled = !m_d3d11 || m_d3d11->DCompDevice() != nullptr;
    if (m_setupWindow) {
        m_window->ApplySetupFrame(SetupWindowPixels(680.0f), SetupWindowPixels(540.0f), compositionEnabled);
        return;
    }
    const bool interactiveOverlay = m_rmlUi && m_rmlUi->WantsInteraction();
    m_window->UpdateStyle(m_frameConfig.second_monitor_mode, m_state->ui.showMenu.load() || interactiveOverlay, compositionEnabled);
}

int Overlay::SetupWindowPixels(float dp) const {
    return static_cast<int>(dp * m_dpiScale * RmlUiDetail::SanitizedUiScale(m_frameConfig.ui_scale) + 0.5f);
}

void Overlay::SyncSetupWindow() {
    const bool setup = m_rmlUi && m_rmlUi->IsOnboardingVisible();
    if (setup == m_setupWindow) return;
    m_setupWindow = setup;
    UpdateWindowStyle();
    UpdateWindowPosition();
    if (m_rmlUi) m_rmlUi->RequestRender();
}

void Overlay::RunLoop() {
    bool done = false;
    bool isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
    bool wasRLActive = false;
    auto lastFrameTime = std::chrono::steady_clock::now();
    auto lastUiDataUpdate = lastFrameTime - std::chrono::milliseconds(100);
    uint64_t lastConfigRevision = Config::Revision();
    // The revision edge must survive iterations that never reach the UI update
    // below (window hidden, focus-transition delay, device revalidation).
    // Consuming it at read time would drop config commits made while hidden and
    // leave the controller rendering the previous mode's DOM.
    bool configDirty = false;
    const auto inputSignature = [this] {
        const auto& ui = m_state->ui;
        return static_cast<unsigned>(ui.showOverlay.load(std::memory_order_relaxed)) |
               static_cast<unsigned>(ui.showMenu.load(std::memory_order_relaxed)) << 1 |
               static_cast<unsigned>(ui.showSessionView.load(std::memory_order_relaxed)) << 2 |
               static_cast<unsigned>(ui.showGraphView.load(std::memory_order_relaxed)) << 3 |
               static_cast<unsigned>(ui.h2hExpanded.load(std::memory_order_relaxed)) << 4 |
               static_cast<unsigned>(ui.dashboardLayoutEditMode.load(std::memory_order_relaxed)) << 5;
    };
    unsigned lastInputSignature = inputSignature();

    while (!done) {
        if (m_state->ui.appExitRequested.load()) {
            std::cout << "[Overlay] Exit requested by external updater.\n";
            break;
        }

        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
            }
        }

        const uint64_t configRevision = Config::Revision();
        if (configRevision != lastConfigRevision) {
            m_frameConfig = Config::Read();
            lastConfigRevision = configRevision;
            configDirty = true;
        }
        if (m_consentHandler && Config::HasCurrentConsent(m_frameConfig)) {
            const auto handler = std::move(m_consentHandler);
            m_consentHandler = nullptr;
            handler();
        }
        const bool configChanged = configDirty;
        if (m_frameConfig.second_monitor_mode != m_lastSecondMonitorMode) {
            m_lastSecondMonitorMode = m_frameConfig.second_monitor_mode;
            UpdateWindowStyle();
            UpdateWindowPosition();
        }

        if (m_resizePending.exchange(false, std::memory_order_acq_rel)) {
            const int width = m_pendingWidth.load(std::memory_order_relaxed);
            const int height = m_pendingHeight.load(std::memory_order_relaxed);
            if (width > 0 && height > 0) ResizeSwapChain(width, height);
        }

        if (!m_d3d11 || !m_d3d11->RenderTargetView() || !m_rmlUi) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool isRLActive = false;
        HWND cachedRlHwnd = nullptr;
        const bool checkFocus = m_frameConfig.require_rl_focus || m_frameConfig.second_monitor_mode;
        if (checkFocus && m_foreground) {
            isRLActive = m_foreground->IsRocketLeagueActive();
            cachedRlHwnd = m_foreground->CachedRocketLeagueWindow();
        }

        bool shouldDraw = true;
        if (m_frameConfig.require_rl_focus) {
            shouldDraw = m_frameConfig.second_monitor_mode ? cachedRlHwnd != nullptr : isRLActive;
        }
        // First-run setup, the post-session recap and Insights must be visible
        // even when Rocket League is not running or focused.
        if (!shouldDraw && m_rmlUi->WantsAttention()) shouldDraw = true;
        if (!shouldDraw) {
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            wasRLActive = false;
            lastFrameTime = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (m_frameConfig.require_rl_focus && !m_frameConfig.second_monitor_mode && isRLActive && !wasRLActive &&
            m_d3d11 && m_d3d11->Device()) {
            const HRESULT deviceReason = m_d3d11->Device()->GetDeviceRemovedReason();
            if (deviceReason != S_OK) {
                std::cout << "[D3D11] Device unhealthy after Rocket League focus transition: 0x"
                          << std::hex << deviceReason << std::dec << "\n";
                if (!HandleDeviceLost("Rocket League focus transition", deviceReason)) done = true;
                lastFrameTime = std::chrono::steady_clock::now();
                continue;
            }
        }

        const bool wasVisible = IsWindowVisible(m_hwnd) != FALSE;
        if (!wasVisible) {
            UpdateWindowStyle();
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            m_rmlUi->RequestRender();
            SetWindowPos(m_hwnd,
                         UsesDesktopWindow() ? HWND_NOTOPMOST : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
            if (m_frameConfig.second_monitor_mode && isRLActive) {
                SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
            UpdateWindow(m_hwnd);
            isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
        } else if (ShouldRaiseSecondMonitorWindow(m_frameConfig.second_monitor_mode, isRLActive, wasRLActive, true)) {
            SetWindowPos(m_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        wasRLActive = isRLActive;

        // RmlUi layout/animation/rendering still runs every rendered frame, but
        // telemetry snapshots and DOM data reconciliation do not need to run at
        // 60-240 Hz. Ten updates per second is responsive for live stats and
        // removes a large amount of repeated map/string/allocation work. Config
        // commits are applied immediately regardless of this cadence.
        const auto uiNow = std::chrono::steady_clock::now();
        const int64_t uiNowMs = RmlUiDetail::SteadyNowMs();
        const auto nextVisibilityWakeMs = m_rmlUi->NextVisibilityWakeMs();
        const bool visibilityWakeDue = nextVisibilityWakeMs.has_value() && uiNowMs >= *nextVisibilityWakeMs;
        constexpr auto kUiDataInterval = std::chrono::milliseconds(100);
        const unsigned currentInputSignature = inputSignature();
        const bool inputChanged = currentInputSignature != lastInputSignature;
        if (configChanged || inputChanged || visibilityWakeDue || m_rmlUi->HasPendingInteractionUpdate() ||
            uiNow - lastUiDataUpdate >= kUiDataInterval) {
            m_rmlUi->Update(m_frameConfig, configChanged, lastConfigRevision);
            configDirty = false;
            lastUiDataUpdate = uiNow;
            lastInputSignature = currentInputSignature;
        }
        SyncSetupWindow();
        const bool needsInteract = m_rmlUi->WantsInteraction();
        isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
        if (needsInteract && isClickThrough) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle &= ~WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd,
                         UsesDesktopWindow() ? HWND_NOTOPMOST : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            if (!UsesDesktopWindow()) SetForegroundWindow(m_hwnd);
            isClickThrough = false;
        } else if (!needsInteract && !isClickThrough && !UsesDesktopWindow()) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            isClickThrough = true;
        }

        const bool renderFrame = m_rmlUi->ShouldRender();
        if (renderFrame) {
            const float clearColor[4] = {0.0f, 0.0f, 0.0f, UsesDesktopWindow() ? 1.0f : 0.0f};
            m_d3d11->Context()->OMSetRenderTargets(1, m_d3d11->RenderTargetViewAddress(), nullptr);
            m_d3d11->Context()->ClearRenderTargetView(m_d3d11->RenderTargetView(), clearColor);
            m_rmlUi->Render();

            const UINT syncInterval = m_frameConfig.vsync ? 1U : 0U;
            const HRESULT presentHr = m_d3d11->SwapChain()->Present(syncInterval, 0);
            if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET || presentHr == DXGI_ERROR_DEVICE_HUNG) {
                if (!HandleDeviceLost("Present", presentHr)) done = true;
                continue;
            }
            if (FAILED(presentHr)) {
                std::cout << "[D3D11] Present failed: 0x" << std::hex << presentHr << std::dec << "\n";
            }
        }

        // Present(1) supplies the wait for rendered vsync frames. Skipped
        // frames and Present(0) still need the configured frame limiter.
        if (!m_frameConfig.vsync || !renderFrame) {
            const int frameCap = m_frameConfig.overlay_fps_cap > 0 ? m_frameConfig.overlay_fps_cap : 60;
            const auto now = std::chrono::steady_clock::now();
            const auto targetDuration = std::chrono::duration<double, std::milli>(1000.0 / static_cast<double>(frameCap));
            const auto elapsed = std::chrono::duration<double, std::milli>(now - lastFrameTime);
            if (elapsed < targetDuration) std::this_thread::sleep_for(targetDuration - elapsed);
        }
        lastFrameTime = std::chrono::steady_clock::now();
    }
}

void Overlay::ShutdownBackgroundTasks() {
    if (m_rmlUi) {
        m_rmlUi->ShutdownBackgroundTasks();
    }
}

void Overlay::Shutdown() {
    timeEndPeriod(1);
    if (m_foreground) {
        m_foreground->Shutdown();
        m_foreground.reset();
    }
    if (m_rmlUi) {
        m_rmlUi->Shutdown();
        m_rmlUi.reset();
    }
    if (m_trayIcon) {
        m_trayIcon->Shutdown();
        m_trayIcon.reset();
    }
    if (m_d3d11) {
        m_d3d11->Shutdown();
        m_d3d11.reset();
    }
    if (m_window) {
        m_window->Destroy();
        m_window.reset();
    }
    m_hwnd = nullptr;
}

void Overlay::UpdateWindowPosition(bool resetSecondMonitorPlacement) {
    if (!m_window || !m_hwnd) return;
    if (m_setupWindow) {
        UpdateWindowStyle();
        return;
    }
    m_window->UpdatePosition(resetSecondMonitorPlacement);
}

void Overlay::SaveSecondMonitorWindowBounds() {
    if (!m_hwnd || IsIconic(m_hwnd) || m_setupWindow) return;
    if (GetForegroundWindow() != m_hwnd && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) return;

    const ConfigData& config = m_frameConfig;
    if (!config.second_monitor_mode) return;

    RECT rect{};
    if (!GetWindowRect(m_hwnd, &rect)) return;
    const int x = rect.left;
    const int y = rect.top;
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    if (config.second_monitor_has_bounds && config.second_monitor_x == x && config.second_monitor_y == y &&
        config.second_monitor_w == width && config.second_monitor_h == height) return;

    Config::Update([=](ConfigData& c) {
        c.second_monitor_has_bounds = true;
        c.second_monitor_x = x;
        c.second_monitor_y = y;
        c.second_monitor_w = width;
        c.second_monitor_h = height;
    });
}

void Overlay::HandleDpiChanged(UINT dpi, const RECT* suggestedRect) {
    if (dpi == 0) return;
    m_dpiScale = static_cast<float>(dpi) / 96.0f;
    if (m_dpiScale < 0.5f) m_dpiScale = 1.0f;
    if (m_window) m_window->SetDpiScale(m_dpiScale);

    const ConfigData& config = m_frameConfig;
    const bool keepSecondMonitorBounds = config.second_monitor_mode && m_hwnd &&
                                         MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONULL) != nullptr;
    if (suggestedRect && m_hwnd && !m_setupWindow) {
        int width = suggestedRect->right - suggestedRect->left;
        int height = suggestedRect->bottom - suggestedRect->top;
        if (config.second_monitor_mode) {
            ClampSecondMonitorSize(width, height, m_dpiScale);
        }
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (config.second_monitor_mode) {
            SaveSecondMonitorWindowBounds();
        }
    } else {
        UpdateWindowPosition(!keepSecondMonitorBounds);
    }
    if (m_rmlUi) m_rmlUi->SetDpiScale(m_dpiScale);
}

void Overlay::ResizeSwapChain(int width, int height) {
    if (!m_d3d11) return;
    const HRESULT hr = m_d3d11->ResizeBuffers(width, height);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
        (void)HandleDeviceLost("ResizeBuffers", hr);
    } else if (FAILED(hr)) {
        std::cout << "[D3D11] Resize failed for " << width << "x" << height << ": 0x" << std::hex << hr << std::dec << "\n";
    } else if (m_rmlUi) {
        m_rmlUi->Resize(width, height, m_dpiScale);
    }
}

bool Overlay::RecreateD3DDevice() {
    if (!m_hwnd || !m_d3d11) return false;

    if (m_rmlUi) m_rmlUi->Shutdown();
    m_d3d11->Shutdown();
    if (!m_d3d11->Create(m_hwnd)) {
        std::cout << "[D3D11] Failed to recreate D3D device.\n";
        return false;
    }
    UpdateWindowStyle();

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = std::max(client.right - client.left, 1L);
    const int height = std::max(client.bottom - client.top, 1L);
    if (!m_rmlUi) m_rmlUi = std::make_unique<RmlUiController>(m_state, m_dbManager, m_accounts, m_remoteConfig, m_tournamentService, m_telemetry, m_accountClient);
    if (!m_rmlUi->Initialize(m_hwnd, m_d3d11->Device(), m_d3d11->Context(), width, height, m_dpiScale)) {
        std::cout << "[RmlUi] Failed to reinitialize UI after D3D device recreation.\n";
        return false;
    }
    return true;
}

bool Overlay::HandleDeviceLost(const char* reason, HRESULT hr) {
    const char* errorName = "Unknown";
    if (hr == DXGI_ERROR_DEVICE_REMOVED)
        errorName = "DXGI_ERROR_DEVICE_REMOVED";
    else if (hr == DXGI_ERROR_DEVICE_RESET)
        errorName = "DXGI_ERROR_DEVICE_RESET";
    else if (hr == DXGI_ERROR_DEVICE_HUNG)
        errorName = "DXGI_ERROR_DEVICE_HUNG";

    std::cout << "[D3D11] Device lost (" << errorName << ") during " << reason
              << ": 0x" << std::hex << hr << std::dec << ". Recreating device.\n";
    if (RecreateD3DDevice()) {
        std::cout << "[D3D11] Device recreated successfully.\n";
        return true;
    }
    std::cout << "[D3D11] Device recreation failed; stopping overlay loop.\n";
    return false;
}
