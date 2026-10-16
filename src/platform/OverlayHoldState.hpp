#pragma once

enum class OverlayHoldSource { Keyboard,
                               Gamepad };

struct OverlayHoldState {
    bool keyboard = false;
    bool gamepad = false;

    void Set(OverlayHoldSource source, bool held) noexcept {
        if (source == OverlayHoldSource::Keyboard)
            keyboard = held;
        else
            gamepad = held;
    }

    [[nodiscard]] bool Visible() const noexcept {
        return keyboard || gamepad;
    }
};
