#include "ui/rml/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
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

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

// Keeps an immutable copy of the document's packaged RCSS. Theme changes are
// layered on top as a stylesheet, so newly created DOM automatically gets the
// current colors without rescanning every matching element after each
// SetInnerRML call.
void RmlUiController::CaptureBaseStyleSheet() {
    m_baseStyleSheet.reset();
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
    UpdateThemeProperties();
    RebuildVisibleUi(true, true);
    if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    ShowToast("Reloaded UI resources from " + m_fileInterface.OverrideDirectory());
}

void RmlUiController::UpdateThemeProperties() {
    if (!m_document || !m_baseStyleSheet) return;

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
    const auto scaledColor = [](ColorRGBA color, float rgbScale, float alpha = -1.0f) {
        color.r = std::clamp(color.r * rgbScale, 0.0f, 1.0f);
        color.g = std::clamp(color.g * rgbScale, 0.0f, 1.0f);
        color.b = std::clamp(color.b * rgbScale, 0.0f, 1.0f);
        if (alpha >= 0.0f) color.a = std::clamp(alpha, 0.0f, 1.0f);
        return CssColor(color);
    };
    const std::string accentDark = scaledColor(m_config.themeAccent, 0.28f, 0.95f);
    const std::string accentMid = scaledColor(m_config.themeAccent, 0.62f, 1.0f);

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
    for (const char* selector : {".card", ".card-title", ".value", ".metric-value", ".setting-name", ".debug-value",
                                 "button, .button, input.text, input.password, select", "select selectvalue, select selectbox option",
                                 ".tooltip-bubble"})
        rule(selector, "color", text);

    rule(".settings-window", "background-color", panel);
    rule(".settings-header", "background-color", scaledColor(m_config.themeSettingsPanel, 1.08f, 1.0f));
    rule(".settings-nav", "background-color", scaledColor(m_config.themeSettingsPanel, 0.90f, 1.0f));
    rule(".settings-page", "background-color", panel);
    rule(".settings-footer", "background-color", scaledColor(m_config.themeSettingsPanel, 1.05f, 1.0f));
    rule(".update-dialog", "background-color", panel);

    // Top bar and graph panel are their own layers, deliberately not derived
    // from the shell background so each stays configurable on its own.
    rule(".dashboard-topbar", "background-color", topbar);
    rule(".win-icon-restore .restore-front", "background-color", topbar);
    rule(".graph-wrap", "background-color", graphPanel);

    for (const char* selector : {".card-subtitle", ".stat-section-title", ".label", ".metric-label", ".muted", ".badge",
                                 ".running-indicator", ".roster-footer", ".rank-table-row", ".debug-label", ".match-mode",
                                 ".lobby-rank-header"})
        rule(selector, "color", muted);
    for (const char* selector : {".dim", ".lobby-rank-matches", ".dashboard-edit-zone", ".setting-help", ".match-time"})
        rule(selector, "color", dim);
    rule(".lobby-rank-header", "background-color", scaledColor(m_config.themeBg, 0.70f, 0.80f));
    for (const char* selector : {".accent", ".setting-title", ".badge.accent"})
        rule(selector, "color", accent);
    rule(".overlay-edit", "border-color", accent);
    rule(".dashboard-widget.dragging", "border-color", accent);
    rule(".settings-nav button.active", "border-color", accent);
    rule(".settings-nav button.active", "background-color", accentDark);
    rule(".player-row", "background-color", CssColor(m_config.themeRosterCard));
    rule(".player-row.self", "background-color", CssColor(m_config.themeRosterCardSelf));
    rule(".player-chip", "background-color", scaledColor(m_config.themeRosterCard, 1.3f, 0.85f));
    rule(".stat-cell", "background-color", CssColor(m_config.themeStatBox));
    rule(".mini-metric", "background-color", CssColor(m_config.themeStatBox));
    rule(".match-data", "background-color", CssColor(m_config.themeMatchRow));
    rule(".match-data.match-even", "background-color", CssColor(m_config.themeMatchRow));
    rule(".match-data.match-odd", "background-color", CssColor(m_config.themeMatchRowAlt));
    rule(".match-data:hover", "background-color", scaledColor(m_config.themeMatchRow, 1.35f, 0.85f));
    rule("button.primary, .button.primary", "background-color", accentMid);
    rule("button.primary, .button.primary", "border-color", accent);
    rule("button.version-update", "color", accent);
    rule("button.version-update:hover", "color", accent);
    rule("button.version-update:hover", "background-color", accentDark);
    rule("button.version-update:hover", "border-color", accentMid);
    rule("button.version-update.failed", "color", loss);
    rule("input.text:focus, input.password:focus, select:focus, select:checked", "border-color", accent);
    rule("input.checkbox:checked", "border-color", accent);
    rule("input.checkbox:checked", "background-color", accentDark);
    rule("input.range sliderprogress", "background-color", accentMid);
    rule("input.range sliderbar", "border-color", accent);
    rule("scrollbarvertical sliderbar:active, scrollbarhorizontal sliderbar:active", "background-color", accent);
    rule("select selectbox option:checked", "background-color", accentDark);

    for (const char* selector : {".win", ".badge.win", ".match-win", ".match-win .match-mode", ".match-win .match-time"})
        rule(selector, "color", win);
    for (const char* selector : {".loss", ".badge.loss", ".match-loss", ".match-loss .match-mode", ".match-loss .match-time"})
        rule(selector, "color", loss);

    for (const auto& swatch : kPlatformPalette) {
        const std::string selector = std::string(".") + swatch.className;
        const std::string color = swatch.color;
        rule(selector, "color", color);
        rule(".badge" + selector, "border-color", color + "66");
        rule(".badge" + selector, "background-color", color + "1f");
    }

    rule(".graph-line", "background-color", graph);
    rule(".graph-polyline", "color", graph);
    rule(".graph-point", "background-color", graph);
    rule(".graph-point-halo", "background-color", graph);
    rule(".graph-line.baseline", "background-color", baseline);
    rule(".graph-point.win, .graph-point-halo.win", "background-color", win);
    rule(".graph-point.loss, .graph-point-halo.loss", "background-color", loss);
    rule(".graph-point.neutral, .graph-point-halo.neutral", "background-color", muted);
    rule(".graph-point.estimated", "background-color", std::string("transparent"));
    rule(".graph-point.estimated", "border-color", text);

    auto themeStyle = Rml::Factory::InstanceStyleSheetString(css.str());
    if (!themeStyle) return;
    auto combined = m_baseStyleSheet->CombineStyleSheetContainer(*themeStyle);
    if (combined) {
        // The view documents link the same packaged RCSS, so one combined
        // sheet serves all of them.
        for (Rml::ElementDocument* document : {m_document, m_insightsDoc, m_onboardingDoc})
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

    *current = HsvToRgb(picked, current->a);
    m_colorPickDirty = true;
    RefreshColorPickPreview(m_drag.kind == DragKind::ColorHue);
}

void RmlUiController::RefreshColorPickPreview(bool updateFieldGradient) {
    if (!m_document || m_settingsPage != SettingsPage::Appearance) return;

    const ColorRGBA* editing = ThemeColorForKey(m_config, m_editColorKey);
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
            if (auto* field = m_document->GetElementById("theme-color-field"))
                field->SetProperty("decorator", "image(gen://sv?h=" + std::to_string(gradientHue) + ")");
            m_colorPickerGradientHue = gradientHue;
        }
    }

    if (auto* marker = m_document->GetElementById("theme-color-field-marker")) {
        marker->SetProperty("left", percent(hsv.s));
        marker->SetProperty("top", percent(1.0f - hsv.v));
    }
    if (auto* marker = m_document->GetElementById("theme-color-hue-marker"))
        marker->SetProperty("left", percent(hue / 360.0f));

    const std::string css = CssColor(*editing);
    if (auto* preview = m_document->GetElementById("theme-color-preview"))
        preview->SetProperty("background-color", css);
    if (auto* swatch = m_document->GetElementById((std::string("theme-swatch-") + m_editColorKey).c_str()))
        swatch->SetProperty("background-color", css);
    if (auto* element = m_document->GetElementById((std::string("theme-hex-") + m_editColorKey).c_str())) {
        if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(css);
    }
}

void RmlUiController::RefreshThemeEditorControls(std::string_view preserveHexKey) {
    if (!m_document || m_settingsPage != SettingsPage::Appearance) return;

    constexpr std::array<const char*, 17> kThemeKeys = {
        "theme_bg", "theme_panel", "theme_topbar", "theme_graph_panel",
        "theme_roster_card", "theme_roster_card_self", "theme_stat_box", "theme_match_row", "theme_match_row_alt",
        "theme_text", "theme_accent", "theme_win", "theme_loss", "theme_dim", "theme_muted",
        "theme_graph", "theme_baseline"};

    for (const char* key : kThemeKeys) {
        const ColorRGBA* color = ThemeColorForKey(m_config, key);
        if (!color) continue;
        const std::string css = CssColor(*color);
        if (auto* swatch = m_document->GetElementById((std::string("theme-swatch-") + key).c_str()))
            swatch->SetProperty("background-color", css);
        if (preserveHexKey != key) {
            if (auto* element = m_document->GetElementById((std::string("theme-hex-") + key).c_str())) {
                if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(css);
            }
        }
    }

    const ColorRGBA* editing = ThemeColorForKey(m_config, m_editColorKey);
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
    if (auto* field = m_document->GetElementById("theme-color-field"))
        field->SetProperty("decorator", "image(gen://sv?h=" + std::to_string(gradientHue) + ")");
    m_colorPickerGradientHue = gradientHue;
    if (auto* marker = m_document->GetElementById("theme-color-field-marker")) {
        marker->SetProperty("left", percent(hsv.s));
        marker->SetProperty("top", percent(1.0f - hsv.v));
    }
    if (auto* marker = m_document->GetElementById("theme-color-hue-marker"))
        marker->SetProperty("left", percent(hue / 360.0f));
    if (auto* preview = m_document->GetElementById("theme-color-preview"))
        preview->SetProperty("background-color", CssColor(*editing));

    const std::array<std::pair<char, int>, 4> channels = {{{'r', channelByte(editing->r)},
                                                           {'g', channelByte(editing->g)},
                                                           {'b', channelByte(editing->b)},
                                                           {'a', channelByte(editing->a)}}};
    for (const auto [component, value] : channels) {
        const std::string id = std::string("theme-color-") + component;
        if (auto* element = m_document->GetElementById(id.c_str())) {
            if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(std::to_string(value));
        }
        if (auto* label = m_document->GetElementById((id + "-value").c_str())) SetElementText(label, std::to_string(value));
    }
}

void RmlUiController::CommitColorPick() {
    if (!m_colorPickDirty) return;
    const ColorRGBA* current = ThemeColorForKey(m_config, m_editColorKey);
    if (!current) {
        m_colorPickDirty = false;
        return;
    }
    const std::string key = m_editColorKey;
    const ColorRGBA color = *current;
    Config::Update([key, color](ConfigData& c) {
        if (ColorRGBA* target = ThemeColorForKey(c, key)) *target = color;
    });
    m_colorPickDirty = false;
    // The picker itself stays lightweight while the mouse is down. Apply the
    // selected color to the rest of the app once, after the drag has finished.
    UpdateThemeProperties();
    RefreshThemeEditorControls();
}
