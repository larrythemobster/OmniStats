#include "TrayIcon.hpp"
#include "core/Config.hpp"
#include <shellapi.h>
#include <gdiplus.h>
#include <objbase.h>
#include <iostream>
#include "core/Storage.hpp"
#include <fstream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <string>

#pragma comment(lib, "gdiplus.lib")

HICON LoadAppIcon(int width, int height) {
    HMODULE hModule = GetModuleHandle(nullptr);
    HICON hIcon = nullptr;
    if (width > 0 && height > 0) {
        hIcon = reinterpret_cast<HICON>(LoadImageW(
            hModule,
            MAKEINTRESOURCEW(1),
            IMAGE_ICON,
            width,
            height,
            LR_DEFAULTCOLOR | LR_SHARED
        ));
    }
    if (!hIcon) {
        hIcon = LoadIconW(hModule, MAKEINTRESOURCEW(1));
    }
    if (!hIcon) {
        hIcon = LoadIcon(NULL, IDI_APPLICATION);
    }
    return hIcon;
}

#define WM_TRAYICON (WM_APP + 2)
#define ID_TRAY_EXIT 1001
#define ID_TRAY_TOGGLE_MODE 1002
TrayIcon::TrayIcon(HWND mainHwnd) : m_mainHwnd(mainHwnd) {}

TrayIcon::~TrayIcon() { Shutdown(); }

bool TrayIcon::Initialize() {
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        m_startupComplete = false;
        m_shutdownRequested = false;
    }
    m_thread = std::thread(&TrayIcon::ThreadFunc, this);
    {
        std::unique_lock<std::mutex> lock(m_startupMutex);
        m_startupCv.wait_for(lock, std::chrono::seconds(2), [this]() { return m_startupComplete; });
    }
    m_initialized = true;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_hWnd != nullptr;
    }
}

void TrayIcon::Shutdown() {
    HWND hwnd = nullptr;
    DWORD threadId = 0;
    {
        std::unique_lock<std::mutex> lock(m_startupMutex);
        m_shutdownRequested = true;
        if (m_thread.joinable() && !m_startupComplete) {
            m_startupCv.wait_for(lock, std::chrono::seconds(2), [this]() { return m_startupComplete; });
        }
        hwnd = m_hWnd;
        threadId = m_threadId;
    }

    if (hwnd) {
        if (!PostMessageW(hwnd, WM_CLOSE, 0, 0) && threadId != 0) {
            PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        }
    } else if (threadId != 0) {
        PostThreadMessageW(threadId, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_initialized = false;
}

HICON TrayIcon::LoadIconFromPNG(bool& owned) {
    owned = false;
    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        return LoadIcon(NULL, IDI_APPLICATION);
    }

    HMODULE hModule = GetModuleHandle(NULL);
    HRSRC hRes = FindResourceA(hModule, "LOGO_PNG", RT_RCDATA);
    if (!hRes) {
        std::cout << "[Tray] LOGO_PNG resource not found in exe\n";
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
        return LoadIcon(NULL, IDI_APPLICATION);
    }

    HGLOBAL hData = LoadResource(hModule, hRes);
    DWORD dataSize = SizeofResource(hModule, hRes);
    void* pData = LockResource(hData);
    if (!pData || dataSize == 0) {
        std::cout << "[Tray] Failed to load LOGO_PNG resource\n";
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
        return LoadIcon(NULL, IDI_APPLICATION);
    }

    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, dataSize);
    void* pGlobal = GlobalLock(hGlobal);
    memcpy(pGlobal, pData, dataSize);
    GlobalUnlock(hGlobal);

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(hGlobal, TRUE, &pStream);
    if (!pStream) {
        GlobalFree(hGlobal);
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
        return LoadIcon(NULL, IDI_APPLICATION);
    }

    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(pStream);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        pStream->Release();
        std::cout << "[Tray] Failed to decode embedded Logo.png\n";
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
        return LoadIcon(NULL, IDI_APPLICATION);
    }

    Gdiplus::Bitmap* resized = new Gdiplus::Bitmap(32, 32, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(resized);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(bmp, 0, 0, 32, 32);
    }

    HICON hIcon = nullptr;
    resized->GetHICON(&hIcon);
    delete resized;
    delete bmp;
    pStream->Release();
    Gdiplus::GdiplusShutdown(m_gdiplusToken);
    m_gdiplusToken = 0;
    owned = hIcon != nullptr;
    return hIcon;
}

void TrayIcon::AddIcon(HWND hwnd) {
    if (!m_appIcon) m_appIcon = LoadIconFromPNG(m_appIconOwned);

    memset(&m_nid, 0, sizeof(NOTIFYICONDATAW));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = m_appIcon;
    wcscpy_s(m_nid.szTip, L"OmniStats");
    BOOL added = Shell_NotifyIconW(NIM_ADD, &m_nid);
    if (!added) {
        std::cout << "[Tray] Failed to add tray icon. Error=" << GetLastError() << "\n";
    }

    // Prefer latest notify icon behavior where available
    // This helps the system understand our icon version and can improve shell interactions
    m_nid.uVersion = NOTIFYICON_VERSION_4;
    BOOL setver = Shell_NotifyIconW(NIM_SETVERSION, &m_nid);
    if (!setver) {
        std::cout << "[Tray] Failed to set tray icon version. Error=" << GetLastError() << "\n";
    }

}

void TrayIcon::RemoveIcon() {
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    if (m_appIcon && m_appIconOwned) { DestroyIcon(m_appIcon); }
    m_appIcon = nullptr;
    m_appIconOwned = false;
    if (m_gdiplusToken) { Gdiplus::GdiplusShutdown(m_gdiplusToken); m_gdiplusToken = 0; }
}

void TrayIcon::ThreadFunc() {
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        m_threadId = GetCurrentThreadId();
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wcx = {};
    wcx.cbSize = sizeof(wcx);
    wcx.lpfnWndProc = [](HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
    static UINT taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
        if (msg == taskbarCreatedMsg) {
            TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
            if (self) self->AddIcon(hWnd);
            return 0;
        }

        switch (msg) {
            case WM_TRAYICON: {
                UINT event = LOWORD(lParam);
                if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK || event == 0x0400 /*NIN_SELECT*/) {
                    POINT pt{};
                    GetCursorPos(&pt);

                    HMENU hMenu = CreatePopupMenu();
                    if (!hMenu) {
                        std::cout << "[Tray] CreatePopupMenu failed. Error=" << GetLastError() << "\n";
                        return 0;
                    }

                    ConfigData conf = Config::Read();
                    UINT modeFlags = MF_STRING;
                    if (conf.second_monitor_mode) modeFlags |= MF_CHECKED;
                    std::wstring menuText = conf.second_monitor_mode ? L"Second Monitor Dashboard: On" : L"Second Monitor Dashboard: Off";
                    AppendMenuW(hMenu, modeFlags, ID_TRAY_TOGGLE_MODE, menuText.c_str());
                    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

                    ShowWindow(hWnd, SW_SHOW);
                    SetForegroundWindow(hWnd);
                    UINT cmd = TrackPopupMenuEx(
                        hMenu,
                        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                        pt.x, pt.y, hWnd, nullptr
                    );
                    ShowWindow(hWnd, SW_HIDE);

                    DestroyMenu(hMenu);
                    PostMessageW(hWnd, WM_NULL, 0, 0);

                    TrayIcon* self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
                    if (cmd == ID_TRAY_EXIT) {
                        if (self && self->m_mainHwnd) {
                            if (!PostMessageW(self->m_mainHwnd, WM_CLOSE, 0, 0)) {
                                std::cout << "[Tray] Failed to post close message. Error=" << GetLastError() << "\n";
                            }
                        }
                    } else if (cmd == ID_TRAY_TOGGLE_MODE) {
                        if (self && self->m_mainHwnd) {
                            if (!PostMessageW(self->m_mainHwnd, WM_TOGGLE_MODE, 0, 0)) {
                                std::cout << "[Tray] Failed to post toggle message. Error=" << GetLastError() << "\n";
                            }
                        }
                    }
                }
                return 0;
            }
            case WM_CLOSE:
                DestroyWindow(hWnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    };
    wcx.hInstance = GetModuleHandle(nullptr);
    wcx.lpszClassName = L"OmniStatsTrayClass";
    bool classRegistered = true;
    if (!RegisterClassExW(&wcx)) {
        DWORD err = GetLastError();
        classRegistered = false;
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::cout << "[Tray] Failed to register tray window class. Error=" << err << "\n";
            {
                std::lock_guard<std::mutex> lock(m_startupMutex);
                m_startupComplete = true;
            }
            m_startupCv.notify_all();
            CoUninitialize();
            return;
        }
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        wcx.lpszClassName, L"OmniStatsTray",
        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wcx.hInstance, nullptr
    );

    if (!hwnd) {
        std::cout << "[Tray] Failed to create tray window.\n";
        {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            m_startupComplete = true;
        }
        m_startupCv.notify_all();
        if (classRegistered) {
            UnregisterClassW(wcx.lpszClassName, wcx.hInstance);
        }
        CoUninitialize();
        return;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    bool shutdownRequested = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        m_hWnd = hwnd;
        shutdownRequested = m_shutdownRequested;
        m_startupComplete = true;
    }
    m_startupCv.notify_all();

    if (shutdownRequested) {
        DestroyWindow(hwnd);
    } else {
        AddIcon(hwnd);
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    RemoveIcon();
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        m_hWnd = nullptr;
        m_threadId = 0;
    }
    if (classRegistered) {
        UnregisterClassW(wcx.lpszClassName, wcx.hInstance);
    }
    CoUninitialize();
}
