#include "ui/rml/RmlUiController.hpp"

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "ui/SystemFonts.hpp"
#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    struct BundledFont {
        const char* resource;
        const char* file;
        const char* family;
        int weight;
        bool required;
    };
    constexpr BundledFont kBundledFonts[] = {
        {"FONT_UI_REGULAR", "Inter-Regular.ttf", "Inter", 400, true},
        {"FONT_UI_SEMIBOLD", "Inter-SemiBold.ttf", "Inter", 600, false},
        {"FONT_UI_BOLD", "Inter-Bold.ttf", "Inter", 700, false},
        {"FONT_MONO_REGULAR", "JetBrainsMono-Regular.ttf", "JetBrains Mono", 400, false},
        {"FONT_MONO_BOLD", "JetBrainsMono-Bold.ttf", "JetBrains Mono", 700, false},
        {"FONT_DISPLAY_REGULAR", "RussoOne-Regular.ttf", "Russo One", 400, false},
    };

    constexpr std::string_view kDefaultUiFont = "Inter";
    constexpr std::string_view kDefaultMonoFont = "JetBrains Mono";
    constexpr std::string_view kDefaultDisplayFont = "Russo One";

    // Every packaged RCSS rule that names a family, grouped by role. UI-role
    // selectors restore body text inside mono/display parents, so their extra
    // specificity must keep winning once all roles are emitted with #app.
    // Window-control glyphs are icons and keep their packaged font.
    constexpr std::string_view kUiFontSelectors =
        "#app, #app .lobby-rank-matches, #app .brand-title .version, #app button.version-update, "
        "#app .match-header .match-mode, #app .match-header .match-score, #app .match-header .match-mmr, "
        "#app .match-header .match-time, #app .previous-games-footer, #app .tooltip-bubble";
    constexpr std::string_view kMonoFontSelectors =
        "#app .mono, #app .chip.chip-mmr, #app .graph-zoom .zoom-step, #app .color-channel-label, #app .graph-label, "
        "#app .graph-current, #app .match-score, #app .match-mmr, #app .debug-value, #app .recap-hero-value, "
        "#app .recap-strip-value, #app .recap-mode-record, #app .recap-mode-mmr";
    constexpr std::string_view kDisplayFontSelectors =
        "#app .card-title, #app .summary-score, #app .brand-title, #app .dashboard-widget-title .name, "
        "#app .recap-title, #app .recap-brand, #app .recap-record";

    // RmlUi reads the family as a quoted RCSS string.
    std::string CssFamilyName(std::string_view family) {
        std::string clean;
        clean.reserve(family.size());
        for (char c : family) {
            if (c == '"' || c == '\\' || c == ';' || c == '{' || c == '}' || c == '\n' || c == '\r') continue;
            clean.push_back(c);
        }
        return clean;
    }

    bool IsBundledFamily(std::string_view family) {
        return std::any_of(std::begin(kBundledFonts), std::end(kBundledFonts),
                           [&](const BundledFont& font) { return family == font.family; });
    }

    std::vector<unsigned char> ReadWholeFile(const std::filesystem::path& path) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size == 0) return {};
        std::ifstream file(path, std::ios::binary);
        if (!file) return {};
        std::vector<unsigned char> bytes(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (file.gcount() != static_cast<std::streamsize>(size)) return {};
        return bytes;
    }
}

// OmniStats ships its own typefaces so the client matches the web brand and does
// not inherit whatever Segoe UI revision a machine happens to have. Faces are
// registered with an explicit family and weight: static Inter/JetBrains Mono files
// name themselves "Inter SemiBold" and friends, which would otherwise register as
// separate families.
bool RmlUiController::LoadBundledFonts() {
    m_fontBlobs.clear();
    m_systemFontAliases.clear();
    for (const auto& font : kBundledFonts) {
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

// RmlUi cannot unload or replace a registered face, and adding faces to an
// existing family makes the match ambiguous. Installed fonts therefore get their
// own alias family, loaded the first time they are chosen and kept until
// Rml::Shutdown.
std::string RmlUiController::ResolveFontFamily(const std::string& choice, std::string_view defaultFamily) {
    if (choice.empty() || IsBundledFamily(choice)) return choice.empty() ? std::string(defaultFamily) : choice;

    auto cached = m_systemFontAliases.find(choice);
    if (cached == m_systemFontAliases.end()) {
        const std::string alias = "omnistats-system " + CssFamilyName(choice);
        bool loadedAny = false;
        std::map<std::filesystem::path, Rml::Span<const Rml::byte>> files;
        for (const auto& face : SystemFonts::FindFaces(choice, {400, 600, 700})) {
            auto file = files.find(face.file);
            if (file == files.end()) {
                auto blob = ReadWholeFile(face.file);
                Rml::Span<const Rml::byte> data;
                if (!blob.empty()) {
                    m_fontBlobs.push_back(std::move(blob));
                    data = {m_fontBlobs.back().data(), m_fontBlobs.back().size()};
                }
                file = files.emplace(face.file, data).first;
            }
            if (file->second.empty()) continue;
            loadedAny |= Rml::LoadFontFace(file->second, alias, Rml::Style::FontStyle::Normal,
                                           static_cast<Rml::Style::FontWeight>(face.weight), false,
                                           static_cast<int>(face.faceIndex));
        }
        if (!loadedAny) std::cerr << "[RmlUi] Warning: could not load installed font \"" << choice << "\"; using default.\n";
        cached = m_systemFontAliases.emplace(choice, loadedAny ? alias : std::string{}).first;
    }
    return cached->second.empty() ? std::string(defaultFamily) : cached->second;
}

std::string RmlUiController::FontOverrideCss() {
    m_appliedFontUi = m_config.font_ui;
    m_appliedFontMono = m_config.font_mono;
    m_appliedFontDisplay = m_config.font_display;

    const std::string ui = ResolveFontFamily(m_config.font_ui, kDefaultUiFont);
    const std::string mono = ResolveFontFamily(m_config.font_mono, kDefaultMonoFont);
    const std::string display = ResolveFontFamily(m_config.font_display, kDefaultDisplayFont);
    if (ui == kDefaultUiFont && mono == kDefaultMonoFont && display == kDefaultDisplayFont) return {};

    // Emit every role once any is customized: the packaged UI-role resets inside
    // mono/display parents lose to #app-prefixed rules otherwise.
    std::ostringstream css;
    css << kUiFontSelectors << " { font-family: \"" << CssFamilyName(ui) << "\"; }\n"
        << kMonoFontSelectors << " { font-family: \"" << CssFamilyName(mono) << "\"; }\n"
        << kDisplayFontSelectors << " { font-family: \"" << CssFamilyName(display) << "\"; }\n";
    return css.str();
}

bool RmlUiController::FontsDifferFromApplied(const ConfigData& config) const {
    return config.font_ui != m_appliedFontUi || config.font_mono != m_appliedFontMono ||
           config.font_display != m_appliedFontDisplay;
}

const std::vector<std::string>& RmlUiController::SystemFontFamilies() {
    if (!m_systemFontFamiliesLoaded) {
        m_systemFontFamilies = SystemFonts::ListFamilies();
        m_systemFontFamiliesLoaded = true;
    }
    return m_systemFontFamilies;
}
