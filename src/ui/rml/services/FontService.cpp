#include "ui/rml/services/FontService.hpp"

#include <RmlUi/Core/Core.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#include "ui/window/SystemFonts.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"

namespace {
    struct BundledFont {
        const char* file;
        const char* family;
        int weight;
        int resource;
        bool required;
    };

    constexpr BundledFont kBundledFonts[] = {
        {"Inter-Regular.ttf", "Inter", 400, 1001, true},
        {"Inter-SemiBold.ttf", "Inter", 600, 1002, false},
        {"Inter-Bold.ttf", "Inter", 700, 1003, false},
        {"JetBrainsMono-Regular.ttf", "JetBrains Mono", 400, 1004, false},
        {"JetBrainsMono-Bold.ttf", "JetBrains Mono", 700, 1005, false},
        {"RussoOne-Regular.ttf", "Russo One", 400, 1006, false},
    };

    constexpr std::string_view kDefaultUiFont = "Inter";
    constexpr std::string_view kDefaultMonoFont = "JetBrains Mono";
    constexpr std::string_view kDefaultDisplayFont = "Russo One";

    constexpr std::string_view kUiFontSelectors =
        "#app, #app .lobby-rank-matches, #app .brand-title .version, #app button.version-update, "
        "#app .match-header .match-mode, #app .match-header .match-score, #app .match-header .match-mmr, "
        "#app .match-header .match-time, #app .previous-games-footer, #app .tooltip-bubble";
    constexpr std::string_view kMonoFontSelectors =
        "#app .mono, #app .chip.chip-mmr, #app .graph-zoom .zoom-step, #app .color-channel-label, #app .graph-label, "
        "#app .match-score, #app .match-mmr, #app .debug-value, #app .recap-hero-value, "
        "#app .recap-strip-value, #app .recap-mode-record, #app .recap-mode-mmr";
    constexpr std::string_view kDisplayFontSelectors =
        "#app .card-title, #app .summary-score, #app .brand-title, #app .dashboard-widget-title .name, "
        "#app .recap-title, #app .recap-brand, #app .recap-record";

    std::string CssFamilyName(std::string_view family) {
        std::string clean;
        clean.reserve(family.size());
        for (char ch : family) {
            if (ch == '"' || ch == '\\' || ch == ';' || ch == '{' || ch == '}' || ch == '\n' || ch == '\r') continue;
            clean.push_back(ch);
        }
        return clean;
    }

    bool IsBundledFamily(std::string_view family) {
        return family == kDefaultUiFont || family == kDefaultMonoFont || family == kDefaultDisplayFont;
    }

    std::vector<Rml::byte> ReadWholeFile(const std::filesystem::path& path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size == 0) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        std::vector<Rml::byte> data(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (file.gcount() != static_cast<std::streamsize>(size)) return {};
        return data;
    }
}

bool FontService::LoadBundledFonts() {
    m_fontBlobs.clear();
    m_loadedSystemFonts.clear();
    for (const auto& font : kBundledFonts) {
        Rml::Span<const Rml::byte> data = RmlUiDetail::EmbeddedResource(font.resource);
        if (data.empty()) {
            auto blob = RmlUiDetail::ReadFontFile(font.file);
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

    for (const char* fallback : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/msgothic.ttc"})
        Rml::LoadFontFace(fallback, true);

    return true;
}

std::string FontService::ResolveFontFamily(const std::string& choice, std::string_view defaultFamily) {
    if (choice.empty() || IsBundledFamily(choice)) return choice.empty() ? std::string(defaultFamily) : choice;

    auto cached = m_loadedSystemFonts.find(choice);
    if (cached == m_loadedSystemFonts.end()) {
        const std::string alias = "omnistats-system " + CssFamilyName(choice);
        bool loadedAny = false;
        std::map<std::filesystem::path, Rml::Span<const Rml::byte>> files;
        for (const auto& face : SystemFonts::FindFaces(choice, {400, 600, 700})) {
            auto it = files.find(face.file);
            if (it == files.end()) {
                auto blob = ReadWholeFile(face.file);
                Rml::Span<const Rml::byte> data;
                if (!blob.empty()) {
                    m_fontBlobs.push_back(std::move(blob));
                    const auto& stored = m_fontBlobs.back();
                    data = {stored.data(), stored.size()};
                }
                it = files.emplace(face.file, data).first;
            }
            if (it->second.empty()) continue;
            loadedAny |= Rml::LoadFontFace(it->second, alias, Rml::Style::FontStyle::Normal,
                                           static_cast<Rml::Style::FontWeight>(face.weight), false,
                                           static_cast<int>(face.faceIndex));
        }
        if (!loadedAny) std::cerr << "[RmlUi] Warning: could not load installed font \"" << choice << "\"; using default.\n";
        cached = m_loadedSystemFonts.emplace(choice, loadedAny ? alias : std::string{}).first;
    }
    return cached->second.empty() ? std::string(defaultFamily) : cached->second;
}

std::string FontService::FontOverrideCss(const ConfigData& config) {
    m_appliedUiFont = config.font_ui;
    m_appliedMonoFont = config.font_mono;
    m_appliedDisplayFont = config.font_display;

    const std::string ui = ResolveFontFamily(config.font_ui, kDefaultUiFont);
    const std::string mono = ResolveFontFamily(config.font_mono, kDefaultMonoFont);
    const std::string display = ResolveFontFamily(config.font_display, kDefaultDisplayFont);
    if (ui == kDefaultUiFont && mono == kDefaultMonoFont && display == kDefaultDisplayFont) return {};

    std::ostringstream css;
    css << kUiFontSelectors << " { font-family: \"" << CssFamilyName(ui) << "\"; }\n"
        << kMonoFontSelectors << " { font-family: \"" << CssFamilyName(mono) << "\"; }\n"
        << kDisplayFontSelectors << " { font-family: \"" << CssFamilyName(display) << "\"; }\n";
    return css.str();
}

bool FontService::FontsDifferFromApplied(const ConfigData& config) const {
    return config.font_ui != m_appliedUiFont || config.font_mono != m_appliedMonoFont ||
           config.font_display != m_appliedDisplayFont;
}

const std::vector<std::string>& FontService::SystemFontFamilies() {
    static const std::vector<std::string> s_families = SystemFonts::ListFamilies();
    return s_families;
}
