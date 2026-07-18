#pragma once
#include <windows.h>
#include <string>

inline constexpr const char* kKeyNames[] = {"Tab", "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Tilde (~)", "Backspace"};
inline constexpr int kKeyCodes[] = {VK_TAB, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12, VK_OEM_3, VK_BACK};
inline constexpr int kNumKeys = 15;

inline const char* GetKeyName(int vk) {
    if (vk <= 0) return "None";
    for (int i = 0; i < kNumKeys; ++i) {
        if (kKeyCodes[i] == vk) return kKeyNames[i];
    }
    return "?";
}

inline std::string GetKeyDisplayName(int vk) {
    for (int i = 0; i < kNumKeys; ++i) {
        if (kKeyCodes[i] == vk) return kKeyNames[i];
    }
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));

    switch (vk) {
    case VK_SPACE:
        return "Space";
    case VK_RETURN:
        return "Enter";
    case VK_ESCAPE:
        return "Escape";
    case VK_SHIFT:
        return "Shift";
    case VK_CONTROL:
        return "Ctrl";
    case VK_MENU:
        return "Alt";
    case VK_LSHIFT:
        return "Left Shift";
    case VK_RSHIFT:
        return "Right Shift";
    case VK_LCONTROL:
        return "Left Ctrl";
    case VK_RCONTROL:
        return "Right Ctrl";
    case VK_LMENU:
        return "Left Alt";
    case VK_RMENU:
        return "Right Alt";
    case VK_LEFT:
        return "Left Arrow";
    case VK_RIGHT:
        return "Right Arrow";
    case VK_UP:
        return "Up Arrow";
    case VK_DOWN:
        return "Down Arrow";
    case VK_INSERT:
        return "Insert";
    case VK_DELETE:
        return "Delete";
    case VK_HOME:
        return "Home";
    case VK_END:
        return "End";
    case VK_PRIOR:
        return "Page Up";
    case VK_NEXT:
        return "Page Down";
    }

    UINT scanCode = MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scanCode != 0) {
        LONG keyNameParam = static_cast<LONG>(scanCode << 16);
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
            keyNameParam |= static_cast<LONG>(1 << 24);
            break;
        }

        char name[64] = {};
        if (GetKeyNameTextA(keyNameParam, name, static_cast<int>(sizeof(name))) > 0) {
            return name;
        }
    }

    return "VK " + std::to_string(vk);
}
