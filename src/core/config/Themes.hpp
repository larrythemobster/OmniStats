#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct ColorRGBA {
    float r, g, b, a;
};

struct ConfigData;

namespace Themes {
    using Palette = std::array<ColorRGBA, 35>;
    struct SavedTheme {
        std::string name;
        Palette colors;
    };
    struct ColorField {
        const char* key;
        const char* label;
        const char* help;
        const char* group;
        ColorRGBA ConfigData::* member;
        const char* linkedTo = nullptr;
    };
    struct Preset {
        std::string_view id;
        std::string_view name;
        Palette colors;
    };

    std::span<const ColorField> Fields();
    Palette Read(const ConfigData& config);
    void Apply(ConfigData& config, const Palette& colors);
    void LinkColor(Palette& colors, std::string_view key);
    bool Equal(const Palette& a, const Palette& b);
    std::span<const Preset> Presets();
    void CompleteLegacy(ConfigData& config);
    void SetAccent(ConfigData& config, ColorRGBA accent);
    bool IsLight(const ConfigData& config);
    ColorRGBA Composite(ColorRGBA foreground, ColorRGBA background);
    double Contrast(ColorRGBA foreground, ColorRGBA background);
    ColorRGBA ReadableText(ColorRGBA background);
    std::string Encode(const Palette& colors);
    bool Decode(std::string_view code, Palette& colors, std::string& error);
    std::vector<std::string> Warnings(const ConfigData& config);
}
