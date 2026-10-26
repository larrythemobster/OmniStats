#include "ui/rml/controller/RmlUiController.hpp"

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>

#include "ui/window/SystemFonts.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"

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
        "#app .match-score, #app .match-mmr, #app .debug-value, #app .recap-hero-value, "
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
    return m_fontService ? m_fontService->LoadBundledFonts() : false;
}

std::string RmlUiController::ResolveFontFamily(const std::string& choice, std::string_view defaultFamily) {
    return m_fontService ? m_fontService->ResolveFontFamily(choice, defaultFamily) : std::string(defaultFamily);
}

std::string RmlUiController::FontOverrideCss() {
    return m_fontService ? m_fontService->FontOverrideCss(m_config) : std::string{};
}

bool RmlUiController::FontsDifferFromApplied(const ConfigData& config) const {
    return m_fontService ? m_fontService->FontsDifferFromApplied(config) : false;
}

const std::vector<std::string>& RmlUiController::SystemFontFamilies() {
    return FontService::SystemFontFamilies();
}
