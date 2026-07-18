#include "OverlayWindow.hpp"
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include <dwmapi.h>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")

OverlayWindow::OverlayWindow() {}

bool OverlayWindow::Create(HICON appIconBig, HICON appIconSmall, WNDPROC wndProc) {
    m_wc = {sizeof(m_wc), CS_CLASSDC, wndProc, 0L, 0L, GetModuleHandle(nullptr),
            appIconBig, nullptr, nullptr, nullptr, L"OmniStats_Class", appIconSmall};
    if (!RegisterClassExW(&m_wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::cout << "[Overlay] Failed to register window class. Error=" << err << "\n";
            return false;
        }
        m_classRegistered = false;
    } else {
        m_classRegistered = true;
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        m_wc.lpszClassName, L"OmniStats",
        WS_POPUP, 0, 0, screenW, screenH, nullptr, nullptr, m_wc.hInstance, nullptr);

    if (!m_hwnd) return false;

    UpdateStyle(false, false);
    SetWindowTextW(m_hwnd, L"OmniStats");
    SetWindowTextA(m_hwnd, "OmniStats");

    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconBig));
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall));

    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    m_frameExtended = true;

    // DPI detection
    UINT dpi = 96;
    typedef UINT(WINAPI * GetDpiForWindow_t)(HWND);
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static GetDpiForWindow_t pGetDpiForWindow = user32 ? (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow") : nullptr;
    if (pGetDpiForWindow && m_hwnd) {
        dpi = pGetDpiForWindow(m_hwnd);
    } else {
        typedef UINT(WINAPI * GetDpiForSystem_t)();
        static GetDpiForSystem_t pGetDpiForSystem = user32 ? (GetDpiForSystem_t)GetProcAddress(user32, "GetDpiForSystem") : nullptr;
        if (pGetDpiForSystem) {
            dpi = pGetDpiForSystem();
        }
    }
    float systemScale = (float)dpi / 96.0f;

    m_dpiScale = systemScale;
    if (m_dpiScale < 0.5f) m_dpiScale = 1.0f;

    m_imguiIniPath = Storage::GetDataDirectory() + "imgui.ini";

    return true;
}

void OverlayWindow::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        if (m_classRegistered) {
            UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
            m_classRegistered = false;
        }
        m_hwnd = nullptr;
    }
}

void OverlayWindow::UpdateStyle(bool secondMonitorMode, bool showMenu) {
    if (!m_hwnd) return;

    LONG style = GetWindowLong(m_hwnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);

    if (secondMonitorMode) {
        style &= ~WS_POPUP;
        style |= WS_OVERLAPPEDWINDOW;

        exStyle &= ~WS_EX_TOPMOST;
        exStyle &= ~WS_EX_TRANSPARENT;
        exStyle &= ~WS_EX_TOOLWINDOW;
        exStyle |= WS_EX_APPWINDOW;

        BOOL useDarkMode = TRUE;
        DwmSetWindowAttribute(m_hwnd, 20, &useDarkMode, sizeof(useDarkMode));
        m_frameExtended = false;

        SetWindowTextW(m_hwnd, L"OmniStats");
        SetWindowTextA(m_hwnd, "OmniStats");
    } else {
        style &= ~WS_OVERLAPPEDWINDOW;
        style |= WS_POPUP;

        exStyle |= WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
        exStyle &= ~WS_EX_APPWINDOW;
        if (!showMenu) {
            exStyle |= WS_EX_TRANSPARENT;
        } else {
            exStyle &= ~WS_EX_TRANSPARENT;
        }

        BOOL useDarkMode = FALSE;
        DwmSetWindowAttribute(m_hwnd, 20, &useDarkMode, sizeof(useDarkMode));
    }

    SetWindowLong(m_hwnd, GWL_STYLE, style);
    SetWindowLong(m_hwnd, GWL_EXSTYLE, exStyle);

    if (!secondMonitorMode) {
        SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
        if (!m_frameExtended) {
            MARGINS margins = {-1, -1, -1, -1};
            DwmExtendFrameIntoClientArea(m_hwnd, &margins);
            m_frameExtended = true;
        }
    }

    SetWindowPos(m_hwnd,
                 secondMonitorMode ? HWND_NOTOPMOST : HWND_TOPMOST,
                 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    if (!secondMonitorMode) {
        RedrawWindow(m_hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

void OverlayWindow::UpdatePosition(bool resetSecondMonitorPlacement) {
    if (!m_hwnd) return;

    RECT rect = GetSecondaryMonitorRect();
    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;

    std::cout << "[Overlay] Resizing window to match monitor bounds: "
              << rect.left << ", " << rect.top << " (" << w << "x" << h << ")\n";

    ConfigData conf = Config::Read();
    if (conf.second_monitor_mode) {
        if (conf.second_monitor_has_bounds && conf.second_monitor_w > 0 && conf.second_monitor_h > 0) {
            RECT savedRect = {
                conf.second_monitor_x,
                conf.second_monitor_y,
                conf.second_monitor_x + conf.second_monitor_w,
                conf.second_monitor_y + conf.second_monitor_h};
            if (MonitorFromRect(&savedRect, MONITOR_DEFAULTTONULL)) {
                SetWindowPos(m_hwnd, HWND_NOTOPMOST,
                             conf.second_monitor_x, conf.second_monitor_y,
                             conf.second_monitor_w, conf.second_monitor_h,
                             SWP_NOACTIVATE);
                return;
            }
        }

        if (!resetSecondMonitorPlacement && MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONULL)) {
            return;
        }

        w = 1024;
        h = 768;
        int x = rect.left + (rect.right - rect.left - w) / 2;
        int y = rect.top + (rect.bottom - rect.top - h) / 2;

        SetWindowPos(m_hwnd, HWND_NOTOPMOST, x, y, w, h, SWP_NOACTIVATE);
    } else {
        SetWindowPos(m_hwnd, HWND_TOPMOST, rect.left, rect.top, w, h, SWP_NOACTIVATE);
    }
}

struct MonitorData {
    RECT rect;
    bool foundSecondary;
};

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MonitorData* data = reinterpret_cast<MonitorData*>(dwData);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(hMonitor, &mi)) {
        if (!(mi.dwFlags & MONITORINFOF_PRIMARY)) {
            data->rect = mi.rcMonitor;
            data->foundSecondary = true;
            return FALSE;
        }
    }
    return TRUE;
}

RECT OverlayWindow::GetSecondaryMonitorRect() const {
    MonitorData data = {};
    data.rect.left = 0;
    data.rect.top = 0;
    data.rect.right = GetSystemMetrics(SM_CXSCREEN);
    data.rect.bottom = GetSystemMetrics(SM_CYSCREEN);

    if (Config::Read().second_monitor_mode) {
        EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&data));
    }
    return data.rect;
}
