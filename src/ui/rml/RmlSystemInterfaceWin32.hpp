#pragma once

#include <RmlUi/Core/SystemInterface.h>
#include <windows.h>

class RmlSystemInterfaceWin32 final : public Rml::SystemInterface {
  public:
    explicit RmlSystemInterfaceWin32(HWND window = nullptr);

    void SetWindow(HWND window) {
        m_window = window;
    }
    double GetElapsedTime() override;
    void JoinPath(Rml::String& translatedPath, const Rml::String& documentPath, const Rml::String& path) override;
    void SetMouseCursor(const Rml::String& cursorName) override;
    bool ApplyMouseCursor();
    void LockCursor(const char* cursorName = nullptr);
    void UnlockCursor();
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;
    void ActivateKeyboard(Rml::Vector2f caretPosition, float lineHeight) override;

  private:
    HWND m_window = nullptr;
    HCURSOR m_mouseCursor = nullptr;
    bool m_cursorLocked = false;
    LARGE_INTEGER m_start{};
    double m_secondsPerTick = 0.0;
};
