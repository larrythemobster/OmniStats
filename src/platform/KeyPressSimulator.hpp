#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <cwchar>
#include <iterator>
#include <string_view>
#include <thread>

inline bool IsRocketLeagueImagePath(std::wstring_view imagePath) {
    const size_t slash = imagePath.find_last_of(L"\\/");
    const std::wstring_view fileName = slash == std::wstring_view::npos ? imagePath : imagePath.substr(slash + 1);
    constexpr std::wstring_view kExecutable = L"RocketLeague.exe";
    return fileName.size() == kExecutable.size() && _wcsnicmp(fileName.data(), kExecutable.data(), kExecutable.size()) == 0;
}

inline bool IsRocketLeagueWindow(HWND hwnd) {
    if (!hwnd) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;
    wchar_t imagePath[MAX_PATH * 4]{};
    DWORD length = static_cast<DWORD>(std::size(imagePath));
    const bool queried = QueryFullProcessImageNameW(process, 0, imagePath, &length) != FALSE;
    CloseHandle(process);
    return queried && IsRocketLeagueImagePath(std::wstring_view(imagePath, length));
}

inline void SimulateSaveReplayKeyPress(int virtualKey, int holdMs = 50) {
    if (!IsRocketLeagueWindow(GetForegroundWindow())) return;

    INPUT ip{};
    ip.type = INPUT_KEYBOARD;
    ip.ki.wScan = static_cast<WORD>(MapVirtualKey(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC));
    ip.ki.time = 0;
    ip.ki.dwExtraInfo = 0;
    ip.ki.wVk = static_cast<WORD>(virtualKey);
    ip.ki.dwFlags = 0;
    SendInput(1, &ip, sizeof(INPUT));

    std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
    ip.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &ip, sizeof(INPUT));
}
