#include "ui/rml/services/InputCaptureService.hpp"
#include <windows.h>
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"

void InputCaptureService::BeginCapture(std::shared_ptr<SessionState> state, BindCaptureTarget target) {
    if (!state) return;
    m_target = target;
    state->ui.inputCaptureActive.store(true);
    state->ui.lastKeyboardKeyPressed.store(-1);
    state->ui.lastControllerButtonPressed.store(-1);
    state->ui.lastRawControllerButtonPressed.store(-1);
}

void InputCaptureService::FinishCapture(std::shared_ptr<SessionState> state) {
    m_target = BindCaptureTarget::None;
    if (state) state->ui.inputCaptureActive.store(false);
}

void InputCaptureService::ClearBind(BindCaptureTarget target) {
    if (target == BindCaptureTarget::KeyMenu) return;
    Config::Update([target](ConfigData& c) {
        switch (target) {
        case BindCaptureTarget::KeyOverlay:
            c.key_overlay = -1;
            break;
        case BindCaptureTarget::KeyCycle:
            c.key_cycle = -1;
            break;
        case BindCaptureTarget::KeyExpand:
            c.key_expand = -1;
            break;
        case BindCaptureTarget::KeySession:
            c.key_session = -1;
            break;
        case BindCaptureTarget::KeySaveReplay:
            c.key_save_replay = -1;
            break;
        case BindCaptureTarget::KeyGraphPanLeft:
            c.key_graph_pan_left = -1;
            break;
        case BindCaptureTarget::KeyGraphPanRight:
            c.key_graph_pan_right = -1;
            break;
        case BindCaptureTarget::KeyLifetimeGraph:
            c.key_lifetime_graph = -1;
            break;
        case BindCaptureTarget::GamepadOverlay:
            c.gamepad_overlay = -1;
            c.gamepad_overlay_raw = false;
            c.gamepad_overlay_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadCycle:
            c.gamepad_cycle = -1;
            c.gamepad_cycle_raw = false;
            c.gamepad_cycle_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadExpand:
            c.gamepad_expand = -1;
            c.gamepad_expand_raw = false;
            c.gamepad_expand_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadSession:
            c.gamepad_session = -1;
            c.gamepad_session_raw = false;
            c.gamepad_session_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadMenu:
            c.gamepad_menu = -1;
            c.gamepad_menu_raw = false;
            c.gamepad_menu_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanLeft:
            c.gamepad_graph_pan_left = -1;
            c.gamepad_graph_pan_left_raw = false;
            c.gamepad_graph_pan_left_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanRight:
            c.gamepad_graph_pan_right = -1;
            c.gamepad_graph_pan_right_raw = false;
            c.gamepad_graph_pan_right_raw_button = -1;
            break;
        default:
            break;
        }
    });
}

bool InputCaptureService::Update(std::shared_ptr<SessionState> state) {
    if (!state || m_target == BindCaptureTarget::None) return false;
    const int vk = state->ui.lastKeyboardKeyPressed.load();
    if (vk == VK_ESCAPE) {
        FinishCapture(state);
        return true;
    }

    const bool controllerTarget = m_target >= BindCaptureTarget::GamepadOverlay;
    if (controllerTarget) {
        const bool mapped = state->ui.controllerIsGameController.load();
        const int mappedButton = state->ui.lastControllerButtonPressed.load();
        const int rawButton = state->ui.lastRawControllerButtonPressed.load();
        if ((mapped && mappedButton >= 0) || rawButton >= 0) {
            const auto target = m_target;
            Config::Update([=](ConfigData& c) {
                const bool useRaw = !(mapped && mappedButton >= 0);
                const int button = useRaw ? rawButton : mappedButton;
                auto set = [&](int& normal, bool& raw, int& rawValue) {
                    raw = useRaw;
                    if (useRaw)
                        rawValue = button;
                    else
                        normal = button;
                };
                switch (target) {
                case BindCaptureTarget::GamepadOverlay:
                    set(c.gamepad_overlay, c.gamepad_overlay_raw, c.gamepad_overlay_raw_button);
                    break;
                case BindCaptureTarget::GamepadCycle:
                    set(c.gamepad_cycle, c.gamepad_cycle_raw, c.gamepad_cycle_raw_button);
                    break;
                case BindCaptureTarget::GamepadExpand:
                    set(c.gamepad_expand, c.gamepad_expand_raw, c.gamepad_expand_raw_button);
                    break;
                case BindCaptureTarget::GamepadSession:
                    set(c.gamepad_session, c.gamepad_session_raw, c.gamepad_session_raw_button);
                    break;
                case BindCaptureTarget::GamepadMenu:
                    set(c.gamepad_menu, c.gamepad_menu_raw, c.gamepad_menu_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanLeft:
                    set(c.gamepad_graph_pan_left, c.gamepad_graph_pan_left_raw, c.gamepad_graph_pan_left_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanRight:
                    set(c.gamepad_graph_pan_right, c.gamepad_graph_pan_right_raw, c.gamepad_graph_pan_right_raw_button);
                    break;
                default:
                    break;
                }
            });
            FinishCapture(state);
            return true;
        }
    } else if (vk > 0) {
        const auto target = m_target;
        Config::Update([=](ConfigData& c) {
            switch (target) {
            case BindCaptureTarget::KeyOverlay:
                c.key_overlay = vk;
                break;
            case BindCaptureTarget::KeyCycle:
                c.key_cycle = vk;
                break;
            case BindCaptureTarget::KeyExpand:
                c.key_expand = vk;
                break;
            case BindCaptureTarget::KeySession:
                c.key_session = vk;
                break;
            case BindCaptureTarget::KeySaveReplay:
                c.key_save_replay = vk;
                break;
            case BindCaptureTarget::KeyGraphPanLeft:
                c.key_graph_pan_left = vk;
                break;
            case BindCaptureTarget::KeyGraphPanRight:
                c.key_graph_pan_right = vk;
                break;
            case BindCaptureTarget::KeyLifetimeGraph:
                c.key_lifetime_graph = vk;
                break;
            default:
                break;
            }
        });
        FinishCapture(state);
        return true;
    }
    return false;
}
