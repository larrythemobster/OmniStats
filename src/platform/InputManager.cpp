#include "platform/InputManager.hpp"
#include "core/config/Config.hpp"
#include "network/telemetry/TelemetryManager.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iostream>
#include <chrono>
#include <algorithm>
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
        DWORD_PTR textLength = 0;
        if (SendMessageTimeoutA(foreground, WM_GETTEXT, sizeof(windowTitle),
                                reinterpret_cast<LPARAM>(windowTitle),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &textLength) == 0 ||
            textLength == 0) {
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

    MmrCategory NextGraphMmrCategory(MmrCategory current, bool showExtraPlaylists) {
        MmrCategory next = NextMmrCategory(current, showExtraPlaylists);
        return next == MmrCategory::Best ? MmrCategory::OneVOne : next;
    }

    void CycleMmrCategory(SessionState& state, bool showExtraPlaylists) {
        const MmrCategory nextRoster = NextMmrCategory(
            state.ui.rosterMmrCategory.load(), showExtraPlaylists);
        state.ui.rosterMmrCategory.store(nextRoster);

        Config::Update(
            [&state, nextRoster, showExtraPlaylists](ConfigData& conf) {
                conf.mmr_category = MmrCategoryToString(nextRoster);
                if (conf.graph_follow_current_playlist) {
                    return;
                }

                const MmrCategory nextGraph = NextGraphMmrCategory(
                    state.ui.graphMmrCategory.load(), showExtraPlaylists);
                state.ui.graphMmrCategory.store(nextGraph);
                conf.graph_mmr_category = MmrCategoryToString(nextGraph);
            });
    }

    void PanGraph(SessionState& state, int direction) {
        if (direction == 0) return;
        const auto category = state.ui.graphMmrCategory.load();
        const std::string playlist = MmrCategoryToString(category);
        int total = 0;
        const auto history = state.history.GetSnapshot();
        if (history->showLifetimeGraph) {
            total = static_cast<int>(history->lifetimeMmrY.size());
        } else {
            if (auto it = history->playlistHistoryY.find(playlist); it != history->playlistHistoryY.end()) {
                total = static_cast<int>(it->second.size());
            }
        }
        constexpr int kVisibleMatches = 25;
        if (total <= kVisibleMatches) return;
        const int maxOffset = total - kVisibleMatches;
        constexpr int step = 5;
        const int currentOffset = state.ui.graphOffset.load();
        const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
        state.ui.graphOffset.store(newOffset);
    }

    void ExpandActiveView(SessionState& state, bool graphDefaultLifetime, TelemetryManager* telemetry) {
        if (state.ui.showSessionView.load()) {
            if (!state.ui.showGraphView.load()) {
                if (telemetry) telemetry->RecordMilestone(TelemetryManager::Milestone::MmrGraphView);
                state.ui.showGraphView.store(true);
                state.history.SetShowLifetimeGraph(graphDefaultLifetime);
                state.ui.graphOffset.store(0);
            } else if (state.history.GetSnapshot()->showLifetimeGraph == graphDefaultLifetime) {
                state.history.SetShowLifetimeGraph(!graphDefaultLifetime);
                state.ui.graphOffset.store(0);
            } else {
                state.ui.showGraphView.store(false);
                state.history.SetShowLifetimeGraph(graphDefaultLifetime);
                state.ui.graphOffset.store(0);
            }
        } else if (state.ui.showOverlay.load()) {
            state.ui.h2hExpanded = !state.ui.h2hExpanded;
        }
    }

    void ToggleSessionView(SessionState& state, bool graphDefaultLifetime) {
        const bool showSessionView = !state.ui.showSessionView.load();
        state.ui.showSessionView.store(showSessionView);
        if (!showSessionView) {
            state.ui.showGraphView.store(false);
            state.history.SetShowLifetimeGraph(graphDefaultLifetime);
            state.ui.graphOffset.store(0);
        }
    }

    // Switches every graph between this session and lifetime history without
    // opening or closing any card.
    void ToggleLifetimeGraph(SessionState& state) {
        state.history.Mutate([](auto& history) { history.showLifetimeGraph = !history.showLifetimeGraph; });
        state.ui.graphOffset.store(0);
    }
} // namespace

void InputManager::HandleHotKeyAction(
    SessionState& state,
    int hotKeyId,
    bool showExtraPlaylists,
    bool graphDefaultLifetime,
    TelemetryManager* telemetry) {
    switch (hotKeyId) {
    case HotKeyMenu:
        ToggleSettingsMenu(state);
        break;
    case HotKeyCycle:
        CycleMmrCategory(state, showExtraPlaylists);
        break;
    case HotKeyExpand:
        ExpandActiveView(state, graphDefaultLifetime, telemetry);
        break;
    case HotKeySession:
        ToggleSessionView(state, graphDefaultLifetime);
        break;
    case HotKeyDashboardEdit:
        if (state.ui.showMenu.load()) {
            ToggleDashboardEditMode(state);
        }
        break;
    case HotKeyPanLeft:
        PanGraph(state, -1);
        break;
    case HotKeyPanRight:
        PanGraph(state, 1);
        break;
    case HotKeyLifetimeGraph:
        ToggleLifetimeGraph(state);
        break;
    }
}

InputManager::InputManager(std::shared_ptr<SessionState> state, std::shared_ptr<TelemetryManager> telemetry)
    : m_state(std::move(state)), m_telemetry(std::move(telemetry)) {
    g_instance.store(this, std::memory_order_release);
}

InputManager::~InputManager() {
    Stop();
}

void InputManager::Start() {
    m_isRunning = true;
    {
        std::lock_guard lock(m_overlayHoldMutex);
        m_overlayHolds = {};
    }
    RefreshConfigCache();

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);

    m_keyboardThread = std::jthread(&InputManager::KeyboardThreadLoop, this);
    m_gamepadThread = std::jthread(&InputManager::GamepadThreadLoop, this);
}

void InputManager::Stop() {
    m_isRunning = false;
    m_gamepadWakeCv.notify_all();

    DWORD keyboardThreadId = m_keyboardThreadId.load(std::memory_order_acquire);
    if (keyboardThreadId != 0) {
        PostThreadMessage(keyboardThreadId, WM_QUIT, 0, 0);
    }
    if (m_keyboardThread.joinable()) {
        HANDLE hThread = reinterpret_cast<HANDLE>(m_keyboardThread.native_handle());
        if (hThread && WaitForSingleObject(hThread, 2000) != WAIT_OBJECT_0) {
            std::cout << "[InputManager] Warning: Keyboard thread did not stop within timeout; detaching.\n";
            m_keyboardThread.detach();
        } else {
            m_keyboardThread.join();
        }
    }
#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
#endif
    bool gamepadThreadStopped = true;
    if (m_gamepadThread.joinable()) {
        HANDLE hGamepad = reinterpret_cast<HANDLE>(m_gamepadThread.native_handle());
        if (hGamepad && WaitForSingleObject(hGamepad, 2000) != WAIT_OBJECT_0) {
            std::cout << "[InputManager] Warning: Gamepad thread did not stop within timeout; detaching.\n";
            m_gamepadThread.detach();
            gamepadThreadStopped = false;
        } else {
            m_gamepadThread.join();
        }
    }
    {
        std::lock_guard lock(m_overlayHoldMutex);
        m_overlayHolds = {};
        if (m_state) m_state->ui.showOverlay.store(false, std::memory_order_relaxed);
    }
    // A detached gamepad thread may still hold open SDL controllers; tearing SDL down would free them under it.
    if (gamepadThreadStopped) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);
    g_instance.store(nullptr, std::memory_order_release);
}

InputManager::BindingSnapshot InputManager::RefreshConfigCache() {
    std::lock_guard lock(m_configCacheMutex);
    const uint64_t revision = Config::Revision();
    if (m_cachedConfigRevision != revision) {
        const ConfigData conf = Config::Read();
        auto& b = m_bindings.bindings;
        b[static_cast<size_t>(Action::Overlay)] = {conf.key_overlay, conf.gamepad_overlay, conf.gamepad_overlay_raw, conf.gamepad_overlay_raw_button};
        b[static_cast<size_t>(Action::Cycle)] = {conf.key_cycle, conf.gamepad_cycle, conf.gamepad_cycle_raw, conf.gamepad_cycle_raw_button};
        b[static_cast<size_t>(Action::Expand)] = {conf.key_expand, conf.gamepad_expand, conf.gamepad_expand_raw, conf.gamepad_expand_raw_button};
        b[static_cast<size_t>(Action::Session)] = {conf.key_session, conf.gamepad_session, conf.gamepad_session_raw, conf.gamepad_session_raw_button};
        b[static_cast<size_t>(Action::Menu)] = {conf.key_menu, conf.gamepad_menu, conf.gamepad_menu_raw, conf.gamepad_menu_raw_button};
        b[static_cast<size_t>(Action::PanLeft)] = {conf.key_graph_pan_left, conf.gamepad_graph_pan_left, conf.gamepad_graph_pan_left_raw, conf.gamepad_graph_pan_left_raw_button};
        b[static_cast<size_t>(Action::PanRight)] = {conf.key_graph_pan_right, conf.gamepad_graph_pan_right, conf.gamepad_graph_pan_right_raw, conf.gamepad_graph_pan_right_raw_button};
        b[static_cast<size_t>(Action::Lifetime)].key = conf.key_lifetime_graph;
        m_bindings.showExtraPlaylists = conf.show_extra_playlists;
        m_bindings.graphDefaultLifetime = conf.graph_default_lifetime;
        m_cachedConfigRevision = revision;
    }
    return m_bindings;
}

void InputManager::SetOverlayHeld(OverlayHoldSource source, bool held) {
    if (held) {
        if (m_telemetry) {
            m_telemetry->RecordMilestone(TelemetryManager::Milestone::OverlayToggle);
        }
    }
    std::lock_guard lock(m_overlayHoldMutex);
    m_overlayHolds.Set(source, held);
    if (m_state && !m_state->ui.inputCaptureActive.load(std::memory_order_relaxed)) {
        m_state->ui.showOverlay.store(m_overlayHolds.Visible(), std::memory_order_relaxed);
    }
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
    int registeredPanLeftKey = -1;
    int registeredPanRightKey = -1;
    int registeredLifetimeKey = -1;
    bool registeredDashboardEdit = false;
    auto unregisterRegisteredHotkeys = [&]() {
        UnregisterHotKey(NULL, HotKeyMenu);
        UnregisterHotKey(NULL, HotKeyCycle);
        UnregisterHotKey(NULL, HotKeyExpand);
        UnregisterHotKey(NULL, HotKeySession);
        UnregisterHotKey(NULL, HotKeyDashboardEdit);
        UnregisterHotKey(NULL, HotKeyPanLeft);
        UnregisterHotKey(NULL, HotKeyPanRight);
        UnregisterHotKey(NULL, HotKeyLifetimeGraph);
        registeredMenuKey = -1;
        registeredCycleKey = -1;
        registeredExpandKey = -1;
        registeredSessionKey = -1;
        registeredPanLeftKey = -1;
        registeredPanRightKey = -1;
        registeredLifetimeKey = -1;
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

        // RegisterHotKey suppresses held-key repeats at the OS message source.
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

    BindingSnapshot bindings;
    auto handleHotKey = [&](int hotKeyId) {
        if (!m_state || m_state->ui.inputCaptureActive.load(std::memory_order_relaxed)) return;
        HandleHotKeyAction(*m_state, hotKeyId, bindings.showExtraPlaylists,
                           bindings.graphDefaultLifetime, m_telemetry.get());
    };

    while (m_isRunning) {
        bindings = RefreshConfigCache();
        const auto key = [&](Action action) { return bindings.bindings[static_cast<size_t>(action)].key; };

        bool inputCaptureActive = false;
        if (m_state) {
            inputCaptureActive = m_state->ui.inputCaptureActive.load(std::memory_order_relaxed);
        }

        if (inputCaptureActive) {
            unregisterRegisteredHotkeys();
        } else {
            registerHotKeyIfNeeded(HotKeyMenu, key(Action::Menu), registeredMenuKey);
            registerHotKeyIfNeeded(HotKeyCycle, key(Action::Cycle), registeredCycleKey);
            registerHotKeyIfNeeded(HotKeyExpand, key(Action::Expand), registeredExpandKey);
            registerHotKeyIfNeeded(HotKeySession, key(Action::Session), registeredSessionKey);
            registerHotKeyIfNeeded(HotKeyLifetimeGraph, key(Action::Lifetime), registeredLifetimeKey);
            registerDashboardEditIfNeeded();
            if (m_state && m_state->ui.showGraphView.load(std::memory_order_relaxed)) {
                registerHotKeyIfNeeded(HotKeyPanLeft, key(Action::PanLeft), registeredPanLeftKey);
                registerHotKeyIfNeeded(HotKeyPanRight, key(Action::PanRight), registeredPanRightKey);
            } else {
                if (registeredPanLeftKey != -1) {
                    UnregisterHotKey(NULL, HotKeyPanLeft);
                    registeredPanLeftKey = -1;
                }
                if (registeredPanRightKey != -1) {
                    UnregisterHotKey(NULL, HotKeyPanRight);
                    registeredPanRightKey = -1;
                }
            }
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
            const int keyOverlay = key(Action::Overlay);
            const bool keyboardHeld = !inputCaptureActive && IsRocketLeagueForeground() &&
                                      (GetAsyncKeyState(VK_MENU) & 0x8000) == 0 &&
                                      keyOverlay > 0 && keyOverlay < 256 &&
                                      (GetAsyncKeyState(keyOverlay) & 0x8000) != 0;
            SetOverlayHeld(OverlayHoldSource::Keyboard, keyboardHeld);
        }

        // The overlay key is polled, so this wait bounds how late a press or release is seen.
        MsgWaitForMultipleObjectsEx(0, NULL, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
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
        const auto bindings = self->RefreshConfigCache();
        const auto key = [&](Action action) { return bindings.bindings[static_cast<size_t>(action)].key; };
        const auto dispatch = [&](int id) {
            HandleHotKeyAction(*st, id, bindings.showExtraPlaylists,
                               bindings.graphDefaultLifetime, self->m_telemetry.get());
        };
        const int keyOverlay = key(Action::Overlay);
        if (self->m_hookOverlayKey != keyOverlay) {
            self->SetOverlayHeld(OverlayHoldSource::Keyboard, false);
            self->m_hookOverlayKey = keyOverlay;
        }
        if (self->m_hookSessionKey != key(Action::Session)) {
            self->m_sessionKeyDown.store(false, std::memory_order_relaxed);
            self->m_hookSessionKey = key(Action::Session);
        }
        if (self->m_hookMenuKey != key(Action::Menu)) {
            self->m_menuKeyDown.store(false, std::memory_order_relaxed);
            self->m_hookMenuKey = key(Action::Menu);
        }
        if (st->ui.inputCaptureActive.load(std::memory_order_relaxed)) {
            self->m_sessionKeyDown.store(false, std::memory_order_relaxed);
            self->m_menuKeyDown.store(false, std::memory_order_relaxed);
            if (kb->vkCode == keyOverlay && !keyDown)
                self->SetOverlayHeld(OverlayHoldSource::Keyboard, false);
            return CallNextHookEx(m_hook, nCode, wParam, lParam);
        }
        const int keyCycle = key(Action::Cycle);
        const int keyExpand = key(Action::Expand);
        const int keySession = key(Action::Session);
        const int keyMenu = key(Action::Menu);

        if (kb->vkCode == keyOverlay) {
            const bool isAltDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
            self->SetOverlayHeld(OverlayHoldSource::Keyboard, keyDown && !isAltDown);
            return CallNextHookEx(m_hook, nCode, wParam, lParam);
        }

        if (kb->vkCode == keyCycle && wParam == WM_KEYDOWN) {
            dispatch(HotKeyCycle);
        }

        if (kb->vkCode == keyExpand && wParam == WM_KEYDOWN) {
            dispatch(HotKeyExpand);
        }
        if (kb->vkCode == static_cast<DWORD>(key(Action::Lifetime)) && wParam == WM_KEYDOWN) {
            dispatch(HotKeyLifetimeGraph);
        }
        if (st->ui.showGraphView.load()) {
            const int keyPanLeft = key(Action::PanLeft);
            const int keyPanRight = key(Action::PanRight);
            if (kb->vkCode == keyPanLeft && wParam == WM_KEYDOWN) {
                dispatch(HotKeyPanLeft);
            }
            if (kb->vkCode == keyPanRight && wParam == WM_KEYDOWN) {
                dispatch(HotKeyPanRight);
            }
        }
        // The low-level hook still receives repeated keydowns, so track the F8 edge.
        if (kb->vkCode == keySession) {
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                self->m_sessionKeyDown.store(false, std::memory_order_relaxed);
            } else if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                       !self->m_sessionKeyDown.exchange(true, std::memory_order_relaxed)) {
                dispatch(HotKeySession);
            }
        }

        // Match RegisterHotKey(MOD_NOREPEAT) behavior when the optional legacy
        // hook is enabled. Holding F5 must never repeatedly create/destroy the
        // Settings DOM on consecutive key-repeat messages.
        if (kb->vkCode == keyMenu) {
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                self->m_menuKeyDown.store(false, std::memory_order_relaxed);
            } else if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                       !self->m_menuKeyDown.exchange(true, std::memory_order_relaxed)) {
                dispatch(HotKeyMenu);
            }
        }

        bool isCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (st->ui.showMenu.load() && isCtrlDown && kb->vkCode == 'E' && wParam == WM_KEYDOWN) {
            dispatch(HotKeyDashboardEdit);
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
    auto lastScan = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    int lastLoggedDeviceCount = -1;

    bool prevControllerStates[MAX_TRACKED_BUTTONS] = {false};
    bool prevRawControllerStates[256] = {false};
    bool prevFallbackStates[256] = {false};

    auto resetPreviousStates = [&]() {
        std::fill_n(prevControllerStates, MAX_TRACKED_BUTTONS, false);
        std::fill_n(prevRawControllerStates, 256, false);
        std::fill_n(prevFallbackStates, 256, false);
    };

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
        auto now = std::chrono::steady_clock::now();

        if (controller == nullptr && fallbackJoystick == nullptr &&
            std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count() >= 2) {
            lastScan = now;
            // Device indices stay valid only while the joystick list is locked.
            SDL_LockJoysticks();
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
            SDL_UnlockJoysticks();
        }

        if (self && self->m_state) {
            auto& st = self->m_state;
            const auto bindings = self->RefreshConfigCache();
            const bool inputCaptureActive = st->ui.inputCaptureActive.load(std::memory_order_relaxed);
            auto dispatchButtons = [&](const bool* current, const bool* previous, int count,
                                       const bool* raw, const bool* previousRaw) {
                auto held = [&](Action action, bool previousState) {
                    const auto& binding = bindings.bindings[static_cast<size_t>(action)];
                    const int index = binding.raw ? binding.rawButton : binding.button;
                    const int limit = binding.raw ? 256 : count;
                    if (index < 0 || index >= limit) return false;
                    const bool* states = binding.raw ? (previousState ? previousRaw : raw)
                                                     : (previousState ? previous : current);
                    return states[index];
                };
                auto pressed = [&](Action action) { return held(action, false) && !held(action, true); };
                self->SetOverlayHeld(OverlayHoldSource::Gamepad, !inputCaptureActive && held(Action::Overlay, false));
                if (inputCaptureActive) return;
                const bool overlayVisible = st->ui.showOverlay.load();
                auto dispatch = [&](Action action, int id) {
                    if (pressed(action)) HandleHotKeyAction(*st, id, bindings.showExtraPlaylists,
                                                            bindings.graphDefaultLifetime, self->m_telemetry.get());
                };
                if (overlayVisible) {
                    dispatch(Action::Cycle, HotKeyCycle);
                    dispatch(Action::Expand, HotKeyExpand);
                    dispatch(Action::Session, HotKeySession);
                }
                dispatch(Action::Menu, HotKeyMenu);
                if (st->ui.showGraphView.load()) {
                    dispatch(Action::PanLeft, HotKeyPanLeft);
                    dispatch(Action::PanRight, HotKeyPanRight);
                }
            };

            if (controller != nullptr) {
                if (!SDL_GameControllerGetAttached(controller)) {
                    SDL_GameControllerClose(controller);
                    controller = nullptr;
                    resetPreviousStates();
                    lastScan = now - std::chrono::seconds(2);
                    updateDebugInfo("", false, false);
                } else {
                    SDL_GameControllerUpdate();

                    bool currentStates[MAX_TRACKED_BUTTONS] = {false};
                    bool currentRawControllerStates[256] = {false};

                    for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX && b < MAX_TRACKED_BUTTONS; ++b) {
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

                    dispatchButtons(currentStates, prevControllerStates, MAX_TRACKED_BUTTONS,
                                    currentRawControllerStates, prevRawControllerStates);
                    std::copy_n(currentStates, MAX_TRACKED_BUTTONS, prevControllerStates);
                    std::copy_n(currentRawControllerStates, 256, prevRawControllerStates);
                }
            } else if (fallbackJoystick != nullptr) {
                if (!SDL_JoystickGetAttached(fallbackJoystick)) {
                    SDL_JoystickClose(fallbackJoystick);
                    fallbackJoystick = nullptr;
                    resetPreviousStates();
                    lastScan = now - std::chrono::seconds(2);
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

                    dispatchButtons(currentStates, prevFallbackStates, 256, currentStates, prevFallbackStates);
                    std::copy_n(currentStates, 256, prevFallbackStates);
                }
            }
        }
        const bool hasController = controller != nullptr || fallbackJoystick != nullptr;
        if (self && !hasController) self->SetOverlayHeld(OverlayHoldSource::Gamepad, false);
        const auto sleepDuration = hasController ? std::chrono::milliseconds(16) : std::chrono::milliseconds(250);
        std::unique_lock<std::mutex> lock(m_gamepadWakeMutex);
        m_gamepadWakeCv.wait_for(lock, sleepDuration, [this] { return !m_isRunning.load(std::memory_order_relaxed); });
    }

    if (controller) SDL_GameControllerClose(controller);
    if (fallbackJoystick) SDL_JoystickClose(fallbackJoystick);
}