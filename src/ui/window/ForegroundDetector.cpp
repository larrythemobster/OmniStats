#include "ui/window/ForegroundDetector.hpp"

namespace {
    ForegroundDetector* s_activeDetector = nullptr;

    HWND FindRocketLeagueWindow() {
        HWND hwnd = FindWindowA("LaunchUnrealUWindowsClient", nullptr);
        if (!hwnd) hwnd = FindWindowA(nullptr, "Rocket League (64-bit, DX11)");
        if (!hwnd) hwnd = FindWindowA(nullptr, "Rocket League (32-bit, DX11)");
        if (!hwnd) hwnd = FindWindowA(nullptr, "Rocket League");
        return hwnd;
    }

    void CALLBACK WinEventProc(
        HWINEVENTHOOK /*hWinEventHook*/,
        DWORD event,
        HWND hwnd,
        LONG /*idObject*/,
        LONG /*idChild*/,
        DWORD /*idEventThread*/,
        DWORD /*dwmsEventTime*/) {
        if (event == EVENT_SYSTEM_FOREGROUND && s_activeDetector) {
            s_activeDetector->OnForegroundChanged(hwnd);
        }
    }
}

bool IsRocketLeagueWindow(std::string_view className, std::string_view windowTitle) {
    if (className == "LaunchUnrealUWindowsClient") {
        return true;
    }
    const bool browserOrExplorer = className.find("Chrome") != std::string_view::npos ||
                                   className.find("Mozilla") != std::string_view::npos ||
                                   className.find("IEFrame") != std::string_view::npos ||
                                   className.find("CabinetWClass") != std::string_view::npos;
    if (!browserOrExplorer) {
        if (windowTitle == "Rocket League (64-bit, DX11)" ||
            windowTitle == "Rocket League (32-bit, DX11)" ||
            windowTitle == "Rocket League") {
            return true;
        }
    }
    return false;
}

ForegroundKind ClassifyForegroundWindow(
    std::string_view className,
    std::string_view windowTitle,
    bool isOverlayHwnd,
    bool isCachedRlHwnd) {
    if (isOverlayHwnd) {
        return ForegroundKind::Overlay;
    }
    if (isCachedRlHwnd) {
        return ForegroundKind::RocketLeague;
    }
    if (IsRocketLeagueWindow(className, windowTitle)) {
        return ForegroundKind::RocketLeague;
    }
    return ForegroundKind::Other;
}

bool ForegroundDetector::Initialize(HWND overlayHwnd) {
    Shutdown();
    m_overlayHwnd = overlayHwnd;
    s_activeDetector = this;
    m_lastFindWindow = std::chrono::steady_clock::now();
    m_cachedRlHwnd = FindRocketLeagueWindow();
    m_hook = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &WinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT);
    UpdateFromHwnd(GetForegroundWindow());
    return m_hook != nullptr;
}

void ForegroundDetector::Shutdown() {
    if (m_hook) {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    if (s_activeDetector == this) {
        s_activeDetector = nullptr;
    }
    m_overlayHwnd = nullptr;
    m_cachedRlHwnd = nullptr;
    m_lastFindWindow = {};
    m_foregroundKind = ForegroundKind::None;
    m_isRLActive = false;
    m_lastExternalForegroundWasRL = false;
}

void ForegroundDetector::OnForegroundChanged(HWND newForeground) {
    UpdateFromHwnd(newForeground);
}
bool ForegroundDetector::IsRocketLeagueActive() noexcept {
    const HWND rlHwnd = CachedRocketLeagueWindow();
    const bool unavailable = !rlHwnd || IsIconic(rlHwnd);
    m_isRLActive = ResolveRocketLeagueFocus(m_foregroundKind, unavailable, m_lastExternalForegroundWasRL);
    return m_isRLActive;
}

HWND ForegroundDetector::CachedRocketLeagueWindow() noexcept {
    if (m_cachedRlHwnd && !IsWindow(m_cachedRlHwnd)) {
        m_cachedRlHwnd = nullptr;
        if (m_foregroundKind == ForegroundKind::RocketLeague) {
            m_foregroundKind = ForegroundKind::None;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_cachedRlHwnd &&
        std::chrono::duration_cast<std::chrono::seconds>(now - m_lastFindWindow).count() >= 2) {
        m_lastFindWindow = now;
        m_cachedRlHwnd = FindRocketLeagueWindow();
    }
    return m_cachedRlHwnd;
}

void ForegroundDetector::UpdateFromHwnd(HWND fg) {
    if (!fg) {
        m_foregroundKind = ForegroundKind::None;
        m_isRLActive = ResolveRocketLeagueFocus(ForegroundKind::None, true, m_lastExternalForegroundWasRL);
        return;
    }

    const bool isCachedRL = (m_cachedRlHwnd && fg == m_cachedRlHwnd);
    const bool isOverlay = (fg == m_overlayHwnd);

    char className[256]{};
    char title[256]{};
    GetClassNameA(fg, className, sizeof(className));

    DWORD_PTR textLength = 0;
    SendMessageTimeoutA(fg, WM_GETTEXT, sizeof(title),
                        reinterpret_cast<LPARAM>(title),
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &textLength);

    ForegroundKind kind = ClassifyForegroundWindow(className, title, isOverlay, isCachedRL);

    bool rlUnavailable = !m_cachedRlHwnd || !IsWindow(m_cachedRlHwnd) || IsIconic(m_cachedRlHwnd);
    if (kind == ForegroundKind::RocketLeague) {
        m_cachedRlHwnd = fg;
        rlUnavailable = IsIconic(fg) != FALSE;
    }

    m_foregroundKind = kind;
    m_isRLActive = ResolveRocketLeagueFocus(kind, rlUnavailable, m_lastExternalForegroundWasRL);
}
