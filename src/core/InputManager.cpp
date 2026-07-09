#include "InputManager.hpp"
#include "Config.hpp"
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>
#include <iostream>
#include <chrono>
#include <cstring>

namespace {

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

InputManager::~InputManager() { Stop(); }

void InputManager::Start() {
  m_isRunning = true;
  RefreshConfigCache();

  // Initialize SDL for gamecontroller on the main thread
  SDL_SetMainReady();
  SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK);

  // 1. Keyboard handling (registered hotkeys + foreground polling)
  m_keyboardThread = std::jthread(&InputManager::KeyboardThreadLoop, this);

  // 2. Gamepad Polling (Needs its own thread)
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
      if (state.ui.showSessionView) {
        state.ui.showGraphView = !state.ui.showGraphView;
      } else {
        state.ui.h2hExpanded = !state.ui.h2hExpanded;
      }
      return;
    }

    if (hotKeyId == HotKeySession) {
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
    registerHotKeyIfNeeded(HotKeyMenu, m_keyMenu.load(std::memory_order_relaxed), registeredMenuKey);
    registerHotKeyIfNeeded(HotKeyCycle, m_keyCycle.load(std::memory_order_relaxed), registeredCycleKey);
    registerHotKeyIfNeeded(HotKeyExpand, m_keyExpand.load(std::memory_order_relaxed), registeredExpandKey);
    registerHotKeyIfNeeded(HotKeySession, m_keySession.load(std::memory_order_relaxed), registeredSessionKey);
    registerDashboardEditIfNeeded();

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
      const bool inputCaptureActive = state.ui.inputCaptureActive.load(std::memory_order_relaxed);
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
    KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
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

    // Overlay Handling (Hold to Show)
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

    // Cycle Playlist Category Instantly
    if (kb->vkCode == keyCycle && wParam == WM_KEYDOWN) {
      MmrCategory next = NextMmrCategory(st->ui.rosterMmrCategory.load(), showExtraPlaylists);
      st->ui.rosterMmrCategory.store(next);
      if (!secondMonitorMode) {
          st->ui.graphMmrCategory.store(next);
      }
      
      // Save the updated category to config so it persists
      Config::Update([next](ConfigData& conf) {
          conf.mmr_category = MmrCategoryToString(next);
      });
      // No MMR reset - instant cycle
    }

    // Toggle Expanded View or Graph View
    if (kb->vkCode == keyExpand && wParam == WM_KEYDOWN) {
      if (st->ui.showSessionView) {
        // If in session view, toggle between text and graph
        st->ui.showGraphView = !st->ui.showGraphView;
      } else {
        // Otherwise, toggle expanded H2H view
        st->ui.h2hExpanded = !st->ui.h2hExpanded;
      }
    }

    // Toggle Session View
    if (kb->vkCode == keySession && wParam == WM_KEYDOWN) {
      st->ui.showSessionView = !st->ui.showSessionView;
      if (!st->ui.showSessionView) {
        st->ui.showGraphView =
            false; // Reset graph view when exiting session view
      }
    }

    // Settings Toggle
    if (kb->vkCode == keyMenu && wParam == WM_KEYDOWN) {
      ToggleSettingsMenu(*st);
    }

    // Dashboard Edit Mode Toggle (Ctrl + E)
    bool isCtrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (st->ui.showMenu.load() && isCtrlDown && kb->vkCode == 'E' && wParam == WM_KEYDOWN) {
      ToggleDashboardEditMode(*st);
    }

    // ESC Handling: Global Close
    if (kb->vkCode == VK_ESCAPE && wParam == WM_KEYDOWN) {
      HandleEscape(*st);
    }
  }
  return CallNextHookEx(m_hook, nCode, wParam, lParam);
}
#endif

void InputManager::GamepadThreadLoop() {
  SDL_GameController *controller = nullptr;
  SDL_Joystick *fallbackJoystick = nullptr;
  auto lastScan = std::chrono::steady_clock::now();
  int lastLoggedDeviceCount = -1;

  // Helper to update controller debug info in SessionState
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

    // Scan for controllers every 2 seconds when none is connected
    if (controller == nullptr && fallbackJoystick == nullptr &&
        std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count() >= 2) {
      lastScan = now;
      int deviceCount = SDL_NumJoysticks();
      bool deviceListChanged = deviceCount != lastLoggedDeviceCount;
      if (deviceListChanged) {
        lastLoggedDeviceCount = deviceCount;
        std::cout << "[Input] SDL joystick count: " << deviceCount << "\n";
      }

      // First pass: try to open as SDL_GameController (preferred)
      for (int i = 0; i < deviceCount; ++i) {
        const char* joyName = SDL_JoystickNameForIndex(i);
        const bool isCtrl = SDL_IsGameController(i) == SDL_TRUE;

        if (deviceListChanged) {
          std::cout << "[Input] SDL device " << i
                    << ": name=" << (joyName ? joyName : "Unknown")
                    << ", is_game_controller=" << (isCtrl ? "true" : "false")
                    << "\n";
        }

        if (isCtrl) {
          controller = SDL_GameControllerOpen(i);
          if (controller) {
            const char* controllerName = SDL_GameControllerName(controller);
            std::cout << "[Input] Opened game controller: "
                      << (controllerName ? controllerName : "Unknown")
                      << "\n";
            std::cout << "[Input] Controller overlay button id: "
                      << (self ? self->m_gamepadOverlay.load(std::memory_order_relaxed) : 4)
                      << "\n";
            updateDebugInfo(controllerName, true, true);
            break;
          }

          if (deviceListChanged) {
            std::cout << "[Input] Failed to open SDL game controller index " << i
                      << ": " << SDL_GetError() << "\n";
          }
        }
      }

      // Second pass: if no game controller found, try legacy joystick fallback
      if (!controller) {
        for (int i = 0; i < deviceCount; ++i) {
          fallbackJoystick = SDL_JoystickOpen(i);
          if (fallbackJoystick) {
            const char* joyName = SDL_JoystickName(fallbackJoystick);
            std::cout << "[Input] Opened fallback joystick: "
                      << (joyName ? joyName : "Unknown")
                      << ", buttons=" << SDL_JoystickNumButtons(fallbackJoystick)
                      << ", axes=" << SDL_JoystickNumAxes(fallbackJoystick)
                      << ", hats=" << SDL_JoystickNumHats(fallbackJoystick)
                      << "\n";
            std::cout << "[Input] Controller overlay button id: "
                      << (self ? self->m_gamepadOverlay.load(std::memory_order_relaxed) : 4)
                      << "\n";
            updateDebugInfo(joyName, false, true);
            break;
          }

          if (deviceListChanged) {
            std::cout << "[Input] Failed to open fallback joystick index " << i
                      << ": " << SDL_GetError() << "\n";
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

      // Prefer SDL's GameController API when available.
      if (controller != nullptr) {
        if (!SDL_GameControllerGetAttached(controller)) {
          SDL_GameControllerClose(controller);
          controller = nullptr;
          updateDebugInfo("", false, false);
        } else {
          SDL_GameControllerUpdate();

          // Track last pressed button for debug readout
          for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
            if (SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(b))) {
              st->ui.lastControllerButtonPressed.store(b);
            }
          }

          SDL_Joystick* rawJoystick = SDL_GameControllerGetJoystick(controller);
          const int rawButtonCount = rawJoystick ? SDL_JoystickNumButtons(rawJoystick) : 0;
          for (int b = 0; b < rawButtonCount; ++b) {
            if (SDL_JoystickGetButton(rawJoystick, b)) {
              st->ui.lastRawControllerButtonPressed.store(b);
            }
          }

          bool buttonHeld = false;
          if (gamepadOverlayRaw) {
            buttonHeld = rawJoystick && gamepadOverlayRawButton >= 0 && gamepadOverlayRawButton < rawButtonCount &&
                SDL_JoystickGetButton(rawJoystick, gamepadOverlayRawButton) != 0;
          } else if (gamepadOverlay >= 0 && gamepadOverlay < SDL_CONTROLLER_BUTTON_MAX) {
            buttonHeld = SDL_GameControllerGetButton(
                controller,
                static_cast<SDL_GameControllerButton>(gamepadOverlay)) != 0;
          }

          if (!inputCaptureActive && buttonHeld) {
            st->ui.showOverlay = true;
          } else if (!inputCaptureActive) {
            if (!(GetAsyncKeyState(keyOverlay) & 0x8000)) {
              st->ui.showOverlay = false;
            }
          }
        }
      }
      // Fall back to the raw joystick API.
      else if (fallbackJoystick != nullptr) {
        if (!SDL_JoystickGetAttached(fallbackJoystick)) {
          SDL_JoystickClose(fallbackJoystick);
          fallbackJoystick = nullptr;
          updateDebugInfo("", false, false);
        } else {
          SDL_JoystickUpdate();

          // Track last pressed button for debug readout
          const int numButtons = SDL_JoystickNumButtons(fallbackJoystick);
          for (int b = 0; b < numButtons; ++b) {
            if (SDL_JoystickGetButton(fallbackJoystick, b)) {
              st->ui.lastControllerButtonPressed.store(b);
              st->ui.lastRawControllerButtonPressed.store(b);
            }
          }

          const int overlayButton = gamepadOverlayRaw ? gamepadOverlayRawButton : gamepadOverlay;
          const bool buttonHeld = overlayButton >= 0 && overlayButton < numButtons &&
              (SDL_JoystickGetButton(fallbackJoystick, overlayButton) != 0);

          if (!inputCaptureActive && buttonHeld) {
            st->ui.showOverlay = true;
          } else if (!inputCaptureActive) {
            if (!(GetAsyncKeyState(keyOverlay) & 0x8000)) {
              st->ui.showOverlay = false;
            }
          }
        }
      }
    }
    const bool hasController = controller != nullptr || fallbackJoystick != nullptr;
    std::this_thread::sleep_for(hasController ? std::chrono::milliseconds(16) : std::chrono::milliseconds(250));
  }

  if (controller) {
    SDL_GameControllerClose(controller);
  }
  if (fallbackJoystick) {
    SDL_JoystickClose(fallbackJoystick);
  }
}
