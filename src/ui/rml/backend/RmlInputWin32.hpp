#pragma once

#include <RmlUi/Core/Input.h>
#include <windows.h>

namespace Rml {
    class Context;
}

namespace RmlInputWin32 {
    int GetKeyModifiers();
    Rml::Input::KeyIdentifier ConvertKey(WPARAM key);
    bool ProcessWindowMessage(Rml::Context* context, HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
}
