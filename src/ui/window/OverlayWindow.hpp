#pragma once
#include <windows.h>

class OverlayWindow {
  public:
    OverlayWindow();
    ~OverlayWindow() {
        Destroy();
    }

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(HICON appIconBig, HICON appIconSmall, WNDPROC wndProc);
    void Destroy();

    void UpdateStyle(bool secondMonitorMode, bool showMenu);
    void UpdatePosition(bool resetSecondMonitorPlacement = true);
    RECT GetSecondaryMonitorRect() const;
    void SetDpiScale(float dpiScale) {
        m_dpiScale = dpiScale;
    }

    HWND Handle() const {
        return m_hwnd;
    }
    float DpiScale() const {
        return m_dpiScale;
    }

  private:
    HWND m_hwnd = nullptr;
    WNDCLASSEXW m_wc = {};
    bool m_classRegistered = false;
    float m_dpiScale = 1.0f;
    bool m_frameExtended = false;
};
