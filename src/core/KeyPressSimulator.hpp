#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <thread>
#include <chrono>

inline void SimulateSaveReplayKeyPress(int virtualKey, int holdMs = 50) {
    HWND hwnd = GetForegroundWindow();
    if (hwnd) {
        char windowTitle[256];
        if (GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle))) {
            std::string title(windowTitle);
            if (title.find("Rocket League") == std::string::npos) {
                return;
            }
        }
    }

    INPUT ip;
    ip.type = INPUT_KEYBOARD;
    ip.ki.wScan = MapVirtualKey(virtualKey, MAPVK_VK_TO_VSC);
    ip.ki.time = 0;
    ip.ki.dwExtraInfo = 0;
    ip.ki.wVk = virtualKey;
    ip.ki.dwFlags = 0;
    SendInput(1, &ip, sizeof(INPUT));
    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    ip.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &ip, sizeof(INPUT));
}
