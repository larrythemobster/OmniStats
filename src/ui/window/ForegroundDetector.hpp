#pragma once
#include <windows.h>
#include <chrono>
#include <string_view>
#include "ui/window/WindowUtils.hpp"

// Pure function determining whether a window's class and title match Rocket League.
bool IsRocketLeagueWindow(std::string_view className, std::string_view windowTitle);

// Pure function classifying a window given its class, title, pid, and context.
ForegroundKind ClassifyForegroundWindow(
    std::string_view className,
    std::string_view windowTitle,
    DWORD windowPid,
    DWORD omniStatsPid,
    bool isOverlayHwnd,
    bool isCachedRlHwnd);

class ForegroundDetector {
  public:
    ForegroundDetector() = default;
    ~ForegroundDetector() {
        Shutdown();
    }

    ForegroundDetector(const ForegroundDetector&) = delete;
    ForegroundDetector& operator=(const ForegroundDetector&) = delete;

    bool Initialize(HWND overlayHwnd);
    void Shutdown();

    void OnForegroundChanged(HWND newForeground);

    bool IsRocketLeagueActive() const noexcept {
        return m_isRLActive;
    }
    HWND CachedRocketLeagueWindow() const noexcept;

  private:
    void UpdateFromHwnd(HWND hwnd);

    HWND m_overlayHwnd = nullptr;
    HWINEVENTHOOK m_hook = nullptr;
    mutable HWND m_cachedRlHwnd = nullptr;
    mutable std::chrono::steady_clock::time_point m_lastFindWindow{};
    bool m_isRLActive = false;
    bool m_lastExternalForegroundWasRL = false;
};
