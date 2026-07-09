#pragma once
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <memory>
#include <condition_variable>
#include <mutex>

// Message posted from tray thread to main window
#define WM_TOGGLE_MODE (WM_APP + 1)

// Free function shared by TrayIcon and Overlay (main window icon)
HICON LoadAppIcon(int width = 0, int height = 0);

class TrayIcon {
public:
    TrayIcon(HWND mainHwnd);
    ~TrayIcon();

    bool Initialize();
    void Shutdown();

private:
    void ThreadFunc();
    void AddIcon(HWND hwnd);
    void RemoveIcon();
    HICON LoadIconFromPNG(bool& owned);

    HWND m_mainHwnd;
    HWND m_hWnd = nullptr;
    DWORD m_threadId = 0;
    std::thread m_thread;
    bool m_initialized = false;
    bool m_startupComplete = false;
    bool m_shutdownRequested = false;
    std::mutex m_startupMutex;
    std::condition_variable m_startupCv;

    NOTIFYICONDATAW m_nid = {};
    HICON m_appIcon = nullptr;
    bool m_appIconOwned = false;
    ULONG_PTR m_gdiplusToken = 0;
};
