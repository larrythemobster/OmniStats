#pragma once

// Shared, stateless helpers for the RmlUi controller translation units.

#include <RmlUi/Core/Types.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "network/api/AccountClient.hpp"
#include "network/omninet/OmniNet.hpp"
#include "ui/config/DashboardLayoutConfig.hpp"
namespace Rml {
    class Element;
}

namespace RmlUiDetail {
    inline constexpr uint64_t kFnvOffset = 1469598103934665603ull;
    inline constexpr uint64_t kFnvPrime = 1099511628211ull;

    enum class PlatformKind { Unknown,
                              Epic,
                              Steam,
                              PlayStation,
                              Xbox,
                              Nintendo,
                              Bot };

    struct PlatformSwatch {
        const char* className;
        const char* color;
    };

    inline constexpr PlatformSwatch kPlatformPalette[] = {
        {"platform-epic", "#f2f3f7"},
        {"platform-steam", "#6dc2f0"},
        {"platform-psn", "#5b8cff"},
        {"platform-xbox", "#59d268"},
        {"platform-switch", "#ff5a5a"},
        {"platform-bot", "#98a2b3"},
        {"platform-unknown", "#98a2b3"},
    };

    struct Hsv {
        float h = 0.0f; // degrees, [0, 360)
        float s = 0.0f;
        float v = 0.0f;
    };

    std::string PromptForFile(HWND parentHwnd, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec);
    int64_t SteadyNowMs();
    float SanitizedScale(float value, float fallback = 1.0f);
    float SanitizedUiScale(float value);
    std::string ToLower(std::string value);
    // Directory holding editable RML/RCSS for hot reload: OMNISTATS_RML_DIR when
    // set, otherwise the source tree in Debug builds, otherwise empty (disabled).
    std::string RmlDevDirectory();

    void HashAppend(uint64_t& hash, std::string_view value);
    void HashAppend(uint64_t& hash, uint64_t value);
    uint64_t AvalancheHash(uint64_t value);

    PlatformKind PlatformKindFor(const std::string& platform);
    std::string PlatformBadgeLabel(PlatformKind kind, const std::string& platform);
    std::string PlatformDisplayName(PlatformKind kind, const std::string& platform);
    const char* PlatformClass(PlatformKind kind);

    Rml::Span<const Rml::byte> EmbeddedResource(const char* name);
    inline Rml::Span<const Rml::byte> EmbeddedResource(int resourceId) {
        return EmbeddedResource(MAKEINTRESOURCEA(resourceId));
    }
    std::vector<Rml::byte> ReadFontFile(const char* fileName);
    std::string TrackerUrlForPlayer(const PlayerData& player);
    std::string Attribute(Rml::Element* element, const char* name);
    const char* GamepadButtonName(int button);
    std::string GamepadBindName(int mappedButton, bool raw, int rawButton);

    DashboardLayout::WidgetId WidgetFromDom(const std::string& value);
    DashboardLayout::Zone ZoneFromDom(const std::string& value);
    std::string MmrLabel(MmrCategory category);
    std::vector<MmrCategory> MmrCategories(bool includeBest, bool extras);

    std::string RankPart(const std::string& tier);
    int TierResourceIndex(const std::string& tier);
    int DivisionLevel(const std::string& tier);
    int DivisionColorResourceIndex(const std::string& tier);
    int PlaylistResourceIndex(const std::string& playlist);

    struct SelectOption {
        std::string value;
        std::string label;
    };

    // Plain text; callers escape. One relationship, e.g. "8 matches · 5W 3L · 63%".
    std::string EncounterDetail(int wins, int losses);
    // Plain text; callers escape. Both relationships, e.g. "With 5-3 · Vs 4-2".
    std::string EncounterSummary(const PersonRecord& record);

    // Every helper below escapes the text it is given; callers pass plain text,
    // never markup.
    std::string Escape(std::string_view text);
    std::string FormatNumber(float value, int precision = 0);
    std::string FormatRecord(int wins, int losses);
    std::string FormatClock(int64_t unixSeconds);
    std::string ToggleSwitch(std::string_view key, bool checked, bool disabled = false);
    std::string ToggleControl(std::string_view key, std::string_view label, std::string_view help, bool checked, bool disabled = false);
    std::string SelectControl(std::string_view key, const std::vector<SelectOption>& options, std::string_view current, const char* klass = "", bool disabled = false);
    std::string SelectRow(std::string_view key, std::string_view label, std::string_view help, const std::vector<SelectOption>& options, std::string_view current, bool disabled = false);
    std::string ActionRow(std::string_view label, std::string_view help, std::string_view actions);
    std::vector<SelectOption> MmrCategoryOptions(bool includeBest, bool extras);
    std::vector<SelectOption> GamemodeScopeOptions();
    std::string GamemodeScopeValue(const ConfigData& config);
    std::vector<SelectOption> OverlayVisibilityModeOptions();
    std::vector<SelectOption> OverlayVisibilityEventOptions();
    std::vector<SelectOption> OverlayVisibilitySecondsOptions();
    int64_t EffectiveMatchSummaryStartMs(const SessionState& state) noexcept;
    int64_t MatchSummaryDurationMs(const ConfigData& config) noexcept;
    std::string StatCell(std::string_view label, std::string_view value, std::string_view valueClass = {});
    std::string StatGrid(std::string_view title, const std::vector<std::pair<std::string, std::string>>& rows);
    std::string SectionStart(std::string_view title);
    std::string SectionEnd();
    std::string Button(std::string_view action, std::string_view label, const char* klass = "", bool disabled = false);

    struct GraphWindow {
        int total = 0;
        int offset = 0;
        int maxOffset = 0;
        int firstIndex = 0;
        int count = 0;
        bool canPanOlder = false;
        bool canPanNewer = false;
    };

    GraphWindow ComputeGraphWindow(int totalMatches, int rawOffset, int visibleMatches = 25);

    ColorRGBA* ThemeColorForKey(ConfigData& config, std::string_view key);
    const ColorRGBA* ThemeColorForKey(const ConfigData& config, std::string_view key);
    const char* ThemeColorLabel(std::string_view key);
    bool ParseThemeComponentKey(std::string_view key, std::string_view& colorKey, char& component);
    bool SetThemeComponent(ConfigData& config, std::string_view key, const std::string& value);
    Hsv RgbToHsv(const ColorRGBA& color);
    ColorRGBA HsvToRgb(const Hsv& hsv, float alpha);
    int QuantizedPickerHue(float hue);
    inline std::string CssColor(const ColorRGBA& color) {
        auto byte = [](float v) {
            if (!std::isfinite(v)) return 0;
            return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
        };
        char buffer[10]{};
        std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", byte(color.r), byte(color.g), byte(color.b), byte(color.a));
        return buffer;
    }
    inline bool RankSourceAvailable(const ConfigData& config) {
        return config.enable_mmr_tracking || (OmniNet::Available() && config.custom_api_enabled && AccountClient::Instance().IsSignedIn());
    }

    inline std::string WidgetDomId(DashboardLayout::WidgetId id) {
        switch (id) {
        case DashboardLayout::WidgetId::LiveRoster:
            return "live-roster";
        case DashboardLayout::WidgetId::LiveMatchStats:
            return "live-match";
        case DashboardLayout::WidgetId::SessionStats:
            return "session";
        case DashboardLayout::WidgetId::MmrGraph:
            return "mmr-graph";
        case DashboardLayout::WidgetId::StreaksStats:
            return "streaks";
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            return "gamemodes";
        case DashboardLayout::WidgetId::LobbyRanks:
            return "lobby-ranks";
        case DashboardLayout::WidgetId::DemoTracker:
            return "demos";
        case DashboardLayout::WidgetId::PreviousGames:
            return "previous-games";
        case DashboardLayout::WidgetId::RankProgress:
            return "rank-progress";
        case DashboardLayout::WidgetId::Tournaments:
            return "tournaments";
        }
        return "live-roster";
    }

    int EnabledLobbyRankColumns(const ConfigData& config);
    float LobbyRanksContentMinDp(const ConfigData& config);
    std::pair<float, float> OverlayWidgetDefaultSize(DashboardLayout::WidgetId widget, float dpiScale);
    std::pair<float, float> OverlayWidgetMinSize(DashboardLayout::WidgetId widget, float dpiScale);
    float OverlayUiScale(const ConfigData* config);
    std::pair<float, float> OverlayContainerSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config = nullptr);
    std::pair<float, float> OverlayContainerMinSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config = nullptr);
}
