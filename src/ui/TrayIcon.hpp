#pragma once
#include <windows.h>
#include <shellapi.h>
#include <thread>
#include <memory>
#include <string>
#include <condition_variable>
#include <mutex>

// Messages posted from the tray thread to the main window
#define WM_TOGGLE_MODE (WM_APP + 1)
#define WM_OPEN_INSIGHTS (WM_APP + 3)

// Free function shared by TrayIcon and Overlay (main window icon)
HICON LoadAppIcon(int width = 0, int height = 0);

class TrayIcon {
  public:
    TrayIcon(HWND mainHwnd);
    ~TrayIcon();

    bool Initialize();
    void Shutdown();
    // Thread-safe; shows a Windows notification from the tray icon.
    void ShowNotification(std::wstring title, std::wstring text);

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
    std::mutex m_notificationMutex;
    std::wstring m_notificationTitle;
    std::wstring m_notificationText;
};
