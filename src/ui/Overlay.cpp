#include "Overlay.hpp"

#include <SDL2/SDL.h>
#include <chrono>
#include <dwmapi.h>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include "core/InputManager.hpp"
#include "ui/WindowUtils.hpp"
#include "ui/rml/RmlUiController.hpp"
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
        if (wParam && Config::Read().second_monitor_mode) return 0;
        break;

    case WM_NCHITTEST:
        if (overlay && Config::Read().second_monitor_mode) {
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
                const int controlsWidth = static_cast<int>(270.0f * overlay->m_dpiScale);
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

    case WM_GETMINMAXINFO:
        if (Config::Read().second_monitor_mode) {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            float dpiScale = overlay ? overlay->m_dpiScale : 1.0f;
            if (!overlay) {
                using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
                static HMODULE user32 = GetModuleHandleW(L"user32.dll");
                static auto getDpiForWindow = user32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")) : nullptr;
                if (getDpiForWindow) dpiScale = static_cast<float>(getDpiForWindow(hWnd)) / 96.0f;
            }
            mmi->ptMinTrackSize.x = static_cast<int>(637.0f * dpiScale);
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

Overlay::Overlay(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)) {}

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

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = std::max(client.right - client.left, 1L);
    const int height = std::max(client.bottom - client.top, 1L);

    m_rmlUi = std::make_unique<RmlUiController>(m_state, m_dbManager);
    if (!m_rmlUi->Initialize(m_hwnd, m_d3d11->Device(), m_d3d11->Context(), width, height, m_dpiScale)) {
        std::cout << "[RmlUi] Failed to initialize UI.\n";
        return false;
    }

    timeBeginPeriod(1);
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    return true;
}

void Overlay::UpdateWindowStyle() {
    if (!m_window) return;
    const ConfigData config = Config::Read();
    const bool interactiveOverlay = m_rmlUi && m_rmlUi->WantsInteraction();
    m_window->UpdateStyle(config.second_monitor_mode, m_state->ui.showMenu.load() || interactiveOverlay);
}

void Overlay::RunLoop() {
    bool done = false;
    bool isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
    HWND cachedRlHwnd = nullptr;
    bool wasRLActive = false;
    auto lastRescan = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto rlFocusReadyAt = (std::chrono::steady_clock::time_point::min)();
    bool validateDeviceAfterFocus = false;
    auto lastFrameTime = std::chrono::steady_clock::now();

    while (!done) {
        if (m_state->ui.appExitRequested.load()) {
            std::cout << "[Overlay] Exit requested by external updater.\n";
            break;
        }

        m_frameConfig = Config::Read();

        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
            }
        }

        MSG msg{};
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        m_frameConfig = Config::Read();
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
        const bool checkFocus = m_frameConfig.require_rl_focus || m_frameConfig.second_monitor_mode;
        if (checkFocus) {
            const auto now = std::chrono::steady_clock::now();
            if ((!cachedRlHwnd || !IsWindow(cachedRlHwnd)) &&
                std::chrono::duration_cast<std::chrono::seconds>(now - lastRescan).count() >= 2) {
                lastRescan = now;
                cachedRlHwnd = FindWindowA("LaunchUnrealUWindowsClient", nullptr);
                if (!cachedRlHwnd) cachedRlHwnd = FindWindowA(nullptr, "Rocket League (64-bit, DX11)");
                if (!cachedRlHwnd) cachedRlHwnd = FindWindowA(nullptr, "Rocket League (32-bit, DX11)");
                if (!cachedRlHwnd) cachedRlHwnd = FindWindowA(nullptr, "Rocket League");
            }

            if (HWND fg = GetForegroundWindow()) {
                if (fg == m_hwnd || (cachedRlHwnd && fg == cachedRlHwnd)) {
                    isRLActive = true;
                } else {
                    char className[256]{};
                    char title[256]{};
                    GetClassNameA(fg, className, sizeof(className));
                    GetWindowTextA(fg, title, sizeof(title));
                    const std::string cls(className);
                    const std::string windowTitle(title);
                    bool isRL = cls == "LaunchUnrealUWindowsClient";
                    if (!isRL) {
                        const bool browserOrExplorer = cls.find("Chrome") != std::string::npos ||
                                                       cls.find("Mozilla") != std::string::npos ||
                                                       cls.find("IEFrame") != std::string::npos ||
                                                       cls.find("CabinetWClass") != std::string::npos;
                        isRL = !browserOrExplorer && (windowTitle == "Rocket League (64-bit, DX11)" ||
                                                      windowTitle == "Rocket League (32-bit, DX11)" ||
                                                      windowTitle == "Rocket League");
                    }
                    isRLActive = isRL;
                }
            }
        }

        bool shouldDraw = true;
        if (m_frameConfig.require_rl_focus) {
            shouldDraw = m_frameConfig.second_monitor_mode ? cachedRlHwnd != nullptr : isRLActive;
        }
        if (!shouldDraw) {
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            wasRLActive = false;
            lastFrameTime = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            continue;
        }

        const auto renderNow = std::chrono::steady_clock::now();
        if (m_frameConfig.require_rl_focus && !m_frameConfig.second_monitor_mode && isRLActive && !wasRLActive) {
            rlFocusReadyAt = renderNow + std::chrono::milliseconds(500);
            validateDeviceAfterFocus = true;
            wasRLActive = true;
            lastFrameTime = renderNow;
            std::cout << "[D3D11] Rocket League gained focus; delaying overlay rendering for 500 ms.\n";
        }

        if (validateDeviceAfterFocus && renderNow < rlFocusReadyAt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        if (validateDeviceAfterFocus) {
            validateDeviceAfterFocus = false;
            if (m_d3d11 && m_d3d11->Device()) {
                const HRESULT deviceReason = m_d3d11->Device()->GetDeviceRemovedReason();
                if (deviceReason != S_OK) {
                    std::cout << "[D3D11] Device unhealthy after Rocket League focus transition: 0x"
                              << std::hex << deviceReason << std::dec << "\n";
                    if (!HandleDeviceLost("Rocket League focus transition", deviceReason)) done = true;
                    lastFrameTime = std::chrono::steady_clock::now();
                    continue;
                }
            }
        }

        const bool wasVisible = IsWindowVisible(m_hwnd) != FALSE;
        if (!wasVisible) {
            UpdateWindowStyle();
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(m_hwnd,
                         m_frameConfig.second_monitor_mode ? HWND_NOTOPMOST : HWND_TOPMOST,
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

        m_rmlUi->Update(m_frameConfig);
        const bool needsInteract = m_rmlUi->WantsInteraction();
        isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
        if (needsInteract && isClickThrough) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle &= ~WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd,
                         m_frameConfig.second_monitor_mode ? HWND_NOTOPMOST : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            if (!m_frameConfig.second_monitor_mode) SetForegroundWindow(m_hwnd);
            isClickThrough = false;
        } else if (!needsInteract && !isClickThrough && !m_frameConfig.second_monitor_mode) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            isClickThrough = true;
        }

        const float clearColor[4] = {0.0f, 0.0f, 0.0f, m_frameConfig.second_monitor_mode ? 1.0f : 0.0f};
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

        if (!m_frameConfig.vsync && m_frameConfig.overlay_fps_cap > 0 && !needsInteract) {
            const auto now = std::chrono::steady_clock::now();
            const auto targetDuration = std::chrono::duration<double, std::milli>(1000.0 / m_frameConfig.overlay_fps_cap);
            const auto elapsed = std::chrono::duration<double, std::milli>(now - lastFrameTime);
            if (elapsed < targetDuration) std::this_thread::sleep_for(targetDuration - elapsed);
        }
        lastFrameTime = std::chrono::steady_clock::now();
    }
}

void Overlay::Shutdown() {
    timeEndPeriod(1);
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
    if (m_window && m_hwnd) m_window->UpdatePosition(resetSecondMonitorPlacement);
}

void Overlay::SaveSecondMonitorWindowBounds() {
    if (!m_hwnd || IsIconic(m_hwnd)) return;
    if (GetForegroundWindow() != m_hwnd && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) return;

    const ConfigData config = Config::Read();
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

    const ConfigData config = Config::Read();
    const bool keepSecondMonitorBounds = config.second_monitor_mode && m_hwnd &&
                                         MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONULL) != nullptr;
    if (suggestedRect && m_hwnd) {
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left, suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
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

    RECT client{};
    GetClientRect(m_hwnd, &client);
    const int width = std::max(client.right - client.left, 1L);
    const int height = std::max(client.bottom - client.top, 1L);
    if (!m_rmlUi) m_rmlUi = std::make_unique<RmlUiController>(m_state, m_dbManager);
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
