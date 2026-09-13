#include "ui/rml/RmlUiController.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

namespace {
    int64_t SteadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    float SanitizedScale(float value, float fallback = 1.0f) {
        if (!std::isfinite(value) || value < 0.5f) return fallback;
        return value;
    }

    float SanitizedUiScale(float value) {
        if (!std::isfinite(value)) return 1.0f;
        return std::clamp(value, 0.5f, 2.0f);
    }

    std::string ToLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    enum class PlatformKind { Unknown,
                              Epic,
                              Steam,
                              PlayStation,
                              Xbox,
                              Nintendo,
                              Bot };

    // Platform identity arrives as the `primaryId` prefix Rocket League reports,
    // which varies in casing and spelling ("Epic", "PS4", "XboxOne", "Unknown").
    PlatformKind PlatformKindFor(const std::string& platform) {
        const std::string lower = ToLower(platform);
        if (lower.empty()) return PlatformKind::Unknown;
        if (lower == "epic" || lower == "epicgames") return PlatformKind::Epic;
        if (lower == "steam") return PlatformKind::Steam;
        if (lower.rfind("ps", 0) == 0 || lower == "playstation") return PlatformKind::PlayStation;
        if (lower.rfind("xb", 0) == 0) return PlatformKind::Xbox;
        if (lower == "switch" || lower == "nintendo") return PlatformKind::Nintendo;
        if (lower == "unknown" || lower == "bot") return PlatformKind::Bot;
        return PlatformKind::Unknown;
    }

    std::string PlatformBadgeLabel(PlatformKind kind, const std::string& platform) {
        switch (kind) {
        case PlatformKind::Epic:
            return "EPIC";
        case PlatformKind::Steam:
            return "STEAM";
        case PlatformKind::PlayStation:
            return "PSN";
        case PlatformKind::Xbox:
            return "XBOX";
        case PlatformKind::Nintendo:
            return "SWITCH";
        case PlatformKind::Bot:
            return "BOT";
        case PlatformKind::Unknown:
            break;
        }

        std::string upper = platform;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return upper;
    }

    std::string PlatformDisplayName(PlatformKind kind, const std::string& platform) {
        switch (kind) {
        case PlatformKind::Epic:
            return "Epic";
        case PlatformKind::Steam:
            return "Steam";
        case PlatformKind::PlayStation:
            return "PlayStation";
        case PlatformKind::Xbox:
            return "Xbox";
        case PlatformKind::Nintendo:
            return "Nintendo";
        case PlatformKind::Bot:
            return "Bot";
        case PlatformKind::Unknown:
            break;
        }
        return platform;
    }

    // Class names only; the palette lives in kPlatformPalette so the theme pass
    // can reapply platform colors after it overwrites `.badge`/`.label` colors.
    const char* PlatformClass(PlatformKind kind) {
        switch (kind) {
        case PlatformKind::Epic:
            return "platform platform-epic";
        case PlatformKind::Steam:
            return "platform platform-steam";
        case PlatformKind::PlayStation:
            return "platform platform-psn";
        case PlatformKind::Xbox:
            return "platform platform-xbox";
        case PlatformKind::Nintendo:
            return "platform platform-switch";
        case PlatformKind::Bot:
            return "platform platform-bot";
        case PlatformKind::Unknown:
            break;
        }
        return "platform platform-unknown";
    }

    struct PlatformSwatch {
        const char* className;
        const char* color;
    };

    constexpr PlatformSwatch kPlatformPalette[] = {
        {"platform-epic", "#f2f3f7"},
        {"platform-steam", "#6dc2f0"},
        {"platform-psn", "#5b8cff"},
        {"platform-xbox", "#59d268"},
        {"platform-switch", "#ff5a5a"},
        {"platform-bot", "#98a2b3"},
        {"platform-unknown", "#98a2b3"},
    };

    Rml::Span<const Rml::byte> EmbeddedResource(const char* name) {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceA(module, name, RT_RCDATA);
        if (!resource) return {};
        HGLOBAL loaded = LoadResource(module, resource);
        if (!loaded) return {};
        const DWORD size = SizeofResource(module, resource);
        const auto* bytes = static_cast<const Rml::byte*>(LockResource(loaded));
        if (!bytes || size == 0) return {};
        return {bytes, static_cast<size_t>(size)};
    }

    std::vector<Rml::byte> ReadFontFile(const char* fileName) {
        const std::string relative = std::string("resources/fonts/") + fileName;
        std::vector<std::string> candidates = {relative};
#ifdef OMNISTATS_SOURCE_DIR
        candidates.push_back(std::string(OMNISTATS_SOURCE_DIR) + "/" + relative);
#endif
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(candidate, ec);
            if (ec || size == 0) continue;
            std::ifstream file(candidate, std::ios::binary);
            if (!file) continue;
            std::vector<Rml::byte> bytes(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
            if (file.gcount() != static_cast<std::streamsize>(size)) continue;
            return bytes;
        }
        return {};
    }

    std::string TrackerUrlForPlayer(const PlayerData& player) {
        const size_t delimiter = player.primaryId.find('|');
        if (delimiter == std::string::npos) return {};

        const std::string rawPlatform = ToLower(player.primaryId.substr(0, delimiter));
        std::string trackerPlatform;
        if (rawPlatform == "epic" || rawPlatform == "epicgames")
            trackerPlatform = "epic";
        else if (rawPlatform == "steam")
            trackerPlatform = "steam";
        else if (rawPlatform == "ps4" || rawPlatform == "psn" || rawPlatform == "playstation")
            trackerPlatform = "psn";
        else if (rawPlatform == "xboxone" || rawPlatform == "xbox" || rawPlatform == "xbl")
            trackerPlatform = "xbl";
        else if (rawPlatform == "switch" || rawPlatform == "nintendo")
            trackerPlatform = "switch";
        else
            return {};

        std::string identifier = player.name;
        if (trackerPlatform == "steam") {
            const size_t idStart = delimiter + 1;
            const size_t secondDelimiter = player.primaryId.find('|', idStart);
            identifier = player.primaryId.substr(idStart, secondDelimiter == std::string::npos ? std::string::npos : secondDelimiter - idStart);
        }
        if (identifier.empty()) return {};

        return "https://rocketleague.tracker.network/rocket-league/profile/" + trackerPlatform + "/" +
               Format::SimpleUrlEncode(identifier) + "/overview";
    }

    std::string Attribute(Rml::Element* element, const char* name) {
        if (!element) return {};
        return element->GetAttribute<Rml::String>(name, "").c_str();
    }

    const char* GamepadButtonName(int button) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return "A";
        case SDL_CONTROLLER_BUTTON_B:
            return "B";
        case SDL_CONTROLLER_BUTTON_X:
            return "X";
        case SDL_CONTROLLER_BUTTON_Y:
            return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:
            return "Select/Back";
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return "Guide";
        case SDL_CONTROLLER_BUTTON_START:
            return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return "Left Thumb";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return "Right Thumb";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return "Left Bumper";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return "Right Bumper";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return "D-Pad Right";
        case 22:
            return "Left Stick Up";
        case 23:
            return "Left Stick Down";
        case 24:
            return "Left Stick Left";
        case 25:
            return "Left Stick Right";
        case 26:
            return "Right Stick Up";
        case 27:
            return "Right Stick Down";
        case 28:
            return "Right Stick Left";
        case 29:
            return "Right Stick Right";
        default:
            return nullptr;
        }
    }

    std::string GamepadBindName(int mappedButton, bool raw, int rawButton) {
        if (raw && rawButton >= 0) return "Raw Button " + std::to_string(rawButton);
        if (!raw) {
            if (const char* name = GamepadButtonName(mappedButton)) return name;
            if (mappedButton >= 0) return "Controller Button " + std::to_string(mappedButton);
        }
        return "None";
    }

    DashboardLayout::WidgetId WidgetFromDom(const std::string& value) {
        if (value == "live-roster") return DashboardLayout::WidgetId::LiveRoster;
        if (value == "live-match") return DashboardLayout::WidgetId::LiveMatchStats;
        if (value == "session") return DashboardLayout::WidgetId::SessionStats;
        if (value == "mmr-graph") return DashboardLayout::WidgetId::MmrGraph;
        if (value == "streaks") return DashboardLayout::WidgetId::StreaksStats;
        if (value == "gamemodes") return DashboardLayout::WidgetId::GamemodeBreakdown;
        if (value == "lobby-ranks") return DashboardLayout::WidgetId::LobbyRanks;
        if (value == "demos") return DashboardLayout::WidgetId::DemoTracker;
        if (value == "previous-games") return DashboardLayout::WidgetId::PreviousGames;
        return DashboardLayout::WidgetId::LiveRoster;
    }

    DashboardLayout::Zone ZoneFromDom(const std::string& value) {
        if (value == "right") return DashboardLayout::Zone::Right;
        if (value == "bottom") return DashboardLayout::Zone::Bottom;
        if (value == "hidden") return DashboardLayout::Zone::Hidden;
        if (value == "top") return DashboardLayout::Zone::Top;
        return DashboardLayout::Zone::Left;
    }

    std::string MmrLabel(MmrCategory category) {
        switch (category) {
        case MmrCategory::Best:
            return "Best Rank";
        case MmrCategory::OneVOne:
            return "1v1";
        case MmrCategory::TwoVTwo:
            return "2v2";
        case MmrCategory::ThreeVThree:
            return "3v3";
        case MmrCategory::Casual:
            return "Casual";
        case MmrCategory::Tourny:
            return "Tournament";
        case MmrCategory::Hoops:
            return "Hoops";
        case MmrCategory::Rumble:
            return "Rumble";
        case MmrCategory::Dropshot:
            return "Dropshot";
        case MmrCategory::SnowDay:
            return "Snow Day";
        case MmrCategory::Heatseeker:
            return "Heatseeker";
        }
        return "Best Rank";
    }

    std::vector<MmrCategory> MmrCategories(bool includeBest, bool extras) {
        std::vector<MmrCategory> result;
        if (includeBest) result.push_back(MmrCategory::Best);
        result.insert(result.end(), {MmrCategory::OneVOne, MmrCategory::TwoVTwo, MmrCategory::ThreeVThree});
        if (extras) result.insert(result.end(), {MmrCategory::Hoops, MmrCategory::Rumble, MmrCategory::Dropshot, MmrCategory::SnowDay, MmrCategory::Heatseeker});
        result.insert(result.end(), {MmrCategory::Casual, MmrCategory::Tourny});
        return result;
    }

    std::string RankPart(const std::string& tier) {
        std::string value = ToLower(tier);
        if (const size_t divPos = value.find(" div"); divPos != std::string::npos) value.resize(divPos);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    int TierResourceIndex(const std::string& tier) {
        const std::string t = RankPart(tier);
        int base = 0;
        if (t.empty() || t == "-" || t.find("unranked") != std::string::npos) return 0;
        if (t.find("supersonic") == 0) return 22;
        if (t.find("bronze") == 0)
            base = 1;
        else if (t.find("silver") == 0)
            base = 4;
        else if (t.find("gold") == 0)
            base = 7;
        else if (t.find("platinum") == 0)
            base = 10;
        else if (t.find("diamond") == 0)
            base = 13;
        else if (t.find("grand champion") == 0)
            base = 19;
        else if (t.find("champion") == 0)
            base = 16;
        else
            return 0;

        if (t.find(" iii") != std::string::npos || t.find(" 3") != std::string::npos) return base + 2;
        if (t.find(" ii") != std::string::npos || t.find(" 2") != std::string::npos) return base + 1;
        return base;
    }

    int DivisionLevel(const std::string& tier) {
        const int tierIndex = TierResourceIndex(tier);
        if (tierIndex <= 0 || tierIndex >= 22) return 0;
        const std::string t = ToLower(tier);
        if (t.find("div iv") != std::string::npos || t.find("division iv") != std::string::npos || t.find("div 4") != std::string::npos || t.find("division 4") != std::string::npos) return 4;
        if (t.find("div iii") != std::string::npos || t.find("division iii") != std::string::npos || t.find("div 3") != std::string::npos || t.find("division 3") != std::string::npos) return 3;
        if (t.find("div ii") != std::string::npos || t.find("division ii") != std::string::npos || t.find("div 2") != std::string::npos || t.find("division 2") != std::string::npos) return 2;
        if (t.find("div i") != std::string::npos || t.find("division i") != std::string::npos || t.find("div 1") != std::string::npos || t.find("division 1") != std::string::npos) return 1;
        return 0;
    }

    int DivisionColorResourceIndex(const std::string& tier) {
        const int index = TierResourceIndex(tier);
        if (index >= 1 && index <= 3) return 1;
        if (index >= 4 && index <= 6) return 2;
        if (index >= 7 && index <= 9) return 3;
        if (index >= 10 && index <= 12) return 4;
        if (index >= 13 && index <= 15) return 5;
        if (index >= 16 && index <= 18) return 6;
        if (index >= 19 && index <= 22) return 7;
        return 0;
    }

    int PlaylistResourceIndex(const std::string& playlist) {
        const std::string p = ToLower(playlist);
        if (p == "1v1" || p == "duel") return 0;
        if (p == "2v2" || p == "doubles") return 1;
        if (p == "3v3" || p == "standard") return 2;
        if (p == "hoops") return 3;
        if (p == "rumble") return 4;
        if (p == "dropshot") return 5;
        if (p == "snowday" || p == "snow day") return 6;
        if (p == "t" || p == "tourny" || p == "tournament" || p == "tournaments") return 7;
        if (p == "4v4") return 8;
        if (p == "heatseeker") return 9;
        return -1;
    }

    std::string ToggleControl(const std::string& key, const std::string& label, const std::string& help, bool checked, bool disabled = false) {
        std::ostringstream out;
        out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << label << "</div>";
        if (!help.empty()) out << "<div class='setting-help'>" << help << "</div>";
        out << "</div><div class='toggle-switch'><input type='checkbox' class='checkbox' data-setting='" << key << "' " << (checked ? "checked='checked' " : "") << (disabled ? "disabled='disabled' " : "") << "/><span class='toggle-thumb'></span></div></div>";
        return out.str();
    }

    std::string SectionStart(const std::string& title) {
        return "<div class='setting-section'><div class='setting-title'>" + title + "</div>";
    }

    std::string SectionEnd() {
        return "</div>";
    }

    std::string Button(const std::string& action, const std::string& label, const char* klass = "") {
        std::ostringstream out;
        out << "<button data-action='" << action << "'";
        if (klass && *klass) out << " class='" << klass << "'";
        out << ">" << label << "</button>";
        return out.str();
    }

    ColorRGBA* ThemeColorForKey(ConfigData& config, std::string_view key) {
        if (key == "theme_bg") return &config.themeBg;
        if (key == "theme_panel") return &config.themeSettingsPanel;
        if (key == "theme_text") return &config.themeText;
        if (key == "theme_accent") return &config.themeAccent;
        if (key == "theme_win") return &config.themeWin;
        if (key == "theme_loss") return &config.themeLoss;
        if (key == "theme_dim") return &config.themeDim;
        if (key == "theme_muted") return &config.themeMuted;
        if (key == "theme_graph") return &config.themeGraphLine;
        if (key == "theme_baseline") return &config.themeGraphBaseline;
        return nullptr;
    }

    const ColorRGBA* ThemeColorForKey(const ConfigData& config, std::string_view key) {
        if (key == "theme_bg") return &config.themeBg;
        if (key == "theme_panel") return &config.themeSettingsPanel;
        if (key == "theme_text") return &config.themeText;
        if (key == "theme_accent") return &config.themeAccent;
        if (key == "theme_win") return &config.themeWin;
        if (key == "theme_loss") return &config.themeLoss;
        if (key == "theme_dim") return &config.themeDim;
        if (key == "theme_muted") return &config.themeMuted;
        if (key == "theme_graph") return &config.themeGraphLine;
        if (key == "theme_baseline") return &config.themeGraphBaseline;
        return nullptr;
    }

    const char* ThemeColorLabel(std::string_view key) {
        if (key == "theme_bg") return "Overlay background";
        if (key == "theme_panel") return "Settings panels";
        if (key == "theme_text") return "Text";
        if (key == "theme_accent") return "Accent";
        if (key == "theme_win") return "Win";
        if (key == "theme_loss") return "Loss";
        if (key == "theme_dim") return "Dim text";
        if (key == "theme_muted") return "Muted info";
        if (key == "theme_graph") return "Graph line";
        if (key == "theme_baseline") return "Graph baseline";
        return "Color";
    }

    bool ParseThemeComponentKey(std::string_view key, std::string_view& colorKey, char& component) {
        const size_t separator = key.rfind(':');
        if (separator == std::string_view::npos || separator + 2 != key.size()) return false;
        colorKey = key.substr(0, separator);
        component = key[separator + 1];
        return component == 'r' || component == 'g' || component == 'b' || component == 'a';
    }

    bool SetThemeComponent(ConfigData& config, std::string_view key, const std::string& value) {
        std::string_view colorKey;
        char component = 0;
        if (!ParseThemeComponentKey(key, colorKey, component)) return false;
        ColorRGBA* color = ThemeColorForKey(config, colorKey);
        if (!color) return false;

        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (!end || end == value.c_str() || *end != '\0') return false;
        const float normalized = static_cast<float>(std::clamp(parsed, 0L, 255L)) / 255.0f;
        switch (component) {
        case 'r':
            color->r = normalized;
            break;
        case 'g':
            color->g = normalized;
            break;
        case 'b':
            color->b = normalized;
            break;
        case 'a':
            color->a = normalized;
            break;
        default:
            return false;
        }
        return true;
    }

    struct Hsv {
        float h = 0.0f; // degrees, [0, 360)
        float s = 0.0f;
        float v = 0.0f;
    };

    Hsv RgbToHsv(const ColorRGBA& color) {
        const float r = std::clamp(color.r, 0.0f, 1.0f);
        const float g = std::clamp(color.g, 0.0f, 1.0f);
        const float b = std::clamp(color.b, 0.0f, 1.0f);
        const float max = std::max({r, g, b});
        const float min = std::min({r, g, b});
        const float span = max - min;

        Hsv hsv;
        hsv.v = max;
        hsv.s = max > 0.0f ? span / max : 0.0f;
        if (span <= 0.0f) return hsv;

        if (max == r)
            hsv.h = 60.0f * std::fmod((g - b) / span, 6.0f);
        else if (max == g)
            hsv.h = 60.0f * ((b - r) / span + 2.0f);
        else
            hsv.h = 60.0f * ((r - g) / span + 4.0f);
        if (hsv.h < 0.0f) hsv.h += 360.0f;
        return hsv;
    }

    ColorRGBA HsvToRgb(const Hsv& hsv, float alpha) {
        const float h = std::fmod(std::fmod(hsv.h, 360.0f) + 360.0f, 360.0f) / 60.0f;
        const float s = std::clamp(hsv.s, 0.0f, 1.0f);
        const float v = std::clamp(hsv.v, 0.0f, 1.0f);
        const float c = v * s;
        const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
        const float m = v - c;

        float r = m, g = m, b = m;
        switch (static_cast<int>(h)) {
        case 0:
            r += c;
            g += x;
            break;
        case 1:
            r += x;
            g += c;
            break;
        case 2:
            g += c;
            b += x;
            break;
        case 3:
            g += x;
            b += c;
            break;
        case 4:
            r += x;
            b += c;
            break;
        default:
            r += c;
            b += x;
            break;
        }
        return ColorRGBA{r, g, b, std::clamp(alpha, 0.0f, 1.0f)};
    }

    int EnabledLobbyRankColumns(const ConfigData& config) {
        int rankColumns = 0;
        rankColumns += config.show_lobby_rank_1v1 ? 1 : 0;
        rankColumns += config.show_lobby_rank_2v2 ? 1 : 0;
        rankColumns += config.show_lobby_rank_3v3 ? 1 : 0;
        rankColumns += config.show_lobby_rank_casual ? 1 : 0;
        rankColumns += config.show_lobby_rank_tourny ? 1 : 0;
        if (config.show_extra_playlists) {
            rankColumns += config.show_lobby_rank_hoops ? 1 : 0;
            rankColumns += config.show_lobby_rank_rumble ? 1 : 0;
            rankColumns += config.show_lobby_rank_dropshot ? 1 : 0;
            rankColumns += config.show_lobby_rank_snowday ? 1 : 0;
            rankColumns += config.show_lobby_rank_heatseeker ? 1 : 0;
        }
        return std::max(rankColumns, 1);
    }

    float LobbyRanksContentMinDp(const ConfigData& config) {
        const int columns = EnabledLobbyRankColumns(config);
        // Keep a protected identity column plus one column per enabled playlist,
        // matching the compact .lobby-rank-* metrics in the stylesheet:
        // 135dp identity column + 4dp row padding + 22dp card padding/border + 4dp tolerance
        // = 165dp base, plus 68dp per enabled playlist column (rank/rating pair
        // plus the games-played gutter).
        return 165.0f + 68.0f * static_cast<float>(columns);
    }

    std::pair<float, float> OverlayWidgetDefaultSize(DashboardLayout::WidgetId widget, float dpiScale) {
        const float dpi = SanitizedScale(dpiScale);
        std::pair<float, float> size;
        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
            size = {440.0f, 260.0f};
            break;
        case DashboardLayout::WidgetId::LiveMatchStats:
            size = {340.0f, 210.0f};
            break;
        case DashboardLayout::WidgetId::SessionStats:
            size = {280.0f, 220.0f};
            break;
        case DashboardLayout::WidgetId::MmrGraph:
            size = {430.0f, 260.0f};
            break;
        case DashboardLayout::WidgetId::StreaksStats:
            size = {300.0f, 115.0f};
            break;
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            size = {340.0f, 210.0f};
            break;
        case DashboardLayout::WidgetId::LobbyRanks:
            // Auto width comes from the per-column minimum below, so this only
            // has to cover the identity column and margins.
            size = {165.0f, 220.0f};
            break;
        case DashboardLayout::WidgetId::DemoTracker:
            size = {280.0f, 84.0f};
            break;
        case DashboardLayout::WidgetId::PreviousGames:
            size = {560.0f, 260.0f};
            break;
        default:
            size = {320.0f, 200.0f};
            break;
        }
        return {size.first * dpi, size.second * dpi};
    }

    std::pair<float, float> OverlayWidgetMinSize(DashboardLayout::WidgetId widget, float dpiScale) {
        const float dpi = SanitizedScale(dpiScale);
        if (widget == DashboardLayout::WidgetId::LiveRoster) return {430.0f * dpi, 120.0f * dpi};
        return {220.0f * dpi, 120.0f * dpi};
    }

    std::pair<float, float> OverlayContainerMinSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config = nullptr);

    // Overlay container geometry is persisted in design pixels at 100% app text
    // size. The drawn box has to grow and shrink with ui_scale exactly like the
    // text inside it: otherwise low scales leave dead space (and high scales
    // overflow), and every drag/resize clamp is computed against a box that is
    // not what the user sees.
    float OverlayUiScale(const ConfigData* config) {
        return config ? SanitizedUiScale(config->ui_scale) : 1.0f;
    }

    std::pair<float, float> OverlayContainerSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config = nullptr) {
        const float ui = OverlayUiScale(config);
        float width = container.w * ui;
        float height = container.h * ui;
        if (container.w <= 1.0f) {
            width = 0.0f;
            for (auto widget : container.widgets)
                width = std::max(width, OverlayWidgetDefaultSize(widget, dpiScale).first * ui);
        }
        if (container.h <= 1.0f) {
            height = 0.0f;
            for (auto widget : container.widgets)
                height += OverlayWidgetDefaultSize(widget, dpiScale).second * ui;
            height += std::max<int>(0, static_cast<int>(container.widgets.size()) - 1) * 42.0f * SanitizedScale(dpiScale) * ui;
        }
        const auto [minWidth, minHeight] = OverlayContainerMinSize(container, dpiScale, config);
        return {std::max(width, minWidth), std::max(height, minHeight)};
    }

    std::pair<float, float> OverlayContainerMinSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config) {
        const float dpi = SanitizedScale(dpiScale);
        const float ui = OverlayUiScale(config);
        const float scale = dpi * ui;
        float width = 220.0f * scale;
        float height = 120.0f * scale;
        if (!container.widgets.empty()) {
            width = 0.0f;
            height = 0.0f;
            for (auto widget : container.widgets) {
                float widgetWidth = 0.0f;
                float widgetHeight = 0.0f;
                if (widget == DashboardLayout::WidgetId::LobbyRanks && config) {
                    widgetWidth = LobbyRanksContentMinDp(*config) * scale;
                    widgetHeight = 120.0f * scale;
                } else {
                    const auto [ww, wh] = OverlayWidgetMinSize(widget, dpi);
                    widgetWidth = ww * ui;
                    widgetHeight = wh * ui;
                }
                width = std::max(width, widgetWidth);
                height += widgetHeight;
            }
            height += std::max<int>(0, static_cast<int>(container.widgets.size()) - 1) * 42.0f * scale;
        }
        return {std::max(width, 220.0f * scale), std::max(height, 120.0f * scale)};
    }
}

RmlUiController::RmlUiController(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)), m_systemInterface(nullptr) {
    if (m_state) m_lastShowMenu = m_state->ui.showMenu.load();
}

RmlUiController::~RmlUiController() {
    Shutdown();
}

// OmniStats ships its own typefaces so the client matches the web brand and does
// not inherit whatever Segoe UI revision a machine happens to have. Faces are
// registered with an explicit family and weight: static Inter/JetBrains Mono files
// name themselves "Inter SemiBold" and friends, which would otherwise register as
// separate families.
bool RmlUiController::LoadBundledFonts() {
    struct BundledFont {
        const char* resource;
        const char* file;
        const char* family;
        int weight;
        bool required;
    };
    static constexpr BundledFont kFonts[] = {
        {"FONT_UI_REGULAR", "Inter-Regular.ttf", "Inter", 400, true},
        {"FONT_UI_SEMIBOLD", "Inter-SemiBold.ttf", "Inter", 600, false},
        {"FONT_UI_BOLD", "Inter-Bold.ttf", "Inter", 700, false},
        {"FONT_MONO_REGULAR", "JetBrainsMono-Regular.ttf", "JetBrains Mono", 400, false},
        {"FONT_MONO_BOLD", "JetBrainsMono-Bold.ttf", "JetBrains Mono", 700, false},
        {"FONT_DISPLAY_REGULAR", "RussoOne-Regular.ttf", "Russo One", 400, false},
    };

    m_fontBlobs.clear();
    for (const auto& font : kFonts) {
        // Packaged builds carry the faces as RCDATA; test and unpacked builds have
        // no application resources, so fall back to the source tree like
        // RmlFileInterface does for RML/RCSS.
        Rml::Span<const Rml::byte> data = EmbeddedResource(font.resource);
        if (data.empty()) {
            auto blob = ReadFontFile(font.file);
            if (!blob.empty()) {
                m_fontBlobs.push_back(std::move(blob));
                const auto& stored = m_fontBlobs.back();
                data = {stored.data(), stored.size()};
            }
        }
        const bool loaded = !data.empty() &&
                            Rml::LoadFontFace(data, font.family, Rml::Style::FontStyle::Normal,
                                              static_cast<Rml::Style::FontWeight>(font.weight));
        if (loaded) continue;
        if (font.required) {
            std::cerr << "[RmlUi] Failed to load font " << font.file
                      << "; refusing to start with an unreadable UI.\n";
            return false;
        }
        std::cerr << "[RmlUi] Warning: failed to load font " << font.file << ".\n";
    }

    // Player names are arbitrary user data. Inter covers Latin/Greek/Cyrillic, so
    // register system faces as fallbacks for everything else instead of drawing
    // missing-glyph boxes.
    for (const char* fallback : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msgothic.ttc"})
        Rml::LoadFontFace(fallback, true);

    return true;
}

bool RmlUiController::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context, int width, int height, float dpiScale) {
    Shutdown();
    m_hwnd = hwnd;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_dpiScale = SanitizedScale(dpiScale);
    m_systemInterface.SetWindow(hwnd);

    if (!m_renderInterface.Initialize(device, context, m_width, m_height)) return false;
    Rml::SetSystemInterface(&m_systemInterface);
    Rml::SetRenderInterface(&m_renderInterface);
    Rml::SetFileInterface(&m_fileInterface);
    m_rmlInterfacesInstalled = true;
    if (!Rml::Initialise()) {
        Shutdown();
        return false;
    }
    m_rmlInitialized = true;

    m_graphLineInstancer = std::make_unique<Rml::ElementInstancerGeneric<RmlMmrGraphLines>>();
    Rml::Factory::RegisterElementInstancer("mmrgraphlines", m_graphLineInstancer.get());

    m_context = Rml::CreateContext("omnistats", Rml::Vector2i(m_width, m_height));
    if (!m_context) {
        Shutdown();
        return false;
    }

    if (!LoadBundledFonts()) {
        Shutdown();
        return false;
    }

    m_document = m_context->LoadDocument("res://main.rml");
    if (!m_document) {
        Shutdown();
        return false;
    }
    m_document->Show();

    for (const char* event : {"click", "change", "input", "mousedown", "mousemove", "mouseup"}) {
        m_context->AddEventListener(event, this);
    }
    m_context->AddEventListener("blur", this, true);
    m_config = Config::Read();
    if (m_pendingBallchasingToken.empty()) m_pendingBallchasingToken = m_config.ballchasing_token;
    SetDpiScale(m_dpiScale);
    SnapshotState();
    RefreshAsyncData();
    UpdateThemeProperties();
    if (!m_config.second_monitor_mode) {
        SetRootRml("dashboard-root", "");
    } else {
        SetRootRml("overlay-root", "");
    }
    RebuildVisibleUi(true);
    return true;
}

void RmlUiController::Shutdown() {
    if (m_context) {
        for (const char* event : {"click", "change", "input", "mousedown", "mousemove", "mouseup"}) {
            m_context->RemoveEventListener(event, this);
        }
        m_context->RemoveEventListener("blur", this, true);
        m_context = nullptr;
        m_document = nullptr;
    }
    if (m_rmlInitialized) {
        Rml::Shutdown();
        m_rmlInitialized = false;
    }
    // RmlUi stores these interfaces as global raw pointers. Clear the pointers
    // installed by this controller even if Rml::Initialise() failed before
    // m_rmlInitialized became true. Headless/uninitialized controller instances
    // must not disturb interfaces owned by another live RmlUi host.
    if (m_rmlInterfacesInstalled) {
        Rml::SetFileInterface(nullptr);
        Rml::SetRenderInterface(nullptr);
        Rml::SetSystemInterface(nullptr);
        m_rmlInterfacesInstalled = false;
    }

    // Factory registrations are non-owning; keep the instancer alive through
    // Rml::Shutdown(), then release it after the factory has torn down.
    m_graphLineInstancer.reset();
    m_renderInterface.Shutdown();
    m_systemInterface.SetWindow(nullptr);
    m_hwnd = nullptr;
}

void RmlUiController::Resize(int width, int height, float dpiScale) {
    const bool sizeChanged = m_width != std::max(width, 1) || m_height != std::max(height, 1);
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_renderInterface.SetViewport(m_width, m_height);
    if (m_context) m_context->SetDimensions(Rml::Vector2i(m_width, m_height));
    SetDpiScale(dpiScale);
    // A window-mode switch changes the client size. Re-center Settings for the
    // new size instead of leaving it positioned (and clipped) for the old one.
    if (sizeChanged) {
        m_settingsPositioned = false;
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    }
}

void RmlUiController::SetDpiScale(float dpiScale) {
    m_dpiScale = SanitizedScale(dpiScale);
    m_appliedUiScale = SanitizedUiScale(m_config.ui_scale);
    if (m_context) m_context->SetDensityIndependentPixelRatio(m_dpiScale * m_appliedUiScale);
}

bool RmlUiController::ProcessWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // Losing the button (focus change or capture loss) never delivers `mouseup`,
    // so release the rebuild hold here or live updates would stay frozen.
    if (message == WM_KILLFOCUS || message == WM_CAPTURECHANGED || message == WM_LBUTTONUP) m_pointerPressed = false;
    return RmlInputWin32::ProcessWindowMessage(m_context, hwnd, message, wParam, lParam);
}

bool RmlUiController::ApplyMouseCursor() {
    return m_systemInterface.ApplyMouseCursor();
}

void RmlUiController::Render() {
    if (!m_context) return;
    m_context->Update();
    m_context->Render();
}

bool RmlUiController::WantsInteraction() const {
    if (!m_state) return false;
    // Preserve the native overlay's click-through contract. In-game UI only
    // becomes interactive while Settings/layout editing is open; otherwise
    // mouse input must continue through to Rocket League. Second-monitor mode
    // is a normal interactive window. RmlUi hover state must not override this.
    return m_state->ui.showMenu.load() || m_state->ui.dashboardLayoutEditMode.load() ||
           m_config.second_monitor_mode || m_drag.kind != DragKind::None;
}
void RmlUiController::Update(const ConfigData& config) {
    const auto sameColor = [](const ColorRGBA& a, const ColorRGBA& b) {
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
    };
    // Compare against the scale actually pushed into the RmlUi context, not the
    // previous config: a `<select>` change writes the new value into m_config and
    // defers the rest of its work, so a config-to-config comparison would never
    // see the change and the context would keep rendering at the old dp ratio
    // while all geometry was computed for the new one.
    const bool scaleChanged = SanitizedUiScale(config.ui_scale) != m_appliedUiScale;
    const bool themeChanged = !sameColor(config.themeBg, m_config.themeBg) ||
                              !sameColor(config.themeSettingsPanel, m_config.themeSettingsPanel) ||
                              !sameColor(config.themeText, m_config.themeText) ||
                              !sameColor(config.themeAccent, m_config.themeAccent) ||
                              !sameColor(config.themeWin, m_config.themeWin) ||
                              !sameColor(config.themeLoss, m_config.themeLoss) ||
                              !sameColor(config.themeDim, m_config.themeDim) ||
                              !sameColor(config.themeMuted, m_config.themeMuted) ||
                              !sameColor(config.themeGraphLine, m_config.themeGraphLine) ||
                              !sameColor(config.themeGraphBaseline, m_config.themeGraphBaseline);

    const auto activeDragKind = m_drag.kind;
    const std::string draggingId = m_drag.containerId;
    float inFlightX = 0.0f, inFlightY = 0.0f, inFlightW = 0.0f, inFlightH = 0.0f;
    bool hasInFlight = false;
    if (activeDragKind == DragKind::OverlayMove || activeDragKind == DragKind::OverlayResize) {
        auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(),
                               [&](const auto& c) { return c.id == draggingId; });
        if (it != m_config.overlay_layout.containers.end()) {
            inFlightX = it->x;
            inFlightY = it->y;
            inFlightW = it->w;
            inFlightH = it->h;
            hasInFlight = true;
        }
    }
    // The same applies to the floating cards: their position lives in m_config
    // while the pointer is down, and the per-frame assignment below would
    // otherwise snap them back to the persisted value every frame.
    const float inFlightSessionX = m_config.session_view_x;
    const float inFlightSessionY = m_config.session_view_y;
    const float inFlightSummaryX = m_config.match_summary_x;
    const float inFlightSummaryY = m_config.match_summary_y;

    m_config = config;

    if (hasInFlight) {
        for (auto& c : m_config.overlay_layout.containers) {
            if (c.id == draggingId) {
                c.x = inFlightX;
                c.y = inFlightY;
                c.w = inFlightW;
                c.h = inFlightH;
                break;
            }
        }
    }
    if (activeDragKind == DragKind::FloatingCard) {
        m_config.session_view_x = inFlightSessionX;
        m_config.session_view_y = inFlightSessionY;
        m_config.match_summary_x = inFlightSummaryX;
        m_config.match_summary_y = inFlightSummaryY;
    }
    SnapshotState();
    RefreshAsyncData();
    const bool settingsOpen = m_state && m_state->ui.showMenu.load();
    if (m_lastShowMenu && !settingsOpen) {
        // Settings can be closed outside the RmlUi document (F5/Escape via
        // InputManager). Treat that the same as an in-document close so no
        // sensitive/re-entrant modal state survives into the next open.
        FinishBindCapture();
        m_showBallchasingToken = false;
        m_confirmReplayUploads = false;
        m_confirmDeleteHistory = false;
        m_editColorKey.clear();
    } else {
        UpdateInputCapture();
    }
    if (scaleChanged) SetDpiScale(m_dpiScale);
    if (themeChanged) UpdateThemeProperties();
    RebuildVisibleUi(false);
}

Rml::Element* RmlUiController::Root(const char* id) const {
    return m_document ? m_document->GetElementById(id) : nullptr;
}

void RmlUiController::SetRootRml(const char* id, const std::string& rml) {
    if (auto* root = Root(id)) {
        const bool prev = m_rebuildingUi;
        m_rebuildingUi = true;
        // Overlay telemetry replaces its DOM frequently. Batch cursor writes
        // and replay the last pointer coordinate against the new tree, matching
        // Settings' stable hover behavior instead of flashing arrow/move.
        m_systemInterface.BeginCursorUpdate();
        root->SetInnerRML(rml);
        UpdateThemeProperties();
        if (m_hasPointerPosition)
            m_context->ProcessMouseMove(m_lastPointerX, m_lastPointerY, 0);
        m_systemInterface.EndCursorUpdate();
        m_rebuildingUi = prev;
    }
}

bool RmlUiController::PointerOverInteractiveOverlay() const {
    if (!m_hasPointerPosition) return false;

    const auto containsPointer = [&](const char* selector) {
        if (auto* app = Root("app")) {
            Rml::ElementList elements;
            app->QuerySelectorAll(elements, selector);
            for (Rml::Element* element : elements) {
                const float left = element->GetAbsoluteLeft();
                const float top = element->GetAbsoluteTop();
                if (m_lastPointerX >= left && m_lastPointerX <= left + element->GetOffsetWidth() &&
                    m_lastPointerY >= top && m_lastPointerY <= top + element->GetOffsetHeight()) {
                    return true;
                }
            }
        }
        return false;
    };

    // These live RML trees are rebuilt on telemetry updates. Replacing a hovered
    // card destroys RmlUi's hover target, which is the cursor flash the static
    // Settings tree does not have.
    return containsPointer(".overlay-card.interactive") || containsPointer(".floating-card-movable");
}

void RmlUiController::SnapshotState() {
    if (!m_state) return;
    {
        std::shared_lock lock(m_state->game.mutex);
        const auto version = m_state->game.version.load();
        if (version != m_lastGameVersion) {
            m_lastGameVersion = version;
            m_snap.matchGuid = m_state->game.matchGuid;
            m_snap.arenaName = m_state->game.arenaName;
            m_snap.score[0] = m_state->game.score[0];
            m_snap.score[1] = m_state->game.score[1];
            m_snap.inMatch = m_state->game.inMatch.load();
            m_snap.inReplay = m_state->game.inReplay.load();
            m_snap.matchFinalized = m_state->game.matchFinalized;
            m_snap.myPrimaryId = m_state->game.myPrimaryId;
            m_snap.myTeam = m_state->game.myTeam;
            m_snap.currentMatch = m_state->game.currentMatch;
            m_snap.sessionTotals = m_state->game.sessionTotals;
            m_snap.roster = m_state->game.roster;
            m_snap.sessionGamemodes = m_state->game.sessionGamemodes;
            m_snap.lastMatchWasVoid = m_state->game.lastMatchWasVoid;
            m_snap.lastMatchVoidReason = m_state->game.lastMatchVoidReason;
            m_snap.matchSummaryScore[0] = m_state->game.matchSummaryScore[0];
            m_snap.matchSummaryScore[1] = m_state->game.matchSummaryScore[1];
            m_snap.matchSummaryMyTeam = m_state->game.matchSummaryMyTeam;
            m_snap.matchSummaryWinnerTeam = m_state->game.matchSummaryWinnerTeam;
        }
    }
    {
        std::shared_lock lock(m_state->history.mutex);
        m_snap.showLifetimeGraph = m_state->history.showLifetimeGraph.load();
        const auto version = m_state->history.version.load();
        if (version != m_lastHistoryVersion) {
            m_lastHistoryVersion = version;
            m_snap.initialMmr = static_cast<float>(m_state->history.initialMmr);
            m_snap.playlistInitialMmr = m_state->history.playlistInitialMmr;
            m_snap.playlistHistoryY = m_state->history.playlistHistoryY;
            m_snap.playlistHistoryEstimated.clear();
            for (const auto& [playlist, history] : m_state->history.playlistHistoryY) {
                auto& flags = m_snap.playlistHistoryEstimated[playlist];
                flags.assign(history.size(), false);
                auto pointIt = m_state->history.playlistMatchPoints.find(playlist);
                if (pointIt == m_state->history.playlistMatchPoints.end()) continue;
                for (const auto& point : pointIt->second)
                    if (point.historyIndex < flags.size()) flags[point.historyIndex] = point.valueEstimated;
            }
            m_snap.lifetimeMmrX = m_state->history.lifetimeMmrX;
            m_snap.lifetimeMmrY = m_state->history.lifetimeMmrY;
            m_snap.recentSavedMatches = m_state->history.pendingRecentMatches;
            for (const auto& saved : m_state->history.recentSavedMatches) {
                const bool shadowed = !saved.matchGuid.empty() && std::any_of(m_state->history.pendingRecentMatches.begin(), m_state->history.pendingRecentMatches.end(), [&](const SessionMatchSummary& pending) {
                    return pending.matchGuid == saved.matchGuid;
                });
                if (!shadowed) m_snap.recentSavedMatches.push_back(saved);
            }
            std::stable_sort(m_snap.recentSavedMatches.begin(), m_snap.recentSavedMatches.end(), [](const auto& a, const auto& b) { return a.endedAtUnix > b.endedAtUnix; });
            m_snap.recentSavedMatchesLoaded = m_state->history.recentSavedMatchesLoaded || !m_state->history.pendingRecentMatches.empty();
        }
    }
    if (m_snap.myPrimaryId.empty() && !m_config.last_primary_id.empty()) m_snap.myPrimaryId = m_config.last_primary_id;
}

void RmlUiController::RefreshAsyncData() {
    if (!m_dbManager || !m_state) return;
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    if (!effectivePrimary.empty()) {
        const auto category = m_state->ui.graphMmrCategory.load();
        if (effectivePrimary != m_lastLifetimeHistoryPrimaryId || category != m_lastLifetimeHistoryCategory) {
            {
                std::unique_lock lock(m_state->history.mutex);
                m_state->history.lifetimeMmrX.clear();
                m_state->history.lifetimeMmrY.clear();
                m_state->history.version++;
            }
            m_snap.lifetimeMmrX.clear();
            m_snap.lifetimeMmrY.clear();
            m_dbManager->AsyncGetLifetimeMmrHistory(effectivePrimary, MmrCategoryToString(category));
            m_lastLifetimeHistoryPrimaryId = effectivePrimary;
            m_lastLifetimeHistoryCategory = category;
        }
        if (effectivePrimary != m_lastDbFetchPrimaryId) {
            m_dbManager->AsyncRefreshDbStats(effectivePrimary);
            m_lastDbFetchPrimaryId = effectivePrimary;
        }
    }
    const int recentLimit = std::clamp(m_config.previous_games_limit, 10, kPreviousGamesMaxLimit);
    if (effectivePrimary != m_lastRecentMatchHistoryPrimaryId || recentLimit != m_lastRecentMatchHistoryLimit) {
        m_dbManager->AsyncGetRecentMatchHistory(effectivePrimary, recentLimit);
        m_lastRecentMatchHistoryPrimaryId = effectivePrimary;
        m_lastRecentMatchHistoryLimit = recentLimit;
    }
}

void RmlUiController::UpdateThemeProperties() {
    auto* app = Root("app");
    if (!app) return;

    // Apply the persisted OmniStats theme as ordinary local properties instead of
    // depending on stylesheet custom-property support in any particular packaged
    // RmlUi version. SetInnerRML replaces descendants, so SetRootRml reapplies this
    // pass after each targeted DOM rebuild.
    const auto setClass = [&](const char* className, const char* property, const std::string& value) {
        Rml::ElementList elements;
        app->GetElementsByClassName(elements, className);
        for (Rml::Element* element : elements)
            element->SetProperty(property, value);
    };
    const auto setSelector = [&](const char* selector, const char* property, const std::string& value) {
        Rml::ElementList elements;
        app->QuerySelectorAll(elements, selector);
        for (Rml::Element* element : elements)
            element->SetProperty(property, value);
    };
    const std::string bg = CssColor(m_config.themeBg);
    const std::string panel = CssColor(m_config.themeSettingsPanel);
    const std::string text = CssColor(m_config.themeText);
    const std::string accent = CssColor(m_config.themeAccent);
    const std::string win = CssColor(m_config.themeWin);
    const std::string loss = CssColor(m_config.themeLoss);
    const std::string dim = CssColor(m_config.themeDim);
    const std::string muted = CssColor(m_config.themeMuted);
    const std::string graph = CssColor(m_config.themeGraphLine);
    const std::string baseline = CssColor(m_config.themeGraphBaseline);
    const auto scaledColor = [](ColorRGBA color, float rgbScale, float alpha = -1.0f) {
        color.r = std::clamp(color.r * rgbScale, 0.0f, 1.0f);
        color.g = std::clamp(color.g * rgbScale, 0.0f, 1.0f);
        color.b = std::clamp(color.b * rgbScale, 0.0f, 1.0f);
        if (alpha >= 0.0f) color.a = std::clamp(alpha, 0.0f, 1.0f);
        return CssColor(color);
    };
    const std::string accentDark = scaledColor(m_config.themeAccent, 0.28f, 0.95f);
    const std::string accentMid = scaledColor(m_config.themeAccent, 0.62f, 1.0f);

    app->SetProperty("color", text);
    setClass("card", "background-color", bg);
    // Dashboard widgets sit one layer above the shell so each card reads as a
    // surface instead of one long text list.
    setClass("dashboard-shell", "background-color", scaledColor(m_config.themeBg, 0.55f, 1.0f));
    setClass("card", "color", text);
    setClass("card-title", "color", text);
    setClass("value", "color", text);
    setClass("metric-value", "color", text);
    setClass("setting-name", "color", text);
    setClass("debug-value", "color", text);
    setSelector("button, .button, input.text, input.password, select", "color", text);
    // RmlUi implements selects with generated child elements; theme those too so
    // changing themeText does not leave the dropdown value/options at RCSS defaults.
    setSelector("select selectvalue, select selectbox option", "color", text);
    setClass("tooltip-bubble", "color", text);

    setClass("settings-window", "background-color", panel);
    setClass("settings-header", "background-color", scaledColor(m_config.themeSettingsPanel, 1.08f, 1.0f));
    setClass("settings-nav", "background-color", scaledColor(m_config.themeSettingsPanel, 0.90f, 1.0f));
    setClass("settings-page", "background-color", panel);
    setClass("settings-footer", "background-color", scaledColor(m_config.themeSettingsPanel, 1.05f, 1.0f));
    setClass("update-dialog", "background-color", panel);

    setClass("card-subtitle", "color", muted);
    setClass("stat-section-title", "color", muted);
    setClass("label", "color", muted);
    setClass("metric-label", "color", muted);
    setClass("muted", "color", muted);
    setClass("badge", "color", muted);
    setClass("running-indicator", "color", muted);
    setClass("roster-footer", "color", muted);
    setClass("rank-table-row", "color", muted);
    setClass("debug-label", "color", muted);
    setClass("match-mode", "color", muted);
    setClass("dim", "color", dim);
    setClass("lobby-rank-matches", "color", dim);
    setClass("dashboard-edit-zone", "color", dim);
    setClass("setting-help", "color", dim);
    setClass("match-time", "color", dim);
    setClass("lobby-rank-header", "color", muted);
    setClass("lobby-rank-header", "background-color", scaledColor(m_config.themeBg, 0.70f, 0.80f));
    setClass("accent", "color", accent);
    setClass("setting-title", "color", accent);
    setSelector(".badge.accent", "color", accent);
    setSelector(".overlay-edit", "border-color", accent);
    setSelector(".dashboard-widget.dragging", "border-color", accent);
    setSelector(".settings-nav button.active", "border-color", accent);
    setSelector(".settings-nav button.active", "background-color", accentDark);
    setSelector("button.primary, .button.primary", "background-color", accentMid);
    setSelector("button.primary, .button.primary", "border-color", accent);
    setSelector("input.text:focus, input.password:focus, select:focus, select:checked", "border-color", accent);
    setSelector("input.checkbox:checked", "border-color", accent);
    setSelector("input.checkbox:checked", "background-color", accentDark);
    setSelector("input.range sliderprogress", "background-color", accentMid);
    setSelector("input.range sliderbar", "border-color", accent);
    setSelector("scrollbarvertical sliderbar:active, scrollbarhorizontal sliderbar:active", "background-color", accent);
    setSelector("select selectbox option:checked", "background-color", accentDark);

    setClass("win", "color", win);
    setSelector(".badge.win", "color", win);
    setClass("loss", "color", loss);
    setSelector(".badge.loss", "color", loss);
    setClass("match-win", "color", win);
    setClass("match-loss", "color", loss);
    // `.match-mode` and `.match-time` carry their own muted/dim colors, so the
    // win/loss result color has to be pushed onto them explicitly for the whole
    // history line to read as one result.
    setSelector(".match-win .match-mode, .match-win .match-time", "color", win);
    setSelector(".match-loss .match-mode, .match-loss .match-time", "color", loss);

    // Platform identity colors are brand colors, not theme colors. They are
    // applied last because the passes above overwrite `.badge` and `.label`.
    for (const auto& swatch : kPlatformPalette) {
        const std::string selector = std::string(".") + swatch.className;
        const std::string color = swatch.color;
        setSelector(selector.c_str(), "color", color);
        setSelector((".badge" + selector).c_str(), "border-color", color + "66");
        setSelector((".badge" + selector).c_str(), "background-color", color + "1f");
    }

    setClass("graph-line", "background-color", graph);
    setClass("graph-polyline", "color", graph);
    setClass("graph-point", "background-color", graph);
    setClass("graph-point-halo", "background-color", graph);
    setSelector(".graph-line.baseline", "background-color", baseline);
    setSelector(".graph-point.win, .graph-point-halo.win", "background-color", win);
    setSelector(".graph-point.loss, .graph-point-halo.loss", "background-color", loss);
    setSelector(".graph-point.neutral, .graph-point-halo.neutral", "background-color", muted);
    setSelector(".graph-point.estimated", "background-color", "transparent");
    setSelector(".graph-point.estimated", "border-color", text);
}

void RmlUiController::RebuildVisibleUi(bool force) {
    if (m_rebuildingUi) return;
    if (m_drag.kind != DragKind::None) return;
    // RmlUi only emits `click` when the element that received `mousedown` is
    // still the hovered element on `mouseup`. Live telemetry used to replace the
    // overlay DOM between those two events, which silently swallowed every
    // click on overlay controls (toolbox, widget remove buttons).
    if (m_pointerPressed && !force) return;
    // Keep this fingerprint complete for any configuration value that changes
    // rendered RML or conditional control state. Live-edited text/theme values
    // are intentionally excluded so an `input` event never replaces the
    // focused text control on the following frame. Their committed `change`
    // handlers force a rebuild, and theme colors have a targeted style path.
    std::ostringstream fp;
    fp << std::setprecision(9)
       << m_config.require_rl_focus << '|' << m_config.second_monitor_mode << '|'
       << m_config.second_monitor_show_roster << '|' << m_config.second_monitor_show_session << '|'
       << m_config.show_match_summary << '|' << m_config.show_running_indicator << '|'
       << m_config.reset_session_on_close << '|' << m_config.run_on_startup << '|'
       << m_config.check_for_updates << '|' << m_config.enable_auto_updates << '|'
       << m_config.last_primary_id << '|';
    for (const auto& id : m_config.known_primary_ids)
        fp << id << ',';
    fp << '|'
       << m_config.show_session_record << '|'
       << m_config.show_session_goals << '|' << m_config.show_session_saves << '|'
       << m_config.show_session_demos << '|' << m_config.show_session_assists << '|'
       << m_config.show_session_goal_participation << '|' << m_config.show_session_mmr_change << '|'
       << m_config.show_session_boost << '|' << m_config.use_rank_icons << '|'
       << m_config.show_lobby_ranks_overlay << '|' << m_config.show_lobby_rank_1v1 << '|'
       << m_config.show_lobby_rank_2v2 << '|' << m_config.show_lobby_rank_3v3 << '|'
       << m_config.show_lobby_rank_casual << '|' << m_config.show_lobby_rank_tourny << '|'
       << m_config.show_lobby_rank_hoops << '|' << m_config.show_lobby_rank_rumble << '|'
       << m_config.show_lobby_rank_dropshot << '|' << m_config.show_lobby_rank_snowday << '|'
       << m_config.show_lobby_rank_heatseeker << '|' << m_config.show_account_wins_overlay << '|'
       << m_config.show_demo_tracker_overlay << '|' << m_config.show_previous_games_summary << '|'
       << m_config.previous_games_limit << '|' << m_config.show_streaks_stats << '|'
       << m_config.show_longest_loss_streak << '|' << m_config.show_gamemode_breakdown << '|'
       << m_config.show_gamemode_record_1v1 << '|' << m_config.show_gamemode_record_2v2 << '|'
       << m_config.show_gamemode_record_3v3 << '|' << m_config.gamemode_breakdown_scope << '|'
       << m_config.mmr_category << '|' << m_config.auto_switch_mmr_category << '|'
       << m_config.graph_follow_current_playlist << '|' << m_config.graph_mmr_category << '|'
       << m_config.show_extra_playlists << '|' << m_config.ui_scale << '|'
       << m_config.imperial_units << '|' << m_config.crossbar_display_mode << '|'
       << m_config.use_roman_numerals << '|' << m_config.auto_upload_replays << '|'
       << m_config.ballchasing_upload_notice_accepted << '|' << m_config.ballchasing_visibility << '|'
       << m_config.auto_save_replays << '|' << m_config.discord_rpc_enabled << '|'
       << m_config.enable_mmr_tracking << '|' << m_config.crash_reports_enabled << '|'
       << m_config.check_stats_api_config_on_startup << '|' << m_config.debug_logging << '|'
       << m_config.key_overlay << '|' << m_config.key_cycle << '|' << m_config.key_expand << '|'
       << m_config.key_session << '|' << m_config.key_menu << '|' << m_config.key_save_replay << '|'
       << m_config.key_graph_pan_left << '|' << m_config.key_graph_pan_right << '|'
       << m_config.gamepad_overlay << '|' << m_config.gamepad_overlay_raw << '|' << m_config.gamepad_overlay_raw_button << '|'
       << m_config.gamepad_cycle << '|' << m_config.gamepad_cycle_raw << '|' << m_config.gamepad_cycle_raw_button << '|'
       << m_config.gamepad_expand << '|' << m_config.gamepad_expand_raw << '|' << m_config.gamepad_expand_raw_button << '|'
       << m_config.gamepad_session << '|' << m_config.gamepad_session_raw << '|' << m_config.gamepad_session_raw_button << '|'
       << m_config.gamepad_menu << '|' << m_config.gamepad_menu_raw << '|' << m_config.gamepad_menu_raw_button << '|'
       << m_config.gamepad_graph_pan_left << '|' << m_config.gamepad_graph_pan_left_raw << '|' << m_config.gamepad_graph_pan_left_raw_button << '|'
       << m_config.gamepad_graph_pan_right << '|' << m_config.gamepad_graph_pan_right_raw << '|' << m_config.gamepad_graph_pan_right_raw_button << '|';

    // Settings should not be rebuilt just because live telemetry/history or
    // persisted overlay geometry changed. Keep a settings-specific base before
    // layout state is appended to the general render fingerprint.
    const std::string settingsConfigFingerprint = fp.str();

    fp << m_config.overlay_layout.version << '|' << m_config.overlay_layout.toolboxOpen << '|';
    for (const auto& container : m_config.overlay_layout.containers) {
        fp << container.id << ':' << container.x << ',' << container.y << ',' << container.w << ',' << container.h << '[';
        for (auto widget : container.widgets)
            fp << static_cast<int>(widget) << ',';
        fp << "];";
    }
    fp << '|' << m_config.dashboard_layout.version << '|' << m_config.dashboard_layout.leftColumnWeight << '|';
    for (const auto& widget : m_config.dashboard_layout.widgets) {
        fp << static_cast<int>(widget.id) << ',' << static_cast<int>(widget.zone) << ',' << widget.order << ','
           << widget.height << ',' << widget.collapsed << ';';
    }
    uint64_t dbStatsVersion = 0;
    if (m_state) {
        dbStatsVersion = m_state->ui.dbStatsVersion.load(std::memory_order_relaxed);
        fp << '|' << m_state->ui.graphOffset.load(std::memory_order_relaxed);
        fp << '|' << m_state->ui.updateChecked.load(std::memory_order_relaxed)
           << '|' << m_state->ui.updateAvailable.load(std::memory_order_relaxed)
           << '|' << m_state->ui.updateDownloading.load(std::memory_order_relaxed)
           << '|' << m_state->ui.updateDownloadFailed.load(std::memory_order_relaxed)
           << '|' << m_state->ui.controllerConnected.load(std::memory_order_relaxed)
           << '|' << m_state->ui.controllerIsGameController.load(std::memory_order_relaxed)
           << '|' << m_state->ui.lastKeyboardKeyPressed.load(std::memory_order_relaxed)
           << '|' << m_state->ui.lastControllerButtonPressed.load(std::memory_order_relaxed)
           << '|' << m_state->ui.lastRawControllerButtonPressed.load(std::memory_order_relaxed);
        if (m_state->ui.updateAvailable.load(std::memory_order_relaxed)) {
            std::lock_guard lock(m_state->ui.updateMutex);
            fp << '|' << m_state->ui.updateAvailableVersion;
        }
        if (m_state->ui.controllerConnected.load(std::memory_order_relaxed)) {
            std::lock_guard lock(m_state->ui.controllerDebugMutex);
            fp << '|' << m_state->ui.controllerDebugName;
        }
        fp << '|' << m_state->ui.statsApiChecked.load(std::memory_order_relaxed);
        {
            std::lock_guard lock(m_state->ui.statsApiMutex);
            const auto& result = m_state->ui.statsApiResult;
            fp << '|' << static_cast<int>(result.status)
               << '|' << result.path
               << '|' << result.message
               << '|' << result.expectedPort
               << '|' << result.actualPort
               << '|' << result.packetSendRate
               << '|' << result.rlRunning;
        }
    }
    const std::string fingerprint = fp.str();

    const bool showMenu = m_state && m_state->ui.showMenu.load();
    const bool showOverlay = m_state && m_state->ui.showOverlay.load();
    const bool showSession = m_state && m_state->ui.showSessionView.load();
    bool showSummary = m_state && m_state->ui.showMatchSummary.load();
    if (showSummary && m_state && (SteadyNowMs() - m_state->ui.matchSummaryStartMs.load()) >= 30000) {
        m_state->ui.showMatchSummary.store(false);
        showSummary = false;
    }
    const bool dashboardEdit = m_state && m_state->ui.dashboardLayoutEditMode.load();

    // Build a narrowly scoped Settings fingerprint. The old implementation had
    // persistent controls; rebuilding the whole Settings DOM on every game
    // version invalidates the hovered element and produces cursor flicker (and
    // destroys focus while typing). Only include state that can actually alter
    // Settings content.
    std::ostringstream settingsFp;
    settingsFp << settingsConfigFingerprint
               << "|page=" << static_cast<int>(m_settingsPage)
               << "|edit=" << dashboardEdit
               << "|bind=" << static_cast<int>(m_bindCaptureTarget)
               << "|showToken=" << m_showBallchasingToken
               << "|confirmReplay=" << m_confirmReplayUploads
               << "|confirmDelete=" << m_confirmDeleteHistory
               << "|editColor=" << m_editColorKey
               << "|statsPathError=" << m_statsApiPathError;
    if (m_state) {
        settingsFp << "|updateChecked=" << m_state->ui.updateChecked.load(std::memory_order_relaxed)
                   << "|updateAvailable=" << m_state->ui.updateAvailable.load(std::memory_order_relaxed)
                   << "|updateDownloading=" << m_state->ui.updateDownloading.load(std::memory_order_relaxed)
                   << "|updateFailed=" << m_state->ui.updateDownloadFailed.load(std::memory_order_relaxed)
                   << "|controllerConnected=" << m_state->ui.controllerConnected.load(std::memory_order_relaxed)
                   << "|controllerMapped=" << m_state->ui.controllerIsGameController.load(std::memory_order_relaxed)
                   << "|lastKeyboard=" << m_state->ui.lastKeyboardKeyPressed.load(std::memory_order_relaxed)
                   << "|lastController=" << m_state->ui.lastControllerButtonPressed.load(std::memory_order_relaxed)
                   << "|lastRawController=" << m_state->ui.lastRawControllerButtonPressed.load(std::memory_order_relaxed)
                   << "|statsApiChecked=" << m_state->ui.statsApiChecked.load(std::memory_order_relaxed);
        if (m_state->ui.updateAvailable.load(std::memory_order_relaxed)) {
            std::lock_guard lock(m_state->ui.updateMutex);
            settingsFp << "|updateVersion=" << m_state->ui.updateAvailableVersion;
        }
        if (m_state->ui.controllerConnected.load(std::memory_order_relaxed)) {
            std::lock_guard lock(m_state->ui.controllerDebugMutex);
            settingsFp << "|controllerName=" << m_state->ui.controllerDebugName;
        }
        {
            std::lock_guard lock(m_state->ui.statsApiMutex);
            const auto& result = m_state->ui.statsApiResult;
            settingsFp << "|statsApi=" << static_cast<int>(result.status)
                       << ',' << result.path << ',' << result.message << ','
                       << result.expectedPort << ',' << result.actualPort << ','
                       << result.packetSendRate << ',' << result.rlRunning;
        }
    }

    // General's identity picker and Ranks' local rank table are the only
    // settings surfaces that depend on live roster data. Fingerprint just the
    // identity/rank fields, not goals/saves/etc., so ordinary telemetry updates
    // cannot replace the Settings DOM under the mouse.
    if (m_settingsPage == SettingsPage::General || m_settingsPage == SettingsPage::Ranks) {
        std::vector<std::string> rosterIds;
        rosterIds.reserve(m_snap.roster.size());
        for (const auto& [id, _] : m_snap.roster)
            rosterIds.push_back(id);
        std::sort(rosterIds.begin(), rosterIds.end());
        for (const auto& id : rosterIds) {
            const auto& player = m_snap.roster.at(id);
            settingsFp << "|player=" << id << ',' << player.name;
        }
        if (m_settingsPage == SettingsPage::Ranks) {
            const std::string effectiveId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
            if (auto it = m_snap.roster.find(effectiveId); it != m_snap.roster.end()) {
                const auto& player = it->second;
                settingsFp << "|me=" << player.fetched << ',' << player.mmr << ',' << player.rankTier;
                for (auto category : MmrCategories(false, m_config.show_extra_playlists)) {
                    const std::string key = MmrCategoryToString(category);
                    const auto mmrIt = player.playlists.find(key);
                    const auto tierIt = player.playlistTiers.find(key);
                    const auto matchesIt = player.playlistMatches.find(key);
                    settingsFp << '|' << key << ':'
                               << (mmrIt == player.playlists.end() ? 0 : mmrIt->second) << ':'
                               << (tierIt == player.playlistTiers.end() ? std::string{} : tierIt->second) << ':'
                               << (matchesIt == player.playlistMatches.end() ? 0 : matchesIt->second);
                }
            }
        }
    }
    const std::string settingsFingerprint = settingsFp.str();
    const bool settingsDirty = force || (showMenu && !m_lastShowMenu) || m_lastSettingsFingerprint != settingsFingerprint;

    const bool showGraphView = m_state && m_state->ui.showGraphView.load();
    const bool h2hExpanded = m_state && m_state->ui.h2hExpanded.load();
    const bool showLifetimeGraph = m_state && m_state->history.showLifetimeGraph.load();
    const int graphOffset = m_state ? m_state->ui.graphOffset.load() : 0;
    const MmrCategory rosterCategory = m_state ? m_state->ui.rosterMmrCategory.load() : MmrCategory::Best;
    const MmrCategory graphCategory = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::Best;
    const bool dirtyState = force || m_lastRenderedGameVersion != m_lastGameVersion || m_lastRenderedHistoryVersion != m_lastHistoryVersion ||
                            m_lastRenderedDbStatsVersion != dbStatsVersion ||
                            m_lastShowMenu != showMenu || m_lastShowOverlay != showOverlay || m_lastShowSessionView != showSession ||
                            m_lastShowMatchSummary != showSummary || m_lastSecondMonitor != m_config.second_monitor_mode ||
                            m_lastDashboardEditMode != dashboardEdit || m_lastShowGraphView != showGraphView || m_lastH2hExpanded != h2hExpanded ||
                            m_lastShowLifetimeGraph != showLifetimeGraph || m_lastGraphOffset != graphOffset ||
                            m_lastRosterMmrCategory != rosterCategory || m_lastGraphMmrCategory != graphCategory ||
                            m_lastConfigFingerprint != fingerprint;

    // Settings remains stable because its DOM only rebuilds when its own input
    // changes. Apply the same rule to a live overlay while the cursor is on one
    // of its interactive cards. We leave the render fingerprint untouched, so
    // telemetry catches up immediately when the cursor leaves.
    const bool deferOverlayRefresh = dirtyState && !force && showMenu && !m_config.second_monitor_mode &&
                                     PointerOverInteractiveOverlay();
    if (!dirtyState && !settingsDirty) {
        if (m_statusUntilMs && SteadyNowMs() >= m_statusUntilMs) {
            m_statusUntilMs = 0;
            RebuildToast();
        }
        return;
    }

    if (dirtyState && !deferOverlayRefresh) {
        if (m_config.second_monitor_mode) {
            SetRootRml("overlay-root", "");
            RebuildDashboard();
        } else {
            SetRootRml("dashboard-root", "");
            RebuildOverlay();
        }
        RebuildToast();

        m_lastRenderedGameVersion = m_lastGameVersion;
        m_lastRenderedHistoryVersion = m_lastHistoryVersion;
        m_lastRenderedDbStatsVersion = dbStatsVersion;
        m_lastShowOverlay = showOverlay;
        m_lastShowSessionView = showSession;
        m_lastShowMatchSummary = showSummary;
        m_lastSecondMonitor = m_config.second_monitor_mode;
        m_lastDashboardEditMode = dashboardEdit;
        m_lastShowGraphView = showGraphView;
        m_lastH2hExpanded = h2hExpanded;
        m_lastShowLifetimeGraph = showLifetimeGraph;
        m_lastGraphOffset = graphOffset;
        m_lastRosterMmrCategory = rosterCategory;
        m_lastGraphMmrCategory = graphCategory;
        m_lastConfigFingerprint = fingerprint;
    }

    if (showMenu) {
        if (settingsDirty) RebuildSettings();
    } else if (m_lastShowMenu) {
        SetRootRml("settings-root", "");
    }

    m_lastShowMenu = showMenu;
    m_lastSettingsFingerprint = settingsFingerprint;
}

std::string RmlUiController::Escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);

    const auto appendAscii = [&out](unsigned char c) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '\"':
            out += "&quot;";
            break;
        case '\'':
            out += "&#39;";
            break;
        default:
            out.push_back(static_cast<char>(c));
            break;
        }
    };
    const auto isContinuation = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };
    const auto appendReplacement = [&out]() { out += "\xEF\xBF\xBD"; };

    for (size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80u) {
            appendAscii(lead);
            ++i;
            continue;
        }

        size_t length = 0;
        bool valid = false;
        if (lead >= 0xC2u && lead <= 0xDFu) {
            length = 2;
            valid = i + length <= text.size() &&
                    isContinuation(static_cast<unsigned char>(text[i + 1]));
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            length = 3;
            if (i + length <= text.size()) {
                const auto b1 = static_cast<unsigned char>(text[i + 1]);
                const auto b2 = static_cast<unsigned char>(text[i + 2]);
                const bool secondValid =
                    (lead == 0xE0u) ? (b1 >= 0xA0u && b1 <= 0xBFu) : (lead == 0xEDu) ? (b1 >= 0x80u && b1 <= 0x9Fu)
                                                                                     : isContinuation(b1);
                valid = secondValid && isContinuation(b2);
            }
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            length = 4;
            if (i + length <= text.size()) {
                const auto b1 = static_cast<unsigned char>(text[i + 1]);
                const auto b2 = static_cast<unsigned char>(text[i + 2]);
                const auto b3 = static_cast<unsigned char>(text[i + 3]);
                const bool secondValid =
                    (lead == 0xF0u) ? (b1 >= 0x90u && b1 <= 0xBFu) : (lead == 0xF4u) ? (b1 >= 0x80u && b1 <= 0x8Fu)
                                                                                     : isContinuation(b1);
                valid = secondValid && isContinuation(b2) && isContinuation(b3);
            }
        }

        if (!valid) {
            appendReplacement();
            ++i;
            continue;
        }

        out.append(text.substr(i, length));
        i += length;
    }
    return out;
}

std::string RmlUiController::CssColor(const ColorRGBA& color) {
    auto byte = [](float v) {
        if (!std::isfinite(v)) return 0;
        return std::clamp(static_cast<int>(std::lround(v * 255.0f)), 0, 255);
    };
    char buffer[10]{};
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X%02X", byte(color.r), byte(color.g), byte(color.b), byte(color.a));
    return buffer;
}

std::string RmlUiController::BoolAttr(bool value) {
    return value ? "true" : "false";
}
std::string RmlUiController::Checked(bool value) {
    return value ? " checked='checked'" : "";
}
std::string RmlUiController::Selected(bool value) {
    return value ? " selected='selected'" : "";
}

std::string RmlUiController::FormatNumber(float value, int precision) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string RmlUiController::FormatRecord(int wins, int losses) {
    return std::to_string(wins) + "-" + std::to_string(losses);
}

std::string RmlUiController::FormatClock(int64_t unixSeconds) {
    if (unixSeconds <= 0) return "--";
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t diff = now - unixSeconds;
    if (diff < 0) diff = 0;
    if (diff < 60) return "now";
    if (diff < 3600) return std::to_string(diff / 60) + "m ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

std::string RmlUiController::FormatDemoKd(int demos, int demoed) {
    if (demoed <= 0) return demos > 0 ? FormatNumber(static_cast<float>(demos), 1) : "0.0";
    return FormatNumber(static_cast<float>(demos) / static_cast<float>(demoed), 2);
}

bool RmlUiController::ValidateStatsApiPath(std::string input, std::string& normalized, std::string& error) {
    const size_t first = input.find_first_not_of(" \t\r\n");
    const size_t last = input.find_last_not_of(" \t\r\n");
    normalized = (first == std::string::npos || last == std::string::npos)
                     ? std::string{}
                     : input.substr(first, last - first + 1);
    error.clear();
    if (normalized.empty()) return true;

    const std::filesystem::path path(normalized);
    if (ToLower(path.filename().string()) != "defaultstatsapi.ini") {
        error = "Filename must be exactly DefaultStatsAPI.ini";
        return false;
    }
    if (!std::filesystem::exists(path)) {
        error = "File does not exist.";
        return false;
    }
    if (!std::filesystem::exists(path.parent_path())) {
        error = "Parent directory does not exist.";
        return false;
    }
    return true;
}

const char* RmlUiController::SettingsPageName(SettingsPage page) {
    switch (page) {
    case SettingsPage::General:
        return "General";
    case SettingsPage::Cards:
        return "Cards";
    case SettingsPage::Ranks:
        return "Ranks";
    case SettingsPage::Shortcuts:
        return "Shortcuts";
    case SettingsPage::Appearance:
        return "Appearance";
    case SettingsPage::Integrations:
        return "Services & API";
    case SettingsPage::Data:
        return "Data";
    case SettingsPage::Troubleshooting:
        return "Troubleshooting";
    }
    return "General";
}

const char* RmlUiController::ZoneName(DashboardLayout::Zone zone) {
    switch (zone) {
    case DashboardLayout::Zone::Left:
        return "left";
    case DashboardLayout::Zone::Right:
        return "right";
    case DashboardLayout::Zone::Bottom:
        return "bottom";
    case DashboardLayout::Zone::Hidden:
        return "hidden";
    case DashboardLayout::Zone::Top:
        return "top";
    }
    return "left";
}

std::string RmlUiController::WidgetDomId(DashboardLayout::WidgetId id) {
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
    }
    return "live-roster";
}

std::string RmlUiController::PlaylistImageForName(const std::string& playlist) {
    const int index = PlaylistResourceIndex(playlist);
    return index >= 0 ? "res://images/Playlists/" + std::to_string(index) + ".png" : "";
}

std::string RmlUiController::RenderRankBadge(const std::string& tier, bool fetched, const std::string& tooltip) const {
    const int tierIndex = fetched ? TierResourceIndex(tier) : 23;
    const int division = fetched ? DivisionLevel(tier) : 0;
    const int filledDivisionIndex = DivisionColorResourceIndex(tier);
    std::ostringstream out;
    out << "<span class='rank-badge tooltip-host'><img class='rank-icon' src='res://images/Tiers/" << tierIndex << ".png'/>";
    if (division > 0) {
        out << "<span class='division-stack'>";
        for (int i = 4; i >= 1; --i) {
            const int resource = i <= division ? filledDivisionIndex : 0;
            out << "<img class='division-pill' src='res://images/Divisions/" << resource << ".png'/>";
        }
        out << "</span>";
    }
    if (!tooltip.empty()) out << "<span class='tooltip-bubble'>" << Escape(tooltip) << "</span>";
    out << "</span>";
    return out.str();
}

std::string RmlUiController::RenderPlayerRoster(int team, const char* label) {
    std::vector<const PlayerData*> players;
    const std::string category = MmrCategoryToString(m_state ? m_state->ui.rosterMmrCategory.load() : MmrCategory::Best);
    for (const auto& [_, player] : m_snap.roster)
        if (player.team == team) players.push_back(&player);
    auto mmrFor = [&](const PlayerData& p) {
        if (category == "best") return p.mmr;
        if (auto it = p.playlists.find(category); it != p.playlists.end()) return it->second;
        return 0;
    };
    auto sortMmrFor = [&](const PlayerData& p) {
        const int selected = mmrFor(p);
        return selected > 0 ? selected : p.mmr;
    };
    std::sort(players.begin(), players.end(), [&](const PlayerData* a, const PlayerData* b) {
        const int ma = sortMmrFor(*a), mb = sortMmrFor(*b);
        if (ma != mb) return ma > mb;
        return a->name < b->name;
    });

    std::ostringstream out;
    const char* cls = team == 0 ? "blue" : "orange";
    out << "<div class='team-block'><div class='team-header'><div class='team-line " << cls << "'></div><div class='grow value'>" << label
        << "</div><span class='badge " << cls << "'>" << players.size() << "</span></div>";
    if (players.empty()) out << "<div class='muted'>Waiting for players...</div>";
    for (const auto* p : players) {
        const bool self = !m_snap.myPrimaryId.empty() && p->primaryId == m_snap.myPrimaryId;
        const int mmr = mmrFor(*p);
        std::string tier = p->rankTier;
        std::string rankSource = category;
        if (category != "best") {
            if (auto it = p->playlistTiers.find(category); it != p->playlistTiers.end())
                tier = it->second;
            else
                tier = "Unranked";
        } else if (mmr > 0) {
            // `best` is a presentation bucket, not a Rocket League playlist.
            // Preserve the previous roster behavior by showing which real playlist
            // supplied the best rank/MMR instead of presenting BEST as a mode.
            const auto matchesMmr = [&](const char* key) {
                auto it = p->playlists.find(key);
                return it != p->playlists.end() && it->second == mmr;
            };
            for (const char* key : {"2v2", "3v3", "1v1"}) {
                if (matchesMmr(key)) {
                    rankSource = key;
                    break;
                }
            }
            if (rankSource == "best" && m_config.show_extra_playlists) {
                for (const char* key : {"hoops", "rumble", "dropshot", "snowday", "heatseeker"}) {
                    if (matchesMmr(key)) {
                        rankSource = key;
                        break;
                    }
                }
            }
        }
        if (category != "casual" && (tier.empty() || tier == "Unranked") && mmr > 0) {
            tier = MMRFetcher::GetRankTierForPlaylistMmr(category == "best" ? rankSource : category, mmr);
        }
        std::string platform;
        if (const auto pos = p->primaryId.find('|'); pos != std::string::npos) platform = p->primaryId.substr(0, pos);
        const auto color = Format::RankColor(tier);
        const auto matchesIt = p->playlistMatches.find(category == "best" ? "best" : rankSource);
        const int matchCount = matchesIt != p->playlistMatches.end() ? matchesIt->second : 0;

        out << "<div class='player-row" << (self ? " self" : "") << "'>";
        if (m_config.use_rank_icons) {
            const std::string rankTooltip = p->fetched || mmr > 0 ? Format::RankTier(tier, m_config.use_roman_numerals) : "Fetching rank...";
            out << "<div class='player-crest'>" << RenderRankBadge(tier, p->fetched || mmr > 0, rankTooltip) << "</div>";
        }

        const std::string trackerUrl = TrackerUrlForPlayer(*p);
        out << "<div class='player-identity'><div class='player-headline'><span class='value player-name-text"
            << (trackerUrl.empty() ? "" : " player-link") << "'";
        if (!trackerUrl.empty()) out << " data-action='open-player-tracker' data-url='" << Escape(trackerUrl) << "'";
        out << ">" << Escape(p->name) << "</span>";
        if (!platform.empty()) {
            const PlatformKind platformKind = PlatformKindFor(platform);
            out << "<span class='badge " << PlatformClass(platformKind) << "'>"
                << Escape(PlatformBadgeLabel(platformKind, platform)) << "</span>";
        }
        out << "</div><div class='player-chips'>";

        // MMR chip. Without rank crests it also has to name the tier, and when
        // the roster shows the player's best rank it names the playlist that
        // produced it.
        out << "<span class='chip chip-mmr' style='color:" << CssColor(color) << "'>";
        if (!m_config.use_rank_icons)
            out << Escape(mmr > 0 ? Format::RankTier(tier, m_config.use_roman_numerals) : (p->fetched ? "Unranked" : "Fetching")) << ' ';
        else if (category == "best" && rankSource != "best")
            out << Escape(MmrLabel(StringToMmrCategory(rankSource))) << ' ';
        out << (mmr > 0 ? std::to_string(mmr) : (p->fetched ? "-" : "...")) << "</span>";

        if (mmr > 0 && matchCount > 0) out << "<span class='chip'>" << matchCount << (matchCount == 1 ? " match" : " matches") << "</span>";
        if (m_config.show_account_wins_overlay && p->totalWins >= 0) out << "<span class='chip'>" << p->totalWins << " wins</span>";
        if (p->goals || p->saves || p->shots || p->assists || p->demos) {
            out << "<span class='chip'>G" << p->goals << " S" << p->saves << " A" << p->assists
                << " Sh" << p->shots << " D" << p->demos << "</span>";
        }
        out << "</div></div>";

        // Trailing status column, one state per player: yourself, a player with
        // shared history, or someone new. A with/vs record against yourself is
        // meaningless, so YOU stands alone.
        const int withGames = p->lifetimeWinsWith + p->lifetimeLossesWith;
        const int againstGames = p->lifetimeWinsAgainst + p->lifetimeLossesAgainst;
        const bool hasEncounterRecord = p->hasLifetimeData && (withGames > 0 || againstGames > 0);
        out << "<div class='player-flags'>";
        if (self) {
            out << "<span class='badge accent'>YOU</span>";
        } else if (hasEncounterRecord) {
            if (withGames) out << "<div class='label'>with " << p->lifetimeWinsWith << '-' << p->lifetimeLossesWith << "</div>";
            if (againstGames) out << "<div class='label'>vs " << p->lifetimeWinsAgainst << '-' << p->lifetimeLossesAgainst << "</div>";
        } else {
            out << "<span class='badge'>NEW</span>";
        }
        out << "</div></div>";
    }
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderLiveMatchStats() {
    const auto& s = m_snap.currentMatch;
    std::vector<std::pair<std::string, std::string>> play = {
        {"Saves", Format::PairCount(s.saves, s.savesSelf)},
        {"Shots", Format::PairCount(s.shots, s.shotsSelf)},
        {"Assists", Format::PairCount(s.assists, s.assistsSelf)},
        {"Demos", Format::PairCount(s.demos, s.demosSelf)},
        {"Crossbars", Format::PairCount(s.crossbars, s.crossbarsSelf)}};
    if (s.demoedSelf > 0) play.insert(play.end() - 1, {"Demoed", std::to_string(s.demoedSelf)});
    std::vector<std::pair<std::string, std::string>> fun = {
        {"Max goal speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Max ball speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Hardest crossbar", m_config.crossbar_display_mode == "speed"
                                 ? Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units)
                                 : Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", false)},
        {"Fastest goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)}};
    if (s.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});

    auto list = [](const auto& rows) {
        std::ostringstream html;
        html << "<div class='metric-list'>";
        for (const auto& [label, value] : rows) {
            html << "<div class='metric-row'><div class='metric-label'>" << label
                 << "</div><div class='metric-value mono'>" << value << "</div></div>";
        }
        html << "</div>";
        return html.str();
    };
    return "<div class='stat-section-title'>PLAY</div>" + list(play) +
           "<div class='stat-section-title' style='margin-top:7dp'>FUN</div>" + list(fun);
}
std::string RmlUiController::RenderStreaksStats() {
    int cw = 0, cl = 0, lw = 0, ll = 0;
    if (m_state) {
        std::lock_guard lock(m_state->ui.dbStatsMutex);
        cw = m_state->ui.cachedDbStats.currentWins;
        cl = m_state->ui.cachedDbStats.currentLosses;
        lw = m_state->ui.cachedDbStats.longestWins;
        ll = m_state->ui.cachedDbStats.longestLosses;
    }
    std::ostringstream out;
    out << "<div class='metric-list'><div class='metric-row'><div class='metric-label'>Current streak</div><div class='metric-value "
        << (cw ? "win" : cl ? "loss"
                            : "")
        << "'>";
    if (cw)
        out << '+' << cw;
    else if (cl)
        out << '-' << cl;
    else
        out << "0";
    out << "</div></div><div class='metric-row'><div class='metric-label'>Longest win</div><div class='metric-value'>" << lw << "</div></div>";
    if (m_config.show_longest_loss_streak) {
        out << "<div class='metric-row'><div class='metric-label'>Longest loss</div><div class='metric-value loss'>" << ll << "</div></div>";
    }
    out << "</div>";
    return out.str();
}
std::string RmlUiController::RenderGamemodeBreakdown(GamemodeBreakdownScope scope) {
    std::ostringstream out;
    const std::array<std::pair<const char*, bool>, 3> modes{{{"1v1", m_config.show_gamemode_record_1v1}, {"2v2", m_config.show_gamemode_record_2v2}, {"3v3", m_config.show_gamemode_record_3v3}}};
    bool any = false;
    out << "<div class='col gap-xs gamemode-breakdown'>";
    for (const auto& [mode, enabled] : modes) {
        if (!enabled) continue;
        any = true;
        int wins = 0, losses = 0, total = 0;
        if (scope == GamemodeBreakdownScope::CurrentSession) {
            if (auto it = m_snap.sessionGamemodes.find(mode); it != m_snap.sessionGamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        } else if (m_state) {
            std::lock_guard lock(m_state->ui.dbStatsMutex);
            if (auto it = m_state->ui.cachedDbStats.gamemodes.find(mode); it != m_state->ui.cachedDbStats.gamemodes.end()) {
                wins = it->second.wins;
                losses = it->second.losses;
                total = it->second.total;
            }
        }
        const float pct = total > 0 ? (100.0f * wins / static_cast<float>(total)) : 0.0f;
        out << "<div class='row'><div class='grow value'>" << mode << "</div><span class='mono'>" << wins << '-' << losses << "</span><span class='label' style='width:70dp;text-align:right'>";
        if (total)
            out << FormatNumber(pct, 1) << "%";
        else
            out << '-';
        out << "</span><span class='label' style='width:45dp;text-align:right'>" << total << "</span></div>";
    }
    if (!any) out << "<div class='muted'>No gamemodes selected.</div>";
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSessionStats(bool compact, bool includeStreak) {
    const auto& stats = m_snap.sessionTotals;
    const int mmr = static_cast<int>(std::lround(stats.totalMmrChange));
    const float gp = stats.teamGoals > 0 ? 100.0f * static_cast<float>(stats.goalParticipations) / static_cast<float>(stats.teamGoals) : 0.0f;

    if (compact) {
        std::vector<std::pair<std::string, std::string>> rows;
        if (m_config.show_session_record) rows.push_back({"Record", FormatRecord(stats.wins, stats.losses)});
        if (m_config.show_session_goals) rows.push_back({"Goals", std::to_string(stats.goals)});
        if (m_config.show_session_saves) rows.push_back({"Saves", std::to_string(stats.saves)});
        if (m_config.show_session_assists) rows.push_back({"Assists", std::to_string(stats.assists)});
        if (m_config.show_session_demos) rows.push_back({"Demos", std::to_string(stats.demos)});
        if (m_config.show_session_boost) rows.push_back({"Boost", std::to_string(CalculateSessionBoostPickedUp(
                                                                      m_snap.sessionTotals, m_snap.currentMatch, m_snap.matchFinalized))});
        if (m_config.show_session_goal_participation) rows.push_back({"Goal participation", stats.teamGoals > 0 ? FormatNumber(gp, 0) + "%" : "--"});
        if (m_config.show_session_mmr_change) rows.push_back({"MMR", (mmr >= 0 ? "+" : "") + std::to_string(mmr)});
        if (includeStreak && m_config.show_streaks_stats) rows.push_back({"Session", std::to_string(stats.wins - stats.losses)});

        std::ostringstream out;
        out << "<div class='metric-list'>";
        for (const auto& [name, value] : rows) {
            std::string cls;
            if (name == "MMR") cls = mmr > 0 ? " win" : mmr < 0 ? " loss"
                                                                : "";
            out << "<div class='metric-row'><div class='metric-label'>" << name
                << "</div><div class='metric-value mono" << cls << "'>" << value << "</div></div>";
        }
        out << "</div>";
        return out.str();
    }

    auto renderRows = [](const char* title, const std::vector<std::pair<std::string, std::string>>& rows) {
        std::ostringstream html;
        html << "<div class='stat-section-title' style='margin-top:7dp'>" << title << "</div><div class='stat-grid cols-2 compact-stats'>";
        for (const auto& [name, value] : rows) {
            html << "<div class='stat-cell'><div class='label'>" << name << "</div><div class='value mono'>" << value << "</div></div>";
        }
        html << "</div>";
        return html.str();
    };

    std::vector<std::pair<std::string, std::string>> summary = {
        {"Record", FormatRecord(stats.wins, stats.losses)},
        {"Goals", std::to_string(stats.goals)},
        {"Goal participation", stats.teamGoals > 0 ? std::to_string(stats.goalParticipations) + " / " + std::to_string(stats.teamGoals) + " (" + FormatNumber(gp, 0) + "%)" : "--"},
        {"MMR change", (mmr >= 0 ? "+" : "") + std::to_string(mmr)}};
    if (includeStreak) summary.push_back({"Session", std::to_string(stats.wins - stats.losses)});

    std::vector<std::pair<std::string, std::string>> play = {
        {"Saves", Format::PairCount(stats.savesTotal, stats.saves)},
        {"Shots", Format::PairCount(stats.shotsTotal, stats.shots)},
        {"Assists", Format::PairCount(stats.assistsTotal, stats.assists)},
        {"Goal participation", stats.teamGoals > 0 ? std::to_string(stats.goalParticipations) + " / " + std::to_string(stats.teamGoals) + " (" + FormatNumber(gp, 0) + "%)" : "--"},
        {"Demos", Format::PairCount(stats.demosTotal, stats.demos)},
        {"Crossbars", Format::PairCount(stats.crossbarsTotal, stats.crossbars)}};

    std::vector<std::pair<std::string, std::string>> fun = {
        {"Max goal speed", Format::PairSpeed(stats.maxGoalSpeed, stats.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Max ball speed", Format::PairSpeed(stats.maxBallSpeed, stats.maxBallSpeedSelf, true, " kph", m_config.imperial_units)},
        {"Hardest crossbar hit", m_config.crossbar_display_mode == "speed"
                                     ? Format::PairSpeed(stats.maxImpactForce * 0.036f, stats.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units)
                                     : Format::PairSpeed(stats.maxImpactForce, stats.maxImpactForceSelf, true, "", false)},
        {"Fastest goal", Format::PairFastest(stats.fastestGoalTime, stats.fastestGoalTimeSelf)}};
    if (stats.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(stats.ownGoals, stats.ownGoalsSelf)});

    std::ostringstream out;
    out << "<div class='stat-grid cols-2 compact-stats'>";
    for (const auto& [name, value] : summary) {
        std::string cls;
        if (name == "MMR change") cls = mmr > 0 ? " win" : mmr < 0 ? " loss"
                                                                   : "";
        out << "<div class='stat-cell'><div class='label'>" << name << "</div><div class='value mono" << cls << "'>" << value << "</div></div>";
    }
    out << "</div><div class='setting-help' style='margin-top:8dp'>Left = lobby total · Right = you</div>";
    out << renderRows("PLAY", play) << renderRows("FUN", fun);
    return out.str();
}

std::string RmlUiController::RenderDemoTracker() {
    const auto session = CalculateSessionDemolitionCounts(m_snap.sessionTotals, m_snap.currentMatch, m_snap.matchFinalized);
    std::ostringstream out;
    out << "<div class='metric-pair'>"
        << "<div class='mini-metric'><div class='label'>GAME K/D</div><div class='value mono'>" << m_snap.currentMatch.demosSelf << '-' << m_snap.currentMatch.demoedSelf
        << " <span class='muted'>" << FormatDemoKd(m_snap.currentMatch.demosSelf, m_snap.currentMatch.demoedSelf) << "</span></div></div>"
        << "<div class='mini-metric'><div class='label'>SESSION K/D</div><div class='value mono'>" << session.demos << '-' << session.demoed
        << " <span class='muted'>" << FormatDemoKd(session.demos, session.demoed) << "</span></div></div></div>";
    return out.str();
}
std::string RmlUiController::RenderPreviousGames(bool includeHeading) {
    std::ostringstream out;
    const int configuredLimit = std::clamp(m_config.previous_games_limit, 10, kPreviousGamesMaxLimit);
    // The dashboard already labels the widget, so only the meta line is needed there.
    out << "<div class='row previous-games-header'>";
    if (includeHeading)
        out << "<div class='card-title grow'>PREVIOUS GAMES</div>";
    else
        out << "<div class='grow'></div>";
    out << "<div class='label'>last " << configuredLimit << " games</div></div>";
    if (!m_snap.recentSavedMatchesLoaded) {
        out << "<div class='muted'>Loading saved match history...</div>";
    } else if (m_snap.recentSavedMatches.empty()) {
        out << "<div class='muted'>No saved games in local history.</div>";
    } else {
        out << "<div class='match-row match-header'>"
            << "<div class='match-mode'>PLAYLIST</div><div class='match-score'>SCORE</div>"
            << "<div class='match-mmr'>MMR</div><div class='match-time'>TIME</div></div>"
            << "<div class='previous-games-list'>";
        const int limit = std::min<int>(configuredLimit, static_cast<int>(m_snap.recentSavedMatches.size()));
        for (int i = 0; i < limit; ++i) {
            const auto& match = m_snap.recentSavedMatches[static_cast<size_t>(i)];
            const std::string playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
            out << "<div class='match-row match-data " << (match.win ? "match-win" : "match-loss")
                << (i % 2 == 0 ? " match-even" : " match-odd") << "'>"
                << "<div class='match-mode'>" << Escape(playlist) << "</div>"
                << "<div class='match-score'>" << match.ourScore << '-' << match.theirScore << "</div>"
                << "<div class='match-mmr'>" << (match.pendingTrackerConfirmation ? "***" : (match.mmrEstimated && match.mmr > 0 ? "~" + std::to_string(match.mmr) : (match.mmr > 0 ? std::to_string(match.mmr) : "--"))) << "</div>"
                << "<div class='match-time'>" << FormatClock(match.endedAtUnix) << "</div></div>";
        }
        out << "</div>";
    }
    out << "<div class='previous-games-footer'><div>Current session: <span class='win'>W:" << m_snap.sessionTotals.wins
        << "</span> <span class='loss'>L:" << m_snap.sessionTotals.losses << "</span></div>"
        << "<div class='dim'>Stored locally</div></div>";
    return out.str();
}

std::string RmlUiController::RenderLobbyRanks() {
    struct Playlist {
        const char* key;
        const char* label;
        bool show;
    };
    const std::vector<Playlist> playlists = {
        {"1v1", "1v1", m_config.show_lobby_rank_1v1}, {"2v2", "2v2", m_config.show_lobby_rank_2v2}, {"3v3", "3v3", m_config.show_lobby_rank_3v3}, {"casual", "Casual", m_config.show_lobby_rank_casual}, {"t", "Tourney", m_config.show_lobby_rank_tourny}, {"hoops", "Hoops", m_config.show_extra_playlists && m_config.show_lobby_rank_hoops}, {"rumble", "Rumble", m_config.show_extra_playlists && m_config.show_lobby_rank_rumble}, {"dropshot", "Dropshot", m_config.show_extra_playlists && m_config.show_lobby_rank_dropshot}, {"snowday", "Snow", m_config.show_extra_playlists && m_config.show_lobby_rank_snowday}, {"heatseeker", "Heat", m_config.show_extra_playlists && m_config.show_lobby_rank_heatseeker}};
    std::vector<const PlayerData*> players;
    for (const auto& [_, p] : m_snap.roster)
        players.push_back(&p);
    std::sort(players.begin(), players.end(), [](const PlayerData* a, const PlayerData* b) {
        if (a->team != b->team) return a->team < b->team;
        auto mmr = [](const PlayerData* p) { auto it = p->playlists.find("2v2"); return it == p->playlists.end() ? p->mmr : it->second; };
        if (mmr(a) != mmr(b)) return mmr(a) > mmr(b);
        return a->name < b->name;
    });
    std::ostringstream out;
    out << "<div class='lobby-rank-table'><div class='player-row lobby-rank-row lobby-rank-header'><div class='player-name'><div class='lobby-rank-heading'>Name</div></div>";
    for (const auto& pl : playlists) {
        if (!pl.show) continue;
        // Wrap header text in a block: RmlUi drops bare text nodes in a flex
        // container, which is what left the lobby header row blank.
        out << "<div class='lobby-rank-cell'><div class='lobby-rank-heading'>" << Escape(pl.label) << "</div></div>";
    }
    out << "</div>";
    for (const auto* p : players) {
        std::string platform;
        if (const auto pos = p->primaryId.find('|'); pos != std::string::npos) platform = p->primaryId.substr(0, pos);
        const PlatformKind platformKind = PlatformKindFor(platform);

        out << "<div class='player-row lobby-rank-row" << (p->primaryId == m_snap.myPrimaryId ? " self" : "") << "'><div class='player-name'><span class='value'>" << Escape(p->name) << "</span>";
        if (!platform.empty())
            out << "<div class='label " << PlatformClass(platformKind) << "'>"
                << Escape(PlatformDisplayName(platformKind, platform)) << "</div>";
        out << "</div>";
        for (const auto& pl : playlists) {
            if (!pl.show) continue;
            int mmr = 0;
            if (auto it = p->playlists.find(pl.key); it != p->playlists.end()) mmr = it->second;
            const int matches = [&]() { auto it = p->playlistMatches.find(pl.key); return it == p->playlistMatches.end() ? 0 : it->second; }();
            std::string tier = "Unranked";
            if (auto it = p->playlistTiers.find(pl.key); it != p->playlistTiers.end()) tier = it->second;
            if (std::string_view(pl.key) != "casual" && (tier.empty() || tier == "Unranked") && mmr > 0) {
                tier = MMRFetcher::GetRankTierForPlaylistMmr(pl.key, mmr);
            }
            out << "<div class='lobby-rank-cell tooltip-host' style='color:" << CssColor(Format::RankColor(tier)) << "'>";
            if (mmr > 0) {
                // Rank above the rating, with the season games played trailing the
                // rating on the same line: a third line made the table taller than
                // the roster it sits under.
                out << "<div class='lobby-rank-value'>";
                if (tier != "Unranked" && !tier.empty() && std::string_view(pl.key) != "casual") {
                    out << "<div class='mono'>" << Escape(Format::AbbreviateRank(tier)) << "</div>";
                }
                out << "<div><span class='mono'>" << mmr << "</span>";
                if (matches > 0) out << "<span class='lobby-rank-matches'>(" << matches << ")</span>";
                out << "</div></div>";
                out << "<span class='tooltip-bubble'>";
                if (std::string_view(pl.key) == "casual") {
                    out << "Casual · MMR " << mmr;
                } else if (tier != "Unranked" && !tier.empty()) {
                    out << Escape(Format::RankTier(tier, m_config.use_roman_numerals)) << " · MMR " << mmr;
                } else {
                    out << "Unranked · MMR " << mmr;
                }
                if (matches > 0) out << " · " << matches << " games this season";
                out << "</span>";
            } else {
                out << "<div class='lobby-rank-value'><div>" << (p->fetched ? "-" : "...") << "</div></div>";
                if (!p->fetched) out << "<span class='tooltip-bubble'>Fetching rank...</span>";
            }
            out << "</div>";
        }
        out << "</div>";
    }
    if (players.empty()) out << "<div class='muted'>Waiting for lobby ranks...</div>";
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderMmrGraph(bool showCategoryBadge) {
    const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const std::string playlist = MmrCategoryToString(category);
    std::vector<float> series;
    std::vector<bool> seriesEstimated;
    std::vector<float> seriesTimes;
    float baseline = -1.0f;
    if (m_snap.showLifetimeGraph) {
        series = m_snap.lifetimeMmrY;
        seriesTimes = m_snap.lifetimeMmrX;
        if (!series.empty()) baseline = series.front();
        seriesEstimated.assign(series.size(), false);
    } else {
        if (auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) series = it->second;
        if (auto it = m_snap.playlistHistoryEstimated.find(playlist); it != m_snap.playlistHistoryEstimated.end()) seriesEstimated = it->second;
        if (auto it = m_snap.playlistInitialMmr.find(playlist); it != m_snap.playlistInitialMmr.end()) baseline = static_cast<float>(it->second);
    }

    constexpr int kVisibleMatches = 25;
    const int total = static_cast<int>(series.size());
    const int maxOffset = std::max(0, total - kVisibleMatches);
    const int offset = m_state ? std::clamp(m_state->ui.graphOffset.load(), 0, maxOffset) : 0;
    const int firstIndex = std::max(0, total - kVisibleMatches - offset);
    const int windowCount = std::min(kVisibleMatches, total - firstIndex);

    std::vector<float> values;
    std::vector<bool> estimated;
    if (windowCount > 0) {
        values.assign(series.begin() + firstIndex, series.begin() + firstIndex + windowCount);
        if (!seriesEstimated.empty()) {
            const int endIdx = std::min(firstIndex + windowCount, static_cast<int>(seriesEstimated.size()));
            if (firstIndex < endIdx) estimated.assign(seriesEstimated.begin() + firstIndex, seriesEstimated.begin() + endIdx);
        }
    }
    if (m_snap.showLifetimeGraph && !values.empty()) {
        baseline = values.front();
    }

    std::ostringstream header;
    header << "<div class='row graph-header'>";
    if (showCategoryBadge) {
        header << "<span class='badge'>" << Escape(MmrLabel(category)) << "</span>";
    }
    header << "<div class='grow'></div>";
    if (total > kVisibleMatches) {
        header << "<div class='graph-zoom'>"
               << Button("graph-pan-older", "&#8592;", "ghost zoom-step")
               << Button("graph-pan-newer", "&#8594;", "ghost zoom-step")
               << "</div>";
    }
    header << "</div>";

    if (values.empty()) {
        std::ostringstream empty;
        empty << header.str();
        if (m_snap.showLifetimeGraph) {
            empty << "<div class='graph-empty'><div class='muted'>No lifetime MMR history in database yet.</div>"
                  << "<div class='label'>Play matches to populate database records!</div></div>";
        } else {
            empty << "<div class='graph-empty'><div class='muted'>No MMR data for " << Escape(MmrLabel(category))
                  << " this session.</div><div class='label'>Play a match to start plotting!</div></div>";
        }
        return empty.str();
    }
    const bool hasBaseline = baseline > 0.0f;

    float minV = values.front();
    float maxV = values.front();
    for (float value : values) {
        minV = std::min(minV, value);
        maxV = std::max(maxV, value);
    }
    if (maxV == minV) {
        maxV += 15.0f;
        minV -= 15.0f;
    } else {
        const float padding = (maxV - minV) * 0.20f;
        maxV += padding;
        minV -= padding;
    }

    constexpr float xMin = 4.0f;
    constexpr float xMax = 80.0f;
    constexpr float yMin = 10.0f;
    constexpr float yMax = 88.0f;
    auto yPct = [&](float value) { return yMax - ((value - minV) / (maxV - minV)) * (yMax - yMin); };
    auto xPct = [&](size_t index) { return values.size() <= 1 ? (xMin + xMax) * 0.5f : xMin + static_cast<float>(index) / static_cast<float>(values.size() - 1) * (xMax - xMin); };

    std::ostringstream out;
    out << header.str() << "<div class='graph-wrap'>";
    const float midV = (minV + maxV) * 0.5f;
    out << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMin << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMax << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-gridline' style='left:" << xMin << "%;top:" << yPct(midV) << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-label graph-max' style='left:" << xMin << "%;top:1%'>" << static_cast<int>(maxV) << "</div>"
        << "<div class='graph-label' style='left:" << xMin << "%;top:" << (yPct(midV) + 1.0f) << "%'>" << static_cast<int>(midV) << "</div>"
        << "<div class='graph-label graph-min' style='left:" << xMin << "%;top:89%'>" << static_cast<int>(minV) << "</div>";

    out << "<div class='graph-label graph-last' style='right:20%;top:1%'>";
    if (total > kVisibleMatches) {
        out << (firstIndex + 1) << '-' << (firstIndex + windowCount) << " of " << total;
    } else {
        out << "last " << total;
    }
    out << "</div>";

    if (!seriesTimes.empty() && firstIndex < static_cast<int>(seriesTimes.size())) {
        const int lastIndex = std::min(firstIndex + windowCount - 1, static_cast<int>(seriesTimes.size()) - 1);
        out << "<div class='graph-label' style='left:" << (xMin + 7.0f) << "%;top:89%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(firstIndex)]))) << "</div>"
            << "<div class='graph-label graph-last' style='right:20%;top:89%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(lastIndex)]))) << "</div>";
    }

    if (hasBaseline && baseline >= minV && baseline <= maxV) {
        const float baseY = yPct(baseline);
        constexpr int dashCount = 13;
        constexpr float dashWidth = 3.6f;
        const float step = (xMax - xMin) / static_cast<float>(dashCount);
        for (int i = 0; i < dashCount; ++i) {
            const float left = xMin + static_cast<float>(i) * step;
            out << "<div class='graph-line baseline' style='left:" << left << "%;top:" << baseY << "%;width:" << dashWidth << "%'></div>";
        }
    }

    out << "<mmrgraphlines class='graph-polyline' points='";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ';';
        out << (xPct(i) / 100.0f) << ',' << (yPct(values[i]) / 100.0f);
    }
    out << "'></mmrgraphlines>";

    for (size_t i = 0; i < values.size(); ++i) {
        std::string cls;
        if (i == 0) {
            if (baseline > 0 && values[i] > baseline)
                cls = " win";
            else if (baseline > 0 && values[i] < baseline)
                cls = " loss";
            else
                cls = " neutral";
        } else if (values[i] > values[i - 1])
            cls = " win";
        else if (values[i] < values[i - 1])
            cls = " loss";
        else
            cls = " neutral";
        const bool isEstimated = i < estimated.size() && estimated[i];
        if (isEstimated) cls += " estimated";
        const bool isCurrent = i + 1 == values.size();
        if (isCurrent) {
            out << "<div class='graph-point-halo" << cls << "' style='left:" << xPct(i) << "%;top:" << yPct(values[i]) << "%'></div>";
            cls += " current";
        }
        out << "<div class='graph-point" << cls << "' style='left:" << xPct(i) << "%;top:" << yPct(values[i]) << "%'></div>";
    }

    const int current = static_cast<int>(std::lround(values.back()));
    const int change = hasBaseline ? static_cast<int>(std::lround(values.back() - baseline)) : 0;
    out << "<div class='graph-current' style='left:83%;top:" << (yPct(values.back()) - 4.0f) << "%;'>" << current
        << " <span class='" << (change >= 0 ? "win" : "loss") << "'>";
    if (change >= 0) out << '+';
    out << change << "</span></div></div>";
    return out.str();
}

std::string RmlUiController::RenderWidget(DashboardLayout::WidgetId id, bool dashboard) {
    auto categorySelect = [&](const char* key, MmrCategory current, bool includeBest) {
        std::ostringstream html;
        html << "<select class='compact-select' data-setting='" << key << "'>";
        for (auto category : MmrCategories(includeBest, m_config.show_extra_playlists)) {
            html << "<option value='" << Escape(MmrCategoryToString(category)) << "'" << Selected(current == category) << ">"
                 << Escape(MmrLabel(category)) << "</option>";
        }
        html << "</select>";
        return html.str();
    };

    switch (id) {
    case DashboardLayout::WidgetId::LiveRoster: {
        const auto rosterCategory = m_state ? m_state->ui.rosterMmrCategory.load() : MmrCategory::Best;
        std::ostringstream html;
        if (dashboard) {
            html << "<div class='row widget-controls'><span class='label grow'>Rank view</span>"
                 << categorySelect("mmr_category", rosterCategory, true) << "</div>";
        } else {
            const std::string categoryName = MmrLabel(rosterCategory);
            const std::string playlistImage = PlaylistImageForName(MmrCategoryToString(rosterCategory));
            html << "<div class='roster-header'><div class='row'><div class='brand-mini grow'>OMNISTATS</div><div class='row gap-xs'>";
            if (m_config.use_rank_icons && !playlistImage.empty()) html << "<img class='playlist-icon' src='" << playlistImage << "'/>";
            html << "<span class='badge'>MMR · " << Escape(categoryName) << "</span></div></div>"
                 << "<div class='label'>" << Escape(m_snap.arenaName.empty() ? (m_snap.inMatch ? "Active match" : "No active match connected") : m_snap.arenaName) << "</div></div>";
        }
        html << RenderPlayerRoster(0, "BLUE") << RenderPlayerRoster(1, "ORANGE");
        if (!dashboard) {
            const std::string cycle = m_config.key_cycle > 0 ? GetKeyDisplayName(m_config.key_cycle) : "Unbound";
            const std::string expand = m_config.key_expand > 0 ? GetKeyDisplayName(m_config.key_expand) : "Unbound";
            const std::string session = m_config.key_session > 0 ? GetKeyDisplayName(m_config.key_session) : "Unbound";
            html << "<div class='roster-footer'><span><b>" << Escape(cycle) << "</b> cycle MMR</span><span><b>" << Escape(expand) << "</b> " << (m_state && m_state->ui.h2hExpanded.load() ? "shrink" : "expand") << "</span><span><b>" << Escape(session) << "</b> session</span></div>";
        }
        return html.str();
    }
    case DashboardLayout::WidgetId::LiveMatchStats:
        return RenderLiveMatchStats();
    case DashboardLayout::WidgetId::SessionStats:
        return RenderSessionStats(true, true);
    case DashboardLayout::WidgetId::MmrGraph: {
        std::string controls;
        if (dashboard) {
            controls = "<div class='row widget-controls'><span class='label grow'>Playlist</span>" +
                       categorySelect("graph_mmr_category", m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo, false) + "</div>";
        }
        return controls + RenderMmrGraph();
    }
    case DashboardLayout::WidgetId::StreaksStats:
        return RenderStreaksStats();
    case DashboardLayout::WidgetId::GamemodeBreakdown: {
        std::string controls;
        if (dashboard) {
            controls = "<div class='row widget-controls'><span class='label grow'>Scope</span>"
                       "<select class='compact-select' data-setting='gamemode_breakdown_scope'>"
                       "<option value='current_session'" +
                       Selected(m_config.gamemode_breakdown_scope != "all_time") + ">Current Session</option>"
                                                                                   "<option value='all_time'" +
                       Selected(m_config.gamemode_breakdown_scope == "all_time") + ">All-Time</option>"
                                                                                   "</select></div>";
        }
        return controls + RenderGamemodeBreakdown(ScopeFromConfigString(m_config.gamemode_breakdown_scope));
    }
    case DashboardLayout::WidgetId::LobbyRanks:
        return RenderLobbyRanks();
    case DashboardLayout::WidgetId::DemoTracker:
        return RenderDemoTracker();
    case DashboardLayout::WidgetId::PreviousGames:
        return RenderPreviousGames(!dashboard);
    }
    return {};
}

// The session view and post-match summary live outside overlay_layout, so they
// carry their own persisted position. An unset position keeps the legacy
// centered placement, and they are only draggable while the overlay accepts
// mouse input (Settings open); in game the overlay stays click-through.
std::string RmlUiController::FloatingCardClass() const {
    return WantsInteraction() ? " floating-card floating-card-movable" : " floating-card";
}

std::string RmlUiController::FloatingCardStyle(float x, float y, float widthDp) const {
    std::ostringstream style;
    style << " style='width:" << widthDp << "dp;";
    if (x >= 0.0f && y >= 0.0f) {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        style << "left:" << x / std::max(rmlScale, 0.01f) << "dp;top:" << y / std::max(rmlScale, 0.01f) << "dp;margin-left:0;";
    } else {
        style << "margin-left:" << -widthDp * 0.5f << "dp;";
    }
    style << "'";
    return style.str();
}

std::string RmlUiController::RenderMatchSummary() {
    const int myTeam = m_snap.matchSummaryMyTeam;
    const int score0 = m_snap.matchSummaryScore[0];
    const int score1 = m_snap.matchSummaryScore[1];
    int winner = m_snap.matchSummaryWinnerTeam;
    if (winner != 0 && winner != 1) {
        winner = score0 > score1 ? 0 : score1 > score0 ? 1
                                                       : -1;
    }
    std::string result = "DRAW";
    std::string resultClass = "muted";
    if (m_snap.lastMatchWasVoid) {
        result = "VOID";
    } else if ((myTeam == 0 || myTeam == 1) && winner != -1) {
        if (winner == myTeam) {
            result = "WIN";
            resultClass = "win";
        } else {
            result = "LOSS";
            resultClass = "loss";
        }
    }
    const int myScore = myTeam == 1 ? score1 : score0;
    const int theirScore = myTeam == 1 ? score0 : score1;

    const auto& s = m_snap.currentMatch;
    std::vector<std::pair<std::string, std::string>> play;
    if (s.saves > 0) play.push_back({"Saves", Format::PairCount(s.saves, s.savesSelf)});
    if (s.shots > 0) play.push_back({"Shots", Format::PairCount(s.shots, s.shotsSelf)});
    if (s.assists > 0) play.push_back({"Assists", Format::PairCount(s.assists, s.assistsSelf)});
    if (s.demos > 0) play.push_back({"Demos", Format::PairCount(s.demos, s.demosSelf)});
    if (s.demoedSelf > 0) play.push_back({"Demoed", std::to_string(s.demoedSelf)});
    if (s.crossbars > 0) play.push_back({"Crossbars", Format::PairCount(s.crossbars, s.crossbarsSelf)});
    if (s.boostPickedUp > 0) play.push_back({"Boost", Format::PairCount(s.boostPickedUp, s.boostPickedUpSelf)});

    std::vector<std::pair<std::string, std::string>> fun;
    if (s.maxGoalSpeed > 0) fun.push_back({"Max goal speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", m_config.imperial_units)});
    if (s.maxBallSpeed > 0) fun.push_back({"Max ball speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", m_config.imperial_units)});
    if (s.maxImpactForce > 0) {
        fun.push_back({"Hardest crossbar hit", m_config.crossbar_display_mode == "speed"
                                                   ? Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", m_config.imperial_units)
                                                   : Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", false)});
    }
    if (s.fastestGoalTime > 0) fun.push_back({"Fastest goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)});
    if (s.ownGoals > 0) fun.push_back({"Own goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});

    auto renderRows = [](const char* title, const auto& rows) {
        std::ostringstream html;
        if (rows.empty()) return html.str();
        html << "<div class='stat-section-title' style='margin-top:7dp'>" << title << "</div><div class='stat-grid cols-2 compact-stats'>";
        for (const auto& [name, value] : rows)
            html << "<div class='stat-cell'><div class='label'>" << name << "</div><div class='value mono'>" << value << "</div></div>";
        html << "</div>";
        return html.str();
    };

    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClass() << "' data-action='floating-card-drag' data-card='match-summary'"
        << FloatingCardStyle(m_config.match_summary_x, m_config.match_summary_y, 420.0f)
        << "><div class='row'><div class='grow value " << resultClass << "' style='font-size:20dp'>" << result << "</div>"
        << "<div class='value mono' style='font-size:20dp'>" << myScore << '-' << theirScore << "</div></div>";
    out << renderRows("PLAY", play) << renderRows("FUN", fun) << "</div>";
    return out.str();
}

std::string RmlUiController::RenderSessionView() {
    const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const bool graph = m_state && m_state->ui.showGraphView.load();
    const bool lifetime = graph && m_snap.showLifetimeGraph;
    std::ostringstream out;
    out << "<div class='card match-summary" << FloatingCardClass() << "' data-action='floating-card-drag' data-card='session-view'"
        << FloatingCardStyle(m_config.session_view_x, m_config.session_view_y, 450.0f)
        << "><div class='row'><div class='card-title grow'>"
        << (lifetime ? "LIFETIME MMR · " : graph ? "SESSION MMR · "
                                                 : "SESSION · ")
        << Escape(MmrLabel(category)) << "</div><span class='badge'>F7 view · F6 playlist</span></div>";
    out << (graph ? RenderMmrGraph(false) : RenderSessionStats(true, true));
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderOverlayContainer(const OverlayLayout::ContainerConfig& container, bool editMode) {
    const bool settingsOpen = m_state && m_state->ui.showMenu.load();
    const bool showOverlay = m_state && m_state->ui.showOverlay.load();
    const bool expanded = m_state && m_state->ui.h2hExpanded.load();
    auto visible = [&](DashboardLayout::WidgetId widget) {
        if (editMode) return true;
        if (settingsOpen) {
            if (widget == DashboardLayout::WidgetId::StreaksStats) return m_config.show_streaks_stats;
            if (widget == DashboardLayout::WidgetId::GamemodeBreakdown) return m_config.show_gamemode_breakdown;
            if (widget == DashboardLayout::WidgetId::LobbyRanks) return m_config.show_lobby_ranks_overlay;
            if (widget == DashboardLayout::WidgetId::DemoTracker) return m_config.show_demo_tracker_overlay;
            if (widget == DashboardLayout::WidgetId::PreviousGames) return m_config.show_previous_games_summary;
            return true;
        }
        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
            return showOverlay;
        case DashboardLayout::WidgetId::MmrGraph:
            return showOverlay;
        case DashboardLayout::WidgetId::LiveMatchStats:
            return showOverlay && expanded;
        case DashboardLayout::WidgetId::SessionStats:
            // Session stats follow the overlay key like every other roster card;
            // they are not a permanent HUD element.
            return showOverlay;
        case DashboardLayout::WidgetId::StreaksStats:
            return showOverlay && m_config.show_streaks_stats;
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            return showOverlay && m_config.show_gamemode_breakdown;
        case DashboardLayout::WidgetId::LobbyRanks:
            return showOverlay && m_config.show_lobby_ranks_overlay;
        case DashboardLayout::WidgetId::DemoTracker:
            return m_config.show_demo_tracker_overlay && m_snap.inMatch;
        case DashboardLayout::WidgetId::PreviousGames:
            return showOverlay && m_config.show_previous_games_summary;
        }
        return false;
    };

    std::vector<DashboardLayout::WidgetId> widgets;
    for (auto widget : container.widgets)
        if (visible(widget)) widgets.push_back(widget);
    if (widgets.empty()) return {};

    OverlayLayout::ContainerConfig visibleContainer = container;
    visibleContainer.widgets = widgets;
    auto [minW, minH] = OverlayContainerMinSize(visibleContainer, m_dpiScale, &m_config);
    auto [w, h] = OverlayContainerSize(visibleContainer, m_dpiScale, &m_config);

    const float dpi = SanitizedScale(m_dpiScale);
    const float rmlScale = dpi * SanitizedUiScale(m_config.ui_scale);

    // Lobby Ranks is a fixed-column compact table. Protect enough horizontal
    // room for the identity column and every enabled playlist so flex layout
    // never has to collapse the player's name or overlap rank/MMR text.
    if (std::find(widgets.begin(), widgets.end(), DashboardLayout::WidgetId::LobbyRanks) != widgets.end()) {
        const float lobbyContentMinDp = LobbyRanksContentMinDp(m_config);
        minW = std::max(minW, lobbyContentMinDp * rmlScale);
        w = std::max(w, minW);
    }

    // The legacy layout manager persisted explicit undersized dimensions after
    // clamping them. Keep that compatibility so old/corrupt layouts repair
    // themselves once instead of remaining invalid in Config forever. Stored
    // geometry is design pixels, so the rendered minimum converts back first.
    const float uiScale = SanitizedUiScale(m_config.ui_scale);
    const float minWStored = minW / uiScale;
    const float minHStored = minH / uiScale;
    const bool clampStoredWidth = container.w > 1.0f && container.w < minWStored;
    const bool clampStoredHeight = container.h > 1.0f && container.h < minHStored;
    if (clampStoredWidth || clampStoredHeight) {
        const std::string containerId = container.id;
        Config::Update([containerId, clampStoredWidth, clampStoredHeight, minWStored, minHStored](ConfigData& c) {
            for (auto& stored : c.overlay_layout.containers) {
                if (stored.id != containerId) continue;
                if (clampStoredWidth) stored.w = minWStored;
                if (clampStoredHeight) stored.h = minHStored;
                break;
            }
        },
                       true);
    }

    // Changing the app text size rescales every card, so a position saved at one
    // scale can push the card past the screen edge at another. Keep the whole
    // card on screen horizontally, and at least its minimum height vertically -
    // cards auto-size their height during play, so clamping against the (taller)
    // edit-mode height would shove them up for no reason.
    float x = container.x;
    if (x + w > static_cast<float>(m_width)) x = std::max(12.0f * dpi, static_cast<float>(m_width) - w - 18.0f * dpi);
    float y = container.y;
    const float visibleHeight = std::min(h, minH);
    if (y + visibleHeight > static_cast<float>(m_height)) y = std::max(0.0f, static_cast<float>(m_height) - visibleHeight);
    const auto toDp = [rmlScale](float pixels) { return pixels / std::max(rmlScale, 0.5f); };

    std::ostringstream out;
    out << "<div class='card overlay-card";
    if (settingsOpen || editMode) out << " interactive";
    if (editMode) out << " overlay-edit";
    out << "' data-container='" << Escape(container.id) << "'";
    if (settingsOpen && !editMode) out << " data-action='overlay-drag'";
    out << " style='left:" << toDp(x) << "dp;top:" << toDp(y) << "dp;width:" << toDp(w) << "dp;";
    // The saved height is edit-mode geometry, matching the pre-RmlUi overlay.
    // During normal play (and while Settings is merely open), overlay windows
    // auto-size vertically to their currently visible widgets. Keeping the saved
    // default height as a permanent min-height is what produced the huge empty
    // main_stack panel after the RmlUi migration.
    if (editMode) out << "height:" << toDp(std::min(h, 900.0f * dpi)) << "dp;";
    out << "'>";
    if (editMode) out << "<div class='row overlay-drag' data-action='overlay-drag' data-container='" << Escape(container.id) << "'><span class='badge accent'>MOVE</span><div class='grow'></div><span class='label'>" << Escape(container.id) << "</span><button class='widget-close' data-action='overlay-remove-container' data-container='" << Escape(container.id) << "' title='Remove this overlay container'>×</button></div>";
    for (size_t i = 0; i < widgets.size(); ++i) {
        const auto widget = widgets[i];
        if (editMode) {
            out << "<div class='row overlay-widget-handle' data-action='overlay-widget-drag' data-container='" << Escape(container.id)
                << "' data-widget='" << WidgetDomId(widget) << "' style='margin-top:" << (i ? 10 : 4) << "dp'>"
                << "<div class='card-subtitle grow'>" << Escape(DashboardLayout::GetWidgetDisplayName(widget)) << "</div>"
                << "<button class='widget-close' data-action='overlay-remove-widget' data-container='" << Escape(container.id)
                << "' data-widget='" << WidgetDomId(widget) << "'>×</button></div>";
        } else if (widgets.size() > 1 && widget != DashboardLayout::WidgetId::PreviousGames) {
            out << "<div class='overlay-widget-title' style='margin-top:" << (i ? 8 : 2) << "dp'>" << Escape(DashboardLayout::GetWidgetDisplayName(widget)) << "</div>";
        }
        out << RenderWidget(widget, false);
    }
    if (editMode) out << "<div class='overlay-resize' data-action='overlay-resize' data-container='" << Escape(container.id) << "'></div>";
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderOverlayToolbox(bool editMode) {
    if (!editMode) return {};

    std::ostringstream out;
    out << "<button class='overlay-toolbox-toggle' data-action='overlay-toggle-toolbox' title='Toggle overlay toolbox'>Widgets</button>";
    if (!m_config.overlay_layout.toolboxOpen) return out.str();

    std::set<DashboardLayout::WidgetId> present;
    for (const auto& c : m_config.overlay_layout.containers)
        for (auto w : c.widgets)
            present.insert(w);
    out << "<div class='card overlay-toolbox'><div class='row'><div class='card-title grow'>Overlay Layout</div>"
        << "<button class='window-control' data-action='overlay-close-toolbox'>×</button></div>"
        << "<div class='label'>Click to add to the center, or drag onto the overlay to place it.</div>"
        << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("reset-overlay", "Reset", "ghost") << "</div>"
        << "<div class='col gap-xs' style='margin-top:8dp'>";
    const std::array<DashboardLayout::WidgetId, 9> all = {DashboardLayout::WidgetId::LiveRoster, DashboardLayout::WidgetId::LiveMatchStats, DashboardLayout::WidgetId::SessionStats, DashboardLayout::WidgetId::MmrGraph, DashboardLayout::WidgetId::StreaksStats, DashboardLayout::WidgetId::GamemodeBreakdown, DashboardLayout::WidgetId::LobbyRanks, DashboardLayout::WidgetId::DemoTracker, DashboardLayout::WidgetId::PreviousGames};
    for (auto w : all) {
        const bool active = present.count(w) != 0;
        out << "<div class='row" << (active ? "" : " toolbox-draggable") << "'";
        if (!active) out << " data-action='overlay-toolbox-drag' data-widget='" << WidgetDomId(w) << "'";
        out << "><div class='grow'>" << Escape(DashboardLayout::GetWidgetDisplayName(w)) << "</div>";
        if (active)
            out << "<span class='badge win'>ACTIVE</span>";
        else
            out << "<button class='compact ghost' data-action='overlay-add-widget' data-widget='" << WidgetDomId(w) << "'>Add / drag</button>";
        out << "</div>";
    }
    out << "</div></div>";
    return out.str();
}

void RmlUiController::RebuildOverlay() {
    if (m_config.second_monitor_mode) {
        SetRootRml("overlay-root", "");
        return;
    }
    std::ostringstream out;
    if (m_config.show_running_indicator) out << "<div class='running-indicator'><span class='win'>●</span> OmniStats</div>";

    if (m_state && m_state->ui.showMatchSummary.load() && m_config.show_match_summary) {
        const int64_t elapsed = SteadyNowMs() - m_state->ui.matchSummaryStartMs.load();
        if (elapsed < 30000)
            out << RenderMatchSummary();
        else
            m_state->ui.showMatchSummary.store(false);
    }
    if (m_state && m_state->ui.showSessionView.load()) out << RenderSessionView();

    const bool editMode = m_state && m_state->ui.showMenu.load() && m_state->ui.dashboardLayoutEditMode.load();
    for (const auto& container : m_config.overlay_layout.containers)
        out << RenderOverlayContainer(container, editMode);
    out << RenderOverlayToolbox(editMode);
    if (editMode) {
        out << "<div id='overlay-snap-x' class='overlay-snap-guide vertical'></div>"
            << "<div id='overlay-snap-y' class='overlay-snap-guide horizontal'></div>";
    }
    SetRootRml("overlay-root", out.str());
}

void RmlUiController::RebuildDashboard() {
    if (!m_config.second_monitor_mode) {
        SetRootRml("dashboard-root", "");
        return;
    }
    const bool editMode = m_state && m_state->ui.dashboardLayoutEditMode.load();
    DashboardLayout::LayoutConfig layout = m_config.dashboard_layout;
    DashboardLayout::Sanitize(layout);

    auto zoneWidgets = [&](DashboardLayout::Zone zone) {
        std::vector<DashboardLayout::WidgetPlacement> result;
        for (const auto& p : layout.widgets) {
            if (p.zone != zone) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::LiveRoster && !m_config.second_monitor_show_roster) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::SessionStats && !m_config.second_monitor_show_session) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::LobbyRanks && !m_config.show_lobby_ranks_overlay) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::DemoTracker && !m_config.show_demo_tracker_overlay) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::PreviousGames && !m_config.show_previous_games_summary) continue;
            result.push_back(p);
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        return result;
    };
    auto renderZone = [&](DashboardLayout::Zone zone, const char* klass) {
        std::ostringstream z;
        const auto placements = zoneWidgets(zone);
        z << "<div class='" << klass << "' data-zone='" << ZoneName(zone) << "'>";
        if (editMode) {
            z << "<div class='dashboard-drop-slot' data-zone='" << ZoneName(zone)
              << "' data-drop-index='0'><span class='drop-hint'>── Drop at top ──</span></div>";
        }
        for (size_t index = 0; index < placements.size(); ++index) {
            const auto& placement = placements[index];
            const std::string id = WidgetDomId(placement.id);
            if (editMode && index > 0) {
                z << "<div class='dashboard-drop-slot' data-zone='" << ZoneName(zone)
                  << "' data-drop-index='" << index << "'><span class='drop-hint'>── Drop Here ──</span></div>";
            }
            z << "<div class='dashboard-widget' data-widget='" << id << "' data-zone='" << ZoneName(zone)
              << "' data-order='" << index << "'>"
              << "<div class='dashboard-widget-title' data-action='dashboard-drag' data-widget='" << id << "'><span class='name'>" << Escape(DashboardLayout::GetWidgetDisplayName(placement.id)) << "</span>";
            if (editMode) z << "<span class='badge accent'>DRAG</span>";
            z << "</div>";
            if (!placement.collapsed) z << RenderWidget(placement.id, true);
            z << "</div>";
        }
        if (editMode && !placements.empty()) {
            z << "<div class='dashboard-drop-slot end' data-zone='" << ZoneName(zone)
              << "' data-drop-index='" << placements.size() << "'><span class='drop-hint'>── Drop at bottom ──</span></div>";
        }
        z << "</div>";
        return z.str();
    };

    std::string updateVersion;
    bool updateFailed = false;
    bool updateDownloading = false;
    bool updateAvailable = false;
    if (m_state) {
        updateAvailable = m_state->ui.updateAvailable.load();
        updateDownloading = m_state->ui.updateDownloading.load();
        updateFailed = m_state->ui.updateDownloadFailed.load();
        if (updateAvailable) {
            std::lock_guard lock(m_state->ui.updateMutex);
            updateVersion = m_state->ui.updateAvailableVersion;
        }
        if (updateAvailable && !m_config.enable_auto_updates && !m_state->ui.updatePromptShown.exchange(true)) m_showUpdatePrompt = true;
    }
    std::string updateLabel;
    if (updateDownloading)
        updateLabel = updateVersion.empty() ? "Starting updater..." : "Starting v" + updateVersion + "...";
    else if (updateFailed)
        updateLabel = updateVersion.empty() ? "Retry Update" : "Retry Update v" + updateVersion;
    else
        updateLabel = updateVersion.empty() ? "Update Available" : "Update Available: v" + updateVersion;

    const bool dashboardHasVisibleWidgets = !zoneWidgets(DashboardLayout::Zone::Top).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Left).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Right).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Bottom).empty();
    const bool isMaximized = m_hwnd && IsZoomed(m_hwnd);
    std::ostringstream out;
    out << "<div class='dashboard-shell" << (editMode ? " dashboard-edit-active" : "")
        << (isMaximized ? " maximized" : "") << "'><div class='dashboard-topbar'><img class='brand-logo' src='res://images/Logo.png'/>"
        << "<div class='row grow' style='align-items:center'><div class='brand-title'>OmniStats <span class='version'>v" << Escape(AppVersion::Current) << "</span></div>"
        << "<div class='match-status'>" << (m_snap.inMatch ? ("ACTIVE MATCH · " + Escape(m_snap.arenaName)) : "WAITING IN LOBBY") << "</div></div>";
    if (updateAvailable) out << Button("update-app", Escape(updateLabel), "primary compact");
    out << "<div class='topbar-actions'>"
        << Button("dashboard-edit", editMode ? "Done Editing" : "Edit Layout", editMode ? "primary compact" : "ghost compact")
        << Button("open-settings", "Settings", "ghost compact")
        << "<button class='window-control' data-action='window-minimize'><span class='win-icon-min'></span></button>"
        << "<button class='window-control' data-action='window-maximize'>"
        << (isMaximized ? "<span class='win-icon-restore'><span class='restore-back'></span><span class='restore-front'></span></span>"
                        : "<span class='win-icon-max'></span>")
        << "</button>"
        << "<button class='window-control danger' data-action='window-close'><span class='win-icon-close'>&#215;</span></button>"
        << "</div></div>";

    out << "<div class='dashboard-content'>";
    if (!editMode && !dashboardHasVisibleWidgets) {
        out << "<div class='card empty-dashboard'><div class='card-title'>Dashboard panels are hidden</div>"
            << "<div class='setting-help'>Open Settings or Edit Layout to re-enable dashboard widgets.</div></div>";
    }
    out << renderZone(DashboardLayout::Zone::Top, "dashboard-zone-top")
        << "<div class='dashboard-columns'><div class='dashboard-column' style='flex-grow:" << std::max(layout.leftColumnWeight, 0.2f) << "'>" << renderZone(DashboardLayout::Zone::Left, "dashboard-zone-left") << "</div>"
        << "<div class='dashboard-column' style='flex-grow:" << std::max(1.0f - layout.leftColumnWeight, 0.2f) << "'>" << renderZone(DashboardLayout::Zone::Right, "dashboard-zone-right") << "</div></div>"
        << renderZone(DashboardLayout::Zone::Bottom, "dashboard-zone-bottom");
    if (editMode) out << "<div class='dashboard-hidden'><div class='card-title'>Hidden Widgets</div>" << renderZone(DashboardLayout::Zone::Hidden, "dashboard-zone-hidden") << Button("reset-dashboard", "Reset Dashboard Layout", "ghost") << "</div>";
    out << "</div>";
    if (m_showUpdatePrompt && updateAvailable) {
        out << "<div class='dashboard-modal'><div class='card update-dialog'><div class='card-title'>Update Available</div><div class='setting-help'>A new version of OmniStats is available";
        if (!updateVersion.empty()) out << ": v" << Escape(updateVersion);
        out << ". Would you like to update and restart the application now?</div><div class='row gap-sm' style='margin-top:12dp'>" << Button("update-app", "Yes, Update Now", "primary") << Button("dismiss-update", "Remind Me Later", "ghost") << "</div></div></div>";
    }
    out << "</div>";
    SetRootRml("dashboard-root", out.str());
}

std::string RmlUiController::RenderSettingsGeneral() {
    std::ostringstream out;
    out << SectionStart("Window & Visibility")
        << ToggleControl("require_rl_focus", "Require Rocket League focus", "Hide the overlay while another app has focus.", m_config.require_rl_focus)
        << ToggleControl("second_monitor_mode", "Second-monitor dashboard", "Use OmniStats as a normal interactive dashboard window.", m_config.second_monitor_mode);
    if (m_config.second_monitor_mode) {
        out << ToggleControl("second_monitor_show_roster", "Show live roster", "", m_config.second_monitor_show_roster)
            << ToggleControl("second_monitor_show_session", "Show session information", "", m_config.second_monitor_show_session)
            << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("reset-dashboard", "Reset Dashboard Layout", "ghost") << "</div>";
    }
    out << SectionEnd();

    out << SectionStart("Match Behavior")
        << ToggleControl("show_match_summary", "Show post-match summary", "Displays the result and match statistics for 30 seconds.", m_config.show_match_summary)
        << ToggleControl("show_running_indicator", "Show running indicator", "Small status badge while the transparent overlay is active.", m_config.show_running_indicator)
        << ToggleControl("reset_session_on_close", "Reset session when OmniStats closes", "", m_config.reset_session_on_close)
        << SectionEnd();

    out << SectionStart("Startup & Updates")
        << ToggleControl("run_on_startup", "Run on Windows startup", "", m_config.run_on_startup)
        << ToggleControl("check_for_updates", "Check for updates", "Checks OmniStats servers for available updates on startup.", m_config.check_for_updates)
        << ToggleControl("enable_auto_updates", "Automatically install updates", "Enabling this also enables update checks and restarts OmniStats when an update is ready.", m_config.enable_auto_updates)
        << SectionEnd();

    out << SectionStart("Player Identity") << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Local account</div><div class='setting-help'>Used for lifetime history before telemetry identifies you.</div></div><select data-setting='identity'>";
    out << "<option value=''" << Selected(m_config.last_primary_id.empty()) << ">Auto-detect</option>";
    std::set<std::string> seen;
    const auto selectableIdentity = [&](const std::string& id) {
        return !id.empty() && (id.rfind("Unknown|", 0) != 0 || id == m_config.last_primary_id);
    };
    for (const auto& id : m_config.known_primary_ids)
        if (selectableIdentity(id)) seen.insert(id);
    for (const auto& [id, p] : m_snap.roster)
        if (selectableIdentity(id)) seen.insert(id);
    for (const auto& id : seen) {
        std::string label = id;
        if (auto it = m_snap.roster.find(id); it != m_snap.roster.end() && !it->second.name.empty()) label = it->second.name + " · " + id;
        out << "<option value='" << Escape(id) << "'" << Selected(m_config.last_primary_id == id) << ">" << Escape(label) << "</option>";
    }
    out << "</select></div>" << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsCards() {
    std::ostringstream out;
    out << SectionStart("Overlay Layout")
        << ToggleControl("overlay_edit_mode", "Edit overlay layout", "Move and resize native overlay containers.", m_state && m_state->ui.dashboardLayoutEditMode.load())
        << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("reset-overlay", "Reset Overlay Layout", "ghost") << "</div>" << SectionEnd();

    out << SectionStart("Session Card")
        << ToggleControl("show_session_record", "Record", "", m_config.show_session_record)
        << ToggleControl("show_session_goals", "Goals", "", m_config.show_session_goals)
        << ToggleControl("show_session_saves", "Saves", "", m_config.show_session_saves)
        << ToggleControl("show_session_demos", "Demos", "", m_config.show_session_demos)
        << ToggleControl("show_session_boost", "Boost", "", m_config.show_session_boost)
        << ToggleControl("show_session_assists", "Assists", "", m_config.show_session_assists)
        << ToggleControl("show_session_goal_participation", "Goal participation", "", m_config.show_session_goal_participation)
        << ToggleControl("show_session_mmr_change", "MMR change", "", m_config.show_session_mmr_change)
        << SectionEnd();

    out << SectionStart("Visible Cards")
        << ToggleControl("use_rank_icons", "Use rank & playlist icons", "", m_config.use_rank_icons)
        << ToggleControl("show_lobby_ranks_overlay", "Lobby ranks", "", m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_account_wins_overlay", "Account wins", "", m_config.show_account_wins_overlay)
        << ToggleControl("show_demo_tracker_overlay", "Demolition tracker", "", m_config.show_demo_tracker_overlay)
        << ToggleControl("show_previous_games_summary", "Previous games", "", m_config.show_previous_games_summary)
        << ToggleControl("show_streaks_stats", "Streaks & stats", "", m_config.show_streaks_stats)
        << ToggleControl("show_gamemode_breakdown", "Gamemode breakdown", "", m_config.show_gamemode_breakdown)
        << SectionEnd();

    out << SectionStart("Lobby Rank Playlists")
        << ToggleControl("show_lobby_rank_1v1", "1v1", "", m_config.show_lobby_rank_1v1, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_2v2", "2v2", "", m_config.show_lobby_rank_2v2, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_3v3", "3v3", "", m_config.show_lobby_rank_3v3, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_casual", "Casual", "", m_config.show_lobby_rank_casual, !m_config.show_lobby_ranks_overlay)
        << ToggleControl("show_lobby_rank_tourny", "Tournament", "", m_config.show_lobby_rank_tourny, !m_config.show_lobby_ranks_overlay);
    if (m_config.show_extra_playlists) {
        out << ToggleControl("show_lobby_rank_hoops", "Hoops", "", m_config.show_lobby_rank_hoops, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_rumble", "Rumble", "", m_config.show_lobby_rank_rumble, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_dropshot", "Dropshot", "", m_config.show_lobby_rank_dropshot, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_snowday", "Snow Day", "", m_config.show_lobby_rank_snowday, !m_config.show_lobby_ranks_overlay)
            << ToggleControl("show_lobby_rank_heatseeker", "Heatseeker", "", m_config.show_lobby_rank_heatseeker, !m_config.show_lobby_ranks_overlay);
    }
    out << SectionEnd();

    out << SectionStart("Card Details")
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Previous games to show</div></div><select data-setting='previous_games_limit'" << (!m_config.show_previous_games_summary ? " disabled='disabled'" : "") << ">";
    for (int count : {10, 20, 30, 40, 50})
        out << "<option value='" << count << "'" << Selected(m_config.previous_games_limit == count) << ">" << count << "</option>";
    out << "</select></div>"
        << ToggleControl("show_longest_loss_streak", "Show longest loss streak", "", m_config.show_longest_loss_streak, !m_config.show_streaks_stats)
        << ToggleControl("show_gamemode_record_1v1", "1v1 breakdown", "", m_config.show_gamemode_record_1v1, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_2v2", "2v2 breakdown", "", m_config.show_gamemode_record_2v2, !m_config.show_gamemode_breakdown)
        << ToggleControl("show_gamemode_record_3v3", "3v3 breakdown", "", m_config.show_gamemode_record_3v3, !m_config.show_gamemode_breakdown)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Gamemode breakdown scope</div></div><select data-setting='gamemode_breakdown_scope'" << (!m_config.show_gamemode_breakdown ? " disabled='disabled'" : "") << ">"
        << "<option value='current_session'" << Selected(m_config.gamemode_breakdown_scope == "current_session") << ">Current Session</option>"
        << "<option value='all_time'" << Selected(m_config.gamemode_breakdown_scope == "all_time") << ">All-Time</option>"
        << "</select></div>" << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsRanks() {
    auto categorySelect = [&](const char* key, MmrCategory current, bool includeBest) {
        std::ostringstream html;
        html << "<select data-setting='" << key << "'>";
        for (auto c : MmrCategories(includeBest, m_config.show_extra_playlists)) {
            if (!includeBest && c == MmrCategory::Best) continue;
            html << "<option value='" << Escape(MmrCategoryToString(c)) << "'" << Selected(current == c) << ">" << Escape(MmrLabel(c)) << "</option>";
        }
        html << "</select>";
        return html.str();
    };
    std::ostringstream out;
    out << SectionStart("Player Ranks")
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Player MMR category</div><div class='setting-help'>Rank displayed beside live lobby players.</div></div>" << categorySelect("mmr_category", StringToMmrCategory(m_config.mmr_category), true) << "</div>"
        << ToggleControl("auto_switch_mmr_category", "Automatically follow current playlist", "", m_config.auto_switch_mmr_category)
        << ToggleControl("show_extra_playlists", "Show extra playlists", "Hoops, Rumble, Dropshot, Snow Day, and Heatseeker.", m_config.show_extra_playlists)
        << SectionEnd();

    out << SectionStart("Personal MMR Graph")
        << ToggleControl("graph_follow_current_playlist", "Follow current playlist", "", m_config.graph_follow_current_playlist)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Default graph category</div></div>" << categorySelect("graph_mmr_category", StringToMmrCategory(m_config.graph_mmr_category), false) << "</div>"
        << SectionEnd();

    out << SectionStart("Your Ranks");
    const PlayerData* me = nullptr;
    std::string effectiveId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    if (!effectiveId.empty()) {
        if (auto it = m_snap.roster.find(effectiveId); it != m_snap.roster.end()) me = &it->second;
    }
    if (me) {
        out << "<div class='rank-table'><div class='rank-table-row header'><div>Playlist</div><div>Rank</div><div>MMR</div><div>Matches</div></div>";
        for (auto category : MmrCategories(false, m_config.show_extra_playlists)) {
            const std::string key = MmrCategoryToString(category);
            std::string tier = "Unranked";
            if (auto it = me->playlistTiers.find(key); it != me->playlistTiers.end()) tier = it->second;
            int mmr = 0;
            if (auto it = me->playlists.find(key); it != me->playlists.end()) mmr = it->second;
            int matches = 0;
            if (auto it = me->playlistMatches.find(key); it != me->playlistMatches.end()) matches = it->second;
            out << "<div class='rank-table-row'><div>" << Escape(MmrLabel(category)) << "</div><div style='color:" << CssColor(Format::RankColor(tier)) << "'>" << Escape(Format::RankTier(tier, m_config.use_roman_numerals)) << "</div><div class='mono'>" << (mmr > 0 ? std::to_string(mmr) : "-") << "</div><div class='mono'>" << (matches > 0 ? std::to_string(matches) : "-") << "</div></div>";
        }
        out << "</div>";
    } else {
        out << "<div class='setting-help'>Join a match to see your ranks. If your account is not detected, select it in General. Rank lookup can be enabled in Integrations.</div>";
    }
    out << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsShortcuts() {
    auto bindTarget = [](BindCaptureTarget target) { return std::to_string(static_cast<int>(target)); };
    auto keyRow = [&](const char* label, BindCaptureTarget target, int key, bool canClear = true) {
        std::ostringstream html;
        const bool active = m_bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << label << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a key..." : Escape(GetKeyDisplayName(key))) << "</button>";
        if (active || canClear) html << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button>";
        html << "</div></div>";
        return html.str();
    };
    auto padName = [&](int value, bool raw, int rawValue) {
        return GamepadBindName(value, raw, rawValue);
    };
    auto padRow = [&](const char* label, BindCaptureTarget target, int value, bool raw, int rawValue) {
        std::ostringstream html;
        const bool active = m_bindCaptureTarget == target;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << label << "</div></div><div class='row gap-xs'>"
             << "<button data-action='capture-bind' data-bind='" << bindTarget(target) << "'>" << (active ? "Press a button..." : padName(value, raw, rawValue)) << "</button>"
             << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind") << "' data-bind='" << bindTarget(target) << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
        return html.str();
    };

    std::ostringstream out;
    out << SectionStart("Keyboard")
        << keyRow("Show overlay", BindCaptureTarget::KeyOverlay, m_config.key_overlay)
        << keyRow("Cycle playlist", BindCaptureTarget::KeyCycle, m_config.key_cycle)
        << keyRow("Expand live stats", BindCaptureTarget::KeyExpand, m_config.key_expand)
        << keyRow("Session view", BindCaptureTarget::KeySession, m_config.key_session)
        << keyRow("Graph pan older", BindCaptureTarget::KeyGraphPanLeft, m_config.key_graph_pan_left)
        << keyRow("Graph pan newer", BindCaptureTarget::KeyGraphPanRight, m_config.key_graph_pan_right)
        << keyRow("Settings", BindCaptureTarget::KeyMenu, m_config.key_menu, false)
        << keyRow("Save replay", BindCaptureTarget::KeySaveReplay, m_config.key_save_replay)
        << SectionEnd();
    out << SectionStart("Controller")
        << padRow("Show overlay", BindCaptureTarget::GamepadOverlay, m_config.gamepad_overlay, m_config.gamepad_overlay_raw, m_config.gamepad_overlay_raw_button)
        << padRow("Cycle playlist", BindCaptureTarget::GamepadCycle, m_config.gamepad_cycle, m_config.gamepad_cycle_raw, m_config.gamepad_cycle_raw_button)
        << padRow("Expand live stats", BindCaptureTarget::GamepadExpand, m_config.gamepad_expand, m_config.gamepad_expand_raw, m_config.gamepad_expand_raw_button)
        << padRow("Session view", BindCaptureTarget::GamepadSession, m_config.gamepad_session, m_config.gamepad_session_raw, m_config.gamepad_session_raw_button)
        << padRow("Graph pan older", BindCaptureTarget::GamepadGraphPanLeft, m_config.gamepad_graph_pan_left, m_config.gamepad_graph_pan_left_raw, m_config.gamepad_graph_pan_left_raw_button)
        << padRow("Graph pan newer", BindCaptureTarget::GamepadGraphPanRight, m_config.gamepad_graph_pan_right, m_config.gamepad_graph_pan_right_raw, m_config.gamepad_graph_pan_right_raw_button)
        << padRow("Settings", BindCaptureTarget::GamepadMenu, m_config.gamepad_menu, m_config.gamepad_menu_raw, m_config.gamepad_menu_raw_button);
    out << "<div class='controller-debug'><div class='setting-help'>Controller diagnostics</div>";
    if (m_state && m_state->ui.controllerConnected.load()) {
        std::string controllerName;
        {
            std::lock_guard lock(m_state->ui.controllerDebugMutex);
            controllerName = m_state->ui.controllerDebugName;
        }
        const bool mapped = m_state->ui.controllerIsGameController.load();
        const int lastMapped = mapped ? m_state->ui.lastControllerButtonPressed.load() : -1;
        const int lastRaw = m_state->ui.lastRawControllerButtonPressed.load();
        out << "<div class='debug-grid'><div class='debug-row'><span class='debug-label'>Device</span><span class='debug-value'>" << Escape(controllerName.empty() ? "connected" : controllerName) << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>SDL GameController</span><span class='debug-value'>" << (mapped ? "yes" : "no · fallback joystick") << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>Overlay bind</span><span class='debug-value'>" << Escape(GamepadBindName(m_config.gamepad_overlay, m_config.gamepad_overlay_raw, m_config.gamepad_overlay_raw_button)) << "</span></div>"
            << "<div class='debug-row'><span class='debug-label'>Last mapped</span><span class='debug-value'>";
        if (lastMapped >= 0)
            out << Escape(GamepadBindName(lastMapped, false, -1)) << " (" << lastMapped << ")";
        else
            out << "none";
        out << "</span></div><div class='debug-row'><span class='debug-label'>Last raw</span><span class='debug-value'>" << (lastRaw >= 0 ? std::to_string(lastRaw) : "none") << "</span></div></div>";
    } else {
        out << "<div class='setting-help'>No controller detected.</div>";
    }
    out << "</div>" << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsAppearance() {
    auto colorRow = [&](const char* key, const char* label, const ColorRGBA& color) {
        std::ostringstream html;
        html << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" << label
             << "</div><div class='setting-help'>Click the swatch to pick a color, or edit the hex value.</div></div>"
             << "<button class='color-dot color-dot-button' style='background-color:" << CssColor(color)
             << "' data-action='edit-color' data-color-key='" << key << "'></button>"
             << "<input type='text' class='text mono' style='width:100dp;margin-left:6dp' data-setting='" << key
             << "' value='" << CssColor(color) << "'/></div>";
        return html.str();
    };
    auto channelByte = [](float value) {
        if (!std::isfinite(value)) return 0;
        return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
    };
    auto rangeRow = [&](const char* channel, char component, int value) {
        std::ostringstream html;
        html << "<div class='color-channel'><span class='color-channel-label'>" << channel << "</span>"
             << "<input type='range' class='range color-range' min='0' max='255' step='1' data-setting='"
             << m_editColorKey << ':' << component << "' value='" << value << "'/>"
             << "<span class='mono color-channel-value'>" << value << "</span></div>";
        return html.str();
    };

    std::ostringstream out;
    out << SectionStart("Scale") << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>App-wide text size</div></div><select data-setting='ui_scale'>"
        << "<option value='0.75'" << Selected(std::fabs(m_config.ui_scale - 0.75f) < .01f) << ">75%</option>"
        << "<option value='0.8'" << Selected(std::fabs(m_config.ui_scale - 0.8f) < .01f) << ">80%</option>"
        << "<option value='0.85'" << Selected(std::fabs(m_config.ui_scale - 0.85f) < .01f) << ">85%</option>"
        << "<option value='0.9'" << Selected(std::fabs(m_config.ui_scale - 0.9f) < .01f) << ">90%</option>"
        << "<option value='0.95'" << Selected(std::fabs(m_config.ui_scale - 0.95f) < .01f) << ">95%</option>"
        << "<option value='1.0'" << Selected(std::fabs(m_config.ui_scale - 1.0f) < .01f) << ">100%</option>"
        << "<option value='1.1'" << Selected(std::fabs(m_config.ui_scale - 1.1f) < .01f) << ">110%</option>"
        << "<option value='1.25'" << Selected(std::fabs(m_config.ui_scale - 1.25f) < .01f) << ">125%</option>"
        << "<option value='1.5'" << Selected(std::fabs(m_config.ui_scale - 1.5f) < .01f) << ">150%</option></select></div>"
        << "<div class='setting-help'>Changes text size throughout the dashboard and overlay.</div>" << SectionEnd();
    out << SectionStart("Colors")
        << colorRow("theme_bg", "Overlay background", m_config.themeBg)
        << colorRow("theme_panel", "Settings panels", m_config.themeSettingsPanel)
        << colorRow("theme_text", "Text", m_config.themeText)
        << colorRow("theme_accent", "Accent", m_config.themeAccent)
        << colorRow("theme_win", "Win", m_config.themeWin)
        << colorRow("theme_loss", "Loss", m_config.themeLoss)
        << colorRow("theme_dim", "Dim text", m_config.themeDim)
        << colorRow("theme_muted", "Muted info", m_config.themeMuted)
        << colorRow("theme_graph", "Graph line", m_config.themeGraphLine)
        << colorRow("theme_baseline", "Graph baseline", m_config.themeGraphBaseline)
        << SectionEnd();

    if (const ColorRGBA* editing = ThemeColorForKey(m_config, m_editColorKey)) {
        const Hsv hsv = RgbToHsv(*editing);
        // A gray color carries no hue, so the picker keeps the last hue the user
        // chose instead of snapping the field back to red.
        const float hue = hsv.s > 0.0f ? hsv.h : m_editColorHue;
        const auto percent = [](float value) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(2) << std::clamp(value, 0.0f, 1.0f) * 100.0f;
            return text.str();
        };

        out << SectionStart(std::string("Color editor · ") + ThemeColorLabel(m_editColorKey))
            << "<div class='color-picker'>"
            << "<div class='color-field' data-action='color-field' style='decorator: image(gen://sv?h="
            << static_cast<int>(std::lround(hue / 2.0f)) * 2 << ");'>"
            << "<div class='color-field-marker' style='left:" << percent(hsv.s) << "%;top:" << percent(1.0f - hsv.v) << "%'></div>"
            << "</div>"
            << "<div class='color-hue' data-action='color-hue'>"
            << "<div class='color-hue-marker' style='left:" << percent(hue / 360.0f) << "%'></div>"
            << "</div></div>"
            << "<div class='color-editor-preview' style='background-color:" << CssColor(*editing) << "'></div>"
            << rangeRow("R", 'r', channelByte(editing->r))
            << rangeRow("G", 'g', channelByte(editing->g))
            << rangeRow("B", 'b', channelByte(editing->b))
            << rangeRow("A", 'a', channelByte(editing->a))
            << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("close-color-editor", "Done", "ghost") << "</div>"
            << SectionEnd();
    }

    out << SectionStart("Units & Formatting")
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Speed units</div></div><select data-setting='speed_units'><option value='metric'" << Selected(!m_config.imperial_units) << ">Kilometers per hour</option><option value='imperial'" << Selected(m_config.imperial_units) << ">Miles per hour</option></select></div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Crossbar hit display</div></div><select data-setting='crossbar_display_mode'><option value='raw'" << Selected(m_config.crossbar_display_mode == "raw") << ">Raw Force (UU/s)</option><option value='speed'" << Selected(m_config.crossbar_display_mode == "speed") << ">Speed (MPH/KPH)</option></select></div>"
        << ToggleControl("use_roman_numerals", "Use Roman numerals", "I, II, III in rank labels.", m_config.use_roman_numerals)
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsIntegrations() {
    if (m_pendingBallchasingToken.empty() && !m_config.ballchasing_token.empty()) m_pendingBallchasingToken = m_config.ballchasing_token;
    std::ostringstream out;
    out << SectionStart("Custom Rank API")
        << "<div class='setting-help'>Authenticate custom API rank lookups with the API key from your OmniStats account.</div>"
        << ToggleControl("custom_api_enabled", "Enable custom API fallback", "Used when Tracker.gg rank requests are blocked or unavailable.", m_config.custom_api_enabled)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API Key</div></div><input type='password' class='text' data-setting='custom_api_key' value='" << Escape(m_config.custom_api_key) << "'/></div>";
    if (m_config.custom_api_key.empty())
        out << "<div class='setting-help'>No key set. Sign in on the website and copy your API key from account settings.</div>";
    else
        out << "<div class='setting-help win'>API key saved and active.</div>";
    out << SectionEnd();
    out << SectionStart("Ballchasing Uploader")
        << "<div class='setting-help'>Upload saved replay files to your Ballchasing account. Add your API token from Ballchasing.com.</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API token</div></div><input type='" << (m_showBallchasingToken ? "text" : "password") << "' class='text' data-setting='ballchasing_token' value='" << Escape(m_pendingBallchasingToken) << "'/><button class='ghost' data-action='toggle-token'>" << (m_showBallchasingToken ? "Hide" : "Show") << "</button></div>";
    if (m_config.ballchasing_token.empty()) out << "<div class='setting-help loss'>An API token is required before replay uploads can succeed.</div>";
    out
        << ToggleControl("auto_upload_replays", "Auto-upload new replays", "Uploads saved replay files using the selected privacy level.", m_config.auto_upload_replays)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Upload visibility</div></div><select data-setting='ballchasing_visibility'>"
        << "<option value='private'" << Selected(m_config.ballchasing_visibility == "private") << ">private</option><option value='unlisted'" << Selected(m_config.ballchasing_visibility == "unlisted") << ">unlisted</option><option value='public'" << Selected(m_config.ballchasing_visibility == "public") << ">public</option></select></div>" << SectionEnd();
    out << SectionStart("Replay Saving")
        << ToggleControl("auto_save_replays", "Auto-save replays", "EAC-friendly: triggers your configured Rocket League save-replay key.", m_config.auto_save_replays);
    if (m_config.auto_save_replays) {
        const bool active = m_bindCaptureTarget == BindCaptureTarget::KeySaveReplay;
        const std::string bindTarget = std::to_string(static_cast<int>(BindCaptureTarget::KeySaveReplay));
        out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Save Replay Bind</div>"
            << "<div class='setting-help'>Matches the Save Replay bind configured in Rocket League.</div></div><div class='row gap-xs'>"
            << "<button data-action='capture-bind' data-bind='" << bindTarget << "'>"
            << (active ? "Press a key..." : Escape(GetKeyDisplayName(m_config.key_save_replay))) << "</button>"
            << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind")
            << "' data-bind='" << bindTarget << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
    }
    out << SectionEnd();
    out << SectionStart("External Services")
        << ToggleControl("discord_rpc_enabled", "Discord Rich Presence", "Changing this may require an app restart.", m_config.discord_rpc_enabled)
        << ToggleControl("enable_mmr_tracking", "Enable MMR tracking (Tracker.gg)", "Sends lobby identifiers to Tracker.gg for rank lookup.", m_config.enable_mmr_tracking)
        << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsData() {
    std::ostringstream out;
    out << SectionStart("Privacy & Diagnostics")
        << "<div class='setting-help'>Startup diagnostics are required after accepting the privacy notice. Match data and player names are not included.</div>"
        << ToggleControl("crash_reports_enabled", "Upload crash reports", "Pending minidumps are sent on the next startup.", m_config.crash_reports_enabled)
        << SectionEnd();
    out << SectionStart("Local Data")
        << "<div class='setting-help mono'>" << Escape(Storage::GetDataDirectory()) << "</div><div class='row gap-sm' style='margin-top:8dp'>"
        << Button("open-data-folder", "Open Data Folder") << Button("export-data", "Export Local Data") << Button("delete-history", "Delete History & Identity", "danger") << "</div>"
        << "<div class='setting-help' style='margin-top:8dp'>Deleting local history also clears the saved local account identity. Settings and service tokens are kept.</div>" << SectionEnd();
    return out.str();
}

std::string RmlUiController::RenderSettingsTroubleshooting() {
    StatsApiConfig::CheckResult result;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
    }
    std::ostringstream out;
    out << SectionStart("Rocket League Connection")
        << "<div class='setting-help'>OmniStats reads live stats through Rocket League's local Stats API.</div>"
        << ToggleControl("check_stats_api_config_on_startup", "Check Stats API config on startup", "", m_config.check_stats_api_config_on_startup)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Detected path</div><div class='setting-help mono'>" << Escape(result.path.empty() ? "(none)" : result.path) << "</div></div></div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Status</div><div class='setting-help'>" << Escape(StatsApiConfig::GetStatusMessage(result.status)) << "</div></div></div>";
    if (!result.message.empty() && result.status != StatsApiConfig::Status::Valid) out << "<div class='setting-help loss'>" << Escape(result.message) << "</div>";
    if (result.status != StatsApiConfig::Status::Valid && result.rlRunning) {
        out << "<div class='setting-help' style='color:" << CssColor(m_config.themeAccent) << "'>Restart Rocket League after fixing for changes to take effect.</div>";
    }
    out << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("statsapi-check", "Check Again") << (result.status != StatsApiConfig::Status::Valid && !result.path.empty() ? Button("statsapi-fix", "Fix Config", "primary") : "") << "</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Manual config path</div><div class='setting-help'>DefaultStatsAPI.ini</div></div><input class='text' style='width:300dp' data-setting='statsapi_path' value='" << Escape(m_config.rocket_league_stats_api_config_path) << "'/></div>";
    if (!m_statsApiPathError.empty()) out << "<div class='setting-help loss'>" << Escape(m_statsApiPathError) << "</div>";
    out << SectionEnd();
    out << SectionStart("Diagnostics & Logs")
        << ToggleControl("debug_logging", "Verbose debug logging", "Off by default; sensitive identifiers are redacted when disabled.", m_config.debug_logging)
        << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("show-log", "Show Log File") << "</div>" << SectionEnd();
    return out.str();
}

void RmlUiController::RebuildSettings() {
    float scrollTop = 0.0f;
    if (auto* root = Root("settings-root")) {
        if (auto* page = root->QuerySelector(".settings-page");
            page && page->GetAttribute<int>("data-page", -1) == static_cast<int>(m_settingsPage)) {
            scrollTop = page->GetScrollTop();
        }
    }
    std::ostringstream content;
    switch (m_settingsPage) {
    case SettingsPage::General:
        content << RenderSettingsGeneral();
        break;
    case SettingsPage::Cards:
        content << RenderSettingsCards();
        break;
    case SettingsPage::Ranks:
        content << RenderSettingsRanks();
        break;
    case SettingsPage::Shortcuts:
        content << RenderSettingsShortcuts();
        break;
    case SettingsPage::Appearance:
        content << RenderSettingsAppearance();
        break;
    case SettingsPage::Integrations:
        content << RenderSettingsIntegrations();
        break;
    case SettingsPage::Data:
        content << RenderSettingsData();
        break;
    case SettingsPage::Troubleshooting:
        content << RenderSettingsTroubleshooting();
        break;
    }

    const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
    const float logicalWidth = static_cast<float>(m_width) / std::max(rmlScale, 0.01f);
    const bool compactViewport = logicalWidth <= 850.0f;
    const float settingsWidth = compactViewport ? static_cast<float>(m_width) * 0.96f
                                                : std::min(900.0f * rmlScale, static_cast<float>(m_width) * 0.94f);
    const float settingsHeight = compactViewport ? static_cast<float>(m_height) * 0.94f
                                                 : std::min(650.0f * rmlScale, static_cast<float>(m_height) * 0.92f);
    if (!m_settingsPositioned) {
        m_settingsX = std::max(0.0f, (static_cast<float>(m_width) - settingsWidth) * 0.5f);
        m_settingsY = std::max(0.0f, (static_cast<float>(m_height) - settingsHeight) * 0.5f);
    } else {
        m_settingsX = std::clamp(m_settingsX, 0.0f, std::max(0.0f, static_cast<float>(m_width) - settingsWidth));
        m_settingsY = std::clamp(m_settingsY, 0.0f, std::max(0.0f, static_cast<float>(m_height) - settingsHeight));
    }
    const float settingsLeftDp = m_settingsX / std::max(rmlScale, 0.01f);
    const float settingsTopDp = m_settingsY / std::max(rmlScale, 0.01f);

    std::ostringstream out;
    out << "<div class='settings-window' style='left:" << settingsLeftDp
        << "dp;top:" << settingsTopDp << "dp'><div class='settings-header row' data-action='settings-drag'>"
        << "<div class='grow'><div class='brand-title'>Settings</div><div class='label'>Changes apply automatically.</div></div>";
    if (m_state && m_state->ui.updateAvailable.load()) {
        std::string ver;
        {
            std::lock_guard lock(m_state->ui.updateMutex);
            ver = m_state->ui.updateAvailableVersion;
        }
        const bool failed = m_state->ui.updateDownloadFailed.load();
        const bool downloading = m_state->ui.updateDownloading.load();
        std::string label = downloading ? (ver.empty() ? "Starting updater..." : "Starting v" + ver + "...")
                            : failed    ? (ver.empty() ? "Retry Update" : "Retry Update v" + ver)
                                        : (ver.empty() ? "Update Available" : "Update Available: v" + ver);
        out << Button("update-app", label, "primary");
    }
    out << "</div><div class='settings-body'><div class='settings-nav'>";
    for (int i = 0; i < 8; ++i) {
        const auto page = static_cast<SettingsPage>(i);
        out << "<button class='" << (page == m_settingsPage ? "active" : "") << "' data-action='settings-page' data-page='" << i << "'>" << SettingsPageName(page) << "</button>";
    }
    out << "</div><div class='settings-page' data-page='" << static_cast<int>(m_settingsPage) << "'>" << content.str() << "</div></div><div class='settings-footer'>" << Button("help-discord", "Help / Discord", "ghost") << Button("close-settings", "Done", "primary") << "</div></div>";

    if (m_confirmReplayUploads) {
        out << "<div class='confirm-backdrop'><div class='card privacy-dialog'><div class='card-title'>Replay Upload Privacy Warning</div>"
            << "<div class='setting-help'>Rocket League replay files can include player names, platform IDs, match timestamps, teams, scores, gameplay events, and other participants in the match.</div>"
            << "<div class='setting-help' style='margin-top:8dp'>Only enable this if you understand that replay data leaves your PC and is handled by Ballchasing under its own terms and privacy policy.</div>"
            << "<div class='setting-help' style='margin-top:8dp'>Current visibility: <b>" << Escape(m_config.ballchasing_visibility) << "</b></div>";
        if (m_config.ballchasing_token.empty()) out << "<div class='setting-help loss' style='margin-top:8dp'>Add a Ballchasing API token before uploads can succeed.</div>";
        out << "<div class='row gap-sm' style='margin-top:12dp'>" << Button("confirm-replay-upload", "Enable Replay Uploads", "primary") << Button("cancel-replay-upload", "Cancel", "ghost") << "</div></div></div>";
    }
    if (m_confirmDeleteHistory) {
        out << "<div class='confirm-backdrop'><div class='card' style='width:470dp'><div class='card-title loss'>Delete local history and saved identity?</div><div class='setting-help'>This removes local match history and your selected account identity. Settings and service tokens are kept. This cannot be undone.</div><div class='row gap-sm' style='margin-top:12dp'>" << Button("confirm-delete-history", "Delete", "danger") << Button("cancel-delete-history", "Cancel", "ghost") << "</div></div></div>";
    }
    SetRootRml("settings-root", out.str());
    if (scrollTop > 0.0f && m_document) {
        m_document->UpdateDocument();
        if (auto* root = Root("settings-root")) {
            if (auto* page = root->QuerySelector(".settings-page")) page->SetScrollTop(scrollTop);
        }
    }
}

void RmlUiController::RebuildToast() {
    if (m_statusMessage.empty() || (m_statusUntilMs && SteadyNowMs() >= m_statusUntilMs)) {
        m_statusMessage.clear();
        SetRootRml("toast-root", "");
        return;
    }
    SetRootRml("toast-root", "<div class='toast " + std::string(m_statusError ? "error" : "success") + "'>" + Escape(m_statusMessage) + "</div>");
}

void RmlUiController::ShowToast(std::string message, bool error) {
    m_statusMessage = std::move(message);
    m_statusError = error;
    m_statusUntilMs = SteadyNowMs() + 5000;
    RebuildToast();
}

std::string RmlUiController::ControlValue(Rml::Element* target) const {
    if (!target) return {};
    if (auto* control = dynamic_cast<Rml::ElementFormControl*>(target)) return control->GetValue().c_str();
    return Attribute(target, "value");
}

bool RmlUiController::EventChecked(Rml::Event& event, Rml::Element* target) const {
    return event.GetParameter<bool>("checked", target && target->HasAttribute("checked"));
}

void RmlUiController::ProcessEvent(Rml::Event& event) {
    if (m_rebuildingUi) return;
    Rml::Element* target = event.GetTargetElement();
    if (!target) return;
    const std::string type = event.GetType().c_str();
    if (type == "mousedown") m_pointerPressed = true;
    if (type == "click")
        HandleClick(target);
    else if (type == "change")
        HandleChange(target, event);
    else if (type == "input")
        HandleInput(target);
    else if (type == "blur" && Attribute(target, "data-setting") == "statsapi_path")
        HandleChange(target, event);
    else if (type == "mousedown")
        HandleMouseDown(target, event);
    else if (type == "mousemove")
        HandleMouseMove(event);
    else if (type == "mouseup")
        HandleMouseUp(event);
}

void RmlUiController::HandleClick(Rml::Element* target) {
    Rml::Element* actionTarget = target;
    std::string action;
    while (actionTarget && action.empty()) {
        action = Attribute(actionTarget, "data-action");
        if (action.empty()) actionTarget = actionTarget->GetParentNode();
    }
    if (!actionTarget || action.empty()) return;
    target = actionTarget;

    if (action == "overlay-add-widget") {
        const auto widget = WidgetFromDom(Attribute(target, "data-widget"));
        const float screenWidth = static_cast<float>(m_width);
        const float screenHeight = static_cast<float>(m_height);
        const auto [defaultWidth, defaultHeight] = OverlayWidgetDefaultSize(widget, m_dpiScale);
        Config::Update([=](ConfigData& c) {
            bool alreadyPresent = false;
            for (const auto& container : c.overlay_layout.containers) {
                if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) {
                    alreadyPresent = true;
                    break;
                }
            }
            if (alreadyPresent) return;
            OverlayLayout::ContainerConfig container;
            container.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
            container.x = std::max(12.0f * SanitizedScale(m_dpiScale), screenWidth * 0.5f - defaultWidth * 0.5f);
            container.y = std::max(12.0f * SanitizedScale(m_dpiScale), screenHeight * 0.5f - defaultHeight * 0.5f);
            container.w = defaultWidth;
            container.h = defaultHeight;
            container.widgets = {widget};
            c.overlay_layout.containers.push_back(std::move(container));
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-remove-widget") {
        const auto widget = WidgetFromDom(Attribute(target, "data-widget"));
        const std::string sourceId = Attribute(target, "data-container");
        Config::Update([&](ConfigData& c) {
            for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                if (it->id != sourceId) continue;
                it->widgets.erase(std::remove(it->widgets.begin(), it->widgets.end(), widget), it->widgets.end());
                if (it->widgets.empty()) c.overlay_layout.containers.erase(it);
                break;
            }
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-remove-container") {
        const std::string sourceId = Attribute(target, "data-container");
        Config::Update([&](ConfigData& c) {
            c.overlay_layout.containers.erase(
                std::remove_if(c.overlay_layout.containers.begin(), c.overlay_layout.containers.end(),
                               [&](const auto& container) { return container.id == sourceId; }),
                c.overlay_layout.containers.end());
            OverlayLayout::Sanitize(c.overlay_layout);
        },
                       true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "window-minimize") {
        if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
    } else if (action == "window-maximize") {
        if (m_hwnd) ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "window-close") {
        if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    } else if (action == "settings-page") {
        FinishBindCapture();
        m_showBallchasingToken = false;
        const int page = std::clamp(std::atoi(Attribute(target, "data-page").c_str()), 0, 7);
        m_settingsPage = static_cast<SettingsPage>(page);
        RebuildSettings();
    } else if (action == "close-settings") {
        FinishBindCapture();
        if (m_state) {
            if (m_state->ui.dashboardLayoutEditMode.exchange(false)) Config::RequestSave();
            m_state->ui.showMenu.store(false);
        }
        SetRootRml("settings-root", "");
        RebuildOverlay();
        if (m_config.second_monitor_mode) RebuildDashboard();
    } else if (action == "open-settings") {
        if (m_state) {
            m_state->ui.showMenu.store(true);
            m_state->ui.dashboardLayoutEditMode.store(false);
        }
        RebuildSettings();
        RebuildOverlay();
        if (m_config.second_monitor_mode) RebuildDashboard();
    } else if (action == "help-discord") {
        ShellExecuteA(nullptr, "open", "https://discord.gg/4KBW35ApvF", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "open-player-tracker") {
        const std::string url = Attribute(target, "data-url");
        if (url.rfind("https://rocketleague.tracker.network/rocket-league/profile/", 0) == 0) {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == "update-app") {
        m_showUpdatePrompt = false;
        if (m_state && !m_state->ui.updateDownloading.load()) ExternalUpdaterLauncher::StartInteractiveUpdate(m_state);
        if (m_config.second_monitor_mode) RebuildDashboard();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    } else if (action == "dismiss-update") {
        m_showUpdatePrompt = false;
        RebuildDashboard();
    } else if (action == "dashboard-edit") {
        if (m_state) {
            const bool enabling = !m_state->ui.dashboardLayoutEditMode.load();
            m_state->ui.dashboardLayoutEditMode.store(enabling);
            if (!enabling) Config::RequestSave();
        }
        RebuildDashboard();
        RebuildOverlay();
    } else if (action == "reset-dashboard") {
        Config::Update([](ConfigData& c) { c.dashboard_layout = DashboardLayout::DefaultLayout(); });
        m_config = Config::Read();
        RebuildDashboard();
        RebuildSettings();
        ShowToast("Dashboard layout reset.");
    } else if (action == "overlay-toggle-toolbox") {
        Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = !c.overlay_layout.toolboxOpen; }, true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-close-toolbox") {
        Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "reset-overlay") {
        Config::Update([](ConfigData& c) { c.overlay_layout = OverlayLayout::DefaultOverlayLayout(); });
        m_config = Config::Read();
        RebuildOverlay();
        RebuildSettings();
        ShowToast("Overlay layout reset.");
    } else if (action == "graph-pan-older") {
        PanGraph(-1);
    } else if (action == "graph-pan-newer") {
        PanGraph(1);
    } else if (action == "capture-bind") {
        const int value = std::atoi(Attribute(target, "data-bind").c_str());
        if (value >= static_cast<int>(BindCaptureTarget::KeyOverlay) && value <= static_cast<int>(BindCaptureTarget::GamepadGraphPanRight) &&
            m_bindCaptureTarget != static_cast<BindCaptureTarget>(value)) {
            BeginBindCapture(static_cast<BindCaptureTarget>(value));
        }
        RebuildSettings();
    } else if (action == "cancel-bind") {
        FinishBindCapture();
        RebuildSettings();
    } else if (action == "clear-bind") {
        const int value = std::atoi(Attribute(target, "data-bind").c_str());
        if (value >= static_cast<int>(BindCaptureTarget::KeyOverlay) && value <= static_cast<int>(BindCaptureTarget::GamepadGraphPanRight)) ClearBind(static_cast<BindCaptureTarget>(value));
        m_config = Config::Read();
        RebuildSettings();
    } else if (action == "toggle-token") {
        m_showBallchasingToken = !m_showBallchasingToken;
        RebuildSettings();
    } else if (action == "edit-color") {
        const std::string key = Attribute(target, "data-color-key");
        m_editColorKey = ThemeColorForKey(m_config, key) ? key : std::string{};
        RebuildSettings();
    } else if (action == "close-color-editor") {
        m_editColorKey.clear();
        RebuildSettings();
    } else if (action == "confirm-replay-upload") {
        Config::Update([](ConfigData& c) { c.ballchasing_upload_notice_accepted = true; c.auto_upload_replays = true; });
        m_confirmReplayUploads = false;
        m_config = Config::Read();
        RebuildSettings();
    } else if (action == "cancel-replay-upload") {
        m_confirmReplayUploads = false;
        RebuildSettings();
    } else if (action == "open-data-folder") {
        const auto path = Storage::GetDataDirectory();
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "export-data") {
        if (!m_dbManager) {
            ShowToast("Export failed: database is unavailable.", true);
        } else {
            std::string exportPath, error;
            if (m_dbManager->ExportLocalData(exportPath, error)) {
                ShowToast("Exported local history.");
                const std::string arg = "/select,\"" + exportPath + "matches.json\"";
                ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            } else
                ShowToast("Export failed: " + error, true);
        }
    } else if (action == "delete-history") {
        m_confirmDeleteHistory = true;
        RebuildSettings();
    } else if (action == "cancel-delete-history") {
        m_confirmDeleteHistory = false;
        RebuildSettings();
    } else if (action == "confirm-delete-history") {
        DeleteLocalHistory();
        m_confirmDeleteHistory = false;
        m_config = Config::Read();
        SnapshotState();
        RebuildSettings();
        RebuildVisibleUi(true);
    } else if (action == "show-log") {
        const std::string path = Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
        const std::string arg = "/select,\"" + path + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (action == "statsapi-check") {
        CheckStatsApi(false);
        RebuildSettings();
    } else if (action == "statsapi-fix") {
        CheckStatsApi(true);
        RebuildSettings();
    }
}

void RmlUiController::PanGraph(int direction) {
    if (!m_state || direction == 0) return;
    const auto category = m_state->ui.graphMmrCategory.load();
    const std::string playlist = MmrCategoryToString(category);
    int total = 0;
    if (m_snap.showLifetimeGraph) {
        total = static_cast<int>(m_snap.lifetimeMmrY.size());
    } else {
        if (auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) {
            total = static_cast<int>(it->second.size());
        }
    }
    constexpr int kVisibleMatches = 25;
    if (total <= kVisibleMatches) return;
    constexpr int step = 5;
    const int maxOffset = total - kVisibleMatches;
    const int currentOffset = m_state->ui.graphOffset.load();
    const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
    m_state->ui.graphOffset.store(newOffset);
    RebuildVisibleUi(false);
}
void RmlUiController::HandleInput(Rml::Element* target) {
    const std::string key = Attribute(target, "data-setting");
    if (key.empty()) return;

    // RmlUi text and range controls emit change events during editing.
    // Update their values without replacing the focused control.
    const std::string value = ControlValue(target);
    bool themeChanged = false;
    auto parseColor = [](std::string text, ColorRGBA& out) {
        if (!text.empty() && text.front() == '#') text.erase(text.begin());
        if (text.size() != 6 && text.size() != 8) return false;
        unsigned long parsed = 0;
        size_t consumed = 0;
        try {
            parsed = std::stoul(text, &consumed, 16);
        } catch (...) {
            return false;
        }
        if (consumed != text.size()) return false;
        if (text.size() == 6) parsed = (parsed << 8) | 0xffu;
        out.r = ((parsed >> 24) & 0xffu) / 255.0f;
        out.g = ((parsed >> 16) & 0xffu) / 255.0f;
        out.b = ((parsed >> 8) & 0xffu) / 255.0f;
        out.a = (parsed & 0xffu) / 255.0f;
        return true;
    };

    if (key == "ballchasing_token") {
        m_pendingBallchasingToken = value;
        Config::Update([&](ConfigData& c) { c.ballchasing_token = value; });
    } else if (key == "custom_api_key") {
        Config::Update([&](ConfigData& c) { c.custom_api_key = value; });
    } else if (key == "statsapi_path") {
        // Validate the completed path on blur, not after each character.
        return;
    } else if (key.rfind("theme_", 0) == 0) {
        std::string_view componentColorKey;
        char component = 0;
        if (ParseThemeComponentKey(key, componentColorKey, component)) {
            Config::Update([&](ConfigData& c) { themeChanged = SetThemeComponent(c, key, value); });
            if (!themeChanged) return;
        } else {
            ColorRGBA color;
            if (!parseColor(value, color)) return;
            Config::Update([&](ConfigData& c) {
                if (ColorRGBA* targetColor = ThemeColorForKey(c, key)) {
                    *targetColor = color;
                    themeChanged = true;
                }
            });
        }
    } else {
        return;
    }

    m_config = Config::Read();
    if (themeChanged) UpdateThemeProperties();
}

void RmlUiController::HandleChange(Rml::Element* target, Rml::Event& event) {
    const std::string key = Attribute(target, "data-setting");
    if (key.empty()) return;
    if (event.GetType() != "blur" &&
        (key == "ballchasing_token" || key == "custom_api_key" || key == "statsapi_path" || key.rfind("theme_", 0) == 0)) {
        HandleInput(target);
        return;
    }
    std::string value = ControlValue(target);
    const bool checked = EventChecked(event, target);

    if (key == "overlay_edit_mode") {
        if (m_state) m_state->ui.dashboardLayoutEditMode.store(checked);
        if (checked)
            Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
        else
            Config::RequestSave();
        m_config = Config::Read();
        RebuildSettings();
        RebuildDashboard();
        RebuildOverlay();
        return;
    }
    if (key == "auto_upload_replays" && checked && !m_config.ballchasing_upload_notice_accepted) {
        m_confirmReplayUploads = true;
        RebuildSettings();
        return;
    }
    if (key == "statsapi_path") {
        std::string normalized;
        if (!ValidateStatsApiPath(value, normalized, m_statsApiPathError)) {
            RebuildSettings();
            return;
        }
        value = std::move(normalized);
        m_statsApiPathError.clear();
    }

    auto parseColor = [](std::string text, ColorRGBA& out) {
        if (!text.empty() && text.front() == '#') text.erase(text.begin());
        if (text.size() != 6 && text.size() != 8) return false;
        unsigned long value = 0;
        size_t consumed = 0;
        try {
            value = std::stoul(text, &consumed, 16);
        } catch (...) {
            return false;
        }
        if (consumed != text.size()) return false;
        if (text.size() == 6) value = (value << 8) | 0xffu;
        out.r = ((value >> 24) & 0xffu) / 255.0f;
        out.g = ((value >> 16) & 0xffu) / 255.0f;
        out.b = ((value >> 8) & 0xffu) / 255.0f;
        out.a = (value & 0xffu) / 255.0f;
        return true;
    };

    bool windowChanged = false;
    bool styleChanged = false;
    Config::Update([&](ConfigData& c) {
        if (key == "require_rl_focus")
            c.require_rl_focus = checked;
        else if (key == "second_monitor_mode") {
            c.second_monitor_mode = checked;
            windowChanged = true;
        } else if (key == "second_monitor_show_roster")
            c.second_monitor_show_roster = checked;
        else if (key == "second_monitor_show_session")
            c.second_monitor_show_session = checked;
        else if (key == "show_match_summary")
            c.show_match_summary = checked;
        else if (key == "show_running_indicator")
            c.show_running_indicator = checked;
        else if (key == "reset_session_on_close")
            c.reset_session_on_close = checked;
        else if (key == "run_on_startup")
            c.run_on_startup = checked;
        else if (key == "check_for_updates") {
            c.check_for_updates = checked;
            if (!checked) c.enable_auto_updates = false;
        } else if (key == "enable_auto_updates") {
            c.enable_auto_updates = checked;
            if (checked) c.check_for_updates = true;
        } else if (key == "identity") {
            c.last_primary_id = value;
            if (!value.empty() && std::find(c.known_primary_ids.begin(), c.known_primary_ids.end(), value) == c.known_primary_ids.end()) c.known_primary_ids.push_back(value);
        } else if (key == "show_session_record")
            c.show_session_record = checked;
        else if (key == "show_session_goals")
            c.show_session_goals = checked;
        else if (key == "show_session_saves")
            c.show_session_saves = checked;
        else if (key == "show_session_demos")
            c.show_session_demos = checked;
        else if (key == "show_session_boost")
            c.show_session_boost = checked;
        else if (key == "show_session_assists")
            c.show_session_assists = checked;
        else if (key == "show_session_goal_participation")
            c.show_session_goal_participation = checked;
        else if (key == "show_session_mmr_change")
            c.show_session_mmr_change = checked;
        else if (key == "use_rank_icons")
            c.use_rank_icons = checked;
        else if (key == "show_lobby_ranks_overlay") {
            c.show_lobby_ranks_overlay = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::LobbyRanks) w.zone = checked ? DashboardLayout::Zone::Left : DashboardLayout::Zone::Hidden;
        } else if (key == "show_account_wins_overlay")
            c.show_account_wins_overlay = checked;
        else if (key == "show_demo_tracker_overlay") {
            c.show_demo_tracker_overlay = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::DemoTracker) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_previous_games_summary") {
            c.show_previous_games_summary = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::PreviousGames) w.zone = checked ? DashboardLayout::Zone::Top : DashboardLayout::Zone::Hidden;
        } else if (key == "show_streaks_stats") {
            c.show_streaks_stats = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::StreaksStats) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_gamemode_breakdown") {
            c.show_gamemode_breakdown = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::GamemodeBreakdown) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_lobby_rank_1v1")
            c.show_lobby_rank_1v1 = checked;
        else if (key == "show_lobby_rank_2v2")
            c.show_lobby_rank_2v2 = checked;
        else if (key == "show_lobby_rank_3v3")
            c.show_lobby_rank_3v3 = checked;
        else if (key == "show_lobby_rank_casual")
            c.show_lobby_rank_casual = checked;
        else if (key == "show_lobby_rank_tourny")
            c.show_lobby_rank_tourny = checked;
        else if (key == "show_lobby_rank_hoops")
            c.show_lobby_rank_hoops = checked;
        else if (key == "show_lobby_rank_rumble")
            c.show_lobby_rank_rumble = checked;
        else if (key == "show_lobby_rank_dropshot")
            c.show_lobby_rank_dropshot = checked;
        else if (key == "show_lobby_rank_snowday")
            c.show_lobby_rank_snowday = checked;
        else if (key == "show_lobby_rank_heatseeker")
            c.show_lobby_rank_heatseeker = checked;
        else if (key == "previous_games_limit")
            c.previous_games_limit = std::clamp(std::atoi(value.c_str()), 10, 50);
        else if (key == "show_longest_loss_streak")
            c.show_longest_loss_streak = checked;
        else if (key == "show_gamemode_record_1v1")
            c.show_gamemode_record_1v1 = checked;
        else if (key == "show_gamemode_record_2v2")
            c.show_gamemode_record_2v2 = checked;
        else if (key == "show_gamemode_record_3v3")
            c.show_gamemode_record_3v3 = checked;
        else if (key == "gamemode_breakdown_scope")
            c.gamemode_breakdown_scope = value;
        else if (key == "mmr_category")
            c.mmr_category = value;
        else if (key == "auto_switch_mmr_category")
            c.auto_switch_mmr_category = checked;
        else if (key == "show_extra_playlists") {
            c.show_extra_playlists = checked;
            if (!checked && IsExtraMmrCategory(StringToMmrCategory(c.mmr_category))) c.mmr_category = "best";
            if (!checked && IsExtraMmrCategory(StringToMmrCategory(c.graph_mmr_category))) c.graph_mmr_category = "2v2";
        } else if (key == "graph_follow_current_playlist")
            c.graph_follow_current_playlist = checked;
        else if (key == "graph_mmr_category")
            c.graph_mmr_category = value;
        else if (key == "ui_scale")
            c.ui_scale = SanitizedUiScale(std::strtof(value.c_str(), nullptr));
        else if (key == "speed_units")
            c.imperial_units = value == "imperial";
        else if (key == "crossbar_display_mode")
            c.crossbar_display_mode = value == "speed" ? "speed" : "raw";
        else if (key == "use_roman_numerals")
            c.use_roman_numerals = checked;
        else if (key == "custom_api_enabled")
            c.custom_api_enabled = checked;
        else if (key == "custom_api_key")
            c.custom_api_key = value;
        else if (key == "ballchasing_token") {
            c.ballchasing_token = value;
            m_pendingBallchasingToken = value;
        } else if (key == "auto_upload_replays")
            c.auto_upload_replays = checked;
        else if (key == "ballchasing_visibility")
            c.ballchasing_visibility = value;
        else if (key == "auto_save_replays")
            c.auto_save_replays = checked;
        else if (key == "discord_rpc_enabled")
            c.discord_rpc_enabled = checked;
        else if (key == "enable_mmr_tracking")
            c.enable_mmr_tracking = checked;
        else if (key == "crash_reports_enabled")
            c.crash_reports_enabled = checked;
        else if (key == "check_stats_api_config_on_startup")
            c.check_stats_api_config_on_startup = checked;
        else if (key == "debug_logging")
            c.debug_logging = checked;
        else if (key == "statsapi_path")
            c.rocket_league_stats_api_config_path = value;
        else if (key.rfind("theme_", 0) == 0) {
            std::string_view componentColorKey;
            char component = 0;
            if (ParseThemeComponentKey(key, componentColorKey, component)) {
                styleChanged = SetThemeComponent(c, key, value);
            } else {
                ColorRGBA parsed;
                if (parseColor(value, parsed)) {
                    if (ColorRGBA* targetColor = ThemeColorForKey(c, key)) {
                        *targetColor = parsed;
                        styleChanged = true;
                    }
                }
            }
        }

        // The rank table is a fixed-column layout, so enabling or disabling a
        // playlist changes how much room it needs. Drop the persisted size and
        // let the container re-fit to the new column count.
        if (key.rfind("show_lobby_rank_", 0) == 0 || key == "show_extra_playlists") {
            for (auto& container : c.overlay_layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
        }
    });

    if (key == "run_on_startup") Config::SetWindowsAutoStart(checked);
    if (key == "identity" && m_state) {
        {
            std::unique_lock lock(m_state->game.mutex);
            m_state->game.myPrimaryId = value;
            m_state->game.myTeam = -1;
            if (!value.empty()) {
                if (auto it = m_state->game.roster.find(value); it != m_state->game.roster.end()) m_state->game.myTeam = it->second.team;
            }
            m_state->game.version.fetch_add(1, std::memory_order_relaxed);
        }
        // Force account-scoped async data to refresh for the newly selected identity.
        m_lastDbFetchPrimaryId.clear();
        m_lastLifetimeHistoryPrimaryId.clear();
        m_lastRecentMatchHistoryPrimaryId.clear();
    }
    if (key == "mmr_category" && m_state) m_state->ui.rosterMmrCategory.store(StringToMmrCategory(value));
    if (key == "graph_mmr_category" && m_state) m_state->ui.graphMmrCategory.store(StringToMmrCategory(value));
    if (key == "graph_follow_current_playlist" && !checked && m_state) m_state->ui.graphMmrCategory.store(StringToMmrCategory(m_config.graph_mmr_category));
    if (key == "show_extra_playlists" && !checked && m_state) {
        if (IsExtraMmrCategory(m_state->ui.rosterMmrCategory.load())) m_state->ui.rosterMmrCategory.store(MmrCategory::Best);
        if (IsExtraMmrCategory(m_state->ui.graphMmrCategory.load())) m_state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    }
    if (key == "statsapi_path") CheckStatsApi(false);

    m_config = Config::Read();
    if (target && target->GetTagName() == "select") {
        // Changing a <select> in settings dispatches a synchronous `change`
        // event from inside WidgetDropDown::ProcessEvent. Rebuilding the DOM
        // here would destroy the <select> before WidgetDropDown finishes
        // closing its selection box, causing a use-after-free crash.
        // Defer any scale update and UI rebuild to the next Update() cycle.
        return;
    }
    if (styleChanged) {
        SetDpiScale(m_dpiScale);
        UpdateThemeProperties();
    }
    if (windowChanged) ShowToast("Window mode changed.");
    RebuildVisibleUi(true);
}

void RmlUiController::BeginBindCapture(BindCaptureTarget target) {
    if (!m_state) return;
    if (m_state->ui.showMenu.load()) m_lastShowMenu = true;
    m_bindCaptureTarget = target;
    m_state->ui.inputCaptureActive.store(true);
    m_state->ui.lastKeyboardKeyPressed.store(-1);
    m_state->ui.lastControllerButtonPressed.store(-1);
    m_state->ui.lastRawControllerButtonPressed.store(-1);
}

void RmlUiController::FinishBindCapture() {
    m_bindCaptureTarget = BindCaptureTarget::None;
    if (m_state) m_state->ui.inputCaptureActive.store(false);
}

void RmlUiController::ClearBind(BindCaptureTarget target) {
    if (target == BindCaptureTarget::KeyMenu) return;
    Config::Update([target](ConfigData& c) {
        switch (target) {
        case BindCaptureTarget::KeyOverlay:
            c.key_overlay = -1;
            break;
        case BindCaptureTarget::KeyCycle:
            c.key_cycle = -1;
            break;
        case BindCaptureTarget::KeyExpand:
            c.key_expand = -1;
            break;
        case BindCaptureTarget::KeySession:
            c.key_session = -1;
            break;
        case BindCaptureTarget::KeySaveReplay:
            c.key_save_replay = -1;
            break;
        case BindCaptureTarget::KeyGraphPanLeft:
            c.key_graph_pan_left = -1;
            break;
        case BindCaptureTarget::KeyGraphPanRight:
            c.key_graph_pan_right = -1;
            break;
        case BindCaptureTarget::GamepadOverlay:
            c.gamepad_overlay = -1;
            c.gamepad_overlay_raw = false;
            c.gamepad_overlay_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadCycle:
            c.gamepad_cycle = -1;
            c.gamepad_cycle_raw = false;
            c.gamepad_cycle_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadExpand:
            c.gamepad_expand = -1;
            c.gamepad_expand_raw = false;
            c.gamepad_expand_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadSession:
            c.gamepad_session = -1;
            c.gamepad_session_raw = false;
            c.gamepad_session_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadMenu:
            c.gamepad_menu = -1;
            c.gamepad_menu_raw = false;
            c.gamepad_menu_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanLeft:
            c.gamepad_graph_pan_left = -1;
            c.gamepad_graph_pan_left_raw = false;
            c.gamepad_graph_pan_left_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanRight:
            c.gamepad_graph_pan_right = -1;
            c.gamepad_graph_pan_right_raw = false;
            c.gamepad_graph_pan_right_raw_button = -1;
            break;
        default:
            break;
        }
    });
}

void RmlUiController::UpdateInputCapture() {
    if (!m_state || m_bindCaptureTarget == BindCaptureTarget::None) return;
    const int vk = m_state->ui.lastKeyboardKeyPressed.load();
    if (vk == VK_ESCAPE) {
        FinishBindCapture();
        RebuildSettings();
        return;
    }
    const bool controllerTarget = m_bindCaptureTarget >= BindCaptureTarget::GamepadOverlay;
    if (controllerTarget) {
        const bool mapped = m_state->ui.controllerIsGameController.load();
        const int mappedButton = m_state->ui.lastControllerButtonPressed.load();
        const int rawButton = m_state->ui.lastRawControllerButtonPressed.load();
        if ((mapped && mappedButton >= 0) || rawButton >= 0) {
            const auto target = m_bindCaptureTarget;
            Config::Update([=](ConfigData& c) {
                const bool useRaw = !(mapped && mappedButton >= 0);
                const int button = useRaw ? rawButton : mappedButton;
                auto set = [&](int& normal, bool& raw, int& rawValue) { raw = useRaw; if (useRaw) rawValue = button; else normal = button; };
                switch (target) {
                case BindCaptureTarget::GamepadOverlay:
                    set(c.gamepad_overlay, c.gamepad_overlay_raw, c.gamepad_overlay_raw_button);
                    break;
                case BindCaptureTarget::GamepadCycle:
                    set(c.gamepad_cycle, c.gamepad_cycle_raw, c.gamepad_cycle_raw_button);
                    break;
                case BindCaptureTarget::GamepadExpand:
                    set(c.gamepad_expand, c.gamepad_expand_raw, c.gamepad_expand_raw_button);
                    break;
                case BindCaptureTarget::GamepadSession:
                    set(c.gamepad_session, c.gamepad_session_raw, c.gamepad_session_raw_button);
                    break;
                case BindCaptureTarget::GamepadMenu:
                    set(c.gamepad_menu, c.gamepad_menu_raw, c.gamepad_menu_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanLeft:
                    set(c.gamepad_graph_pan_left, c.gamepad_graph_pan_left_raw, c.gamepad_graph_pan_left_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanRight:
                    set(c.gamepad_graph_pan_right, c.gamepad_graph_pan_right_raw, c.gamepad_graph_pan_right_raw_button);
                    break;
                default:
                    break;
                }
            });
            FinishBindCapture();
            m_config = Config::Read();
            RebuildSettings();
        }
    } else if (vk > 0) {
        const auto target = m_bindCaptureTarget;
        Config::Update([=](ConfigData& c) {
            switch (target) {
            case BindCaptureTarget::KeyOverlay:
                c.key_overlay = vk;
                break;
            case BindCaptureTarget::KeyCycle:
                c.key_cycle = vk;
                break;
            case BindCaptureTarget::KeyExpand:
                c.key_expand = vk;
                break;
            case BindCaptureTarget::KeySession:
                c.key_session = vk;
                break;
            case BindCaptureTarget::KeyMenu:
                c.key_menu = vk;
                break;
            case BindCaptureTarget::KeySaveReplay:
                c.key_save_replay = vk;
                break;
            case BindCaptureTarget::KeyGraphPanLeft:
                c.key_graph_pan_left = vk;
                break;
            case BindCaptureTarget::KeyGraphPanRight:
                c.key_graph_pan_right = vk;
                break;
            default:
                break;
            }
        });
        FinishBindCapture();
        m_config = Config::Read();
        RebuildSettings();
    }
}

void RmlUiController::HandleMouseDown(Rml::Element* target, Rml::Event& event) {
    if (target) {
        std::string tag = target->GetTagName();
        std::string targetAction = Attribute(target, "data-action");
        if (tag == "button" || targetAction == "overlay-remove-widget" || targetAction == "overlay-remove-container") {
            return;
        }
    }

    auto isDragAction = [](const std::string& a) {
        return a == "dashboard-drag" || a == "overlay-toolbox-drag" ||
               a == "overlay-widget-drag" || a == "settings-drag" ||
               a == "overlay-drag" || a == "overlay-resize" ||
               a == "floating-card-drag" || a == "color-field" || a == "color-hue";
    };

    Rml::Element* actionElement = target;
    std::string action = Attribute(actionElement, "data-action");
    while (!isDragAction(action) && actionElement && actionElement->GetParentNode()) {
        actionElement = actionElement->GetParentNode();
        action = Attribute(actionElement, "data-action");
    }
    if (action == "dashboard-drag") {
        m_drag = {};
        m_drag.kind = DragKind::DashboardWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-toolbox-drag") {
        m_drag = {};
        m_drag.kind = DragKind::OverlayToolboxWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-widget-drag") {
        m_drag = {};
        m_drag.kind = DragKind::OverlayWidget;
        m_drag.widget = WidgetFromDom(Attribute(actionElement, "data-widget"));
        m_drag.sourceContainerId = Attribute(actionElement, "data-container");
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_systemInterface.LockCursor("move");
    } else if (action == "settings-drag") {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        const float logicalWidth = static_cast<float>(m_width) / std::max(rmlScale, 0.01f);
        const bool compactViewport = logicalWidth <= 850.0f;
        const float settingsWidth = compactViewport ? static_cast<float>(m_width) * 0.96f
                                                    : std::min(900.0f * rmlScale, static_cast<float>(m_width) * 0.94f);
        const float settingsHeight = compactViewport ? static_cast<float>(m_height) * 0.94f
                                                     : std::min(650.0f * rmlScale, static_cast<float>(m_height) * 0.92f);
        if (!m_settingsPositioned) {
            m_settingsX = std::max(0.0f, (static_cast<float>(m_width) - settingsWidth) * 0.5f);
            m_settingsY = std::max(0.0f, (static_cast<float>(m_height) - settingsHeight) * 0.5f);
        }
        m_settingsPositioned = true;
        m_drag = {};
        m_drag.kind = DragKind::SettingsMove;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_drag.startX = m_settingsX;
        m_drag.startY = m_settingsY;
        m_drag.startW = settingsWidth;
        m_drag.startH = settingsHeight;
        m_systemInterface.LockCursor("move");
    } else if (action == "overlay-drag" || action == "overlay-resize") {
        const std::string id = Attribute(actionElement, "data-container");
        auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(), [&](const auto& c) { return c.id == id; });
        if (it == m_config.overlay_layout.containers.end()) return;
        m_drag = {};
        m_drag.kind = action == "overlay-drag" ? DragKind::OverlayMove : DragKind::OverlayResize;
        m_drag.containerId = id;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        m_drag.startX = it->x;
        m_drag.startY = it->y;
        m_drag.allowDock = m_state && m_state->ui.dashboardLayoutEditMode.load();
        const auto [resolvedWidth, resolvedHeight] = OverlayContainerSize(*it, m_dpiScale, &m_config);
        m_drag.startW = resolvedWidth;
        m_drag.startH = resolvedHeight;
        if (auto* root = Root("overlay-root")) {
            if (auto* element = root->QuerySelector(("[data-container='" + id + "']").c_str())) {
                const float renderedWidth = element->GetOffsetWidth();
                const float renderedHeight = element->GetOffsetHeight();
                if (renderedWidth > 1.0f) m_drag.startW = renderedWidth;
                if (renderedHeight > 1.0f) m_drag.startH = renderedHeight;
            }
        }
        m_systemInterface.LockCursor(action == "overlay-resize" ? "resize" : "move");
    } else if (action == "floating-card-drag") {
        const std::string card = Attribute(actionElement, "data-card");
        if (card.empty()) return;
        m_drag = {};
        m_drag.kind = DragKind::FloatingCard;
        m_drag.containerId = card;
        m_drag.startMouseX = event.GetParameter<float>("mouse_x", 0.0f);
        m_drag.startMouseY = event.GetParameter<float>("mouse_y", 0.0f);
        // Centered cards have no stored position yet, so seed the drag from where
        // the card is actually drawn.
        m_drag.startX = actionElement->GetAbsoluteLeft();
        m_drag.startY = actionElement->GetAbsoluteTop();
        m_drag.startW = actionElement->GetOffsetWidth();
        m_drag.startH = actionElement->GetOffsetHeight();
        m_systemInterface.LockCursor("move");
    } else if (action == "color-field" || action == "color-hue") {
        m_drag = {};
        m_drag.kind = action == "color-field" ? DragKind::ColorField : DragKind::ColorHue;
        // The picker rect is captured once: rebuilding the settings DOM on every
        // move would otherwise invalidate the element mid-drag.
        m_drag.startX = actionElement->GetAbsoluteLeft();
        m_drag.startY = actionElement->GetAbsoluteTop();
        m_drag.startW = std::max(actionElement->GetClientWidth(), 1.0f);
        m_drag.startH = std::max(actionElement->GetClientHeight(), 1.0f);
        m_systemInterface.LockCursor("cross");
        ApplyColorPick(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
    }
}

// Maps a pointer position inside the saturation/value field or hue strip onto the
// color being edited. The picker rect was captured on mousedown, so this stays
// correct while the settings DOM is rebuilt between moves.
void RmlUiController::ApplyColorPick(float mouseX, float mouseY) {
    ColorRGBA* current = ThemeColorForKey(m_config, m_editColorKey);
    if (!current) return;

    const float u = std::clamp((mouseX - m_drag.startX) / m_drag.startW, 0.0f, 1.0f);
    const float v = std::clamp((mouseY - m_drag.startY) / m_drag.startH, 0.0f, 1.0f);
    const Hsv existing = RgbToHsv(*current);
    Hsv picked = existing;
    picked.h = existing.s > 0.0f ? existing.h : m_editColorHue;
    if (m_drag.kind == DragKind::ColorHue) {
        picked.h = u * 360.0f;
        // A fully black or white swatch has nothing for a hue to act on; give the
        // dragged hue something visible instead of leaving the field blank.
        if (picked.v <= 0.0f) picked.v = 1.0f;
        if (picked.s <= 0.0f) picked.s = 1.0f;
    } else {
        picked.s = u;
        picked.v = 1.0f - v;
    }
    m_editColorHue = picked.h;

    const ColorRGBA updated = HsvToRgb(picked, current->a);
    const std::string key = m_editColorKey;
    Config::Update([key, updated](ConfigData& c) {
        if (ColorRGBA* target = ThemeColorForKey(c, key)) *target = updated;
    });
    m_config = Config::Read();
    UpdateThemeProperties();
    RebuildSettings();
}

void RmlUiController::HandleMouseMove(Rml::Event& event) {
    m_lastPointerX = static_cast<int>(std::lround(event.GetParameter<float>("mouse_x", 0.0f)));
    m_lastPointerY = static_cast<int>(std::lround(event.GetParameter<float>("mouse_y", 0.0f)));
    m_hasPointerPosition = true;
    if (m_drag.kind == DragKind::SettingsMove) {
        const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
        const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
        const float dx = mouseX - m_drag.startMouseX;
        const float dy = mouseY - m_drag.startMouseY;
        m_settingsX = std::clamp(m_drag.startX + dx, 0.0f, std::max(0.0f, static_cast<float>(m_width) - m_drag.startW));
        m_settingsY = std::clamp(m_drag.startY + dy, 0.0f, std::max(0.0f, static_cast<float>(m_height) - m_drag.startH));

        if (auto* root = Root("settings-root")) {
            if (auto* window = root->QuerySelector(".settings-window")) {
                const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
                window->SetProperty("left", std::to_string(m_settingsX / std::max(rmlScale, 0.01f)) + "dp");
                window->SetProperty("top", std::to_string(m_settingsY / std::max(rmlScale, 0.01f)) + "dp");
            }
        }
        return;
    }
    if (m_drag.kind == DragKind::ColorField || m_drag.kind == DragKind::ColorHue) {
        ApplyColorPick(event.GetParameter<float>("mouse_x", 0.0f), event.GetParameter<float>("mouse_y", 0.0f));
        return;
    }
    if (m_drag.kind == DragKind::FloatingCard) {
        const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
        const float dx = event.GetParameter<float>("mouse_x", 0.0f) - m_drag.startMouseX;
        const float dy = event.GetParameter<float>("mouse_y", 0.0f) - m_drag.startMouseY;
        const float x = std::clamp(m_drag.startX + dx, 0.0f, std::max(0.0f, static_cast<float>(m_width) - m_drag.startW));
        const float y = std::clamp(m_drag.startY + dy, 0.0f, std::max(0.0f, static_cast<float>(m_height) - m_drag.startH));
        if (m_drag.containerId == "session-view") {
            m_config.session_view_x = x;
            m_config.session_view_y = y;
        } else {
            m_config.match_summary_x = x;
            m_config.match_summary_y = y;
        }
        if (auto* root = Root("overlay-root")) {
            if (auto* card = root->QuerySelector(("[data-card='" + m_drag.containerId + "']").c_str())) {
                card->SetProperty("left", std::to_string(x / std::max(rmlScale, 0.01f)) + "dp");
                card->SetProperty("top", std::to_string(y / std::max(rmlScale, 0.01f)) + "dp");
                card->SetProperty("margin-left", "0");
            }
        }
        return;
    }
    if (m_drag.kind == DragKind::DashboardWidget) {
        const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
        const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
        if (auto* root = Root("dashboard-root")) {
            const std::string draggedDomId = WidgetDomId(m_drag.widget);
            if (auto* draggedEl = root->QuerySelector(("[data-widget='" + draggedDomId + "']").c_str())) {
                draggedEl->SetClass("dragging", true);
            }
            Rml::Element* hovered = m_context ? m_context->GetElementAtPoint(Rml::Vector2f(mouseX, mouseY)) : nullptr;
            Rml::Element* targetSlot = nullptr;
            while (hovered && hovered != root) {
                if (hovered->HasAttribute("data-drop-index")) {
                    targetSlot = hovered;
                    break;
                }
                if (hovered->HasAttribute("data-widget") && hovered->HasAttribute("data-order")) {
                    targetSlot = hovered;
                    break;
                }
                hovered = hovered->GetParentNode();
            }
            Rml::ElementList previousTargets;
            root->QuerySelectorAll(previousTargets, ".drag-target");
            for (auto* el : previousTargets) {
                if (el != targetSlot) el->SetClass("drag-target", false);
            }
            if (targetSlot) {
                targetSlot->SetClass("drag-target", true);
            }
        }
        return;
    }
    if (m_drag.kind != DragKind::OverlayMove && m_drag.kind != DragKind::OverlayResize) return;
    const float dpi = SanitizedScale(m_dpiScale);
    const float rmlScale = dpi * SanitizedUiScale(m_config.ui_scale);
    const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
    const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
    const float dx = mouseX - m_drag.startMouseX;
    const float dy = mouseY - m_drag.startMouseY;
    const float screenWidth = static_cast<float>(m_width);
    const float screenHeight = static_cast<float>(m_height);
    const float moveMargin = 0.0f;
    const float resizeMargin = 24.0f * dpi;
    const float snap = 12.0f * dpi;

    auto it = std::find_if(m_config.overlay_layout.containers.begin(), m_config.overlay_layout.containers.end(), [&](const auto& c) { return c.id == m_drag.containerId; });
    if (it == m_config.overlay_layout.containers.end()) return;

    struct SnapCandidate {
        float value;
        float guide;
    };
    struct SnapResult {
        float value;
        float guide;
        bool snapped;
    };
    auto snapNearest = [&](float value, const std::vector<SnapCandidate>& candidates) {
        SnapResult result{value, 0.0f, false};
        float bestDistance = snap;
        for (const auto& candidate : candidates) {
            const float distance = std::abs(value - candidate.value);
            if (distance < bestDistance) {
                bestDistance = distance;
                result = {candidate.value, candidate.guide, true};
            }
        }
        return result;
    };

    SnapResult xSnap{0.0f, 0.0f, false};
    SnapResult ySnap{0.0f, 0.0f, false};
    if (m_drag.kind == DragKind::OverlayMove) {
        float x = std::max(0.0f, m_drag.startX + dx);
        float y = std::max(0.0f, m_drag.startY + dy);
        std::vector<SnapCandidate> xCandidates{{moveMargin, moveMargin}, {screenWidth - moveMargin - m_drag.startW, screenWidth - moveMargin}};
        std::vector<SnapCandidate> yCandidates{{moveMargin, moveMargin}, {screenHeight - moveMargin - m_drag.startH, screenHeight - moveMargin}};
        // Snap against what each neighbour actually occupies on screen, for the
        // same reason the dragged container is clamped to its rendered size.
        auto* overlayRoot = Root("overlay-root");
        for (const auto& other : m_config.overlay_layout.containers) {
            if (other.id == it->id) continue;
            auto [ow, oh] = OverlayContainerSize(other, m_dpiScale, &m_config);
            if (overlayRoot) {
                if (auto* element = overlayRoot->QuerySelector(("[data-container='" + other.id + "']").c_str())) {
                    if (element->GetOffsetWidth() > 1.0f) ow = element->GetOffsetWidth();
                    if (element->GetOffsetHeight() > 1.0f) oh = element->GetOffsetHeight();
                }
            }
            xCandidates.insert(xCandidates.end(), {{other.x, other.x}, {other.x + ow, other.x + ow}, {other.x - m_drag.startW, other.x}, {other.x + ow - m_drag.startW, other.x + ow}});
            yCandidates.insert(yCandidates.end(), {{other.y, other.y}, {other.y + oh, other.y + oh}, {other.y - m_drag.startH, other.y}, {other.y + oh - m_drag.startH, other.y + oh}});
        }
        xSnap = snapNearest(x, xCandidates);
        ySnap = snapNearest(y, yCandidates);
        it->x = std::clamp(xSnap.value, 0.0f, std::max(0.0f, screenWidth - m_drag.startW));
        it->y = std::clamp(ySnap.value, 0.0f, std::max(0.0f, screenHeight - m_drag.startH));
    } else {
        const auto [minWidth, minHeight] = OverlayContainerMinSize(*it, dpi, &m_config);
        float newWidth = std::max(minWidth, m_drag.startW + dx);
        float newHeight = std::max(minHeight, m_drag.startH + dy);
        std::vector<SnapCandidate> rightCandidates{{screenWidth - resizeMargin, screenWidth - resizeMargin}};
        std::vector<SnapCandidate> bottomCandidates{{screenHeight - resizeMargin, screenHeight - resizeMargin}};
        for (const auto& other : m_config.overlay_layout.containers) {
            if (other.id == it->id) continue;
            const auto [ow, oh] = OverlayContainerSize(other, m_dpiScale, &m_config);
            if (std::abs(newWidth - ow) < snap) {
                newWidth = ow;
                xSnap = {it->x + newWidth, it->x + newWidth, true};
            }
            if (std::abs(newHeight - oh) < snap) {
                newHeight = oh;
                ySnap = {it->y + newHeight, it->y + newHeight, true};
            }
            rightCandidates.insert(rightCandidates.end(), {{other.x, other.x}, {other.x + ow, other.x + ow}});
            bottomCandidates.insert(bottomCandidates.end(), {{other.y, other.y}, {other.y + oh, other.y + oh}});
        }
        const auto rightSnap = snapNearest(it->x + newWidth, rightCandidates);
        const auto bottomSnap = snapNearest(it->y + newHeight, bottomCandidates);
        if (rightSnap.snapped) xSnap = rightSnap;
        if (bottomSnap.snapped) ySnap = bottomSnap;
        const float snappedRight = rightSnap.snapped ? rightSnap.value : it->x + newWidth;
        const float snappedBottom = bottomSnap.snapped ? bottomSnap.value : it->y + newHeight;
        // Every value above is in rendered screen pixels; stored geometry is
        // design pixels at 100% text size, so divide the scale back out.
        const float uiScale = SanitizedUiScale(m_config.ui_scale);
        it->w = std::clamp(snappedRight - it->x, minWidth, std::max(minWidth, screenWidth - resizeMargin - it->x)) / uiScale;
        it->h = std::clamp(snappedBottom - it->y, minHeight, std::max(minHeight, screenHeight - resizeMargin - it->y)) / uiScale;
    }

    if (auto* root = Root("overlay-root")) {
        const auto toDp = [rmlScale](float pixels) { return pixels / std::max(rmlScale, 0.5f); };
        const std::string selector = "[data-container='" + m_drag.containerId + "']";
        if (auto* element = root->QuerySelector(selector)) {
            element->SetProperty("left", std::to_string(toDp(it->x)) + "dp");
            element->SetProperty("top", std::to_string(toDp(it->y)) + "dp");
            if (m_drag.kind == DragKind::OverlayResize) {
                const auto [resolvedWidth, resolvedHeight] = OverlayContainerSize(*it, m_dpiScale, &m_config);
                element->SetProperty("width", std::to_string(toDp(resolvedWidth)) + "dp");
                element->SetProperty("min-height", std::to_string(toDp(resolvedHeight)) + "dp");
            }
        }
        if (auto* guide = root->QuerySelector("#overlay-snap-x")) {
            guide->SetProperty("display", xSnap.snapped ? "block" : "none");
            if (xSnap.snapped) guide->SetProperty("left", std::to_string(toDp(xSnap.guide)) + "dp");
        }
        if (auto* guide = root->QuerySelector("#overlay-snap-y")) {
            guide->SetProperty("display", ySnap.snapped ? "block" : "none");
            if (ySnap.snapped) guide->SetProperty("top", std::to_string(toDp(ySnap.guide)) + "dp");
        }
    }
}
void RmlUiController::HandleMouseUp(Rml::Event& event) {
    m_systemInterface.UnlockCursor();
    const float mouseX = event.GetParameter<float>("mouse_x", 0.0f);
    const float mouseY = event.GetParameter<float>("mouse_y", 0.0f);
    if (auto* root = Root("overlay-root")) {
        if (auto* guide = root->QuerySelector("#overlay-snap-x")) guide->SetProperty("display", "none");
        if (auto* guide = root->QuerySelector("#overlay-snap-y")) guide->SetProperty("display", "none");
    }

    if (m_drag.kind == DragKind::SettingsMove) {
        // Position is intentionally session-local; reopening Settings keeps the
        // user's last placement without changing the existing config format.
    } else if (m_drag.kind == DragKind::ColorField || m_drag.kind == DragKind::ColorHue) {
        // ApplyColorPick already committed every sample.
    } else if (m_drag.kind == DragKind::FloatingCard) {
        const float sessionX = m_config.session_view_x;
        const float sessionY = m_config.session_view_y;
        const float summaryX = m_config.match_summary_x;
        const float summaryY = m_config.match_summary_y;
        Config::Update([=](ConfigData& c) {
            c.session_view_x = sessionX;
            c.session_view_y = sessionY;
            c.match_summary_x = summaryX;
            c.match_summary_y = summaryY;
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::DashboardWidget) {
        Rml::Element* element = m_context ? m_context->GetElementAtPoint(Rml::Vector2f(mouseX, mouseY)) : nullptr;
        if (!element) element = event.GetTargetElement();
        DashboardLayout::Zone targetZone = DashboardLayout::Zone::Left;
        int targetIndex = -1;
        bool found = false;

        Rml::Element* cur = element;
        while (cur) {
            const std::string dropIndex = Attribute(cur, "data-drop-index");
            const std::string zone = Attribute(cur, "data-zone");
            if (!zone.empty() && !dropIndex.empty()) {
                targetZone = ZoneFromDom(zone);
                targetIndex = std::max(0, std::atoi(dropIndex.c_str()));
                found = true;
                break;
            }
            const std::string widgetAttr = Attribute(cur, "data-widget");
            const std::string orderAttr = Attribute(cur, "data-order");
            if (!zone.empty() && !widgetAttr.empty() && !orderAttr.empty()) {
                targetZone = ZoneFromDom(zone);
                const int order = std::atoi(orderAttr.c_str());
                const float top = cur->GetAbsoluteTop();
                const float height = cur->GetOffsetHeight();
                targetIndex = (mouseY > top + height * 0.5f) ? (order + 1) : order;
                found = true;
                break;
            }
            if (!zone.empty() && !found) {
                targetZone = ZoneFromDom(zone);
                found = true;
            }
            cur = cur->GetParentNode();
        }

        if (found) {
            MoveDashboardWidget(m_drag.widget, targetZone, targetIndex);
            Config::RequestSave();
        }
        RebuildDashboard();
    } else if (m_drag.kind == DragKind::OverlayToolboxWidget) {
        std::string targetId;
        bool insertAtTop = false;
        for (const auto& container : m_config.overlay_layout.containers) {
            const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
            if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                targetId = container.id;
                insertAtTop = mouseY < container.y + height * 0.5f;
                break;
            }
        }
        const auto widget = m_drag.widget;
        Config::Update([=](ConfigData& c) {
            for (const auto& container : c.overlay_layout.containers) {
                if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) return;
            }
            if (!targetId.empty()) {
                for (auto& target : c.overlay_layout.containers) {
                    if (target.id != targetId) continue;
                    if (insertAtTop)
                        target.widgets.insert(target.widgets.begin(), widget);
                    else
                        target.widgets.push_back(widget);
                    target.w = 0.0f;
                    target.h = 0.0f;
                    OverlayLayout::Sanitize(c.overlay_layout);
                    return;
                }
            }
            OverlayLayout::ContainerConfig detached;
            detached.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
            detached.x = std::max(0.0f, mouseX - 24.0f * SanitizedScale(m_dpiScale));
            detached.y = std::max(0.0f, mouseY - 24.0f * SanitizedScale(m_dpiScale));
            const auto [width, height] = OverlayWidgetDefaultSize(widget, m_dpiScale);
            detached.w = width;
            detached.h = height;
            detached.widgets = {widget};
            c.overlay_layout.containers.push_back(std::move(detached));
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::OverlayWidget) {
        std::string targetId;
        bool insertAtTop = false;
        for (const auto& container : m_config.overlay_layout.containers) {
            if (container.id == m_drag.sourceContainerId) continue;
            const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
            if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                targetId = container.id;
                insertAtTop = mouseY < container.y + height * 0.5f;
                break;
            }
        }

        const auto widget = m_drag.widget;
        const std::string sourceId = m_drag.sourceContainerId;
        Config::Update([=](ConfigData& c) {
            bool removed = false;
            for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                if (it->id != sourceId) continue;
                const auto widgetIt = std::find(it->widgets.begin(), it->widgets.end(), widget);
                if (widgetIt != it->widgets.end()) {
                    it->widgets.erase(widgetIt);
                    removed = true;
                }
                if (it->widgets.empty()) c.overlay_layout.containers.erase(it);
                break;
            }
            if (!removed) return;

            bool inserted = false;
            if (!targetId.empty()) {
                for (auto& target : c.overlay_layout.containers) {
                    if (target.id != targetId) continue;
                    if (insertAtTop)
                        target.widgets.insert(target.widgets.begin(), widget);
                    else
                        target.widgets.push_back(widget);
                    target.w = 0.0f;
                    target.h = 0.0f;
                    inserted = true;
                    break;
                }
            }
            if (!inserted) {
                OverlayLayout::ContainerConfig detached;
                detached.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
                detached.x = std::max(0.0f, mouseX - 24.0f * SanitizedScale(m_dpiScale));
                detached.y = std::max(0.0f, mouseY - 24.0f * SanitizedScale(m_dpiScale));
                const auto [width, height] = OverlayWidgetDefaultSize(widget, m_dpiScale);
                detached.w = width;
                detached.h = height;
                detached.widgets = {widget};
                c.overlay_layout.containers.push_back(std::move(detached));
            }
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (m_drag.kind == DragKind::OverlayMove || m_drag.kind == DragKind::OverlayResize) {
        if (m_drag.kind == DragKind::OverlayMove && m_drag.allowDock) {
            std::string targetId;
            bool dockAtTop = false;
            for (const auto& container : m_config.overlay_layout.containers) {
                if (container.id == m_drag.containerId) continue;
                const auto [width, height] = OverlayContainerSize(container, m_dpiScale, &m_config);
                if (mouseX >= container.x && mouseX <= container.x + width && mouseY >= container.y && mouseY <= container.y + height) {
                    targetId = container.id;
                    // Match the legacy container docking preview: only the top
                    // quarter inserts above; the center and lower region stack
                    // the dragged container below the target.
                    dockAtTop = mouseY < container.y + height * 0.25f;
                    break;
                }
            }
            if (!targetId.empty()) {
                const std::string draggedId = m_drag.containerId;
                auto layout = m_config.overlay_layout;
                OverlayLayout::ContainerConfig dragged;
                bool found = false;
                for (auto it = layout.containers.begin(); it != layout.containers.end(); ++it) {
                    if (it->id == draggedId) {
                        dragged = *it;
                        layout.containers.erase(it);
                        found = true;
                        break;
                    }
                }
                if (found) {
                    for (auto& target : layout.containers) {
                        if (target.id != targetId) continue;
                        if (dockAtTop)
                            target.widgets.insert(target.widgets.begin(), dragged.widgets.begin(), dragged.widgets.end());
                        else
                            target.widgets.insert(target.widgets.end(), dragged.widgets.begin(), dragged.widgets.end());
                        target.w = 0.0f;
                        target.h = 0.0f;
                        break;
                    }
                    OverlayLayout::Sanitize(layout);
                    m_config.overlay_layout = std::move(layout);
                }
            }
        }
        const auto layout = m_config.overlay_layout;
        Config::Update([&](ConfigData& c) { c.overlay_layout = layout; });
        m_config = Config::Read();
        RebuildOverlay();
    }
    m_drag = {};
    m_pointerPressed = false;
}

void RmlUiController::MoveDashboardWidget(DashboardLayout::WidgetId widget, DashboardLayout::Zone zone, int insertIndex) {
    Config::Update([=](ConfigData& c) {
        DashboardLayout::Sanitize(c.dashboard_layout);

        std::vector<DashboardLayout::WidgetId> zoneIds;
        int currentIndex = -1;
        int sourceIndex = 0;
        for (const auto& placement : c.dashboard_layout.widgets) {
            if (placement.zone != zone) continue;
            if (placement.id == widget)
                currentIndex = sourceIndex;
            else
                zoneIds.push_back(placement.id);
            ++sourceIndex;
        }

        int targetIndex = insertIndex < 0 ? static_cast<int>(zoneIds.size()) : insertIndex;
        if (currentIndex >= 0 && currentIndex < targetIndex) --targetIndex;
        targetIndex = std::clamp(targetIndex, 0, static_cast<int>(zoneIds.size()));
        zoneIds.insert(zoneIds.begin() + targetIndex, widget);

        for (auto& placement : c.dashboard_layout.widgets) {
            if (placement.id == widget) {
                placement.zone = zone;
                break;
            }
        }
        for (int i = 0; i < static_cast<int>(zoneIds.size()); ++i) {
            for (auto& placement : c.dashboard_layout.widgets) {
                if (placement.id == zoneIds[i]) {
                    placement.order = i;
                    break;
                }
            }
        }
        DashboardLayout::Sanitize(c.dashboard_layout);
    });
    m_config = Config::Read();
}

void RmlUiController::DeleteLocalHistory() {
    std::string error;
    if (!m_dbManager || !m_dbManager->DeleteLocalMatchHistory(error)) {
        ShowToast("Delete failed: " + (error.empty() ? std::string("database is unavailable.") : error), true);
        return;
    }
    DeleteFileA((Storage::GetDataDirectory() + "matches.jsonl").c_str());
    DeleteFileA((Storage::GetDataDirectory() + "mmr_history.jsonl").c_str());
    if (m_state) {
        {
            std::unique_lock lock(m_state->game.mutex);
            m_state->game.myPrimaryId.clear();
            m_state->game.myTeam = -1;
            m_state->game.version++;
        }
        {
            std::unique_lock lock(m_state->history.mutex);
            m_state->history.lifetimeMmrX.clear();
            m_state->history.lifetimeMmrY.clear();
            m_state->history.recentSavedMatches.clear();
            m_state->history.pendingRecentMatches.clear();
            m_state->history.recentSavedMatchesLoaded = true;
            m_state->history.version++;
        }
        {
            std::lock_guard lock(m_state->ui.dbStatsMutex);
            m_state->ui.cachedDbStats = {};
            m_state->ui.dbStatsDirty.store(true);
            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Config::Update([](ConfigData& c) { c.last_primary_id.clear(); c.known_primary_ids.clear(); });
    m_lastDbFetchPrimaryId.clear();
    m_lastLifetimeHistoryPrimaryId.clear();
    m_lastRecentMatchHistoryPrimaryId.clear();
    ShowToast("Deleted local history and identity.");
}

void RmlUiController::CheckStatsApi(bool repair) {
    if (!m_state) return;
    StatsApiConfig::CheckResult oldResult;
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        oldResult = m_state->ui.statsApiResult;
    }
    std::string path = m_config.rocket_league_stats_api_config_path;
    if (path.empty()) path = oldResult.path.empty() ? StatsApiConfig::DetectConfigPath() : oldResult.path;
    bool repaired = true;
    if (repair && !path.empty()) repaired = ExternalUpdaterLauncher::RepairStatsApiConfig(path, m_config.port);
    auto result = StatsApiConfig::VerifyConfig(path, m_config.port);
    if (repair && (!repaired || result.status != StatsApiConfig::Status::Valid)) {
        result.message = "Automatic repair did not complete. If an elevation prompt appeared, approve it, or manually set PacketSendRate=30 and Port=" + std::to_string(m_config.port) + ".";
    }
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        m_state->ui.statsApiResult = result;
    }
    m_state->ui.statsApiChecked.store(true);
    ShowToast(StatsApiConfig::GetStatusMessage(result.status), result.status != StatsApiConfig::Status::Valid);
}
