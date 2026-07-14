#pragma once
#include <windows.h>
#include <memory>
#include <atomic>
#include <thread>
#include "SessionState.hpp"

#ifndef OMNISTATS_ENABLE_LOW_LEVEL_HOOK
#define OMNISTATS_ENABLE_LOW_LEVEL_HOOK 0
#endif

class InputManager {
public:
    InputManager(std::shared_ptr<SessionState> state);
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
    void RefreshConfigCache();
    
    std::shared_ptr<SessionState> m_state;
    std::jthread m_keyboardThread;
    std::jthread m_gamepadThread;
    std::atomic<bool> m_isRunning{false};
    std::atomic<DWORD> m_keyboardThreadId{0};
    std::atomic<int> m_keyOverlay{VK_TAB};
    std::atomic<int> m_keyCycle{VK_F6};
    std::atomic<int> m_keyExpand{VK_F7};
    std::atomic<int> m_keySession{VK_F8};
    std::atomic<int> m_keyMenu{VK_F5};
    std::atomic<int> m_gamepadOverlay{4};
    std::atomic<bool> m_gamepadOverlayRaw{false};
    std::atomic<int> m_gamepadOverlayRawButton{4};
    std::atomic<int> m_gamepadCycle{-1};
    std::atomic<bool> m_gamepadCycleRaw{false};
    std::atomic<int> m_gamepadCycleRawButton{-1};
    std::atomic<int> m_gamepadExpand{-1};
    std::atomic<bool> m_gamepadExpandRaw{false};
    std::atomic<int> m_gamepadExpandRawButton{-1};
    std::atomic<int> m_gamepadSession{-1};
    std::atomic<bool> m_gamepadSessionRaw{false};
    std::atomic<int> m_gamepadSessionRawButton{-1};
    std::atomic<int> m_gamepadMenu{-1};
    std::atomic<bool> m_gamepadMenuRaw{false};
    std::atomic<int> m_gamepadMenuRawButton{-1};
    std::atomic<bool> m_secondMonitorMode{false};
    std::atomic<bool> m_showExtraPlaylists{true};
#if OMNISTATS_ENABLE_LOW_LEVEL_HOOK
    static inline HHOOK m_hook = nullptr;
#endif
    static inline std::atomic<InputManager*> g_instance{nullptr};
};
