#pragma once

#include <atomic>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <windows.h>

#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "ui/window/D3D11Device.hpp"
#include "ui/window/OverlayWindow.hpp"
#include "ui/window/TrayIcon.hpp"
#include "ui/window/ForegroundDetector.hpp"

class DatabaseManager;
class RmlUiController;
class AccountManagerService;
class RemoteConfig;
class TelemetryManager;
namespace Tournaments {
    class TournamentScheduleService;
}

class Overlay {
  public:
    friend LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    Overlay(std::shared_ptr<SessionState> state,
            std::shared_ptr<DatabaseManager> dbManager = nullptr,
            std::shared_ptr<AccountManagerService> accounts = nullptr,
            std::shared_ptr<RemoteConfig> remoteConfig = nullptr,
            std::shared_ptr<Tournaments::TournamentScheduleService> tournamentService = nullptr,
            std::shared_ptr<TelemetryManager> telemetry = nullptr);
    ~Overlay();

    bool Initialize();
    void RunLoop();
    void Shutdown();
    void UpdateWindowStyle();
    void ResizeSwapChain(int width, int height);
    // Runs once on the UI thread after the user accepts the current Privacy Policy and Terms.
    void SetConsentHandler(std::function<void()> handler) {
        m_consentHandler = std::move(handler);
    }

  private:
    void UpdateWindowPosition(bool resetSecondMonitorPlacement = true);
    void SyncSetupWindow();
    int SetupWindowPixels(float dp) const;
    bool UsesDesktopWindow() const noexcept {
        return m_frameConfig.second_monitor_mode || m_setupWindow;
    }
    void SaveSecondMonitorWindowBounds();
    void HandleDpiChanged(UINT dpi, const RECT* suggestedRect);
    bool RecreateD3DDevice();
    bool HandleDeviceLost(const char* reason, HRESULT hr);

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<AccountManagerService> m_accounts;
    ConfigData m_frameConfig;
    std::shared_ptr<RemoteConfig> m_remoteConfig;
    std::shared_ptr<Tournaments::TournamentScheduleService> m_tournamentService;
    std::shared_ptr<TelemetryManager> m_telemetry;

    std::unique_ptr<OverlayWindow> m_window;
    std::unique_ptr<TrayIcon> m_trayIcon;
    std::unique_ptr<D3D11Device> m_d3d11;
    std::unique_ptr<RmlUiController> m_rmlUi;
    std::unique_ptr<ForegroundDetector> m_foreground;

    HWND m_hwnd = nullptr;
    float m_dpiScale = 1.0f;
    bool m_lastSecondMonitorMode = false;
    bool m_setupWindow = false;
    std::function<void()> m_consentHandler;

    std::atomic<bool> m_resizePending{false};
    std::atomic<int> m_pendingWidth{0};
    std::atomic<int> m_pendingHeight{0};
};
