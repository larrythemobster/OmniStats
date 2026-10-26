#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/presenters/SettingsPresenter.hpp"
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string SettingsPresenter::RenderShortcuts(const ConfigData& config, const std::shared_ptr<SessionState>& state, BindCaptureTarget bindCaptureTarget) {
    auto bindTarget = [](BindCaptureTarget target) { return std::to_string(static_cast<int>(target)); };
    auto keyRow = [&](const char* label, BindCaptureTarget target, int key, bool canClear = true) {
        std::ostringstream html;
        const bool active = bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << Escape(label) << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a key..." : Escape(GetKeyDisplayName(key))) << "</button>";
        if (active || canClear)
            html << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button>";
        else
            html << "<span class='bind-fixed'>Required</span>";
        html << "</div></div>";
        return html.str();
    };
    auto padName = [&](int value, bool raw, int rawValue) {
        return GamepadBindName(value, raw, rawValue);
    };
    auto padRow = [&](const char* label, BindCaptureTarget target, int value, bool raw, int rawValue) {
        std::ostringstream html;
        const bool active = bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << Escape(label) << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a button..." : Escape(padName(value, raw, rawValue))) << "</button>"
             << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
        return html.str();
    };

    std::ostringstream out;
    out << SectionStart("Keyboard")
        << keyRow("Show overlay", BindCaptureTarget::KeyOverlay, config.key_overlay)
        << keyRow("Cycle playlist", BindCaptureTarget::KeyCycle, config.key_cycle)
        << keyRow("Expand live stats", BindCaptureTarget::KeyExpand, config.key_expand)
        << keyRow("Session view", BindCaptureTarget::KeySession, config.key_session)
        << keyRow("Graph pan older", BindCaptureTarget::KeyGraphPanLeft, config.key_graph_pan_left)
        << keyRow("Graph pan newer", BindCaptureTarget::KeyGraphPanRight, config.key_graph_pan_right)
        << keyRow("Session / lifetime graph", BindCaptureTarget::KeyLifetimeGraph, config.key_lifetime_graph)
        << keyRow("Settings", BindCaptureTarget::KeyMenu, config.key_menu, false)
        << keyRow("Save replay", BindCaptureTarget::KeySaveReplay, config.key_save_replay)
        << SectionEnd();
    out << SectionStart("Controller")
        << padRow("Show overlay", BindCaptureTarget::GamepadOverlay, config.gamepad_overlay, config.gamepad_overlay_raw, config.gamepad_overlay_raw_button)
        << padRow("Cycle playlist", BindCaptureTarget::GamepadCycle, config.gamepad_cycle, config.gamepad_cycle_raw, config.gamepad_cycle_raw_button)
        << padRow("Expand live stats", BindCaptureTarget::GamepadExpand, config.gamepad_expand, config.gamepad_expand_raw, config.gamepad_expand_raw_button)
        << padRow("Session view", BindCaptureTarget::GamepadSession, config.gamepad_session, config.gamepad_session_raw, config.gamepad_session_raw_button)
        << padRow("Graph pan older", BindCaptureTarget::GamepadGraphPanLeft, config.gamepad_graph_pan_left, config.gamepad_graph_pan_left_raw, config.gamepad_graph_pan_left_raw_button)
        << padRow("Graph pan newer", BindCaptureTarget::GamepadGraphPanRight, config.gamepad_graph_pan_right, config.gamepad_graph_pan_right_raw, config.gamepad_graph_pan_right_raw_button)
        << padRow("Settings", BindCaptureTarget::GamepadMenu, config.gamepad_menu, config.gamepad_menu_raw, config.gamepad_menu_raw_button);
    out << "<div class='controller-debug'><div class='setting-help'>Controller diagnostics</div>";
    if (state && state->ui.controllerConnected.load()) {
        std::string controllerName;
        {
            std::lock_guard lock(state->ui.controllerDebugMutex);
            controllerName = state->ui.controllerDebugName;
        }
        const bool mapped = state->ui.controllerIsGameController.load();
        const int lastMapped = mapped ? state->ui.lastControllerButtonPressed.load() : -1;
        const int lastRaw = state->ui.lastRawControllerButtonPressed.load();
        out << "<div class='debug-grid'><div class='debug-row'><span class='debug-label'>Device</span><span class='debug-value'>" << Escape(controllerName.empty() ? "connected" : controllerName) << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>SDL GameController</span><span class='debug-value'>" << (mapped ? "yes" : "no · fallback joystick") << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>Overlay bind</span><span class='debug-value'>" << Escape(GamepadBindName(config.gamepad_overlay, config.gamepad_overlay_raw, config.gamepad_overlay_raw_button)) << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>Last mapped</span><span class='debug-value'>";
        if (lastMapped >= 0)
            out << Escape(GamepadBindName(lastMapped, false, -1)) << " (" << lastMapped << ")";
        else
            out << "none";
        out << "</span></div><div class='debug-row'><span class='debug-label'>Last raw</span><span class='debug-value'>" << (lastRaw >= 0 ? std::to_string(lastRaw) : "none") << "</span></div></div>";
    } else {
        out << "<div class='setting-help'>No controller detected.</div>";
    }
    out << "</div>" << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsShortcuts() {
    return m_settingsPresenter->RenderShortcuts(m_config, m_state, m_bindCaptureTarget);
}
