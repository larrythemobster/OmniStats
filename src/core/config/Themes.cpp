#include "core/config/Themes.hpp"
#include "core/config/Config.hpp"

#include <algorithm>
#include <cmath>

namespace Themes {
    namespace {
        constexpr ColorField fields[] = {
            {"theme_bg", "App backgrounds", "Overlay, settings, dialogs, top bar and graph backgrounds", "Surfaces", &ConfigData::themeBg},
            {"theme_panel", "Settings panels", "Settings and dialog backgrounds", "Surfaces", &ConfigData::themeSettingsPanel, "theme_bg"},
            {"theme_topbar", "Top bar", "Dashboard top bar", "Surfaces", &ConfigData::themeTopbar, "theme_bg"},
            {"theme_graph_panel", "Graph panel", "Graph background", "Graph", &ConfigData::themeGraphPanel, "theme_bg"},
            {"theme_roster_card", "Cards and rows", "Player cards, stat boxes, match rows and badge backgrounds", "Surfaces", &ConfigData::themeRosterCard},
            {"theme_roster_card_self", "Your roster card", "Your player card background", "Surfaces", &ConfigData::themeRosterCardSelf, "theme_roster_card"},
            {"theme_scout_tag", "Scout badges and tags", "Scout badge background", "Surfaces", &ConfigData::themeScoutTag},
            {"theme_stat_box", "Stat boxes", "Session statistics and K/D background", "Surfaces", &ConfigData::themeStatBox, "theme_roster_card"},
            {"theme_match_row", "Match row", "Previous games row background", "Surfaces", &ConfigData::themeMatchRow, "theme_roster_card"},
            {"theme_match_row_alt", "Alternate match row", "Alternating previous games rows", "Surfaces", &ConfigData::themeMatchRowAlt, "theme_roster_card"},
            {"theme_text", "Main text", "Names, values and headings", "Text", &ConfigData::themeText},
            {"theme_accent", "Accent", "Actions, selections and highlights", "Text", &ConfigData::themeAccent},
            {"theme_win", "Win", "Positive results", "Results", &ConfigData::themeWin},
            {"theme_loss", "Loss", "Negative results", "Results", &ConfigData::themeLoss},
            {"theme_dim", "Hint text", "Timestamps and low-priority labels", "Text", &ConfigData::themeDim},
            {"theme_muted", "Secondary text", "Descriptions and supporting labels", "Text", &ConfigData::themeMuted},
            {"theme_graph", "Graph line", "MMR series", "Graph", &ConfigData::themeGraphLine},
            {"theme_baseline", "Graph baseline", "MMR reference line", "Graph", &ConfigData::themeGraphBaseline},
            {"theme_primary_button", "Primary button", "Primary button background", "Controls", &ConfigData::themePrimaryButton},
            {"theme_primary_border", "Primary border", "Primary button border", "Controls", &ConfigData::themePrimaryBorder},
            {"theme_primary_hover", "Primary hover", "Hovered primary button", "Controls", &ConfigData::themePrimaryHover},
            {"theme_primary_pressed", "Primary pressed", "Pressed primary button", "Controls", &ConfigData::themePrimaryPressed},
            {"theme_button_text", "Button text", "Button label color", "Controls", &ConfigData::themeButtonText},
            {"theme_button_bg", "Button background", "Secondary button background", "Controls", &ConfigData::themeButtonBg},
            {"theme_button_border", "Button border", "Secondary button border", "Controls", &ConfigData::themeButtonBorder},
            {"theme_button_hover", "Button hover", "Hovered secondary button", "Controls", &ConfigData::themeButtonHover},
            {"theme_input_bg", "Input background", "Text input background", "Controls", &ConfigData::themeInputBg},
            {"theme_input_border", "Input border", "Text input border", "Controls", &ConfigData::themeInputBorder},
            {"theme_input_text", "Input text", "Text input label color", "Controls", &ConfigData::themeInputText},
            {"theme_border", "Borders", "Panel and general borders", "Surfaces", &ConfigData::themeBorder},
            {"theme_selection", "Selection", "Selected row background", "Surfaces", &ConfigData::themeSelection},
            {"theme_row_hover", "Row hover", "Hovered row background", "Surfaces", &ConfigData::themeRowHover},
            {"theme_badge_bg", "Badge background", "Badge background color", "Surfaces", &ConfigData::themeBadgeBg, "theme_roster_card"},
            {"theme_graph_grid", "Graph grid", "Graph grid lines", "Graph", &ConfigData::themeGraphGrid},
            {"theme_button_pressed", "Button pressed", "Pressed secondary button", "Controls", &ConfigData::themeButtonPressed},
        };
        constexpr ColorRGBA black{0, 0, 0, 1}, white{1, 1, 1, 1};
        double Linear(double c) {
            return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
        }
        double Luminance(ColorRGBA c) {
            return 0.2126 * Linear(c.r) + 0.7152 * Linear(c.g) + 0.0722 * Linear(c.b);
        }
        bool Valid(ColorRGBA c) {
            for (float x : {c.r, c.g, c.b, c.a})
                if (!std::isfinite(x) || x < 0 || x > 1) return false;
            return true;
        }
        int Hex(char c) {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }
    }
    std::span<const ColorField> Fields() {
        return fields;
    }
    Palette Read(const ConfigData& config) {
        Palette result{};
        for (size_t i = 0; i < result.size(); ++i)
            result[i] = config.*fields[i].member;
        return result;
    }
    void Apply(ConfigData& config, const Palette& colors) {
        for (size_t i = 0; i < colors.size(); ++i)
            config.*fields[i].member = colors[i];
    }
    void LinkColor(Palette& colors, std::string_view key) {
        for (size_t i = 0; i < colors.size(); ++i) {
            if (key != fields[i].key || fields[i].linkedTo)
                continue;
            for (size_t j = 0; j < colors.size(); ++j)
                if (fields[j].linkedTo && key == fields[j].linkedTo)
                    colors[j] = colors[i];
            return;
        }
    }
    bool Equal(const Palette& a, const Palette& b) {
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b || a[i].a != b[i].a) return false;
        return true;
    }
    void SetAccent(ConfigData& config, ColorRGBA accent) {
        config.themeAccent = accent;
        config.themeScoutTag = {accent.r * 0.55f, accent.g * 0.55f, accent.b * 0.55f, accent.a};
        config.themePrimaryButton = {accent.r * 0.62f, accent.g * 0.62f, accent.b * 0.62f, 1};
        config.themePrimaryBorder = accent;
        config.themePrimaryHover = config.themePrimaryPressed = config.themePrimaryButton;
        config.themeSelection = {accent.r * 0.28f, accent.g * 0.28f, accent.b * 0.28f, 0.95f};
        if (IsLight(config)) {
            config.themePrimaryButton = {0.8f + accent.r * 0.2f, 0.8f + accent.g * 0.2f, 0.8f + accent.b * 0.2f, 1};
            config.themePrimaryHover = config.themePrimaryPressed = config.themePrimaryButton;
            config.themeSelection = {0.8f + accent.r * 0.2f, 0.8f + accent.g * 0.2f, 0.8f + accent.b * 0.2f, 0.95f};
        }
    }
    void CompleteLegacy(ConfigData& c) {
        const auto scout = c.themeScoutTag;
        SetAccent(c, c.themeAccent);
        c.themeScoutTag = scout;
        c.themeButtonText = c.themeInputText = c.themeText;
        c.themeBorder = {c.themeText.r, c.themeText.g, c.themeText.b, 0.10f};
        if (IsLight(c)) {
            c.themeButtonBg = {0.88f, 0.89f, 0.91f, 1};
            c.themeButtonBorder = {0.65f, 0.67f, 0.70f, 1};
            c.themeButtonHover = {0.82f, 0.84f, 0.87f, 1};
            c.themeButtonPressed = {0.76f, 0.78f, 0.82f, 1};
            c.themeInputBg = {1, 1, 1, 1};
            c.themeInputBorder = {0.60f, 0.62f, 0.66f, 1};
            c.themeRowHover = {0, 0, 0, 0.06f};
            c.themeBadgeBg = {0.86f, 0.88f, 0.91f, 0.9f};
            c.themeGraphGrid = {0.35f, 0.38f, 0.42f, 0.35f};
        } else {
            c.themeButtonBg = {37.0f / 255, 37.0f / 255, 37.0f / 255, 1};
            c.themeButtonBorder = {57.0f / 255, 57.0f / 255, 57.0f / 255, 1};
            c.themeButtonHover = {48.0f / 255, 48.0f / 255, 48.0f / 255, 1};
            c.themeButtonPressed = {28.0f / 255, 28.0f / 255, 28.0f / 255, 1};
            c.themeInputBg = {18.0f / 255, 18.0f / 255, 18.0f / 255, 1};
            c.themeInputBorder = {59.0f / 255, 59.0f / 255, 59.0f / 255, 1};
            c.themeRowHover = {238.0f / 255, 241.0f / 255, 245.0f / 255, 13.0f / 255};
            c.themeBadgeBg = {20.0f / 255, 20.0f / 255, 20.0f / 255, 204.0f / 255};
            c.themeGraphGrid = {105.0f / 255, 114.0f / 255, 128.0f / 255, 64.0f / 255};
        }
    }
    ColorRGBA Composite(ColorRGBA fg, ColorRGBA bg) {
        const float a = fg.a + bg.a * (1 - fg.a);
        if (a <= 0) return {0, 0, 0, 0};
        return {(fg.r * fg.a + bg.r * bg.a * (1 - fg.a)) / a,
                (fg.g * fg.a + bg.g * bg.a * (1 - fg.a)) / a,
                (fg.b * fg.a + bg.b * bg.a * (1 - fg.a)) / a, a};
    }
    double Contrast(ColorRGBA fg, ColorRGBA bg) {
        bg = Composite(bg, black);
        fg = Composite(fg, bg);
        const double a = Luminance(fg), b = Luminance(bg);
        return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
    }
    ColorRGBA ReadableText(ColorRGBA bg) {
        return Contrast(black, bg) >= Contrast(white, bg) ? black : white;
    }
    bool IsLight(const ConfigData& config) {
        return Contrast(black, config.themeSettingsPanel) > Contrast(white, config.themeSettingsPanel);
    }
    std::span<const Preset> Presets() {
        // RGBA bytes are sRGB, matching the renderer and share-code format.
        const auto palette = [](const std::array<unsigned, 35>& bytes) {
            Palette result{};
            for (size_t i = 0; i < bytes.size(); ++i) {
                const auto v = bytes[i];
                result[i] = {((v >> 24) & 255) / 255.0f, ((v >> 16) & 255) / 255.0f,
                             ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f};
            }
            return result;
        };
        static const std::array<Preset, 5> presets = {{{"red", "OmniStats Red", Read(ConfigData{})},
                                                       {"purple", "Purple Focus", palette({0x121212ff, 0x161616ff, 0x1b1b1bff, 0x121212ff, 0x1d1d1dff, 0x28232eff, 0x482966ff, 0x1d1d1dff, 0x1d1d1dff, 0x191919ff, 0xeeeeeeff, 0xb88ee7ff, 0x8cdda7ff, 0xff9b9bff, 0xa4a4a4ff, 0xbababaff, 0xc5a8e9ff, 0x7a7a7aff, 0x592e81ff, 0xa271d5ff, 0x693897ff, 0x482867ff, 0xf2f2f2ff, 0x262626ff, 0x424242ff, 0x333333ff, 0x121212ff, 0x424242ff, 0xeeeeeeff, 0xbababa38, 0x32293eff, 0xb88ee712, 0x1d1d1dff, 0x80808033, 0x1f1f1fff})},
                                                       {"green", "Green Focus", palette({0x121212ff, 0x161616ff, 0x1b1b1bff, 0x121212ff, 0x1d1d1dff, 0x212722ff, 0x24432bff, 0x1d1d1dff, 0x1d1d1dff, 0x191919ff, 0xeeeeeeff, 0x59bd73ff, 0x8cdda7ff, 0xff9b9bff, 0xa4a4a4ff, 0xbababaff, 0x6ad184ff, 0x7a7a7aff, 0x285233ff, 0x579f68ff, 0x31623dff, 0x23432aff, 0xf2f2f2ff, 0x262626ff, 0x424242ff, 0x333333ff, 0x121212ff, 0x424242ff, 0xeeeeeeff, 0xbababa38, 0x273128ff, 0x59bd7312, 0x1d1d1dff, 0x80808033, 0x1f1f1fff})},
                                                       {"contrast", "High contrast", palette({0x000000ff, 0x000000ff, 0x000000ff, 0x000000ff, 0x000000ff, 0x111111ff, 0x214a87ff, 0x000000ff, 0x080808ff, 0x000000ff, 0xffffffff, 0x72b5ffff, 0x72f59aff, 0xff9494ff, 0xffffffff, 0xffffffff, 0x85c5ffff, 0xffec71ff, 0x15467aff, 0xa0ccffff, 0x205e9bff, 0x103960ff, 0xffffffff, 0x202020ff, 0xb0b0b0ff, 0x383838ff, 0x000000ff, 0xb0b0b0ff, 0xffffffff, 0xffffff80, 0x285483ff, 0xffffff25, 0x202020ff, 0xffffff80, 0x101010ff})},
                                                       {"light", "Light", palette({0xf0f2f6ff, 0xf9fafcff, 0xe8edf4ff, 0xffffffff, 0xffffffff, 0xe4ecf7ff, 0xe1cdd2ff, 0xf4f6f9ff, 0xffffffff, 0xeef1f6ff, 0x141923ff, 0x73202cff, 0x12602bff, 0xa31720ff, 0x303741ff, 0x454d59ff, 0x234e9cff, 0x755000ff, 0xe3d0d3ff, 0x73202cff, 0xd5b8bdff, 0xc9a4aaff, 0x141923ff, 0xe0e4ebff, 0xa0a8b4ff, 0xd1d8e1ff, 0xffffffff, 0x979fabff, 0x141923ff, 0x14192340, 0xe9cbd0ff, 0x00000010, 0xe0e5edee, 0x59657570, 0xc3ccd7ff})}}};
        return presets;
    }
    std::string Encode(const Palette& colors) {
        for (const auto& c : colors)
            if (!Valid(c)) return {};
        constexpr char hex[] = "0123456789abcdef";
        std::string result = "omni-theme:v2:";
        result.reserve(result.size() + colors.size() * 8);
        for (const auto& c : colors)
            for (float x : {c.r, c.g, c.b, c.a}) {
                const int byte = static_cast<int>(std::lround(x * 255));
                result.push_back(hex[byte >> 4]);
                result.push_back(hex[byte & 15]);
            }
        return result;
    }
    bool Decode(std::string_view code, Palette& colors, std::string& error) {
        constexpr std::string_view v1 = "omni-theme:v1:", v2 = "omni-theme:v2:";
        const bool legacy = code.starts_with(v1);
        if (!legacy && !code.starts_with(v2)) {
            error = "Unsupported or invalid theme code version";
            return false;
        }
        const auto prefix = legacy ? v1 : v2;
        const size_t count = legacy ? 18 : colors.size();
        if (code.size() != prefix.size() + count * 8) {
            error = "Invalid theme code length";
            return false;
        }
        Palette decoded{};
        size_t pos = prefix.size();
        for (size_t i = 0; i < count; ++i) {
            float values[4];
            for (float& value : values) {
                const int hi = Hex(code[pos++]), lo = Hex(code[pos++]);
                if (hi < 0 || lo < 0) {
                    error = "Invalid theme code color";
                    return false;
                }
                value = static_cast<float>(hi * 16 + lo) / 255.0f;
            }
            decoded[i] = {values[0], values[1], values[2], values[3]};
        }
        if (legacy) {
            ConfigData config{};
            Themes::Apply(config, decoded);
            CompleteLegacy(config);
            decoded = Read(config);
        }
        colors = decoded;
        error.clear();
        return true;
    }
    std::vector<std::string> Warnings(const ConfigData& c) {
        std::vector<std::string> warnings;
        const ColorRGBA base = Composite(c.themeBg, black);
        const ColorRGBA surfaces[] = {base, Composite(c.themeSettingsPanel, base), Composite(c.themeTopbar, base), Composite(c.themeGraphPanel, base), Composite(c.themeRosterCard, base), Composite(c.themeRosterCardSelf, base), Composite(c.themeStatBox, base), Composite(c.themeMatchRow, base), Composite(c.themeMatchRowAlt, base)};
        auto check = [&](ColorRGBA color, double minimum, const char* message) {
            for (const auto& surface : surfaces)
                if (Contrast(color, surface) < minimum) {
                    warnings.emplace_back(message);
                    break;
                }
        };
        check(c.themeText, 4.5, "Text has low contrast on a background.");
        check(c.themeDim, 4.5, "Hint text has low contrast on a background.");
        check(c.themeMuted, 4.5, "Secondary text has low contrast on a background.");
        check(c.themeWin, 3.0, "Win color has low contrast on a background.");
        check(c.themeLoss, 3.0, "Loss color has low contrast on a background.");
        const auto graph = Composite(c.themeGraphPanel, base);
        if (Contrast(c.themeGraphLine, graph) < 3) warnings.emplace_back("Graph line has low contrast on the graph panel.");
        if (Contrast(c.themeGraphBaseline, graph) < 3) warnings.emplace_back("Graph baseline has low contrast on the graph panel.");
        return warnings;
    }
}
