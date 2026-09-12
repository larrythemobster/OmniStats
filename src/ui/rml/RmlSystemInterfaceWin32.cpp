#include "ui/rml/RmlSystemInterfaceWin32.hpp"

#include <imm.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace {
    std::wstring ToWide(const std::string& utf8) {
        if (utf8.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), count);
        return result;
    }

    std::string ToUtf8(const wchar_t* text) {
        if (!text || !*text) return {};
        const int chars = static_cast<int>(wcslen(text));
        const int count = WideCharToMultiByte(CP_UTF8, 0, text, chars, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(count), '\0');
        if (count > 0) WideCharToMultiByte(CP_UTF8, 0, text, chars, result.data(), count, nullptr, nullptr);
        return result;
    }
}

RmlSystemInterfaceWin32::RmlSystemInterfaceWin32(HWND window) : m_window(window) {
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&m_start);
    m_secondsPerTick = frequency.QuadPart > 0 ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;
}

double RmlSystemInterfaceWin32::GetElapsedTime() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - m_start.QuadPart) * m_secondsPerTick;
}

void RmlSystemInterfaceWin32::JoinPath(Rml::String& out, const Rml::String& documentPath, const Rml::String& path) {
    if (path.rfind("res://", 0) == 0 || path.find("://") != Rml::String::npos || (path.size() > 2 && path[1] == ':')) {
        out = path;
        return;
    }
    if (documentPath.rfind("res://", 0) == 0) {
        std::string base = documentPath;
        const size_t slash = base.find_last_of('/');
        if (slash != std::string::npos)
            base.resize(slash + 1);
        else
            base = "res://";
        out = base + path;
        return;
    }
    Rml::SystemInterface::JoinPath(out, documentPath, path);
}

void RmlSystemInterfaceWin32::SetMouseCursor(const Rml::String& cursorName) {
    HCURSOR cursor = LoadCursor(nullptr, IDC_ARROW);
    if (cursorName == "pointer")
        cursor = LoadCursor(nullptr, IDC_HAND);
    else if (cursorName == "move" || cursorName.rfind("rmlui-scroll", 0) == 0)
        cursor = LoadCursor(nullptr, IDC_SIZEALL);
    else if (cursorName == "resize")
        cursor = LoadCursor(nullptr, IDC_SIZENWSE);
    else if (cursorName == "cross")
        cursor = LoadCursor(nullptr, IDC_CROSS);
    else if (cursorName == "text")
        cursor = LoadCursor(nullptr, IDC_IBEAM);
    else if (cursorName == "unavailable")
        cursor = LoadCursor(nullptr, IDC_NO);

    // Do not mutate the window-class cursor here. Windows sends WM_SETCURSOR
    // while the pointer moves and DefWindowProc will otherwise race RmlUi's
    // hover cursor, producing a visible arrow/hand flash over clickable items.
    // Cache the cursor selected by RmlUi and re-apply it from WM_SETCURSOR.
    m_mouseCursor = cursor;
    ApplyMouseCursor();
}

bool RmlSystemInterfaceWin32::ApplyMouseCursor() {
    if (!m_mouseCursor) m_mouseCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!m_mouseCursor) return false;
    SetCursor(m_mouseCursor);
    return true;
}

void RmlSystemInterfaceWin32::SetClipboardText(const Rml::String& text) {
    if (!OpenClipboard(m_window)) return;
    EmptyClipboard();
    const std::wstring wide = ToWide(text);
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            std::memcpy(data, wide.c_str(), bytes);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void RmlSystemInterfaceWin32::GetClipboardText(Rml::String& text) {
    text.clear();
    if (!OpenClipboard(m_window)) return;
    HANDLE memory = GetClipboardData(CF_UNICODETEXT);
    if (memory) {
        const auto* wide = static_cast<const wchar_t*>(GlobalLock(memory));
        if (wide) {
            text = ToUtf8(wide);
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
}

void RmlSystemInterfaceWin32::ActivateKeyboard(Rml::Vector2f caretPosition, float lineHeight) {
    if (!m_window) return;
    HIMC context = ImmGetContext(m_window);
    if (!context) return;
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_FORCE_POSITION;
    composition.ptCurrentPos = {static_cast<LONG>(caretPosition.x), static_cast<LONG>(caretPosition.y)};
    ImmSetCompositionWindow(context, &composition);

    CANDIDATEFORM candidate{};
    candidate.dwStyle = CFS_EXCLUDE;
    candidate.ptCurrentPos = composition.ptCurrentPos;
    candidate.rcArea = {composition.ptCurrentPos.x, composition.ptCurrentPos.y,
                        composition.ptCurrentPos.x + 1, composition.ptCurrentPos.y + static_cast<LONG>(lineHeight) + 2};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(m_window, context);
}
