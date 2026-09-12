#pragma once

#include <atomic>
#include <d3d11.h>
#include <memory>
#include <windows.h>

#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include "ui/D3D11Device.hpp"
#include "ui/OverlayWindow.hpp"
#include "ui/TrayIcon.hpp"

class DatabaseManager;
class RmlUiController;

class Overlay {
  public:
    friend LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    Overlay(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager = nullptr);
    ~Overlay();

    bool Initialize();
    void RunLoop();
    void Shutdown();
    void UpdateWindowStyle();
    void ResizeSwapChain(int width, int height);

  private:
    void UpdateWindowPosition(bool resetSecondMonitorPlacement = true);
    void SaveSecondMonitorWindowBounds();
    void HandleDpiChanged(UINT dpi, const RECT* suggestedRect);
    bool RecreateD3DDevice();
    bool HandleDeviceLost(const char* reason, HRESULT hr);

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    ConfigData m_frameConfig;

    std::unique_ptr<OverlayWindow> m_window;
    std::unique_ptr<TrayIcon> m_trayIcon;
    std::unique_ptr<D3D11Device> m_d3d11;
    std::unique_ptr<RmlUiController> m_rmlUi;

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    bool m_lastSecondMonitorMode = false;

    std::atomic<bool> m_resizePending{false};
    std::atomic<int> m_pendingWidth{0};
    std::atomic<int> m_pendingHeight{0};
};
