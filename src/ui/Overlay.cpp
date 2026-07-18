#include "Overlay.hpp"
#include "core/Config.hpp"
#include "core/InputManager.hpp"
#include "database/DatabaseManager.hpp"
#include "core/Storage.hpp"
#include "network/TelemetryManager.hpp"
#include "ui/widgets/MmrGraphWidget.hpp"
#include "ui/widgets/ToggleWidget.hpp"
#include "ui/Formatting.hpp"
#include "ui/RankIconAssets.hpp"
#include "ui/ThemeManager.hpp"
#include "ui/ImGuiGuards.hpp"
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>
#include <implot.h>
#include <dwmapi.h>
#include <iostream>
#include <algorithm>
#include <thread>
#include <filesystem>
#include <cmath>
static float ClampFontScale(float scale) {
    if (std::isnan(scale) || std::isinf(scale)) {
        return 1.0f;
    }
    if (scale < 0.5f) return 0.5f;
    if (scale > 5.0f) return 5.0f;
    return scale;
}
#pragma comment(lib, "dwmapi.lib")
#include <SDL2/SDL.h>
#pragma comment(lib, "shell32.lib")
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
static bool s_isQuitting = false;
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (InputManager::HandleWindowMessage(msg, wParam, lParam))
        return 0;
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
            if (ptr != 0) {
                Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
                overlay->ResizeSwapChain(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
                overlay->SaveSecondMonitorWindowBounds();
            }
        }
        return 0;
    case WM_MOVE: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            overlay->SaveSecondMonitorWindowBounds();
        }
        return 0;
    }
    case WM_EXITSIZEMOVE: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            overlay->SaveSecondMonitorWindowBounds();
        }
        return 0;
    }
    case WM_DISPLAYCHANGE: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            overlay->UpdateWindowPosition(false);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            overlay->HandleDpiChanged(LOWORD(wParam), reinterpret_cast<const RECT*>(lParam));
        }
        return 0;
    }

    case WM_NCCALCSIZE:
        if (wParam) {
            if (Config::Read().second_monitor_mode) {
                // Strips the standard Windows title bar while keeping native drop shadows and borders
                return 0;
            }
        }
        break;
    case WM_NCHITTEST: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            if (Config::Read().second_monitor_mode) {
                LRESULT hit = DefWindowProcW(hWnd, msg, wParam, lParam);
                if (hit == HTCLIENT) {
                    POINT pt;
                    pt.x = static_cast<int>(static_cast<short>(LOWORD(lParam)));
                    pt.y = static_cast<int>(static_cast<short>(HIWORD(lParam)));
                    ScreenToClient(hWnd, &pt);
                    RECT rect;
                    GetClientRect(hWnd, &rect);
                    int width = rect.right - rect.left;
                    int height = rect.bottom - rect.top;
                    const int borderSize = static_cast<int>(8 * overlay->m_dpiScale);
                    if (pt.x <= borderSize && pt.y <= borderSize) return HTTOPLEFT;
                    if (pt.x >= width - borderSize && pt.y <= borderSize) return HTTOPRIGHT;
                    if (pt.x <= borderSize && pt.y >= height - borderSize) return HTBOTTOMLEFT;
                    if (pt.x >= width - borderSize && pt.y >= height - borderSize) return HTBOTTOMRIGHT;
                    if (pt.x <= borderSize) return HTLEFT;
                    if (pt.x >= width - borderSize) return HTRIGHT;
                    if (pt.y <= borderSize) return HTTOP;
                    if (pt.y >= height - borderSize) return HTBOTTOM;
                    float titleBarHeight = 40.0f * overlay->m_dpiScale;
                    float buttonsWidth = 46.0f * 4.0f * overlay->m_dpiScale; // Edit, minimize, maximize, close
                    if (pt.y >= 0 && pt.y <= titleBarHeight) {
                        if (pt.x >= width - buttonsWidth) {
                            return HTCLIENT; // Hover/click gets routed to ImGui controls
                        }
                        return HTCAPTION; // Dragging/snapping behavior
                    }
                }
                return hit;
            }
        }
        break;
    }
    case WM_TOGGLE_MODE: {
        LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
        if (ptr != 0) {
            Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
            Config::Update([](ConfigData& c) {
                c.second_monitor_mode = !c.second_monitor_mode;
            });
            overlay->UpdateWindowStyle();
            overlay->UpdateWindowPosition();
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        // Enforce a minimum dashboard size when in second-monitor (dashboard) mode.
        // This prevents layout breakage when the window is made too small.
        if (Config::Read().second_monitor_mode) {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            float dpiScale = 1.0f;
            LONG_PTR ptr = GetWindowLongPtrW(hWnd, GWLP_USERDATA);
            if (ptr != 0) {
                Overlay* overlay = reinterpret_cast<Overlay*>(ptr);
                dpiScale = overlay->m_dpiScale;
            } else {
                typedef UINT(WINAPI * GetDpiForWindow_t)(HWND);
                static HMODULE user32 = GetModuleHandleW(L"user32.dll");
                static GetDpiForWindow_t pGetDpiForWindow = user32 ? (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
                if (pGetDpiForWindow) {
                    dpiScale = static_cast<float>(pGetDpiForWindow(hWnd)) / 96.0f;
                }
            }
            // Enforce horizontal minimum only; do not clamp vertical size
            const int kMinW = static_cast<int>(637 * dpiScale);
            mmi->ptMinTrackSize.x = kMinW;
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        if (!s_isQuitting) {
            s_isQuitting = true;
            PostQuitMessage(0);
        }
        return 0;
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
    : m_state(state), m_dbManager(dbManager) {}
Overlay::~Overlay() {
    Shutdown();
}
bool Overlay::Initialize() {
    HICON appIconBig = LoadAppIcon(32, 32);
    HICON appIconSmall = LoadAppIcon(16, 16);
    m_window = std::make_unique<OverlayWindow>();
    if (!m_window->Create(appIconBig, appIconSmall, WndProc)) return false;
    m_hwnd = m_window->Handle();
    m_dpiScale = m_window->DpiScale();
    m_imguiIniPath = m_window->ImGuiIniPath();
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    m_trayIcon = std::make_unique<TrayIcon>(m_hwnd);
    if (!m_trayIcon->Initialize()) {
        std::cout << "[Tray] Tray icon failed to initialize. Continuing without tray icon.\n";
        m_trayIcon.reset();
    }
    UpdateWindowStyle();
    UpdateWindowPosition();
    m_d3d11 = std::make_unique<D3D11Device>();
    if (!m_d3d11->Create(m_hwnd)) return false;
    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = m_imguiIniPath.c_str();
    m_frameConfig = Config::Read();
    if (!LoadFonts()) return false;
    ImGui::StyleColorsDark();
    ApplyTheme();
    ImGui_ImplWin32_Init(m_hwnd);
    if (!ImGui_ImplDX11_Init(m_d3d11->Device(), m_d3d11->Context())) {
        std::cout << "[D3D11] Failed to initialize ImGui DX11 backend.\n";
        return false;
    }
    m_imguiDx11Initialized = true;
    m_rankIcons = std::make_unique<RankIconAssets>();
    if (!m_rankIcons->Load(m_d3d11->Device())) {
        std::cout << "[RankIcons] Rank icon resources failed to load. Text labels will be used.\n";
    }
    // Construct panels after ImGui/fonts are ready
    RefreshPanelContexts();
    return true;
}
// Applies the full theme to all ImGui widget styles — called at startup and on color changes
void Overlay::ApplyTheme() {
    m_frameConfig = Config::Read();
    ImGui::GetIO().FontGlobalScale = 1.0f;
    float desiredFontScale = ClampFontScale(m_dpiScale * m_frameConfig.ui_scale);
    if (std::abs(desiredFontScale - m_loadedFontScale) > 0.05f) {
        m_fontReloadPending = true;
    }
    ThemeManager::Apply(m_frameConfig, m_dpiScale);
}
bool Overlay::LoadFonts() {
    fontRegular = nullptr;
    fontBold = nullptr;
    fontSmall = nullptr;
    fontSmallBold = nullptr;
    fontMono = nullptr;
    lobbyFontSmall = nullptr;
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    float scale = ClampFontScale(m_dpiScale * m_frameConfig.ui_scale);
    const char* segoePath = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* segoeBoldPath = "C:\\Windows\\Fonts\\segoeuib.ttf";
    const char* consolaBoldPath = "C:\\Windows\\Fonts\\consolab.ttf";
    if (std::filesystem::exists(segoePath)) {
        fontRegular = io.Fonts->AddFontFromFileTTF(segoePath, 15.0f * scale, &fontConfig);
    }
    if (!fontRegular) {
        fontRegular = io.Fonts->AddFontDefault();
    }
    if (!fontRegular) {
        return false;
    }
    if (std::filesystem::exists(segoeBoldPath)) {
        fontBold = io.Fonts->AddFontFromFileTTF(segoeBoldPath, 15.0f * scale, &fontConfig);
    }
    if (std::filesystem::exists(segoePath)) {
        fontSmall = io.Fonts->AddFontFromFileTTF(segoePath, 12.0f * scale, &fontConfig);
    }
    if (std::filesystem::exists(segoeBoldPath)) {
        fontSmallBold = io.Fonts->AddFontFromFileTTF(segoeBoldPath, 12.0f * scale, &fontConfig);
    }
    if (std::filesystem::exists(consolaBoldPath)) {
        fontMono = io.Fonts->AddFontFromFileTTF(consolaBoldPath, 13.0f * scale, &fontConfig);
    }
    if (std::filesystem::exists(segoePath)) {
        lobbyFontSmall = io.Fonts->AddFontFromFileTTF(segoePath, 13.0f * scale, &fontConfig);
    }
    if (!fontBold) fontBold = fontRegular;
    if (!fontSmall) fontSmall = fontRegular;
    if (!fontSmallBold) fontSmallBold = fontBold;
    if (!fontMono) fontMono = fontRegular;
    if (!lobbyFontSmall) lobbyFontSmall = fontSmall;
    m_loadedFontScale = scale;
    return true;
}
bool Overlay::RebuildFontsForCurrentScale() {
    if (m_rebuildingFonts) {
        return false;
    }
    m_rebuildingFonts = true;
    struct ReentryGuard {
        bool& flag;
        explicit ReentryGuard(bool& f) : flag(f) {}
        ~ReentryGuard() {
            flag = false;
        }
    } guard(m_rebuildingFonts);
    if (!ImGui::GetCurrentContext()) {
        std::cout << "[Overlay] RebuildFontsForCurrentScale: No current ImGui context.\n";
        return false;
    }
    if (!m_d3d11 || !m_d3d11->Device() || !m_d3d11->Context()) {
        std::cout << "[Overlay] RebuildFontsForCurrentScale: D3D11 device or context not initialized.\n";
        return false;
    }
    m_frameConfig = Config::Read();
    float uiScale = m_frameConfig.ui_scale;
    float requestedScale = ClampFontScale(m_dpiScale * uiScale);
    bool segoeAvailable = std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf");
    bool segoeBoldAvailable = std::filesystem::exists("C:\\Windows\\Fonts\\segoeuib.ttf");
    bool consolaBoldAvailable = std::filesystem::exists("C:\\Windows\\Fonts\\consolab.ttf");
    std::cout << "[Overlay] Rebuilding fonts: "
              << "dpiScale=" << m_dpiScale
              << ", uiScale=" << uiScale
              << ", requestedScale=" << requestedScale
              << ", segoeAvailable=" << segoeAvailable
              << ", segoeBoldAvailable=" << segoeBoldAvailable
              << ", consolaBoldAvailable=" << consolaBoldAvailable
              << ", imguiDx11Initialized=" << m_imguiDx11Initialized << "\n";
    if (m_imguiDx11Initialized) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
    }
    if (!LoadFonts()) {
        std::cout << "[Overlay] LoadFonts failed. Trying to fall back to default fonts.\n";
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        fontRegular = io.Fonts->AddFontDefault();
        fontBold = fontRegular;
        fontSmall = fontRegular;
        fontSmallBold = fontRegular;
        fontMono = fontRegular;
        lobbyFontSmall = fontRegular;
        if (!fontRegular) {
            std::cout << "[Overlay] CRITICAL: Even default font could not be loaded.\n";
            return false;
        }
    }
    if (m_imguiDx11Initialized && !ImGui_ImplDX11_CreateDeviceObjects()) {
        std::cout << "[Overlay] Failed to recreate ImGui DX11 device objects.\n";
        return false;
    }
    m_fontReloadPending = false;
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ThemeManager::Apply(m_frameConfig, m_dpiScale);
    RefreshPanelContexts();
    return true;
}
void Overlay::RefreshPanelContexts() {
    auto ctx = MakeCtx();
    auto dashFuncs = MakeDashFuncs();
    m_settingsPanel = std::make_unique<SettingsPanel>(ctx);
    m_dashboardPanel = std::make_unique<DashboardPanel>(ctx, m_hwnd, dashFuncs);
}
void Overlay::HandleDpiChanged(UINT dpi, const RECT* suggestedRect) {
    if (dpi == 0) return;
    m_dpiScale = static_cast<float>(dpi) / 96.0f;
    if (m_dpiScale < 0.5f) m_dpiScale = 1.0f;
    if (m_window) m_window->SetDpiScale(m_dpiScale);
    ConfigData conf = Config::Read();
    bool keepSecondMonitorBounds = conf.second_monitor_mode && m_hwnd && MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONULL) != nullptr;
    if (suggestedRect && m_hwnd && !keepSecondMonitorBounds) {
        SetWindowPos(m_hwnd, nullptr,
                     suggestedRect->left,
                     suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    m_fontReloadPending = true;
    UpdateWindowPosition(!keepSecondMonitorBounds);
}
void Overlay::RunLoop() {
    bool done = false;
    bool isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
    HWND cachedRlHwnd = nullptr;
    auto lastRescan = std::chrono::steady_clock::now();
    auto lastFrameTime = std::chrono::steady_clock::now();
    while (!done) {
        if (m_state->ui.appExitRequested.load()) {
            std::cout << "[Overlay] Exit requested by external updater.\n";
            done = true;
            break;
        }
        m_frameConfig = Config::Read();

        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) {
            // Drain SDL event queue on the main thread to keep controller hotplug state moving.
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
            }
        }
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        // Re-read config after processing messages because a settings change might have occurred
        m_frameConfig = Config::Read();
        float desiredFontScale = ClampFontScale(m_dpiScale * m_frameConfig.ui_scale);
        if (m_fontReloadPending || std::abs(desiredFontScale - m_loadedFontScale) > 0.05f) {
            (void)RebuildFontsForCurrentScale();
        }
        bool shouldDraw = true;
        if (m_frameConfig.require_rl_focus) {
            auto now = std::chrono::steady_clock::now();
            if (!cachedRlHwnd || !IsWindow(cachedRlHwnd) || std::chrono::duration_cast<std::chrono::seconds>(now - lastRescan).count() >= 2) {
                lastRescan = now;
                cachedRlHwnd = ::FindWindowA("LaunchUnrealUWindowsClient", nullptr);
                if (!cachedRlHwnd) cachedRlHwnd = ::FindWindowA(nullptr, "Rocket League (64-bit, DX11)");
                if (!cachedRlHwnd) cachedRlHwnd = ::FindWindowA(nullptr, "Rocket League (32-bit, DX11)");
                if (!cachedRlHwnd) cachedRlHwnd = ::FindWindowA(nullptr, "Rocket League");
            }
            HWND fg = GetForegroundWindow();
            bool isRLActive = false;
            if (fg) {
                if (fg == m_hwnd) {
                    isRLActive = true;
                } else if (cachedRlHwnd && fg == cachedRlHwnd) {
                    isRLActive = true;
                } else {
                    char className[256];
                    GetClassNameA(fg, className, sizeof(className));
                    std::string cls(className);
                    char title[256];
                    GetWindowTextA(fg, title, sizeof(title));
                    std::string t(title);
                    bool isRL = (cls == "LaunchUnrealUWindowsClient");
                    if (!isRL) {
                        bool isBrowserOrExplorer = (cls.find("Chrome") != std::string::npos ||
                                                    cls.find("Mozilla") != std::string::npos ||
                                                    cls.find("IEFrame") != std::string::npos ||
                                                    cls.find("CabinetWClass") != std::string::npos);
                        if (!isBrowserOrExplorer && (t == "Rocket League (64-bit, DX11)" || t == "Rocket League (32-bit, DX11)" || t == "Rocket League")) {
                            isRL = true;
                        }
                    }
                    if (isRL) {
                        isRLActive = true;
                    }
                }
            }
            if (m_frameConfig.second_monitor_mode) {
                if (!cachedRlHwnd) {
                    shouldDraw = false;
                }
            } else {
                if (!isRLActive) {
                    shouldDraw = false;
                }
            }
        }
        if (!shouldDraw) {
            if (IsWindowVisible(m_hwnd)) ShowWindow(m_hwnd, SW_HIDE);
            lastFrameTime = std::chrono::steady_clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(150)); // Sleep to save CPU while game is out of focus
            continue;
        } else {
            if (!IsWindowVisible(m_hwnd)) {
                // Borderless games can reclaim z-order while the overlay is hidden.
                UpdateWindowStyle();
                ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
                SetWindowPos(m_hwnd,
                             m_frameConfig.second_monitor_mode ? HWND_NOTOPMOST : HWND_TOPMOST,
                             0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
                UpdateWindow(m_hwnd);
                isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
            }
        }
        bool needsInteract = m_state->ui.showMenu || m_frameConfig.second_monitor_mode;
        isClickThrough = (GetWindowLong(m_hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT) != 0;
        if (needsInteract && isClickThrough) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle &= ~WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd,
                         m_frameConfig.second_monitor_mode ? HWND_NOTOPMOST : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SetForegroundWindow(m_hwnd);
            isClickThrough = false;
        } else if (!needsInteract && !isClickThrough) {
            LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
            exStyle |= WS_EX_TRANSPARENT;
            SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(m_hwnd,
                         m_frameConfig.second_monitor_mode ? HWND_NOTOPMOST : HWND_TOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            isClickThrough = true;
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderUI();
        ImGui::Render();
        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        m_d3d11->Context()->OMSetRenderTargets(1, m_d3d11->RenderTargetViewAddress(), nullptr);
        m_d3d11->Context()->ClearRenderTargetView(m_d3d11->RenderTargetView(), clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        UINT syncInterval = m_frameConfig.vsync ? 1 : 0;
        HRESULT presentHr = m_d3d11->SwapChain()->Present(syncInterval, 0);
        if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
            if (!HandleDeviceLost("Present", presentHr)) {
                done = true;
            }
            continue;
        }
        if (FAILED(presentHr)) {
            std::cout << "[D3D11] Present failed: " << std::hex << presentHr << std::dec << "\n";
        }
        if (!m_frameConfig.vsync && m_frameConfig.overlay_fps_cap > 0) {
            auto now = std::chrono::steady_clock::now();
            auto targetDuration = std::chrono::duration<double, std::milli>(1000.0 / m_frameConfig.overlay_fps_cap);
            auto elapsed = std::chrono::duration<double, std::milli>(now - lastFrameTime);
            if (elapsed < targetDuration) {
                std::this_thread::sleep_for(targetDuration - elapsed);
            }
        }
        lastFrameTime = std::chrono::steady_clock::now();
    }
}
RenderContext Overlay::MakeCtx() {
    return {
        .state = *m_state,
        .config = m_frameConfig,
        .db = m_dbManager.get(),
        .snap = &m_snap,
        .dpiScale = m_dpiScale,
        .pendingBallchasingToken = &m_pendingBallchasingToken,
        .fontRegular = fontRegular,
        .fontBold = fontBold,
        .fontSmall = fontSmall,
        .fontSmallBold = fontSmallBold,
        .fontMono = fontMono};
}
DashRenderFuncs Overlay::MakeDashFuncs() {
    DashRenderFuncs f;
    f.playerRoster = [this](int a, const char* b, ImColor c, const char* d) { RenderPlayerRoster(a, b, c, d); };
    f.liveMatchStats = [this](const char* a) { RenderLiveMatchStats(a); };
    f.streaksStatsTable = [this](const char* a) { RenderStreaksStatsTable(a); };
    f.gamemodeBreakdownTable = [this](const char* a, GamemodeBreakdownScope scope) { RenderGamemodeBreakdownTable(a, scope); };
    f.sessionStatsTable = [this](const char* a, bool b, bool c) { RenderSessionStatsTable(a, b, c); };
    f.lobbyRanksTable = [this](const char* a) { RenderLobbyRanksTable(a); };
    f.demoTrackerTable = [this](const char* a) { RenderDemoTrackerTable(a); };
    return f;
}
void Overlay::Shutdown() {
    if (m_trayIcon) {
        m_trayIcon->Shutdown();
        m_trayIcon.reset();
    }
    if (m_d3d11) {
        if (m_imguiDx11Initialized) {
            ImGui_ImplDX11_Shutdown();
            m_imguiDx11Initialized = false;
        }
        ImGui_ImplWin32_Shutdown();
        ImPlot::DestroyContext();
        ImGui::DestroyContext();
        if (m_rankIcons) {
            m_rankIcons->Shutdown();
            m_rankIcons.reset();
        }
        m_d3d11->Shutdown();
        m_d3d11.reset();
    }
    if (m_window) {
        m_window->Destroy();
        m_window.reset();
        m_hwnd = nullptr;
    }
}
void Overlay::UpdateWindowPosition(bool resetSecondMonitorPlacement) {
    if (!m_window || !m_hwnd) return;
    m_window->UpdatePosition(resetSecondMonitorPlacement);
    if (m_d3d11) {
        RECT client;
        GetClientRect(m_hwnd, &client);
        HRESULT hr = m_d3d11->ResizeBuffers(client.right - client.left, client.bottom - client.top);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            (void)HandleDeviceLost("ResizeBuffers", hr);
        } else if (FAILED(hr)) {
            std::cout << "[D3D11] Resize failed while updating window position.\n";
        }
    }
}
void Overlay::SaveSecondMonitorWindowBounds() {
    if (!m_hwnd || IsIconic(m_hwnd)) return;
    if (GetForegroundWindow() != m_hwnd && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
        return;
    }
    ConfigData conf = Config::Read();
    if (!conf.second_monitor_mode) return;
    RECT rect;
    if (!GetWindowRect(m_hwnd, &rect)) return;
    int x = rect.left;
    int y = rect.top;
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    if (w <= 0 || h <= 0) return;
    if (conf.second_monitor_has_bounds && conf.second_monitor_x == x && conf.second_monitor_y == y && conf.second_monitor_w == w && conf.second_monitor_h == h) {
        return;
    }
    Config::Update([x, y, w, h](ConfigData& c) {
        c.second_monitor_has_bounds = true;
        c.second_monitor_x = x;
        c.second_monitor_y = y;
        c.second_monitor_w = w;
        c.second_monitor_h = h;
    });
}
void Overlay::ResizeSwapChain(int width, int height) {
    if (m_d3d11) {
        HRESULT hr = m_d3d11->ResizeBuffers(width, height);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            (void)HandleDeviceLost("ResizeBuffers", hr);
        } else if (FAILED(hr)) {
            std::cout << "[D3D11] Resize failed for " << width << "x" << height << ".\n";
        }
    }
}
bool Overlay::RecreateD3DDevice() {
    if (!m_hwnd || !m_d3d11) return false;
    if (m_imguiDx11Initialized) {
        ImGui_ImplDX11_Shutdown();
        m_imguiDx11Initialized = false;
    }
    if (m_rankIcons) {
        m_rankIcons->Shutdown();
    }
    m_d3d11->Shutdown();
    if (!m_d3d11->Create(m_hwnd)) {
        std::cout << "[D3D11] Failed to recreate D3D device.\n";
        return false;
    }
    if (!ImGui_ImplDX11_Init(m_d3d11->Device(), m_d3d11->Context())) {
        std::cout << "[D3D11] Failed to reinitialize ImGui DX11 backend.\n";
        return false;
    }
    m_imguiDx11Initialized = true;
    if (m_rankIcons && !m_rankIcons->Load(m_d3d11->Device())) {
        std::cout << "[RankIcons] Failed to reload rank icons after D3D device recreation.\n";
    }
    RECT client;
    GetClientRect(m_hwnd, &client);
    HRESULT hr = m_d3d11->ResizeBuffers(client.right - client.left, client.bottom - client.top);
    if (FAILED(hr)) {
        std::cout << "[D3D11] Failed to resize recreated device: " << std::hex << hr << std::dec << "\n";
        return false;
    }
    return true;
}
bool Overlay::HandleDeviceLost(const char* reason, HRESULT hr) {
    std::cout << "[D3D11] Device lost during " << reason << ": " << std::hex << hr << std::dec << ". Recreating device.\n";
    if (RecreateD3DDevice()) {
        std::cout << "[D3D11] Device recreated successfully.\n";
        return true;
    }
    std::cout << "[D3D11] Device recreation failed; stopping overlay loop.\n";
    return false;
}
