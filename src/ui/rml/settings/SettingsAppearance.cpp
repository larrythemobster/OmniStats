#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/presenters/SettingsPresenter.hpp"
#include "ui/rml/services/FontService.hpp"
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

using namespace RmlUiDetail;

std::string SettingsPresenter::RenderAppearance(const ConfigData& config, const std::string& editColorKey, float editColorHue, bool showAdvanced) {
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
           << "<div class='row' style='margin-bottom:6dp'><div class='setting-name grow' style='color:#eef1f5'>Editing "
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
    auto colorRow = [&](const char* key, const char* label, const ColorRGBA& color) {
        std::ostringstream html;
        const bool isEditing = (editColorKey == key);
        html << "<div class='setting-row" << (isEditing ? " color-row-active" : "") << "'><div class='setting-info'><div class='setting-name'>" << label
             << "</div></div>"
             << "<button id='theme-swatch-" << key << "' class='color-dot color-dot-button" << (isEditing ? " active" : "") << "' style='background-color:" << CssColor(color)
             << "' data-action='edit-color' data-color-key='" << key << "'></button>"
             << "<input id='theme-hex-" << key << "' type='text' class='text mono' style='width:100dp;margin-left:6dp' data-setting='" << key
             << "' value='" << CssColor(color) << "'/></div>";
        if (isEditing) {
            html << renderColorEditor(key, color);
        }
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
    out << SectionStart("Scale")
        << SelectRow("ui_scale", "App-wide text size", "Changes text size throughout the dashboard and overlay.", scales, currentScale)
        << SectionEnd();
    out << SectionStart("Units and formatting")
        << SelectRow("speed_units", "Speed units", "", {{"metric", "Kilometers per hour"}, {"imperial", "Miles per hour"}}, config.imperial_units ? "imperial" : "metric")
        << SelectRow("crossbar_display_mode", "Crossbar hit display", "", {{"raw", "Raw Force (UU/s)"}, {"speed", "Speed (MPH/KPH)"}}, config.crossbar_display_mode)
        << ToggleControl("use_roman_numerals", "Use Roman numerals", "I, II, III in rank labels.", config.use_roman_numerals)
        << SectionEnd()
        << "<div class='settings-disclosure'>"
        << Button("toggle-advanced-appearance", showAdvanced ? "Hide fonts and colors" : "Show fonts and colors", "ghost")
        << "</div>";
    if (!showAdvanced) return out.str();
    out << SectionStart("Fonts")
        << ActionRow("Reset fonts", "Restore Inter, JetBrains Mono, and Russo One.", Button("reset-fonts", "Reset", "ghost"))
        << SelectRow("font_ui", "Text", "Labels, names, and body text. Default: Inter.", fontOptions(config.font_ui), config.font_ui)
        << SelectRow("font_mono", "Numbers", "MMR, scores, graph labels, and stat values. Default: JetBrains Mono.", fontOptions(config.font_mono), config.font_mono)
        << SelectRow("font_display", "Titles", "Card titles, the app name, and recap headlines. Default: Russo One.", fontOptions(config.font_display), config.font_display)
        << SectionEnd();
    out << SectionStart("Colors")
        << "<div class='setting-help section-help'>Click a swatch to pick a color, or edit the hex value.</div>"
        << ActionRow("Reset colors", "Restore the default color theme. Reset theme and layout also restores card positions.",
                     Button("reset-theme-colors", "Reset colors", "ghost") + Button("reset-theme-and-layout", "Reset theme and layout", "ghost"))
        << colorRow("theme_bg", "Overlay background", config.themeBg)
        << colorRow("theme_panel", "Settings panels", config.themeSettingsPanel)
        << colorRow("theme_topbar", "Top bar", config.themeTopbar)
        << colorRow("theme_graph_panel", "Graph panel", config.themeGraphPanel)
        << colorRow("theme_roster_card", "Roster card", config.themeRosterCard)
        << colorRow("theme_roster_card_self", "Roster card (you)", config.themeRosterCardSelf)
        << colorRow("theme_scout_tag", "Scout badge & tags", config.themeScoutTag)
        << colorRow("theme_stat_box", "Stat boxes / K/D", config.themeStatBox)
        << colorRow("theme_match_row", "Previous games row", config.themeMatchRow)
        << colorRow("theme_match_row_alt", "Previous games row (alt)", config.themeMatchRowAlt)
        << colorRow("theme_text", "Text", config.themeText)
        << colorRow("theme_accent", "Accent", config.themeAccent)
        << colorRow("theme_win", "Win", config.themeWin)
        << colorRow("theme_loss", "Loss", config.themeLoss)
        << colorRow("theme_dim", "Dim text", config.themeDim)
        << colorRow("theme_muted", "Muted info", config.themeMuted)
        << colorRow("theme_graph", "Graph line", config.themeGraphLine)
        << colorRow("theme_baseline", "Graph baseline", config.themeGraphBaseline)
        << SectionEnd();

    return out.str();
}

std::string RmlUiController::RenderSettingsAppearance() {
    return m_settingsPresenter->RenderAppearance(m_config, m_editColorKey, m_editColorHue, m_showAdvancedAppearance);
}
