#include "ui/rml/RmlInputWin32.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>
#include <string>

namespace {
    wchar_t g_highSurrogate = 0;
}

namespace RmlInputWin32 {
    int GetKeyModifiers() {
        int modifiers = 0;
        if (GetKeyState(VK_CAPITAL) & 1) modifiers |= Rml::Input::KM_CAPSLOCK;
        if (GetKeyState(VK_NUMLOCK) & 1) modifiers |= Rml::Input::KM_NUMLOCK;
        if (GetKeyState(VK_SCROLL) & 1) modifiers |= Rml::Input::KM_SCROLLLOCK;
        if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= Rml::Input::KM_SHIFT;
        if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= Rml::Input::KM_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) modifiers |= Rml::Input::KM_ALT;
        if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000)) modifiers |= Rml::Input::KM_META;
        return modifiers;
    }

    Rml::Input::KeyIdentifier ConvertKey(WPARAM key) {
        if (key >= 'A' && key <= 'Z') return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_A + (key - 'A'));
        if (key >= '0' && key <= '9') return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_0 + (key - '0'));
        if (key >= VK_F1 && key <= VK_F12) return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_F1 + (key - VK_F1));
        if (key >= VK_F13 && key <= VK_F24) return static_cast<Rml::Input::KeyIdentifier>(Rml::Input::KI_F13 + (key - VK_F13));
        switch (key) {
        case VK_BACK:
            return Rml::Input::KI_BACK;
        case VK_TAB:
            return Rml::Input::KI_TAB;
        case VK_RETURN:
            return Rml::Input::KI_RETURN;
        case VK_PAUSE:
            return Rml::Input::KI_PAUSE;
        case VK_CAPITAL:
            return Rml::Input::KI_CAPITAL;
        case VK_ESCAPE:
            return Rml::Input::KI_ESCAPE;
        case VK_SPACE:
            return Rml::Input::KI_SPACE;
        case VK_PRIOR:
            return Rml::Input::KI_PRIOR;
        case VK_NEXT:
            return Rml::Input::KI_NEXT;
        case VK_END:
            return Rml::Input::KI_END;
        case VK_HOME:
            return Rml::Input::KI_HOME;
        case VK_LEFT:
            return Rml::Input::KI_LEFT;
        case VK_UP:
            return Rml::Input::KI_UP;
        case VK_RIGHT:
            return Rml::Input::KI_RIGHT;
        case VK_DOWN:
            return Rml::Input::KI_DOWN;
        case VK_INSERT:
            return Rml::Input::KI_INSERT;
        case VK_DELETE:
            return Rml::Input::KI_DELETE;
        case VK_SNAPSHOT:
            return Rml::Input::KI_SNAPSHOT;
        case VK_LWIN:
            return Rml::Input::KI_LWIN;
        case VK_RWIN:
            return Rml::Input::KI_RWIN;
        case VK_APPS:
            return Rml::Input::KI_APPS;
        case VK_NUMPAD0:
            return Rml::Input::KI_NUMPAD0;
        case VK_NUMPAD1:
            return Rml::Input::KI_NUMPAD1;
        case VK_NUMPAD2:
            return Rml::Input::KI_NUMPAD2;
        case VK_NUMPAD3:
            return Rml::Input::KI_NUMPAD3;
        case VK_NUMPAD4:
            return Rml::Input::KI_NUMPAD4;
        case VK_NUMPAD5:
            return Rml::Input::KI_NUMPAD5;
        case VK_NUMPAD6:
            return Rml::Input::KI_NUMPAD6;
        case VK_NUMPAD7:
            return Rml::Input::KI_NUMPAD7;
        case VK_NUMPAD8:
            return Rml::Input::KI_NUMPAD8;
        case VK_NUMPAD9:
            return Rml::Input::KI_NUMPAD9;
        case VK_MULTIPLY:
            return Rml::Input::KI_MULTIPLY;
        case VK_ADD:
            return Rml::Input::KI_ADD;
        case VK_SUBTRACT:
            return Rml::Input::KI_SUBTRACT;
        case VK_DECIMAL:
            return Rml::Input::KI_DECIMAL;
        case VK_DIVIDE:
            return Rml::Input::KI_DIVIDE;
        case VK_NUMLOCK:
            return Rml::Input::KI_NUMLOCK;
        case VK_SCROLL:
            return Rml::Input::KI_SCROLL;
        case VK_SHIFT:
            return (GetKeyState(VK_RSHIFT) & 0x8000) ? Rml::Input::KI_RSHIFT : Rml::Input::KI_LSHIFT;
        case VK_CONTROL:
            return (GetKeyState(VK_RCONTROL) & 0x8000) ? Rml::Input::KI_RCONTROL : Rml::Input::KI_LCONTROL;
        case VK_MENU:
            return (GetKeyState(VK_RMENU) & 0x8000) ? Rml::Input::KI_RMENU : Rml::Input::KI_LMENU;
        case VK_LSHIFT:
            return Rml::Input::KI_LSHIFT;
        case VK_RSHIFT:
            return Rml::Input::KI_RSHIFT;
        case VK_LCONTROL:
            return Rml::Input::KI_LCONTROL;
        case VK_RCONTROL:
            return Rml::Input::KI_RCONTROL;
        case VK_LMENU:
            return Rml::Input::KI_LMENU;
        case VK_RMENU:
            return Rml::Input::KI_RMENU;
        case VK_OEM_1:
            return Rml::Input::KI_OEM_1;
        case VK_OEM_PLUS:
            return Rml::Input::KI_OEM_PLUS;
        case VK_OEM_COMMA:
            return Rml::Input::KI_OEM_COMMA;
        case VK_OEM_MINUS:
            return Rml::Input::KI_OEM_MINUS;
        case VK_OEM_PERIOD:
            return Rml::Input::KI_OEM_PERIOD;
        case VK_OEM_2:
            return Rml::Input::KI_OEM_2;
        case VK_OEM_3:
            return Rml::Input::KI_OEM_3;
        case VK_OEM_4:
            return Rml::Input::KI_OEM_4;
        case VK_OEM_5:
            return Rml::Input::KI_OEM_5;
        case VK_OEM_6:
            return Rml::Input::KI_OEM_6;
        case VK_OEM_7:
            return Rml::Input::KI_OEM_7;
        case VK_OEM_102:
            return Rml::Input::KI_OEM_102;
        default:
            return Rml::Input::KI_UNKNOWN;
        }
    }

    bool ProcessWindowMessage(Rml::Context* context, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (!context) return false;
        const int modifiers = GetKeyModifiers();
        switch (message) {
        case WM_LBUTTONDOWN:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            SetCapture(hwnd);
            context->ProcessMouseButtonDown(0, modifiers);
            return true;
        case WM_LBUTTONUP:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            ReleaseCapture();
            context->ProcessMouseButtonUp(0, modifiers);
            return true;
        case WM_LBUTTONDBLCLK:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonDown(0, modifiers);
            return true;
        case WM_RBUTTONDOWN:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonDown(1, modifiers);
            return true;
        case WM_RBUTTONUP:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonUp(1, modifiers);
            return true;
        case WM_RBUTTONDBLCLK:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonDown(1, modifiers);
            return true;
        case WM_MBUTTONDOWN:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonDown(2, modifiers);
            return true;
        case WM_MBUTTONUP:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonUp(2, modifiers);
            return true;
        case WM_MBUTTONDBLCLK:
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            context->ProcessMouseButtonDown(2, modifiers);
            return true;
        case WM_MOUSEMOVE: {
            context->ProcessMouseMove(static_cast<int>(static_cast<short>(LOWORD(lParam))),
                                      static_cast<int>(static_cast<short>(HIWORD(lParam))), modifiers);
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            TrackMouseEvent(&track);
            return true;
        }
        case WM_MOUSELEAVE:
            context->ProcessMouseLeave();
            if (auto* sys = Rml::GetSystemInterface()) sys->SetMouseCursor("");
            return true;
        case WM_KILLFOCUS:
            ReleaseCapture();
            context->ProcessMouseLeave();
            if (auto* sys = Rml::GetSystemInterface()) sys->SetMouseCursor("");
            if (Rml::Element* focused = context->GetFocusElement()) focused->Blur();
            g_highSurrogate = 0;
            return false;
        case WM_SETFOCUS:
            g_highSurrogate = 0;
            return false;
        case WM_MOUSEWHEEL: {
            POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            ScreenToClient(hwnd, &pt);
            context->ProcessMouseMove(pt.x, pt.y, modifiers);
            const float delta = static_cast<float>(static_cast<short>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA);
            context->ProcessMouseWheel(Rml::Vector2f(0.0f, -delta), modifiers);
            return true;
        }
        case WM_MOUSEHWHEEL: {
            POINT pt{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
            ScreenToClient(hwnd, &pt);
            context->ProcessMouseMove(pt.x, pt.y, modifiers);
            const float delta = static_cast<float>(static_cast<short>(HIWORD(wParam))) / static_cast<float>(WHEEL_DELTA);
            context->ProcessMouseWheel(Rml::Vector2f(delta, 0.0f), modifiers);
            return true;
        }
        case WM_KEYDOWN:
            context->ProcessKeyDown(ConvertKey(wParam), modifiers);
            return true;
        case WM_SYSKEYDOWN:
            if (wParam == VK_F4 || wParam == VK_SPACE) return false;
            context->ProcessKeyDown(ConvertKey(wParam), modifiers);
            return true;
        case WM_KEYUP:
            context->ProcessKeyUp(ConvertKey(wParam), modifiers);
            return true;
        case WM_SYSKEYUP:
            if (wParam == VK_F4 || wParam == VK_SPACE) return false;
            context->ProcessKeyUp(ConvertKey(wParam), modifiers);
            return true;
        case WM_CHAR: {
            // Windows delivers UTF-16 surrogate pairs as two WM_CHAR messages.
            const wchar_t c = static_cast<wchar_t>(wParam);
            Rml::Character character = static_cast<Rml::Character>(c);
            if (c >= 0xD800 && c < 0xDC00) {
                g_highSurrogate = c;
                return true;
            }
            if (c >= 0xDC00 && c < 0xE000) {
                if (g_highSurrogate == 0) return true;
                const char32_t hi = static_cast<char32_t>(g_highSurrogate - 0xD800);
                const char32_t lo = static_cast<char32_t>(c - 0xDC00);
                character = static_cast<Rml::Character>(0x10000 + ((hi << 10) | lo));
            } else if (c == L'\r') {
                character = static_cast<Rml::Character>(L'\n');
            }
            g_highSurrogate = 0;
            if ((static_cast<char32_t>(character) >= 32 || character == static_cast<Rml::Character>(L'\n')) &&
                character != static_cast<Rml::Character>(127)) {
                context->ProcessTextInput(character);
            }
            return true;
        }
        default:
            return false;
        }
    }
}
