#include "OverlayLayoutConfig.hpp"
#include "core/SessionState.hpp"
#include <algorithm>
#include <cctype>
#include <set>

namespace OverlayLayout {

    namespace {
        std::string NormalizeToken(std::string_view input) {
            std::string out;
            out.reserve(input.size());
            for (unsigned char ch : input) {
                if (ch == ' ' || ch == '-' || ch == '_') continue;
                out.push_back(static_cast<char>(std::tolower(ch)));
            }
            return out;
        }

        ContainerConfig::Visibility SanitizedCopy(ContainerConfig::Visibility vis) noexcept {
            Sanitize(vis);
            return vis;
        }

        int64_t ActiveEventTimestampMs(VisibilityEvent event, const VisibilityState& state) noexcept {
            const auto& ts = state.timestamps;
            int64_t eventMs = 0;
            bool requiresInMatch = true;
            switch (event) {
            case Visibility::MatchStart:
                eventMs = ts.matchStartMs;
                break;
            case Visibility::FirstCountdown:
                eventMs = ts.firstCountdownMs;
                break;
            case Visibility::Countdown:
                eventMs = ts.countdownMs;
                break;
            case Visibility::Goal:
                eventMs = ts.goalMs;
                break;
            case Visibility::MatchEnd:
                eventMs = ts.matchEndMs;
                requiresInMatch = false;
                break;
            case Visibility::Podium:
                eventMs = ts.podiumMs;
                requiresInMatch = false;
                break;
            default:
                return 0;
            }

            if (eventMs <= 0) return 0;
            if (requiresInMatch && !state.inMatch) return 0;
            if (ts.matchStartMs > 0 && eventMs < ts.matchStartMs) return 0;
            if (event == Visibility::Podium && ts.matchEndMs > 0 &&
                (ts.matchStartMs <= 0 || ts.matchEndMs >= ts.matchStartMs) &&
                eventMs < ts.matchEndMs) {
                return 0;
            }
            return eventMs;
        }
    } // namespace

    const char* ToConfigString(VisibilityMode mode) noexcept {
        switch (mode) {
        case Visibility::KeyHeld:
            return "key_held";
        case Visibility::AlwaysInMatch:
            return "always_in_match";
        case Visibility::AfterEvent:
            return "after_event";
        case Visibility::MenusOnly:
            return "menus_only";
        default:
            return "key_held";
        }
    }

    const char* ToConfigString(VisibilityEvent event) noexcept {
        switch (event) {
        case Visibility::MatchStart:
            return "match_start";
        case Visibility::FirstCountdown:
            return "first_countdown";
        case Visibility::Countdown:
            return "countdown";
        case Visibility::Goal:
            return "goal";
        case Visibility::MatchEnd:
            return "match_end";
        case Visibility::Podium:
            return "podium";
        default:
            return "first_countdown";
        }
    }

    VisibilityMode VisibilityModeFromConfigString(std::string_view value) noexcept {
        const std::string token = NormalizeToken(value);
        if (token == "alwaysinmatch" || token == "always") return Visibility::AlwaysInMatch;
        if (token == "afterevent") return Visibility::AfterEvent;
        if (token == "menusonly" || token == "menuonly" || token == "inmenusonly") return Visibility::MenusOnly;
        return Visibility::KeyHeld;
    }

    VisibilityEvent VisibilityEventFromConfigString(std::string_view value) noexcept {
        const std::string token = NormalizeToken(value);
        if (token == "matchstart") return Visibility::MatchStart;
        if (token == "firstcountdown" || token == "firstkickoff" || token == "firstkickoffcountdown")
            return Visibility::FirstCountdown;
        if (token == "countdown" || token == "eachcountdown" || token == "eachkickoff" || token == "eachkickoffcountdown")
            return Visibility::Countdown;
        if (token == "goal" || token == "goalscored") return Visibility::Goal;
        if (token == "matchend" || token == "matchended") return Visibility::MatchEnd;
        if (token == "podium" || token == "podiumstart") return Visibility::Podium;
        return Visibility::FirstCountdown;
    }

    LayoutConfig DefaultOverlayLayout() {
        LayoutConfig config;
        config.version = kCurrentLayoutVersion;
        config.toolboxOpen = false;

        // 1. Demo Tracker, Session Stats, Streaks Stats (docked)
        {
            ContainerConfig c;
            c.id = "demo_tracker";
            c.x = 1270.0f;
            c.y = 0.0f;
            c.w = 220.0f;
            c.h = 489.0f;
            c.widgets = {
                DashboardLayout::WidgetId::DemoTracker,
                DashboardLayout::WidgetId::StreaksStats,
                DashboardLayout::WidgetId::SessionStats};
            config.containers.push_back(c);
        }

        // 2. Lobby Ranks (new-install default: 8 s after first countdown + while key held)
        {
            ContainerConfig c;
            c.id = "lobby_ranks";
            c.x = 743.5f;
            c.y = 774.0f;
            c.w = 0.0f;
            c.h = 0.0f;
            c.widgets = {DashboardLayout::WidgetId::LobbyRanks};
            c.visibility.mode = Visibility::AfterEvent;
            c.visibility.event = Visibility::FirstCountdown;
            c.visibility.seconds = kDefaultVisibilitySeconds;
            c.visibility.alsoWhileKeyHeld = true;
            c.visibility.hideDuringReplay = false;
            config.containers.push_back(c);
        }

        // 3. Previous Games
        {
            ContainerConfig c;
            c.id = "previous_games";
            c.x = 427.5f;
            c.y = 0.0f;
            c.w = 316.0f;
            c.h = 415.0f;
            c.widgets = {DashboardLayout::WidgetId::PreviousGames};
            config.containers.push_back(c);
        }

        // 4. Main H2H Stack (Live Roster, Live Match Stats, Gamemode Breakdown) (docked)
        {
            ContainerConfig c;
            c.id = "main_stack";
            c.x = 1490.0f;
            c.y = 0.0f;
            c.w = 430.0f;
            c.h = 759.0f;
            c.widgets = {
                DashboardLayout::WidgetId::LiveRoster,
                DashboardLayout::WidgetId::LiveMatchStats,
                DashboardLayout::WidgetId::GamemodeBreakdown};
            config.containers.push_back(c);
        }

        return config;
    }

    void Sanitize(ContainerConfig::Visibility& visibility) {
        const int modeInt = static_cast<int>(visibility.mode);
        if (modeInt < static_cast<int>(Visibility::KeyHeld) ||
            modeInt > static_cast<int>(Visibility::MenusOnly)) {
            visibility.mode = Visibility::KeyHeld;
        }
        const int eventInt = static_cast<int>(visibility.event);
        if (eventInt < static_cast<int>(Visibility::MatchStart) ||
            eventInt > static_cast<int>(Visibility::Podium)) {
            visibility.event = Visibility::FirstCountdown;
        }
        visibility.seconds = std::clamp(visibility.seconds, kMinVisibilitySeconds, kMaxVisibilitySeconds);
    }

    void Sanitize(LayoutConfig& layout) {
        if (layout.version < 1) {
            layout.version = 1;
        }

        // Version 2 compacted the lobby-rank table to single-line rows and
        // narrower columns. Widths persisted against the old metrics leave a
        // large empty card, so drop them once and let the container auto-size.
        if (layout.version < 2) {
            for (auto& container : layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
            layout.version = 2;
        }

        // Version 3 calculates lobby-rank table width dynamically from enabled
        // playlist columns. Reset previously clamped oversized dimensions so
        // containers auto-size to the new compact metrics.
        if (layout.version < 3) {
            for (auto& container : layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
            layout.version = 3;
        }

        // Version 4 adds per-container visibility rules. Existing layouts keep
        // their deserialized visibility (KeyHeld when absent in older configs).
        if (layout.version < kCurrentLayoutVersion) {
            layout.version = kCurrentLayoutVersion;
        }

        // Ensure we don't have duplicate widgets across multiple containers
        std::set<DashboardLayout::WidgetId> seenWidgets;
        std::vector<ContainerConfig> validContainers;

        for (auto& c : layout.containers) {
            Sanitize(c.visibility);
            std::vector<DashboardLayout::WidgetId> validWidgets;
            for (auto w : c.widgets) {
                if (seenWidgets.find(w) == seenWidgets.end()) {
                    seenWidgets.insert(w);
                    validWidgets.push_back(w);
                }
            }
            if (!validWidgets.empty()) {
                c.widgets = validWidgets;
                validContainers.push_back(c);
            }
        }
        layout.containers = validContainers;
    }

    VisibilityState SnapshotVisibilityState(const SessionState& state, bool editMode) noexcept {
        VisibilityState snap;
        snap.timestamps.matchStartMs = state.ui.lastMatchStartMs.load(std::memory_order_relaxed);
        snap.timestamps.firstCountdownMs = state.ui.firstCountdownOfMatchMs.load(std::memory_order_relaxed);
        snap.timestamps.countdownMs = state.ui.lastCountdownMs.load(std::memory_order_relaxed);
        snap.timestamps.goalMs = state.ui.lastGoalMs.load(std::memory_order_relaxed);
        snap.timestamps.matchEndMs = state.ui.lastMatchEndMs.load(std::memory_order_relaxed);
        snap.timestamps.podiumMs = state.ui.lastPodiumMs.load(std::memory_order_relaxed);
        snap.keyHeld = state.ui.showOverlay.load(std::memory_order_relaxed);
        snap.inMatch = state.game.inMatch.load(std::memory_order_relaxed);
        snap.inReplay = state.ui.inGoalReplay.load(std::memory_order_relaxed);
        snap.inMenus = !snap.inMatch && !state.game.inReplay.load(std::memory_order_relaxed);
        snap.editMode = editMode;
        return snap;
    }

    bool ContainerVisible(const ContainerConfig& config, const VisibilityState& state, int64_t nowMs) noexcept {
        if (state.editMode) return true;

        const auto vis = SanitizedCopy(config.visibility);
        if (vis.hideDuringReplay && state.inReplay) return false;

        switch (vis.mode) {
        case Visibility::KeyHeld:
            return state.keyHeld;
        case Visibility::AlwaysInMatch:
            return state.inMatch || (vis.alsoWhileKeyHeld && state.keyHeld);
        case Visibility::MenusOnly:
            return (state.inMenus && !state.inMatch) || (vis.alsoWhileKeyHeld && state.keyHeld);
        case Visibility::AfterEvent: {
            if (vis.alsoWhileKeyHeld && state.keyHeld) return true;
            const int64_t eventMs = ActiveEventTimestampMs(vis.event, state);
            if (eventMs <= 0 || nowMs < eventMs) return false;
            const int64_t durationMs = static_cast<int64_t>(vis.seconds) * 1000LL;
            return (nowMs - eventMs) < durationMs;
        }
        }
        return state.keyHeld;
    }

    std::optional<int64_t> NextVisibilityChangeMs(const ContainerConfig& config,
                                                  const VisibilityState& state,
                                                  int64_t nowMs) noexcept {
        if (state.editMode) return std::nullopt;

        const auto vis = SanitizedCopy(config.visibility);
        if (vis.hideDuringReplay && state.inReplay) return std::nullopt;
        if (vis.mode != Visibility::AfterEvent) return std::nullopt;
        if (vis.alsoWhileKeyHeld && state.keyHeld) return std::nullopt;

        const int64_t eventMs = ActiveEventTimestampMs(vis.event, state);
        if (eventMs <= 0) return std::nullopt;
        if (nowMs < eventMs) return eventMs;

        const int64_t endMs = eventMs + static_cast<int64_t>(vis.seconds) * 1000LL;
        if (nowMs < endMs) return endMs;
        return std::nullopt;
    }

    std::optional<int64_t> NextVisibilityChangeMs(const LayoutConfig& layout,
                                                  const VisibilityState& state,
                                                  int64_t nowMs) noexcept {
        std::optional<int64_t> earliest;
        for (const auto& container : layout.containers) {
            const auto candidate = NextVisibilityChangeMs(container, state, nowMs);
            if (!candidate.has_value()) continue;
            if (!earliest.has_value() || *candidate < *earliest) {
                earliest = candidate;
            }
        }
        return earliest;
    }

} // namespace OverlayLayout
