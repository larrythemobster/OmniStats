#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "core/config/Themes.hpp"
#include <cctype>

namespace {
    ColorRGBA MixThemeColor(ColorRGBA a, ColorRGBA b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
    }

    ColorRGBA AccentOn(ColorRGBA accent, ColorRGBA background) {
        accent.a = 1.0f;
        background.a = 1.0f;
        const ColorRGBA ink = Themes::ReadableText(background);
        for (int i = 0; i < 20 && Themes::Contrast(accent, background) < 4.5; ++i)
            accent = MixThemeColor(accent, ink, 0.15f);
        return accent;
    }

    // Keep packaged state colors intact when the corresponding palette is unchanged.
    std::string ThemeStyleSheet(std::string_view source, const ConfigData& config, bool light) {
        static const ConfigData defaults;
        const RmlUiDetail::Hsv originalAccent = RmlUiDetail::RgbToHsv(defaults.themeAccent);
        const RmlUiDetail::Hsv chosenAccent = RmlUiDetail::RgbToHsv(config.themeAccent);
        const auto trim = [](std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);
            return text;
        };
        std::string result;
        result.reserve(source.size());
        std::string_view selector;
        size_t statement = 0;
        bool inBlock = false;
        for (size_t i = 0; i < source.size();) {
            if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                const size_t end = source.find("*/", i + 2);
                const size_t count = end == std::string_view::npos ? source.size() - i : end + 2 - i;
                result.append(source.substr(i, count));
                i += count;
                if (!inBlock) statement = i;
                continue;
            }
            if (source[i] == '{') {
                selector = source.substr(statement, i - statement);
                inBlock = true;
                statement = i + 1;
            } else if (source[i] == '}') {
                inBlock = false;
                statement = i + 1;
            } else if (source[i] == ';' && inBlock) {
                statement = i + 1;
            }
            if (inBlock && source[i] == '#') {
                size_t end = i + 1;
                while (end < source.size() && std::isxdigit(static_cast<unsigned char>(source[end])))
                    ++end;
                const size_t digits = end - i - 1;
                const auto decl = source.substr(statement, i - statement);
                const auto colon = decl.find(':');
                const bool picker = selector.find(".color-field") != std::string_view::npos ||
                                    selector.find(".color-hue") != std::string_view::npos ||
                                    selector.find(".color-dot") != std::string_view::npos;
                const bool scrim = selector.find("backdrop") != std::string_view::npos ||
                                   selector.find(".dashboard-modal") != std::string_view::npos;
                if ((digits == 6 || digits == 8) && colon != std::string_view::npos && !picker && !scrim) {
                    const auto hex = [&](size_t p) {
                        const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
                        return (digit(source[p]) * 16 + digit(source[p + 1])) / 255.0f;
                    };
                    ColorRGBA c{hex(i + 1), hex(i + 3), hex(i + 5), digits == 8 ? hex(i + 7) : 1.0f};
                    const float lo = std::min({c.r, c.g, c.b});
                    const float hi = std::max({c.r, c.g, c.b});
                    const auto property = decl.substr(0, colon);
                    const bool foreground = property.find("color") != std::string_view::npos &&
                                            property.find("background") == std::string_view::npos &&
                                            property.find("border") == std::string_view::npos;
                    const bool shadow = property.find("shadow") != std::string_view::npos;
                    const bool semantic = selector.find("loss") != std::string_view::npos ||
                                          selector.find("danger") != std::string_view::npos ||
                                          selector.find("error") != std::string_view::npos ||
                                          selector.find("platform-") != std::string_view::npos ||
                                          selector.find(".blue") != std::string_view::npos ||
                                          selector.find(".orange") != std::string_view::npos;
                    const auto name = trim(selector);
                    const auto prop = trim(property);
                    std::optional<ColorRGBA> control;
                    if (name == "button, .button") {
                        if (prop == "background-color") control = config.themeButtonBg;
                        if (prop == "border") control = config.themeButtonBorder;
                    } else if (name == "button:hover, .button:hover") {
                        if (prop == "background-color") control = config.themeButtonHover;
                        if (prop == "border-color") {
                            control = config.themeButtonBorder;
                            control->r = std::clamp(control->r * 80.0f / 57.0f, 0.0f, 1.0f);
                            control->g = std::clamp(control->g * 80.0f / 57.0f, 0.0f, 1.0f);
                            control->b = std::clamp(control->b * 80.0f / 57.0f, 0.0f, 1.0f);
                        }
                    } else if (name == "button:active, .button:active" && prop == "background-color") {
                        control = config.themeButtonPressed;
                    } else if (name == "input.text, input.password, select") {
                        if (prop == "background-color") control = config.themeInputBg;
                        if (prop == "border") control = config.themeInputBorder;
                        if (prop == "color") control = config.themeInputText;
                    } else if (name == ".badge" && prop == "background-color") {
                        control = config.themeBadgeBg;
                    }
                    if (control) {
                        result += RmlUiDetail::CssColor(*control);
                        i = end;
                        continue;
                    }
                    if (prop.find("border") != std::string_view::npos && hi - lo < 0.13f && !semantic) {
                        ColorRGBA mapped = config.themeBorder;
                        if (light) {
                            mapped.a *= c.a;
                        } else {
                            mapped.r = std::clamp(c.r * mapped.r / defaults.themeBorder.r, 0.0f, 1.0f);
                            mapped.g = std::clamp(c.g * mapped.g / defaults.themeBorder.g, 0.0f, 1.0f);
                            mapped.b = std::clamp(c.b * mapped.b / defaults.themeBorder.b, 0.0f, 1.0f);
                            mapped.a = std::clamp(c.a * mapped.a / defaults.themeBorder.a, 0.0f, 1.0f);
                        }
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                    if (light && foreground && hi - lo >= 0.13f) {
                        ColorRGBA mapped = AccentOn(c, config.themeSettingsPanel);
                        mapped.a = c.a;
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                    const bool red = c.r > 0.45f && c.r > c.g * 1.8f && c.r > c.b * 1.8f && c.g < 0.35f && c.b < 0.4f;
                    if (red && !semantic) {
                        RmlUiDetail::Hsv mappedHue = RmlUiDetail::RgbToHsv(c);
                        mappedHue.h = std::fmod(mappedHue.h + chosenAccent.h - originalAccent.h + 360.0f, 360.0f);
                        mappedHue.s *= chosenAccent.s;
                        mappedHue.v = std::clamp(mappedHue.v * chosenAccent.v / originalAccent.v, 0.0f, 1.0f);
                        ColorRGBA mapped = RmlUiDetail::HsvToRgb(mappedHue, c.a);
                        if (light && foreground) mapped = AccentOn(mapped, config.themeSettingsPanel);
                        mapped.a = c.a;
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                    if (light && !foreground && !semantic && c.r > c.g * 1.25f && c.r > c.b * 1.15f && hi < 0.35f) {
                        ColorRGBA mapped = MixThemeColor(config.themeSettingsPanel, config.themeAccent, 0.12f);
                        mapped.a = c.a;
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                    if (!light && hi - lo < 0.13f && !shadow && !semantic) {
                        ColorRGBA mapped = c;
                        if (foreground) {
                            const bool secondary = hi < 0.75f;
                            const ColorRGBA target = secondary ? config.themeMuted : config.themeText;
                            const ColorRGBA original = secondary ? defaults.themeMuted : defaults.themeText;
                            mapped.r *= target.r / original.r;
                            mapped.g *= target.g / original.g;
                            mapped.b *= target.b / original.b;
                        } else if (hi < 0.55f) {
                            mapped.r += config.themeSettingsPanel.r - defaults.themeSettingsPanel.r;
                            mapped.g += config.themeSettingsPanel.g - defaults.themeSettingsPanel.g;
                            mapped.b += config.themeSettingsPanel.b - defaults.themeSettingsPanel.b;
                        }
                        mapped.r = std::clamp(mapped.r, 0.0f, 1.0f);
                        mapped.g = std::clamp(mapped.g, 0.0f, 1.0f);
                        mapped.b = std::clamp(mapped.b, 0.0f, 1.0f);
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                    if (light && hi - lo < 0.13f && !shadow) {
                        ColorRGBA mapped;
                        if (foreground) {
                            mapped = MixThemeColor(config.themeMuted, config.themeText, std::clamp((hi - 0.45f) / 0.5f, 0.0f, 1.0f));
                        } else if (hi > 0.65f) {
                            mapped = config.themeText;
                        } else {
                            const float adjustment = 0.10f - hi * 0.42f;
                            mapped = config.themeSettingsPanel;
                            mapped.r = std::clamp(mapped.r + adjustment, 0.0f, 1.0f);
                            mapped.g = std::clamp(mapped.g + adjustment, 0.0f, 1.0f);
                            mapped.b = std::clamp(mapped.b + adjustment, 0.0f, 1.0f);
                        }
                        mapped.a = c.a;
                        result += RmlUiDetail::CssColor(mapped);
                        i = end;
                        continue;
                    }
                }
            }
            result += source[i++];
        }
        return result;
    }
}

using namespace RmlUiDetail;

// Keeps an immutable copy of the document's packaged RCSS. Theme changes are
// layered on top as a stylesheet, so newly created DOM automatically gets the
// current colors without rescanning every matching element after each
// SetInnerRML call.
void RmlUiController::CaptureBaseStyleSheet() {
    m_baseStyleSheet.reset();
    m_baseStyleSheetSource.clear();
    if (const auto file = m_fileInterface.Open("res://omnistats.rcss")) {
        m_baseStyleSheetSource.resize(m_fileInterface.Length(file));
        m_baseStyleSheetSource.resize(m_fileInterface.Read(m_baseStyleSheetSource.data(), m_baseStyleSheetSource.size(), file));
        m_fileInterface.Close(file);
    }
    if (!m_document) return;
    if (const auto* packagedStyle = m_document->GetStyleSheetContainer()) {
        if (auto emptyStyle = Rml::Factory::InstanceStyleSheetString("#__omnistats_theme_base__ { color: inherit; }"))
            m_baseStyleSheet = packagedStyle->CombineStyleSheetContainer(*emptyStyle);
    }
}

void RmlUiController::ReloadUiResources() {
    if (!m_document) return;
    Rml::Factory::ClearStyleSheetCache();
    Rml::Factory::ClearTemplateCache();
    m_document->ReloadStyleSheet();
    CaptureBaseStyleSheet();
    // View documents hold their markup in RML, so reload them whole.
    CloseViewDocuments();
    LoadViewDocuments();
    if (m_onboardingVisible) RefreshOnboarding();
    if (m_historyVisible) RefreshHistory(true);
    if (m_accountsVisible) RefreshAccounts(true);
    UpdateThemeProperties();
    RebuildVisibleUi(true, true);
    if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    ShowToast("Reloaded UI resources from " + m_fileInterface.OverrideDirectory());
}

void RmlUiController::UpdateThemeProperties() {
    if (!m_document || !m_baseStyleSheet) return;
    const ConfigData config = m_themeEditor.PreviewConfig();
    const ConfigData& m_config = config;
    const bool light = Themes::IsLight(config);
    Rml::SharedPtr<Rml::StyleSheetContainer> baseStyle = m_baseStyleSheet;
    if (!m_baseStyleSheetSource.empty()) {
        if (auto mapped = Rml::Factory::InstanceStyleSheetString(ThemeStyleSheet(m_baseStyleSheetSource, config, light))) {
            if (auto combined = m_baseStyleSheet->CombineStyleSheetContainer(*mapped)) baseStyle = std::move(combined);
        }
    }

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
    const std::string topbar = CssColor(m_config.themeTopbar);
    const std::string graphPanel = CssColor(m_config.themeGraphPanel);
    const auto scaledColor = [light](ColorRGBA color, float rgbScale, float alpha = -1.0f) {
        if (light && std::min({color.r, color.g, color.b}) > 0.55f && rgbScale != 1.0f) {
            const float adjustment = (1.0f - rgbScale) * 0.25f;
            color.r = std::clamp(color.r + adjustment, 0.0f, 1.0f);
            color.g = std::clamp(color.g + adjustment, 0.0f, 1.0f);
            color.b = std::clamp(color.b + adjustment, 0.0f, 1.0f);
        } else {
            color.r = std::clamp(color.r * rgbScale, 0.0f, 1.0f);
            color.g = std::clamp(color.g * rgbScale, 0.0f, 1.0f);
            color.b = std::clamp(color.b * rgbScale, 0.0f, 1.0f);
        }
        if (alpha >= 0.0f) color.a = std::clamp(alpha, 0.0f, 1.0f);
        return CssColor(color);
    };
    const ColorRGBA accentFill = m_config.themeAccent;
    const std::string accentDark = CssColor(config.themeSelection);
    const ColorRGBA accentMidColor{accentFill.r * 0.62f, accentFill.g * 0.62f, accentFill.b * 0.62f, 1.0f};
    const std::string accentMid = CssColor(accentMidColor);
    const auto labelOn = [&](ColorRGBA requested, ColorRGBA background) {
        background = Themes::Composite(background, config.themeSettingsPanel);
        return CssColor(Themes::Contrast(requested, background) >= 4.5 ? requested : Themes::ReadableText(background));
    };
    const std::string accentInk = labelOn(config.themeText, accentMidColor);

    // Theme overrides are compiled only when a theme value actually changes.
    // Keeping them in the document stylesheet means structural DOM rebuilds do
    // not need class/selector scans or local-property reapplication.
    std::ostringstream css;
    const auto rule = [&](std::string_view selectors, std::string_view property, const std::string& value) {
        // Prefix theme rules with the document id so the dynamic theme has the
        // same practical precedence as the old local SetProperty() pass, even
        // over more-specific packaged hover/state rules.
        size_t begin = 0;
        bool first = true;
        while (begin < selectors.size()) {
            const size_t comma = selectors.find(',', begin);
            const size_t end = comma == std::string_view::npos ? selectors.size() : comma;
            std::string_view selector = selectors.substr(begin, end - begin);
            while (!selector.empty() && selector.front() == ' ')
                selector.remove_prefix(1);
            while (!selector.empty() && selector.back() == ' ')
                selector.remove_suffix(1);
            if (!first) css << ", ";
            if (selector == "#app")
                css << selector;
            else
                css << "#app " << selector;
            first = false;
            if (comma == std::string_view::npos) break;
            begin = comma + 1;
        }
        css << " { " << property << ": " << value << "; }\n";
    };

    rule("#app", "color", text);
    rule(".card", "background-color", bg);
    rule(".dashboard-shell", "background-color", scaledColor(m_config.themeBg, 0.55f, 1.0f));
    for (const char* selector : {".card", ".card-title", ".value", ".metric-value", ".setting-name", ".setting-title", ".debug-value",
                                 ".tooltip-bubble", ".color-editor-panel", ".setting-section"})
        rule(selector, "color", text);
    rule("button, .button", "color", CssColor(config.themeButtonText));
    rule("input.text, input.password, select, select selectvalue, select selectbox option, textarea", "color", CssColor(config.themeInputText));
    rule("select selectbox, textarea", "background-color", CssColor(config.themeInputBg));
    rule("select, input.text, input.password, textarea", "border-color", CssColor(config.themeInputBorder));

    rule(".settings-window", "background-color", panel);
    rule(".settings-header", "background-color", scaledColor(m_config.themeSettingsPanel, 1.08f, 1.0f));
    rule(".settings-nav", "background-color", scaledColor(m_config.themeSettingsPanel, 0.90f, 1.0f));
    rule(".settings-page", "background-color", panel);
    rule(".settings-footer", "background-color", scaledColor(m_config.themeSettingsPanel, 1.05f, 1.0f));
    rule(".update-dialog", "background-color", panel);
    rule(".accounts-window", "background-color", panel);
    rule(".accounts-detail-card", "background-color", panel);
    rule(".accounts-dialog-card", "background-color", panel);

    // Top bar and graph panel are their own layers, deliberately not derived
    // from the shell background so each stays configurable on its own.
    rule(".dashboard-topbar", "background-color", topbar);
    rule(".win-icon-restore .restore-front", "background-color", topbar);
    rule(".graph-wrap", "background-color", graphPanel);

    for (const char* selector : {".card-subtitle", ".stat-section-title", ".label", ".metric-label", ".muted", ".badge",
                                 ".running-indicator", ".roster-footer", ".rank-table-row", ".debug-label", ".match-mode",
                                 ".lobby-rank-header", ".setting-help"})
        rule(selector, "color", muted);
    for (const char* selector : {".dim", ".lobby-rank-matches", ".dashboard-edit-zone", ".match-time"})
        rule(selector, "color", dim);
    rule(".lobby-rank-header", "background-color", scaledColor(m_config.themeBg, 0.70f, 0.80f));
    rule(".accent", "color", accent);
    for (const char* selector : {".badge.accent", ".accounts-active-label"}) {
        rule(selector, "color", accentInk);
        rule(selector, "background-color", accentMid);
        rule(selector, "border-color", accent);
    }
    rule(".accounts-active-dot", "background-color", accent);
    rule(".overlay-edit", "border-color", accent);
    rule(".overlay-card.dock-hover-target", "border-color", accent);
    rule(".overlay-card.dock-hover-target", "background-color", CssColor(MixThemeColor(m_config.themeBg, accentFill, 0.12f)));
    rule(".dashboard-widget.dragging", "border-color", accent);
    rule(".settings-nav button.active", "border-color", accent);
    rule(".settings-nav button.active", "background-color", accentDark);
    rule(".settings-nav button.active", "color", text);
    rule(".choice-card.selected", "border-color", accent);
    rule(".choice-card.selected, .session-row.item.selected", "background-color", accentDark);
    rule(".session-row.item:hover, .history-row.item:hover, .accounts-row.item:hover, .people-row.item:hover", "background-color", CssColor(config.themeRowHover));
    rule("select selectbox option:checked", "background-color", accentDark);
    rule("select selectbox option:hover, select selectbox option:checked", "color", CssColor(config.themeInputText));
    rule(".player-row", "background-color", CssColor(m_config.themeRosterCard));
    rule(".player-row.self", "background-color", CssColor(m_config.themeRosterCardSelf));
    for (const char* selector : {".note-badge", ".chip.chip-tag", ".scout-tag"}) {
        rule(selector, "background-color", CssColor(m_config.themeScoutTag));
        rule(selector, "color", CssColor(Themes::ReadableText(Themes::Composite(m_config.themeScoutTag, m_config.themeRosterCard))));
        rule(selector, "border-color", scaledColor(m_config.themeScoutTag, 1.35f, 1.0f));
    }
    rule(".note-badge:hover, .scout-tag:hover", "border-color", scaledColor(m_config.themeScoutTag, 1.9f, 1.0f));
    rule(".player-chip", "background-color", scaledColor(m_config.themeRosterCard, 1.3f, 0.85f));
    rule(".stat-cell", "background-color", CssColor(m_config.themeStatBox));
    rule(".mini-metric", "background-color", CssColor(m_config.themeStatBox));
    rule(".match-data", "background-color", CssColor(m_config.themeMatchRow));
    rule(".match-data.match-even", "background-color", CssColor(m_config.themeMatchRow));
    rule(".match-data.match-odd", "background-color", CssColor(m_config.themeMatchRowAlt));
    rule(".match-data:hover", "background-color", scaledColor(m_config.themeMatchRow, 1.35f, 0.85f));
    rule("button.primary, .button.primary", "background-color", CssColor(config.themePrimaryButton));
    rule("button.primary, .button.primary", "border-color", CssColor(config.themePrimaryBorder));
    rule("button.primary, .button.primary", "color", labelOn(config.themeButtonText, config.themePrimaryButton));
    rule("button.primary:hover, .button.primary:hover", "background-color", CssColor(config.themePrimaryHover));
    rule("button.primary:hover, .button.primary:hover", "color", labelOn(config.themeButtonText, config.themePrimaryHover));
    rule("button.primary:active, .button.primary:active", "background-color", CssColor(config.themePrimaryPressed));
    rule("button.primary:active, .button.primary:active", "color", labelOn(config.themeButtonText, config.themePrimaryPressed));
    rule("button.version-update", "color", accent);
    rule("button.version-update:hover", "color", accent);
    rule("button.version-update:hover", "background-color", accentDark);
    rule("button.version-update:hover", "border-color", accentMid);
    rule("button.version-update.failed", "color", loss);
    rule("input.text:focus, input.password:focus, select:focus, select:checked", "border-color", accent);
    rule("input.checkbox:checked", "border-color", scaledColor(m_config.themeAccent, 1.6f, 1.0f));
    rule("input.checkbox:checked", "background-color", accent);
    rule("input.checkbox:checked ~ .toggle-thumb", "background-color", accentInk);
    rule("input.range sliderprogress", "background-color", accentMid);
    rule("input.range sliderbar", "border-color", accent);
    rule("scrollbarvertical sliderbar:active, scrollbarhorizontal sliderbar:active", "background-color", accent);

    const std::string line = CssColor(config.themeBorder);
    ColorRGBA softBorder = config.themeBorder;
    softBorder.a *= 0.6f;
    const std::string lineSoft = CssColor(softBorder);
    const std::string accentBright = scaledColor(m_config.themeAccent, 1.6f, 1.0f);
    rule(".view-window", "background-color", panel);
    rule(".view-window", "border-color", line);
    rule(".view-header", "background-color", scaledColor(m_config.themeSettingsPanel, 1.08f, 1.0f));
    rule(".view-header", "border-bottom-color", line);
    rule(".view-tabs", "border-bottom-color", line);
    rule(".tab-button", "color", muted);
    rule(".tab-button:hover, .tab-button.active", "color", text);
    rule(".tab-button.active", "border-bottom-color", accentBright);
    rule(".tab-button.pill", "border-color", line);
    rule(".tab-button.pill.active", "border-color", accentBright);
    rule(".tab-button.pill.active", "background-color", accentDark);
    rule(".recap-card", "background-color", scaledColor(m_config.themeBg, 1.0f, 1.0f));
    rule(".recap-card", "border-color", line);
    rule(".recap-card", "border-top-color", accent);
    for (const char* selector : {".recap-title", ".recap-hero-value", ".recap-strip-value", ".recap-mode-name", ".recap-mode-record"})
        rule(selector, "color", text);
    for (const char* selector : {".recap-date", ".recap-brand", ".recap-dash", ".recap-games", ".recap-hero-label", ".recap-strip-label",
                                 ".recap-section-title", ".recap-mode-mmr", ".recap-empty"})
        rule(selector, "color", muted);
    rule(".recap-foot", "color", scaledColor(m_config.themeMuted, 1.0f, 0.6f));
    rule(".recap-hero-divider", "background-color", line);
    rule(".recap-bar, .recap-mode-track", "background-color", scaledColor(m_config.themeLoss, 1.0f, 0.85f));
    rule(".recap-mode-track.empty", "background-color", line);
    rule(".recap-bar-fill", "background-color", win);
    rule(".recap-strip", "background-color", CssColor(m_config.themeStatBox));
    rule(".recap-strip", "border-color", lineSoft);
    rule(".recap-strip-cell", "border-left-color", lineSoft);
    rule(".recap-mode-row", "border-top-color", lineSoft);
    rule(".people-table", "border-color", line);
    rule(".people-row", "border-top-color", lineSoft);
    rule(".people-row.header", "background-color", scaledColor(m_config.themeText, 1.0f, 0.03f));
    rule(".sessions-table", "border-color", line);
    rule(".session-row", "border-top-color", lineSoft);
    rule(".session-row.header", "background-color", scaledColor(m_config.themeText, 1.0f, 0.03f));
    rule(".session-bar-track", "background-color", scaledColor(m_config.themeLoss, 1.0f, 0.85f));
    rule(".recap-compare", "border-top-color", line);
    rule(".recap-compare-row", "border-top-color", lineSoft);
    rule(".history-toolbar", "border-bottom-color", line);
    rule(".history-table, .history-scoreboard-table", "border-color", line);
    rule(".history-row, .history-scoreboard-row", "border-top-color", lineSoft);
    rule(".history-row.header, .history-scoreboard-row.header", "background-color", scaledColor(m_config.themeText, 1.0f, 0.03f));
    rule(".history-detail-card", "background-color", panel);
    rule(".history-detail-card", "border-color", line);
    rule(".history-detail-header", "border-bottom-color", line);
    rule(".trend-track", "background-color", lineSoft);
    rule(".trend-bar", "background-color", muted);
    rule(".trend-bar.win", "background-color", win);
    rule(".trend-bar.loss", "background-color", loss);

    for (const char* selector : {".win", ".badge.win", ".match-win", ".match-win .match-mode", ".match-win .match-time"})
        rule(selector, "color", win);
    for (const char* selector : {".loss", ".badge.loss", ".match-loss", ".match-loss .match-mode", ".match-loss .match-time"})
        rule(selector, "color", loss);

    for (const auto& swatch : kPlatformPalette) {
        const std::string selector = std::string(".") + swatch.className;
        const std::string color = swatch.color;
        if (!light) rule(selector, "color", color);
        rule(".badge" + selector, "border-color", color + "66");
        rule(".badge" + selector, "background-color", color + "1f");
    }

    rule(".graph-polyline", "color", graph);
    rule(".graph-point", "background-color", graph);
    rule(".graph-point", "border-color", graphPanel);
    rule(".graph-point.start", "border-color", muted);
    rule(".graph-point-halo", "background-color", graph);
    rule(".graph-baseline", "color", baseline);
    rule(".graph-point.win, .graph-point-halo.win", "background-color", win);
    rule(".graph-point.loss, .graph-point-halo.loss", "background-color", loss);
    rule(".graph-point.neutral, .graph-point-halo.neutral", "background-color", muted);
    rule(".graph-point.estimated", "background-color", std::string("transparent"));
    rule(".graph-point.estimated", "border-color", text);
    rule(".graph-gridline", "background-color", CssColor(config.themeGraphGrid));
    rule(".theme-preset.selected", "border-color", accent);
    rule(".theme-field-error, .theme-message.error, .theme-invalid", "color", loss);
    rule(".theme-invalid", "border-color", loss);
    for (const char* tier : {"Unranked", "Bronze", "Silver", "Gold", "Platinum", "Diamond", "Champion", "Grand Champion", "Supersonic Legend"}) {
        const ColorRGBA rank = Format::RankColor(tier);
        rule("[data-rank-color='" + CssColor(rank) + "']", "color",
             CssColor(light ? AccentOn(rank, config.themeSettingsPanel) : rank));
    }
    rule(".settings-footer button.ghost", "color", labelOn(config.themeButtonText, config.themeSettingsPanel));
    rule(".settings-footer button:disabled", "opacity", "0.55");

    css << FontOverrideCss();

    auto themeStyle = Rml::Factory::InstanceStyleSheetString(css.str());
    if (!themeStyle) return;
    auto combined = baseStyle->CombineStyleSheetContainer(*themeStyle);
    if (combined) {
        // The view documents link the same packaged RCSS, so one combined
        // sheet serves all of them.
        for (Rml::ElementDocument* document : {m_document, m_insightsDoc, m_onboardingDoc, m_historyDoc, m_accountsDoc, m_playerNoteDoc})
            if (document) document->SetStyleSheetContainer(combined);
        m_renderDirty = true;
    }
}

// Maps a pointer position inside the saturation/value field or hue strip onto the
// color being edited. The picker rect is captured once on mousedown. The color is
// previewed entirely in the persistent DOM while dragging, then committed once on
// mouse-up. In particular, do not rebuild the dynamic theme stylesheet here: this
// function runs synchronously from WM_MOUSEMOVE, and stylesheet compilation/layout
// on every pointer sample makes the picker visibly lag behind the cursor.
void RmlUiController::ThemeEditor::ApplyColorPick(float mouseX, float mouseY) {
    ColorRGBA* current = Color(m_editColorKey);
    if (!current) return;

    const float u = std::clamp((mouseX - m_owner.m_layoutDrag.m_drag.startX) / m_owner.m_layoutDrag.m_drag.startW, 0.0f, 1.0f);
    const float v = std::clamp((mouseY - m_owner.m_layoutDrag.m_drag.startY) / m_owner.m_layoutDrag.m_drag.startH, 0.0f, 1.0f);
    const Hsv existing = RgbToHsv(*current);
    Hsv picked = existing;
    picked.h = existing.s > 0.0f ? existing.h : m_editColorHue;
    if (m_owner.m_layoutDrag.m_drag.kind == DragKind::ColorHue) {
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

    *current = HsvToRgb(picked, current->a);
    m_colorPickDirty = true;
    RefreshColorPickPreview(m_owner.m_layoutDrag.m_drag.kind == DragKind::ColorHue);
}

void RmlUiController::ThemeEditor::RefreshColorPickPreview(bool updateFieldGradient) {
    if (!m_owner.m_document || m_owner.m_settingsPage != SettingsPage::Appearance) return;

    const ColorRGBA* editing = Color(m_editColorKey);
    if (!editing) return;

    const Hsv hsv = RgbToHsv(*editing);
    const float hue = hsv.s > 0.0f ? hsv.h : m_editColorHue;
    const auto percent = [](float value) {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%.2f%%", std::clamp(value, 0.0f, 1.0f) * 100.0f);
        return std::string(text);
    };

    // Updating the procedural SV image can require a texture lookup/upload. Six
    // degree buckets are visually indistinguishable at this control size, while
    // cutting a full hue sweep from hundreds of texture changes to at most 60.
    if (updateFieldGradient) {
        const int gradientHue = QuantizedPickerHue(hue);
        if (gradientHue != m_colorPickerGradientHue) {
            if (auto* field = m_owner.m_document->GetElementById("theme-color-field"))
                field->SetProperty("decorator", "image(gen://sv?h=" + std::to_string(gradientHue) + ")");
            m_colorPickerGradientHue = gradientHue;
        }
    }

    if (auto* marker = m_owner.m_document->GetElementById("theme-color-field-marker")) {
        marker->SetProperty("left", percent(hsv.s));
        marker->SetProperty("top", percent(1.0f - hsv.v));
    }
    if (auto* marker = m_owner.m_document->GetElementById("theme-color-hue-marker"))
        marker->SetProperty("left", percent(hue / 360.0f));

    const std::string css = m_owner.CssColor(*editing);
    if (auto* preview = m_owner.m_document->GetElementById("theme-color-preview"))
        preview->SetProperty("background-color", css);
    if (auto* swatch = m_owner.m_document->GetElementById((std::string("theme-swatch-") + m_editColorKey).c_str()))
        swatch->SetProperty("background-color", css);
    if (auto* element = m_owner.m_document->GetElementById((std::string("theme-hex-") + m_editColorKey).c_str())) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(css);
    }
}

void RmlUiController::ThemeEditor::RefreshThemeEditorControls(std::string_view preserveHexKey) {
    if (!m_owner.m_document || m_owner.m_settingsPage != SettingsPage::Appearance) return;

    const ConfigData config = PreviewConfig();
    RefreshStatus();
    for (const auto& field : Themes::Fields()) {
        const char* key = field.key;
        const ColorRGBA* color = &(config.*field.member);
        if (!color) continue;
        const std::string css = m_owner.CssColor(*color);
        if (auto* swatch = m_owner.m_document->GetElementById((std::string("theme-swatch-") + key).c_str()))
            swatch->SetProperty("background-color", css);
        if (preserveHexKey != key && !m_invalidColors.contains(key)) {
            if (auto* element = m_owner.m_document->GetElementById((std::string("theme-hex-") + key).c_str())) {
                if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(css);
            }
        }
    }

    const ColorRGBA* editing = ThemeColorForKey(config, m_editColorKey);
    if (!editing) return;
    const Hsv hsv = RgbToHsv(*editing);
    const float hue = hsv.s > 0.0f ? hsv.h : m_editColorHue;
    const auto channelByte = [](float value) {
        if (!std::isfinite(value)) return 0;
        return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
    };
    const auto percent = [](float value) {
        char text[24]{};
        std::snprintf(text, sizeof(text), "%.2f%%", std::clamp(value, 0.0f, 1.0f) * 100.0f);
        return std::string(text);
    };

    const int gradientHue = QuantizedPickerHue(hue);
    if (auto* field = m_owner.m_document->GetElementById("theme-color-field"))
        field->SetProperty("decorator", "image(gen://sv?h=" + std::to_string(gradientHue) + ")");
    m_colorPickerGradientHue = gradientHue;
    if (auto* marker = m_owner.m_document->GetElementById("theme-color-field-marker")) {
        marker->SetProperty("left", percent(hsv.s));
        marker->SetProperty("top", percent(1.0f - hsv.v));
    }
    if (auto* marker = m_owner.m_document->GetElementById("theme-color-hue-marker"))
        marker->SetProperty("left", percent(hue / 360.0f));
    if (auto* preview = m_owner.m_document->GetElementById("theme-color-preview"))
        preview->SetProperty("background-color", m_owner.CssColor(*editing));

    const std::array<std::pair<char, int>, 4> channels = {{{'r', channelByte(editing->r)},
                                                           {'g', channelByte(editing->g)},
                                                           {'b', channelByte(editing->b)},
                                                           {'a', channelByte(editing->a)}}};
    for (const auto [component, value] : channels) {
        const std::string id = std::string("theme-color-") + component;
        if (auto* element = m_owner.m_document->GetElementById(id.c_str())) {
            if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(std::to_string(value));
        }
        if (auto* label = m_owner.m_document->GetElementById((id + "-value").c_str())) m_owner.SetElementText(label, std::to_string(value));
    }
}

void RmlUiController::ThemeEditor::CommitColorPick() {
    if (!m_colorPickDirty) return;
    const ColorRGBA* current = Color(m_editColorKey);
    if (!current) {
        m_colorPickDirty = false;
        return;
    }
    if (m_editColorKey == "theme_accent") {
        ConfigData config = PreviewConfig();
        Themes::SetAccent(config, *current);
        m_draft = Themes::Read(config);
    }
    Themes::LinkColor(EnsureDraft(), m_editColorKey);
    m_invalidColors.erase(m_editColorKey);
    m_colorPickDirty = false;
    // The picker itself stays lightweight while the mouse is down. Apply the
    // selected color to the rest of the app once, after the drag has finished.
    m_owner.UpdateThemeProperties();
    RefreshThemeEditorControls();
}
