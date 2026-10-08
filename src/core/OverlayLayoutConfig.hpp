#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "core/DashboardLayoutConfig.hpp"

class SessionState;

namespace OverlayLayout {

    inline constexpr int kMinVisibilitySeconds = 1;
    inline constexpr int kMaxVisibilitySeconds = 30;
    inline constexpr int kDefaultVisibilitySeconds = 8;
    inline constexpr int kCurrentLayoutVersion = 4;

    struct ContainerConfig {
        struct Visibility {
            enum Mode {
                KeyHeld = 0,
                AlwaysInMatch = 1,
                AfterEvent = 2,
                MenusOnly = 3
            };
            enum Event {
                MatchStart = 0,
                FirstCountdown = 1,
                Countdown = 2,
                Goal = 3,
                MatchEnd = 4,
                Podium = 5
            };

            Mode mode = KeyHeld;
            Event event = FirstCountdown;
            int seconds = kDefaultVisibilitySeconds;
            bool alsoWhileKeyHeld = true;
            bool hideDuringReplay = false;

            bool operator==(const Visibility&) const = default;
        };

        std::string id;
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        std::vector<DashboardLayout::WidgetId> widgets;
        Visibility visibility{};
    };

    using Visibility = ContainerConfig::Visibility;
    using VisibilityMode = ContainerConfig::Visibility::Mode;
    using VisibilityEvent = ContainerConfig::Visibility::Event;

    struct VisibilityTimestamps {
        int64_t matchStartMs = 0;
        int64_t firstCountdownMs = 0;
        int64_t countdownMs = 0;
        int64_t goalMs = 0;
        int64_t matchEndMs = 0;
        int64_t podiumMs = 0;

        bool operator==(const VisibilityTimestamps&) const = default;
    };

    struct VisibilityState {
        VisibilityTimestamps timestamps{};
        bool keyHeld = false;
        bool inMatch = false;
        bool inReplay = false;
        bool inMenus = true;
        bool editMode = false;

        bool operator==(const VisibilityState&) const = default;
    };

    struct LayoutConfig {
        int version = 1;
        bool toolboxOpen = false;
        std::vector<ContainerConfig> containers;
    };

    LayoutConfig DefaultOverlayLayout();
    void Sanitize(ContainerConfig::Visibility& visibility);
    void Sanitize(LayoutConfig& layout);

    const char* ToConfigString(VisibilityMode mode) noexcept;
    const char* ToConfigString(VisibilityEvent event) noexcept;
    VisibilityMode VisibilityModeFromConfigString(std::string_view value) noexcept;
    VisibilityEvent VisibilityEventFromConfigString(std::string_view value) noexcept;

    VisibilityState SnapshotVisibilityState(const SessionState& state, bool editMode = false) noexcept;
    bool ContainerVisible(const ContainerConfig& config, const VisibilityState& state, int64_t nowMs) noexcept;
    std::optional<int64_t> NextVisibilityChangeMs(const ContainerConfig& config,
                                                  const VisibilityState& state,
                                                  int64_t nowMs) noexcept;
    std::optional<int64_t> NextVisibilityChangeMs(const LayoutConfig& layout,
                                                  const VisibilityState& state,
                                                  int64_t nowMs) noexcept;

} // namespace OverlayLayout
