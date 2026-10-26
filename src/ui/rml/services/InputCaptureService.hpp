#pragma once

#include <memory>
#include <string>

class SessionState;

enum class BindCaptureTarget {
    None,
    KeyOverlay,
    KeyCycle,
    KeyExpand,
    KeySession,
    KeyMenu,
    KeySaveReplay,
    KeyGraphPanLeft,
    KeyGraphPanRight,
    KeyLifetimeGraph,
    GamepadOverlay,
    GamepadCycle,
    GamepadExpand,
    GamepadSession,
    GamepadMenu,
    GamepadGraphPanLeft,
    GamepadGraphPanRight
};

class InputCaptureService {
  public:
    InputCaptureService() = default;

    void BeginCapture(std::shared_ptr<SessionState> state, BindCaptureTarget target);
    void FinishCapture(std::shared_ptr<SessionState> state);
    void ClearBind(BindCaptureTarget target);

    // Checks pending key/button presses in state; returns true if a bind was captured or cancelled.
    bool Update(std::shared_ptr<SessionState> state);

    bool IsCapturing() const noexcept {
        return m_target != BindCaptureTarget::None;
    }
    BindCaptureTarget ActiveTarget() const noexcept {
        return m_target;
    }

  private:
    BindCaptureTarget m_target = BindCaptureTarget::None;
};
