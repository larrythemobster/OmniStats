#include "ui/rml/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string RmlUiController::RenderSettingsShortcuts() {
    auto bindTarget = [](BindCaptureTarget target) { return std::to_string(static_cast<int>(target)); };
    auto keyRow = [&](const char* label, BindCaptureTarget target, int key, bool canClear = true) {
        std::ostringstream html;
        const bool active = m_bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << Escape(label) << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a key..." : Escape(GetKeyDisplayName(key))) << "</button>";
        if (active || canClear) html << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button>";
        html << "</div></div>";
        return html.str();
    };
    auto padName = [&](int value, bool raw, int rawValue) {
        return GamepadBindName(value, raw, rawValue);
    };
    auto padRow = [&](const char* label, BindCaptureTarget target, int value, bool raw, int rawValue) {
        std::ostringstream html;
        const bool active = m_bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << Escape(label) << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a button..." : Escape(padName(value, raw, rawValue))) << "</button>"
             << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
        return html.str();
    };

    std::ostringstream out;
    out << SectionStart("Keyboard")
        << keyRow("Show overlay", BindCaptureTarget::KeyOverlay, m_config.key_overlay)
        << keyRow("Cycle playlist", BindCaptureTarget::KeyCycle, m_config.key_cycle)
        << keyRow("Expand live stats", BindCaptureTarget::KeyExpand, m_config.key_expand)
        << keyRow("Session view", BindCaptureTarget::KeySession, m_config.key_session)
        << keyRow("Graph pan older", BindCaptureTarget::KeyGraphPanLeft, m_config.key_graph_pan_left)
        << keyRow("Graph pan newer", BindCaptureTarget::KeyGraphPanRight, m_config.key_graph_pan_right)
        << keyRow("Settings", BindCaptureTarget::KeyMenu, m_config.key_menu, false)
        << keyRow("Save replay", BindCaptureTarget::KeySaveReplay, m_config.key_save_replay)
        << SectionEnd();
    out << SectionStart("Controller")
        << padRow("Show overlay", BindCaptureTarget::GamepadOverlay, m_config.gamepad_overlay, m_config.gamepad_overlay_raw, m_config.gamepad_overlay_raw_button)
        << padRow("Cycle playlist", BindCaptureTarget::GamepadCycle, m_config.gamepad_cycle, m_config.gamepad_cycle_raw, m_config.gamepad_cycle_raw_button)
        << padRow("Expand live stats", BindCaptureTarget::GamepadExpand, m_config.gamepad_expand, m_config.gamepad_expand_raw, m_config.gamepad_expand_raw_button)
        << padRow("Session view", BindCaptureTarget::GamepadSession, m_config.gamepad_session, m_config.gamepad_session_raw, m_config.gamepad_session_raw_button)
        << padRow("Graph pan older", BindCaptureTarget::GamepadGraphPanLeft, m_config.gamepad_graph_pan_left, m_config.gamepad_graph_pan_left_raw, m_config.gamepad_graph_pan_left_raw_button)
        << padRow("Graph pan newer", BindCaptureTarget::GamepadGraphPanRight, m_config.gamepad_graph_pan_right, m_config.gamepad_graph_pan_right_raw, m_config.gamepad_graph_pan_right_raw_button)
        << padRow("Settings", BindCaptureTarget::GamepadMenu, m_config.gamepad_menu, m_config.gamepad_menu_raw, m_config.gamepad_menu_raw_button);
    out << "<div class='controller-debug'><div class='setting-help'>Controller diagnostics</div>";
    if (m_state && m_state->ui.controllerConnected.load()) {
        std::string controllerName;
        {
            std::lock_guard lock(m_state->ui.controllerDebugMutex);
            controllerName = m_state->ui.controllerDebugName;
        }
        const bool mapped = m_state->ui.controllerIsGameController.load();
        const int lastMapped = mapped ? m_state->ui.lastControllerButtonPressed.load() : -1;
        const int lastRaw = m_state->ui.lastRawControllerButtonPressed.load();
        out << "<div class='debug-grid'><div class='debug-row'><span class='debug-label'>Device</span><span class='debug-value'>" << Escape(controllerName.empty() ? "connected" : controllerName) << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>SDL GameController</span><span class='debug-value'>" << (mapped ? "yes" : "no · fallback joystick") << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>Overlay bind</span><span class='debug-value'>" << Escape(GamepadBindName(m_config.gamepad_overlay, m_config.gamepad_overlay_raw, m_config.gamepad_overlay_raw_button)) << "</span></div>"
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
