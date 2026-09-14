#pragma once
#include <windows.h>

// Keep second-monitor mode large enough for the two-column dashboard to remain
// usable. Values are logical (96-DPI) pixels and are scaled for the monitor.
inline constexpr int kSecondMonitorMinWidthDp = 800;
inline constexpr int kSecondMonitorMinHeightDp = 500;

inline SIZE GetSecondMonitorMinimumSize(float dpiScale) {
    if (dpiScale < 0.5f) dpiScale = 1.0f;
    SIZE size{};
    size.cx = static_cast<LONG>(kSecondMonitorMinWidthDp * dpiScale + 0.5f);
    size.cy = static_cast<LONG>(kSecondMonitorMinHeightDp * dpiScale + 0.5f);
    return size;
}

inline void ClampSecondMonitorSize(int& width, int& height, float dpiScale) {
    const SIZE minimum = GetSecondMonitorMinimumSize(dpiScale);
    if (width < minimum.cx) width = minimum.cx;
    if (height < minimum.cy) height = minimum.cy;
}

// Compute a centered rectangle of size (w,h) inside monitorRect
inline RECT ComputeCenteredRect(const RECT& monitorRect, int w, int h) {
    RECT r;
    int monitorW = monitorRect.right - monitorRect.left;
    int monitorH = monitorRect.bottom - monitorRect.top;
    int x = monitorRect.left + (monitorW - w) / 2;
    int y = monitorRect.top + (monitorH - h) / 2;
    r.left = x;
    r.top = y;
    r.right = x + w;
    r.bottom = y + h;
    return r;
}

// Compute window styles for overlay vs second-monitor mode and whether the settings menu is shown.
// This mutates the provided style/exStyle values (matching existing code patterns that first read
// the current window styles and then toggle bits).
inline void ComputeWindowStyles(bool secondMonitorMode, bool showMenu, LONG& style, LONG& exStyle) {
    if (secondMonitorMode) {
        style &= ~WS_POPUP;
        style |= WS_OVERLAPPEDWINDOW;

        exStyle &= ~WS_EX_TOPMOST;
        exStyle &= ~WS_EX_TRANSPARENT;
        exStyle &= ~WS_EX_TOOLWINDOW;
        exStyle &= ~WS_EX_LAYERED;
        exStyle |= WS_EX_APPWINDOW;
    } else {
        style &= ~WS_OVERLAPPEDWINDOW;
        style |= WS_POPUP;

        exStyle |= WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
        exStyle &= ~WS_EX_APPWINDOW;
        if (!showMenu)
            exStyle |= WS_EX_TRANSPARENT;
        else
            exStyle &= ~WS_EX_TRANSPARENT;
    }
}

// Determine if second-monitor dashboard should be raised to the top of the non-topmost
// window band when Rocket League regains active foreground status.
inline bool ShouldRaiseSecondMonitorWindow(bool secondMonitorMode, bool isRLActive, bool wasRLActive, bool isWindowVisible) {
    return secondMonitorMode && isRLActive && !wasRLActive && isWindowVisible;
}
