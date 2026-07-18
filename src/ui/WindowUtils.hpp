#pragma once
#include <windows.h>

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
