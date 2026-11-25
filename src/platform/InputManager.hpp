#pragma once
#include <windows.h>
#include <memory>
#include <cstdint>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <array>
#include "core/state/SessionState.hpp"
#include "platform/OverlayHoldState.hpp"

#ifndef OMNISTATS_ENABLE_LOW_LEVEL_HOOK
#define OMNISTATS_ENABLE_LOW_LEVEL_HOOK 0
#endif

class TelemetryManager;

class InputManager {
  public:
    enum HotKeyId : int {
        HotKeyMenu = 1,
        HotKeyCycle = 2,
        HotKeyExpand = 3,
        HotKeySession = 4,
        HotKeyDashboardEdit = 5,
        HotKeyPanLeft = 6,
        HotKeyPanRight = 7,
        HotKeyLifetimeGraph = 8,
    };

    static void HandleHotKeyAction(
        SessionState& state,
        int hotKeyId,
        bool showExtraPlaylists,
        bool graphDefaultLifetime,
        TelemetryManager* telemetry);

    InputManager(std::shared_ptr<SessionState> state, std::shared_ptr<TelemetryManager> telemetry = nullptr);
    ~InputManager();

    void Start();
    void Stop();
    static bool HandleWindowMessage(UINT msg, WPARAM wParam, LPARAM lParam);

#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif

  private:
    void KeyboardThreadLoop();
    void GamepadThreadLoop();
    enum class Action : size_t { Overlay,
                                 Cycle,
                                 Expand,
                                 Session,
                                 Menu,
                                 PanLeft,
                                 PanRight,
                                 Lifetime,
                                 Count };
    struct Binding {
        int key = -1;
        int button = -1;
        bool raw = false;
        int rawButton = -1;
    };
    struct BindingSnapshot {
        std::array<Binding, static_cast<size_t>(Action::Count)> bindings{};
        bool showExtraPlaylists = true;
        bool graphDefaultLifetime = false;
    };
    BindingSnapshot RefreshConfigCache();
    void SetOverlayHeld(OverlayHoldSource source, bool held);

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<TelemetryManager> m_telemetry;
    std::jthread m_keyboardThread;
    std::jthread m_gamepadThread;
    std::mutex m_gamepadWakeMutex;
    std::condition_variable m_gamepadWakeCv;
    std::atomic<bool> m_isRunning{false};
    std::mutex m_overlayHoldMutex;
    OverlayHoldState m_overlayHolds;
    std::mutex m_configCacheMutex;
    uint64_t m_cachedConfigRevision{UINT64_MAX};
    BindingSnapshot m_bindings;
    std::atomic<DWORD> m_keyboardThreadId{0};
#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    int m_hookOverlayKey{-1};
    int m_hookSessionKey{-1};
    int m_hookMenuKey{-1};
    std::atomic<bool> m_sessionKeyDown{false};
    std::atomic<bool> m_menuKeyDown{false};
    static inline HHOOK m_hook = nullptr;
#endif
    static inline std::atomic<InputManager*> g_instance{nullptr};
};
