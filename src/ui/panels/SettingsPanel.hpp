#pragma once
#include <string>
#include "ui/RenderContext.hpp"

struct SettingsResult {
    bool styleChanged = false;
    bool windowChanged = false;
};

class SettingsPanel {
public:
    explicit SettingsPanel(RenderContext ctx);
    ~SettingsPanel();
    SettingsResult Render();
    void RenderContent(const std::string& idSuffix, bool& styleChanged, bool& windowChanged);
private:
    enum class BindCaptureTarget {
        None,
        KeyOverlay,
        KeyCycle,
        KeyExpand,
        KeySession,
        KeyMenu,
        KeySaveReplay,
        GamepadOverlay
    };

    RenderContext ctx;
    BindCaptureTarget m_bindCaptureTarget = BindCaptureTarget::None;
};
