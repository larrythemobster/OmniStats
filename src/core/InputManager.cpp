#include "InputManager.hpp"
#include "Config.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iostream>
#include <chrono>
#include <cstring>

namespace {
    constexpr int STICK_DEADZONE = 20000;
    constexpr int GAMEPAD_STICK_LS_UP = 22;
    constexpr int GAMEPAD_STICK_LS_DOWN = 23;
    constexpr int GAMEPAD_STICK_LS_LEFT = 24;
    constexpr int GAMEPAD_STICK_LS_RIGHT = 25;
    constexpr int GAMEPAD_STICK_RS_UP = 26;
    constexpr int GAMEPAD_STICK_RS_DOWN = 27;
    constexpr int GAMEPAD_STICK_RS_LEFT = 28;
    constexpr int GAMEPAD_STICK_RS_RIGHT = 29;
    constexpr int MAX_TRACKED_BUTTONS = 35;

    enum HotKeyId {
        HotKeyMenu = 1,
        HotKeyCycle = 2,
        HotKeyExpand = 3,
        HotKeySession = 4,
        HotKeyDashboardEdit = 5,
    };

    bool IsRocketLeagueForeground() {
        HWND foreground = GetForegroundWindow();
        if (!foreground) {
            return false;
        }

        char className[256] = {0};
        if (GetClassNameA(foreground, className, sizeof(className)) > 0 &&
            std::strcmp(className, "LaunchUnrealUWindowsClient") == 0) {
            return true;
        }

        char windowTitle[256] = {0};
        if (GetWindowTextA(foreground, windowTitle, sizeof(windowTitle)) <= 0) {
            return false;
        }

        return std::strcmp(windowTitle, "Rocket League (64-bit, DX11)") == 0 ||
               std::strcmp(windowTitle, "Rocket League (32-bit, DX11)") == 0 ||
               std::strcmp(windowTitle, "Rocket League") == 0;
    }

    void ToggleDashboardEditMode(SessionState& state) {
        bool current = state.ui.dashboardLayoutEditMode.load();
        state.ui.dashboardLayoutEditMode.store(!current);
        if (!current) {
            Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
        } else {
            Config::RequestSave();
        }
    }

    void ToggleSettingsMenu(SessionState& state) {
        bool showMenu = !state.ui.showMenu.load();
        state.ui.showMenu = showMenu;
        if (showMenu) {
            state.ui.dashboardLayoutEditMode.store(false);
        } else if (state.ui.dashboardLayoutEditMode.load()) {
            state.ui.dashboardLayoutEditMode.store(false);
            Config::RequestSave();
        }
    }

    void HandleEscape(SessionState& state) {
        if (state.ui.dashboardLayoutEditMode.load()) {
            state.ui.dashboardLayoutEditMode.store(false);
            Config::RequestSave();
        }
        state.ui.showMenu = false;
        state.ui.showOverlay = false;
    }

    MmrCategory NextMmrCategory(MmrCategory current, bool showExtraPlaylists) {
        if (current == MmrCategory::Best)
            return MmrCategory::OneVOne;
        if (current == MmrCategory::OneVOne)
            return MmrCategory::TwoVTwo;
        if (current == MmrCategory::TwoVTwo)
            return MmrCategory::ThreeVThree;
        if (current == MmrCategory::ThreeVThree)
            return showExtraPlaylists ? MmrCategory::Hoops : MmrCategory::Casual;
        if (current == MmrCategory::Hoops)
            return showExtraPlaylists ? MmrCategory::Rumble : MmrCategory::Casual;
        if (current == MmrCategory::Rumble)
            return showExtraPlaylists ? MmrCategory::Dropshot : MmrCategory::Casual;
        if (current == MmrCategory::Dropshot)
            return showExtraPlaylists ? MmrCategory::SnowDay : MmrCategory::Casual;
        if (current == MmrCategory::SnowDay)
            return showExtraPlaylists ? MmrCategory::Heatseeker : MmrCategory::Casual;
        if (current == MmrCategory::Heatseeker)
            return MmrCategory::Casual;
        if (current == MmrCategory::Casual)
            return MmrCategory::Tourny;
        return MmrCategory::Best;
    }

} // namespace

InputManager::InputManager(std::shared_ptr<SessionState> state)
    : m_state(state) {
    g_instance.store(this, std::memory_order_release);
}

InputManager::~InputManager() {
    Stop();
}

void InputManager::Start() {
    m_isRunning = true;
    RefreshConfigCache();

    SDL_SetMainReady();
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);

    m_keyboardThread = std::jthread(&InputManager::KeyboardThreadLoop, this);
    m_gamepadThread = std::jthread(&InputManager::GamepadThreadLoop, this);
}

void InputManager::Stop() {
    m_isRunning = false;
    DWORD keyboardThreadId = m_keyboardThreadId.load(std::memory_order_acquire);
    if (keyboardThreadId != 0) {
        PostThreadMessage(keyboardThreadId, WM_QUIT, 0, 0);
    }
    if (m_keyboardThread.joinable())
        m_keyboardThread.join();
#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
#endif
    if (m_gamepadThread.joinable())
        m_gamepadThread.join();

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    g_instance.store(nullptr, std::memory_order_release);
}

void InputManager::RefreshConfigCache() {
    const ConfigData conf = Config::Read();
    m_keyOverlay.store(conf.key_overlay, std::memory_order_relaxed);
    m_keyCycle.store(conf.key_cycle, std::memory_order_relaxed);
    m_keyExpand.store(conf.key_expand, std::memory_order_relaxed);
    m_keySession.store(conf.key_session, std::memory_order_relaxed);
    m_keyMenu.store(conf.key_menu, std::memory_order_relaxed);
    m_gamepadOverlay.store(conf.gamepad_overlay, std::memory_order_relaxed);
    m_gamepadOverlayRaw.store(conf.gamepad_overlay_raw, std::memory_order_relaxed);
    m_gamepadOverlayRawButton.store(conf.gamepad_overlay_raw_button, std::memory_order_relaxed);
    m_gamepadCycle.store(conf.gamepad_cycle, std::memory_order_relaxed);
    m_gamepadCycleRaw.store(conf.gamepad_cycle_raw, std::memory_order_relaxed);
    m_gamepadCycleRawButton.store(conf.gamepad_cycle_raw_button, std::memory_order_relaxed);

    m_gamepadExpand.store(conf.gamepad_expand, std::memory_order_relaxed);
    m_gamepadExpandRaw.store(conf.gamepad_expand_raw, std::memory_order_relaxed);
    m_gamepadExpandRawButton.store(conf.gamepad_expand_raw_button, std::memory_order_relaxed);

    m_gamepadSession.store(conf.gamepad_session, std::memory_order_relaxed);
    m_gamepadSessionRaw.store(conf.gamepad_session_raw, std::memory_order_relaxed);
    m_gamepadSessionRawButton.store(conf.gamepad_session_raw_button, std::memory_order_relaxed);

    m_gamepadMenu.store(conf.gamepad_menu, std::memory_order_relaxed);
    m_gamepadMenuRaw.store(conf.gamepad_menu_raw, std::memory_order_relaxed);
    m_gamepadMenuRawButton.store(conf.gamepad_menu_raw_button, std::memory_order_relaxed);
    m_secondMonitorMode.store(conf.second_monitor_mode, std::memory_order_relaxed);
    m_showExtraPlaylists.store(conf.show_extra_playlists, std::memory_order_relaxed);
}

bool InputManager::HandleWindowMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    (void)lParam;
    auto* self = g_instance.load(std::memory_order_acquire);
    if (!self || !self->m_state) {
        return false;
    }

    auto& state = *self->m_state;
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) &&
        state.ui.inputCaptureActive.load(std::memory_order_relaxed)) {
        state.ui.lastKeyboardKeyPressed.store(static_cast<int>(wParam));
        return false;
    }

    if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && state.ui.showMenu.load()) {
        HandleEscape(state);
        return true;
    }

    return false;
}

void InputManager::KeyboardThreadLoop() {
    m_keyboardThreadId.store(GetCurrentThreadId(), std::memory_order_release);

    MSG msg;
    PeekMessage(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    if (!m_isRunning) {
        m_keyboardThreadId.store(0, std::memory_order_release);
        return;
    }

#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    m_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                              GetModuleHandle(NULL), 0);
    if (!m_hook) {
        std::cout << "[Input] Failed to install keyboard hook!\n";
        m_keyboardThreadId.store(0, std::memory_order_release);
        return;
    }

    while (m_isRunning) {
        BOOL result = GetMessage(&msg, nullptr, 0, 0);
        if (result <= 0) break;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
#else
    int registeredMenuKey = -1;
    int registeredCycleKey = -1;
    int registeredExpandKey = -1;
    int registeredSessionKey = -1;
    bool registeredDashboardEdit = false;

    auto unregisterRegisteredHotkeys = [&]() {
        UnregisterHotKey(NULL, HotKeyMenu);
        UnregisterHotKey(NULL, HotKeyCycle);
        UnregisterHotKey(NULL, HotKeyExpand);
        UnregisterHotKey(NULL, HotKeySession);
        UnregisterHotKey(NULL, HotKeyDashboardEdit);
        registeredMenuKey = -1;
        registeredCycleKey = -1;
        registeredExpandKey = -1;
        registeredSessionKey = -1;
        registeredDashboardEdit = false;
    };

    auto registerHotKeyIfNeeded = [](int id, int key, int& registeredKey) {
        if (registeredKey == key) {
            return;
        }

        if (registeredKey != -1) {
            UnregisterHotKey(NULL, id);
            registeredKey = -1;
        }

        if (key <= 0) {
            return;
        }

        if (RegisterHotKey(NULL, id, MOD_NOREPEAT, static_cast<UINT>(key))) {
            registeredKey = key;
        } else {
            std::cout << "[Input] Failed to register hotkey " << id << " for key " << key
                      << ". Error: " << GetLastError() << "\n";
        }
    };

    auto registerDashboardEditIfNeeded = [&]() {
        if (registeredDashboardEdit) {
            return;
        }
        if (RegisterHotKey(NULL, HotKeyDashboardEdit, MOD_CONTROL | MOD_NOREPEAT, 'E')) {
            registeredDashboardEdit = true;
        } else {
            std::cout << "[Input] Failed to register dashboard edit hotkey. Error: "
                      << GetLastError() << "\n";
        }
    };

    auto handleHotKey = [&](int hotKeyId) {
        if (!m_state) {
            return;
        }

        auto& state = *m_state;
        if (state.ui.inputCaptureActive.load(std::memory_order_relaxed)) {
            return;
        }

        if (hotKeyId == HotKeyMenu) {
            ToggleSettingsMenu(state);
            return;
        }

        if (hotKeyId == HotKeyCycle) {
            if (!state.ui.showOverlay.load()) {
                return;
            }
            MmrCategory next = NextMmrCategory(
                state.ui.rosterMmrCategory.load(),
                m_showExtraPlaylists.load(std::memory_order_relaxed));
            state.ui.rosterMmrCategory.store(next);
            if (!m_secondMonitorMode.load(std::memory_order_relaxed)) {
                state.ui.graphMmrCategory.store(next);
            }
            Config::Update([next](ConfigData& conf) {
                conf.mmr_category = MmrCategoryToString(next);
            });
            return;
        }

        if (hotKeyId == HotKeyExpand) {
            if (!state.ui.showOverlay.load()) {
                return;
            }
            if (state.ui.showSessionView) {
                state.ui.showGraphView = !state.ui.showGraphView;
            } else {
                state.ui.h2hExpanded = !state.ui.h2hExpanded;
            }
            return;
        }

        if (hotKeyId == HotKeySession) {
            if (!state.ui.showOverlay.load()) {
                return;
            }
            state.ui.showSessionView = !state.ui.showSessionView;
            if (!state.ui.showSessionView) {
                state.ui.showGraphView = false;
            }
            return;
        }

        if (hotKeyId == HotKeyDashboardEdit && state.ui.showMenu.load()) {
            ToggleDashboardEditMode(state);
        }
    };

    while (m_isRunning) {
        RefreshConfigCache();

        bool inputCaptureActive = false;
        if (m_state) {
            inputCaptureActive = m_state->ui.inputCaptureActive.load(std::memory_order_relaxed);
        }

        if (inputCaptureActive) {
            unregisterRegisteredHotkeys();
        } else {
            registerHotKeyIfNeeded(HotKeyMenu, m_keyMenu.load(std::memory_order_relaxed), registeredMenuKey);
            registerHotKeyIfNeeded(HotKeyCycle, m_keyCycle.load(std::memory_order_relaxed), registeredCycleKey);
            registerHotKeyIfNeeded(HotKeyExpand, m_keyExpand.load(std::memory_order_relaxed), registeredExpandKey);
            registerHotKeyIfNeeded(HotKeySession, m_keySession.load(std::memory_order_relaxed), registeredSessionKey);
            registerDashboardEditIfNeeded();
        }

        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_isRunning = false;
                break;
            }
            if (msg.message == WM_HOTKEY) {
                handleHotKey(static_cast<int>(msg.wParam));
            } else {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
        }

        if (!m_isRunning) {
            break;
        }

        if (m_state) {
            auto& state = *m_state;
            const bool controllerConnected = state.ui.controllerConnected.load(std::memory_order_relaxed);
            if (!inputCaptureActive && IsRocketLeagueForeground()) {
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
                    (GetAsyncKeyState(m_keyOverlay.load(std::memory_order_relaxed)) & 0x8000)) {
                    state.ui.showOverlay = true;
                } else if (!controllerConnected) {
                    state.ui.showOverlay = false;
                }
            } else if (!inputCaptureActive && !controllerConnected) {
                state.ui.showOverlay = false;
            }
        }

        MsgWaitForMultipleObjectsEx(0, NULL, 25, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    unregisterRegisteredHotkeys();
#endif
    m_keyboardThreadId.store(0, std::memory_order_release);
}

#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
LRESULT CALLBACK InputManager::LowLevelKeyboardProc(int nCode, WPARAM wParam,
                                                    LPARAM lParam) {
    auto* self = g_instance.load(std::memory_order_acquire);
    if (!self) return CallNextHookEx(m_hook, nCode, wParam, lParam);

    if (nCode == HC_ACTION && self->m_state) {
        auto& st = self->m_state;
        KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
        const bool keyDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        if (keyDown) {
            st->ui.lastKeyboardKeyPressed.store(static_cast<int>(kb->vkCode));
        }
        if (st->ui.inputCaptureActive.load(std::memory_order_relaxed)) {
            return CallNextHookEx(m_hook, nCode, wParam, lParam);
        }
        const int keyOverlay = self->m_keyOverlay.load(std::memory_order_relaxed);
        const int keyCycle = self->m_keyCycle.load(std::memory_order_relaxed);
        const int keyExpand = self->m_keyExpand.load(std::memory_order_relaxed);
        const int keySession = self->m_keySession.load(std::memory_order_relaxed);
        const int keyMenu = self->m_keyMenu.load(std::memory_order_relaxed);
        const bool secondMonitorMode = self->m_secondMonitorMode.load(std::memory_order_relaxed);
        const bool showExtraPlaylists = self->m_showExtraPlaylists.load(std::memory_order_relaxed);

        if (kb->vkCode == keyOverlay) {
            bool isAltDown = (GetKeyState(VK_MENU) & 0x8000);
            if (!isAltDown) {
                if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
                    st->ui.showOverlay = true;
                else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
                    st->ui.showOverlay = false;
                return CallNextHookEx(m_hook, nCode, wParam, lParam);
            }
        }

        if (kb->vkCode == keyCycle && wParam == WM_KEYDOWN && st->ui.showOverlay.load()) {
            MmrCategory next = NextMmrCategory(st->ui.rosterMmrCategory.load(), showExtraPlaylists);
            st->ui.rosterMmrCategory.store(next);
            if (!secondMonitorMode) {
                st->ui.graphMmrCategory.store(next);
            }
            Config::Update([next](ConfigData& conf) {
                conf.mmr_category = MmrCategoryToString(next);
            });
        }

        if (kb->vkCode == keyExpand && wParam == WM_KEYDOWN && st->ui.showOverlay.load()) {
            if (st->ui.showSessionView) {
                st->ui.showGraphView = !st->ui.showGraphView;
            } else {
                st->ui.h2hExpanded = !st->ui.h2hExpanded;
            }
        }

        if (kb->vkCode == keySession && wParam == WM_KEYDOWN && st->ui.showOverlay.load()) {
            st->ui.showSessionView = !st->ui.showSessionView;
            if (!st->ui.showSessionView) {
                st->ui.showGraphView = false;
            }
        }

        if (kb->vkCode == keyMenu && wParam == WM_KEYDOWN) {
            ToggleSettingsMenu(*st);
        }

        bool isCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (st->ui.showMenu.load() && isCtrlDown && kb->vkCode == 'E' && wParam == WM_KEYDOWN) {
            ToggleDashboardEditMode(*st);
        }

        if (kb->vkCode == VK_ESCAPE && wParam == WM_KEYDOWN) {
            HandleEscape(*st);
        }
    }
    return CallNextHookEx(m_hook, nCode, wParam, lParam);
}
#endif

void InputManager::GamepadThreadLoop() {
    SDL_GameController* controller = nullptr;
    SDL_Joystick* fallbackJoystick = nullptr;
    auto lastScan = std::chrono::steady_clock::now();
    int lastLoggedDeviceCount = -1;

    bool prevControllerStates[MAX_TRACKED_BUTTONS] = {false};
    bool prevRawControllerStates[256] = {false};
    bool prevFallbackStates[256] = {false};

    auto updateDebugInfo = [&](const char* name, bool isGameController, bool connected) {
        auto* self = g_instance.load(std::memory_order_acquire);
        if (self && self->m_state) {
            auto& ui = self->m_state->ui;
            ui.controllerIsGameController.store(isGameController);
            ui.controllerConnected.store(connected);
            {
                std::lock_guard<std::mutex> lock(ui.controllerDebugMutex);
                ui.controllerDebugName = name ? name : "Unknown";
            }
        }
    };

    while (m_isRunning) {
        auto* self = g_instance.load(std::memory_order_acquire);
        if (self) {
            self->RefreshConfigCache();
        }
        auto now = std::chrono::steady_clock::now();

        if (controller == nullptr && fallbackJoystick == nullptr &&
            std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count() >= 2) {
            lastScan = now;
            int deviceCount = SDL_NumJoysticks();
            bool deviceListChanged = deviceCount != lastLoggedDeviceCount;
            if (deviceListChanged) {
                lastLoggedDeviceCount = deviceCount;
            }

            for (int i = 0; i < deviceCount; ++i) {
                const bool isCtrl = SDL_IsGameController(i) == SDL_TRUE;
                if (isCtrl) {
                    controller = SDL_GameControllerOpen(i);
                    if (controller) {
                        updateDebugInfo(SDL_GameControllerName(controller), true, true);
                        break;
                    }
                }
            }

            if (!controller) {
                for (int i = 0; i < deviceCount; ++i) {
                    fallbackJoystick = SDL_JoystickOpen(i);
                    if (fallbackJoystick) {
                        updateDebugInfo(SDL_JoystickName(fallbackJoystick), false, true);
                        break;
                    }
                }
            }
        }

        if (self && self->m_state) {
            auto& st = self->m_state;
            const int gamepadOverlay = self->m_gamepadOverlay.load(std::memory_order_relaxed);
            const bool gamepadOverlayRaw = self->m_gamepadOverlayRaw.load(std::memory_order_relaxed);
            const int gamepadOverlayRawButton = self->m_gamepadOverlayRawButton.load(std::memory_order_relaxed);
            const int keyOverlay = self->m_keyOverlay.load(std::memory_order_relaxed);
            const bool inputCaptureActive = st->ui.inputCaptureActive.load(std::memory_order_relaxed);

            if (controller != nullptr) {
                if (!SDL_GameControllerGetAttached(controller)) {
                    SDL_GameControllerClose(controller);
                    controller = nullptr;
                    updateDebugInfo("", false, false);
                } else {
                    SDL_GameControllerUpdate();

                    bool currentStates[MAX_TRACKED_BUTTONS] = {false};
                    bool currentRawControllerStates[256] = {false};

                    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
                        currentStates[b] = SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(b)) != 0;
                        if (currentStates[b]) st->ui.lastControllerButtonPressed.store(b);
                    }

                    currentStates[GAMEPAD_STICK_LS_UP] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) < -STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_LS_DOWN] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) > STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_LS_LEFT] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) < -STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_LS_RIGHT] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) > STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_RS_UP] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) < -STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_RS_DOWN] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY) > STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_RS_LEFT] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) < -STICK_DEADZONE;
                    currentStates[GAMEPAD_STICK_RS_RIGHT] = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX) > STICK_DEADZONE;

                    for (int i = GAMEPAD_STICK_LS_UP; i <= GAMEPAD_STICK_RS_RIGHT; ++i) {
                        if (currentStates[i]) st->ui.lastControllerButtonPressed.store(i);
                    }

                    SDL_Joystick* rawJoystick = SDL_GameControllerGetJoystick(controller);
                    const int rawButtonCount = rawJoystick ? SDL_JoystickNumButtons(rawJoystick) : 0;
                    for (int b = 0; b < rawButtonCount && b < 256; ++b) {
                        if (SDL_JoystickGetButton(rawJoystick, b)) {
                            currentRawControllerStates[b] = true;
                            st->ui.lastRawControllerButtonPressed.store(b);
                        }
                    }

                    auto isPressed = [&](int bind, bool isRaw, int rawBind) {
                        if (isRaw) {
                            if (rawBind < 0 || rawBind >= 256) return false;
                            return currentRawControllerStates[rawBind] && !prevRawControllerStates[rawBind];
                        }
                        if (bind < 0 || bind >= MAX_TRACKED_BUTTONS) return false;
                        return currentStates[bind] && !prevControllerStates[bind];
                    };

                    bool buttonHeld = false;
                    if (gamepadOverlayRaw) {
                        buttonHeld = currentRawControllerStates[gamepadOverlayRawButton];
                    } else if (gamepadOverlay >= 0 && gamepadOverlay < MAX_TRACKED_BUTTONS) {
                        buttonHeld = currentStates[gamepadOverlay];
                    }

                    if (!inputCaptureActive) {
                        if (buttonHeld) {
                            st->ui.showOverlay = true;
                        } else if (!(GetAsyncKeyState(keyOverlay) & 0x8000)) {
                            st->ui.showOverlay = false;
                        }

                        const bool overlayVisible = st->ui.showOverlay.load();

                        if (overlayVisible && isPressed(self->m_gamepadCycle.load(std::memory_order_relaxed),
                                                        self->m_gamepadCycleRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadCycleRawButton.load(std::memory_order_relaxed))) {
                            MmrCategory next = NextMmrCategory(st->ui.rosterMmrCategory.load(), self->m_showExtraPlaylists.load(std::memory_order_relaxed));
                            st->ui.rosterMmrCategory.store(next);
                            if (!self->m_secondMonitorMode.load(std::memory_order_relaxed)) st->ui.graphMmrCategory.store(next);
                            Config::Update([next](ConfigData& conf) { conf.mmr_category = MmrCategoryToString(next); });
                        }

                        if (overlayVisible && isPressed(self->m_gamepadExpand.load(std::memory_order_relaxed),
                                                        self->m_gamepadExpandRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadExpandRawButton.load(std::memory_order_relaxed))) {
                            if (st->ui.showSessionView)
                                st->ui.showGraphView = !st->ui.showGraphView;
                            else
                                st->ui.h2hExpanded = !st->ui.h2hExpanded;
                        }

                        if (overlayVisible && isPressed(self->m_gamepadSession.load(std::memory_order_relaxed),
                                                        self->m_gamepadSessionRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadSessionRawButton.load(std::memory_order_relaxed))) {
                            st->ui.showSessionView = !st->ui.showSessionView;
                            if (!st->ui.showSessionView) st->ui.showGraphView = false;
                        }

                        if (isPressed(self->m_gamepadMenu.load(std::memory_order_relaxed),
                                      self->m_gamepadMenuRaw.load(std::memory_order_relaxed),
                                      self->m_gamepadMenuRawButton.load(std::memory_order_relaxed))) {
                            ToggleSettingsMenu(*st);
                        }
                    }

                    for (int i = 0; i < MAX_TRACKED_BUTTONS; ++i)
                        prevControllerStates[i] = currentStates[i];
                    for (int i = 0; i < 256; ++i)
                        prevRawControllerStates[i] = currentRawControllerStates[i];
                }
            } else if (fallbackJoystick != nullptr) {
                if (!SDL_JoystickGetAttached(fallbackJoystick)) {
                    SDL_JoystickClose(fallbackJoystick);
                    fallbackJoystick = nullptr;
                    updateDebugInfo("", false, false);
                } else {
                    SDL_JoystickUpdate();
                    const int numButtons = SDL_JoystickNumButtons(fallbackJoystick);

                    bool currentStates[256] = {false};
                    for (int b = 0; b < numButtons && b < 256; ++b) {
                        currentStates[b] = SDL_JoystickGetButton(fallbackJoystick, b) != 0;
                        if (currentStates[b]) {
                            st->ui.lastControllerButtonPressed.store(b);
                            st->ui.lastRawControllerButtonPressed.store(b);
                        }
                    }

                    auto isPressed = [&](int bind, bool isRaw, int rawBind) {
                        int activeBind = isRaw ? rawBind : bind;
                        if (activeBind < 0 || activeBind >= 256) return false;
                        return currentStates[activeBind] && !prevFallbackStates[activeBind];
                    };

                    const int overlayButton = gamepadOverlayRaw ? gamepadOverlayRawButton : gamepadOverlay;
                    const bool buttonHeld = overlayButton >= 0 && overlayButton < 256 && currentStates[overlayButton];

                    if (!inputCaptureActive) {
                        if (buttonHeld) {
                            st->ui.showOverlay = true;
                        } else if (!(GetAsyncKeyState(keyOverlay) & 0x8000)) {
                            st->ui.showOverlay = false;
                        }

                        const bool overlayVisible = st->ui.showOverlay.load();

                        if (overlayVisible && isPressed(self->m_gamepadCycle.load(std::memory_order_relaxed),
                                                        self->m_gamepadCycleRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadCycleRawButton.load(std::memory_order_relaxed))) {
                            MmrCategory next = NextMmrCategory(st->ui.rosterMmrCategory.load(), self->m_showExtraPlaylists.load(std::memory_order_relaxed));
                            st->ui.rosterMmrCategory.store(next);
                            if (!self->m_secondMonitorMode.load(std::memory_order_relaxed)) st->ui.graphMmrCategory.store(next);
                            Config::Update([next](ConfigData& conf) { conf.mmr_category = MmrCategoryToString(next); });
                        }

                        if (overlayVisible && isPressed(self->m_gamepadExpand.load(std::memory_order_relaxed),
                                                        self->m_gamepadExpandRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadExpandRawButton.load(std::memory_order_relaxed))) {
                            if (st->ui.showSessionView)
                                st->ui.showGraphView = !st->ui.showGraphView;
                            else
                                st->ui.h2hExpanded = !st->ui.h2hExpanded;
                        }

                        if (overlayVisible && isPressed(self->m_gamepadSession.load(std::memory_order_relaxed),
                                                        self->m_gamepadSessionRaw.load(std::memory_order_relaxed),
                                                        self->m_gamepadSessionRawButton.load(std::memory_order_relaxed))) {
                            st->ui.showSessionView = !st->ui.showSessionView;
                            if (!st->ui.showSessionView) st->ui.showGraphView = false;
                        }

                        if (isPressed(self->m_gamepadMenu.load(std::memory_order_relaxed),
                                      self->m_gamepadMenuRaw.load(std::memory_order_relaxed),
                                      self->m_gamepadMenuRawButton.load(std::memory_order_relaxed))) {
                            ToggleSettingsMenu(*st);
                        }
                    }

                    for (int i = 0; i < 256; ++i)
                        prevFallbackStates[i] = currentStates[i];
                }
            }
        }
        const bool hasController = controller != nullptr || fallbackJoystick != nullptr;
        std::this_thread::sleep_for(hasController ? std::chrono::milliseconds(16) : std::chrono::milliseconds(250));
    }

    if (controller) SDL_GameControllerClose(controller);
    if (fallbackJoystick) SDL_JoystickClose(fallbackJoystick);
}