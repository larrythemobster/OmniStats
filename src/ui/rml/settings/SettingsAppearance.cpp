#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/presenters/SettingsPresenter.hpp"
#include "ui/rml/services/FontService.hpp"
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "core/config/Themes.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string SettingsPresenter::RenderAppearance(const ConfigData& config, const std::string& editColorKey, float editColorHue, bool showAdvanced,
                                                bool pending, const std::string& nameDraft, const std::string& importDraft,
                                                const std::string& message, bool messageError, bool canUndo,
                                                const std::set<std::string>& openGroups) {
    auto rangeRow = [&](const char* channel, char component, int value) {
        std::ostringstream html;
        html << "<div class='color-channel'><span class='color-channel-label'>" << channel << "</span>"
             << "<input id='theme-color-" << component << "' type='range' class='range color-range' min='0' max='255' step='1' data-setting='"
             << editColorKey << ':' << component << "' value='" << value << "'/>"
             << "<span id='theme-color-" << component << "-value' class='mono color-channel-value'>" << value << "</span></div>";
        return html.str();
    };
    auto channelByte = [](float value) {
        if (!std::isfinite(value)) return 0;
        return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
    };
    auto renderColorEditor = [&](const char* key, const ColorRGBA& color) {
        const Hsv hsv = RgbToHsv(color);
        const float hue = hsv.s > 0.0f ? hsv.h : editColorHue;
        const auto percent = [](float value) {
            std::ostringstream text;
            text << std::fixed << std::setprecision(2) << std::clamp(value, 0.0f, 1.0f) * 100.0f;
            return text.str();
        };
        std::ostringstream ed;
        ed << "<div class='color-editor-panel' id='active-color-editor'>"
           << "<div class='row' style='margin-bottom:6dp'><div class='setting-name grow'>Editing "
           << Escape(ThemeColorLabel(key)) << "</div>"
           << "<button class='ghost compact' data-action='close-color-editor'>Close</button></div>"
           << "<div class='color-picker'>"
           << "<div id='theme-color-field' class='color-field' data-action='color-field' style='decorator: image(gen://sv?h="
           << QuantizedPickerHue(hue) << ");'>"
           << "<div id='theme-color-field-marker' class='color-field-marker' style='left:" << percent(hsv.s) << "%;top:" << percent(1.0f - hsv.v) << "%'></div>"
           << "</div>"
           << "<div id='theme-color-hue' class='color-hue' data-action='color-hue'>"
           << "<div id='theme-color-hue-marker' class='color-hue-marker' style='left:" << percent(hue / 360.0f) << "%'></div>"
           << "</div></div>"
           << "<div id='theme-color-preview' class='color-editor-preview' style='background-color:" << CssColor(color) << "'></div>"
           << rangeRow("R", 'r', channelByte(color.r))
           << rangeRow("G", 'g', channelByte(color.g))
           << rangeRow("B", 'b', channelByte(color.b))
           << rangeRow("A", 'a', channelByte(color.a))
           << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("close-color-editor", "Done", "ghost") << "</div>"
           << "</div>";
        return ed.str();
    };
    auto colorRow = [&](const Themes::ColorField& field) {
        const char* key = field.key;
        const ColorRGBA& color = config.*(field.member);
        std::ostringstream html;
        const bool isEditing = (editColorKey == key);
        html << "<div class='theme-color-row'><div class='setting-row"
             << (isEditing ? " color-row-active" : "") << "'><div class='setting-info'><div class='setting-name'>" << Escape(field.label)
             << "</div><div class='setting-help'>" << Escape(field.help) << "</div></div>"
             << "<button id='theme-swatch-" << key << "' class='color-dot color-dot-button" << (isEditing ? " active" : "") << "' style='background-color:" << CssColor(color)
             << "' data-action='edit-color' data-color-key='" << key << "' aria-label='Edit " << Escape(field.label) << "'></button>"
             << "<input id='theme-hex-" << key << "' type='text' class='text mono theme-hex' data-setting='" << key
             << "' value='" << CssColor(color) << "'/></div><div id='theme-error-" << key << "' class='theme-field-error'></div>";
        if (isEditing && editColorKey != "theme_accent") html << renderColorEditor(key, color);
        html << "</div>";
        return html.str();
    };

    struct ScaleChoice {
        float scale;
        const char* value;
        const char* label;
    };
    static constexpr ScaleChoice kScales[] = {
        {0.75f, "0.75", "75%"}, {0.8f, "0.8", "80%"}, {0.85f, "0.85", "85%"}, {0.9f, "0.9", "90%"}, {0.95f, "0.95", "95%"}, {1.0f, "1.0", "100%"}, {1.1f, "1.1", "110%"}, {1.25f, "1.25", "125%"}, {1.5f, "1.5", "150%"}};
    std::vector<SelectOption> scales;
    std::string currentScale;
    for (const auto& choice : kScales) {
        scales.push_back({choice.value, choice.label});
        if (std::fabs(config.ui_scale - choice.scale) < .01f) currentScale = choice.value;
    }

    auto fontOptions = [&](const std::string& current) {
        static constexpr const char* kBundled[] = {"Inter", "JetBrains Mono", "Russo One"};
        std::vector<SelectOption> options;
        options.push_back({"", "Default"});
        for (const char* family : kBundled)
            options.push_back({family, std::string(family) + " (bundled)"});
        bool currentListed = current.empty() || std::any_of(std::begin(kBundled), std::end(kBundled), [&](const char* family) { return current == family; });
        for (const auto& family : FontService::SystemFontFamilies()) {
            if (std::any_of(std::begin(kBundled), std::end(kBundled), [&](const char* bundled) { return family == bundled; })) continue;
            currentListed |= family == current;
            options.push_back({family, family});
        }
        if (!currentListed) options.push_back({current, current + " (not installed)"});
        return options;
    };

    std::ostringstream out;
    out << SectionStart("Themes")
        << "<div class='setting-help section-help'>Choose a starting point, then adjust the accent or individual colors.</div>"
        << "<div class='theme-presets'>";
    const auto current = Themes::Read(config);
    for (const auto& preset : Themes::Presets()) {
        const auto& p = preset.colors;
        out << "<button class='theme-preset" << (Themes::Equal(current, p) ? " selected" : "")
            << "' data-action='theme-preset' data-theme-id='" << Escape(preset.id) << "'>"
            << "<div class='theme-mini' style='background-color:" << CssColor(p[0]) << ";color:" << CssColor(p[10]) << "'>"
            << "<div class='theme-mini-roster' style='background-color:" << CssColor(p[4]) << "'>Player <span style='color:" << CssColor(p[11]) << "'>YOU</span></div>"
            << "<div class='theme-mini-bottom'><span class='theme-mini-button' style='background-color:" << CssColor(p[18])
            << ";border-color:" << CssColor(p[19]) << ";color:" << CssColor(Themes::Contrast(p[22], p[18]) >= 4.5 ? p[22] : Themes::ReadableText(p[18])) << "'>Play</span>"
            << "<span class='theme-mini-graph' style='border-color:" << CssColor(p[17]) << ";color:" << CssColor(p[16]) << "'>&#9585;&#9586;&#9585;</span></div></div>"
            << "<div class='theme-preset-name'>" << Escape(preset.name) << "</div></button>";
    }
    out << "</div><div class='setting-row'><div class='setting-info'><div class='setting-name'>Accent</div>"
        << "<div class='setting-help'>Used for actions, selection and scout tags.</div></div>"
        << "<button id='theme-accent-swatch' class='color-dot color-dot-button' style='background-color:" << CssColor(config.themeAccent)
        << "' data-action='edit-color' data-color-key='theme_accent' aria-label='Edit accent color'></button></div>";
    if (editColorKey == "theme_accent") out << renderColorEditor("theme_accent", config.themeAccent);
    out << "<div class='theme-apply-row'><span id='theme-draft-status' class='setting-help grow'>"
        << (pending ? "Unapplied changes. Apply to keep them; closing Settings discards them." : "No unapplied changes.")
        << "</span></div>"
        << "<div id='theme-validation-message' class='theme-message" << (messageError ? " error" : "") << "'>" << Escape(message) << "</div><div id='theme-warnings' class='theme-warning'>";
    for (const auto& warning : Themes::Warnings(config))
        out << Escape(warning) << " ";
    out << "</div>";
    out << ActionRow("Reset colors", "Restore the default colors without changing your layout.",
                     Button("reset-theme-colors", "Reset colors", "ghost") + (canUndo ? Button("theme-undo-reset", "Undo reset", "ghost") : std::string{}))
        << "<div class='theme-saved-heading'>Saved themes</div>"
        << "<div class='theme-input-row'><input type='text' class='text' data-setting='theme_name' value='" << Escape(nameDraft)
        << "' placeholder='Theme name'/>" << Button("theme-save", "Save", "ghost") << Button("theme-copy", "Copy current theme", "ghost") << "</div>";
    if (config.custom_themes.empty()) out << "<div class='setting-help'>Save a theme to use it again later.</div>";
    for (size_t i = 0; i < config.custom_themes.size(); ++i) {
        out << "<div class='theme-saved-row'><span class='grow'>" << Escape(config.custom_themes[i].name)
            << "</span><button class='ghost' data-action='theme-load' data-theme-index='" << i << "'>Load</button>"
            << "<button class='ghost' data-action='theme-delete' data-theme-index='" << i << "'>Delete</button></div>";
    }
    out << "<div class='theme-saved-heading'>Import theme</div><div class='setting-help'>Paste a theme code. Imports are previewed until you apply them.</div>"
        << "<div class='theme-input-row'><input type='text' class='text mono theme-import' data-setting='theme_import' value='" << Escape(importDraft)
        << "' placeholder='Paste theme code'/>" << Button("theme-import", "Import", "ghost") << "</div>" << SectionEnd();
    out << "<div class='settings-disclosure'>"
        << Button("toggle-advanced-appearance", showAdvanced ? "Hide advanced colors" : "Show advanced colors", "ghost")
        << "</div>";
    if (showAdvanced) {
        out << SectionStart("Advanced colors")
            << "<div class='setting-help section-help'>Click a swatch or edit its hex value.</div>";
        const std::string activeGroup = openGroups.empty() ? "Surfaces" : *openGroups.begin();
        std::set<std::string> renderedGroups;
        out << "<div class='theme-group-tabs'>";
        for (const auto& field : Themes::Fields()) {
            if (field.linkedTo != nullptr) continue;
            const std::string group = field.group;
            if (!renderedGroups.insert(group).second) continue;
            out << "<button class='theme-group-toggle ghost" << (group == activeGroup ? " active" : "")
                << "' data-action='theme-toggle-group' data-theme-group='" << Escape(group) << "'>"
                << Escape(group) << "</button>";
        }
        out << "</div><div class='theme-group'>";
        for (const auto& field : Themes::Fields()) {
            if (field.linkedTo == nullptr && activeGroup == field.group) out << colorRow(field);
        }
        out << "</div>";
        out << SectionEnd();
    }
    out << SectionStart("Scale")
        << SelectRow("ui_scale", "App-wide text size", "Changes text size throughout the dashboard and overlay.", scales, currentScale)
        << SectionEnd();
    out << SectionStart("Units and formatting")
        << SelectRow("speed_units", "Speed units", "", {{"metric", "Kilometers per hour"}, {"imperial", "Miles per hour"}}, config.imperial_units ? "imperial" : "metric")
        << SelectRow("crossbar_display_mode", "Crossbar hit display", "", {{"raw", "Raw Force (UU/s)"}, {"speed", "Speed (MPH/KPH)"}}, config.crossbar_display_mode)
        << ToggleControl("use_roman_numerals", "Use Roman numerals", "I, II, III in rank labels.", config.use_roman_numerals)
        << SectionEnd();
    out << SectionStart("Fonts")
        << ActionRow("Reset fonts", "Restore Inter, JetBrains Mono, and Russo One.", Button("reset-fonts", "Reset", "ghost"))
        << SelectRow("font_ui", "Text", "Labels, names, and body text. Default: Inter.", fontOptions(config.font_ui), config.font_ui)
        << SelectRow("font_mono", "Numbers", "MMR, scores, graph labels, and stat values. Default: JetBrains Mono.", fontOptions(config.font_mono), config.font_mono)
        << SelectRow("font_display", "Titles", "Card titles, the app name, and recap headlines. Default: Russo One.", fontOptions(config.font_display), config.font_display)
        << SectionEnd();

    return out.str();
}

std::string RmlUiController::RenderSettingsAppearance() {
    return m_settingsPresenter->RenderAppearance(m_themeEditor.PreviewConfig(), m_themeEditor.m_editColorKey,
                                                 m_themeEditor.m_editColorHue, m_showAdvancedAppearance, m_themeEditor.Pending(), m_themeEditor.m_nameDraft,
                                                 m_themeEditor.m_importDraft, m_themeEditor.m_message, m_themeEditor.m_messageError,
                                                 m_themeEditor.m_resetUndo.has_value(), m_themeEditor.m_openGroups);
}
