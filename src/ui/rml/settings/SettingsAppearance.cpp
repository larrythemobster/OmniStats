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

std::string RmlUiController::RenderSettingsAppearance() {
    auto rangeRow = [&](const char* channel, char component, int value) {
        std::ostringstream html;
        html << "<div class='color-channel'><span class='color-channel-label'>" << channel << "</span>"
             << "<input id='theme-color-" << component << "' type='range' class='range color-range' min='0' max='255' step='1' data-setting='"
             << m_editColorKey << ':' << component << "' value='" << value << "'/>"
             << "<span id='theme-color-" << component << "-value' class='mono color-channel-value'>" << value << "</span></div>";
        return html.str();
    };
    auto channelByte = [](float value) {
        if (!std::isfinite(value)) return 0;
        return std::clamp(static_cast<int>(std::lround(value * 255.0f)), 0, 255);
    };
    auto renderColorEditor = [&](const char* key, const ColorRGBA& color) {
        const Hsv hsv = RgbToHsv(color);
        const float hue = hsv.s > 0.0f ? hsv.h : m_editColorHue;
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
        const bool isEditing = (m_editColorKey == key);
        html << "<div class='setting-row" << (isEditing ? " color-row-active" : "") << "'><div class='setting-info'><div class='setting-name'>" << label
             << "</div><div class='setting-help'>Click the swatch to pick a color, or edit the hex value.</div></div>"
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
        if (std::fabs(m_config.ui_scale - choice.scale) < .01f) currentScale = choice.value;
    }

    std::ostringstream out;
    out << SectionStart("Scale")
        << SelectRow("ui_scale", "App-wide text size", "Changes text size throughout the dashboard and overlay.", scales, currentScale)
        << SectionEnd();
    out << SectionStart("Colors")
        << "<div class='row wrap gap-sm' style='margin-bottom:8dp'>"
        << Button("reset-theme-colors", "Reset Colors to Default", "ghost")
        << Button("reset-theme-and-layout", "Reset Theme & Layout to Default", "ghost")
        << "</div>"
        << colorRow("theme_bg", "Overlay background", m_config.themeBg)
        << colorRow("theme_panel", "Settings panels", m_config.themeSettingsPanel)
        << colorRow("theme_topbar", "Top bar", m_config.themeTopbar)
        << colorRow("theme_graph_panel", "Graph panel", m_config.themeGraphPanel)
        << colorRow("theme_roster_card", "Roster card", m_config.themeRosterCard)
        << colorRow("theme_roster_card_self", "Roster card (you)", m_config.themeRosterCardSelf)
        << colorRow("theme_stat_box", "Stat boxes / K/D", m_config.themeStatBox)
        << colorRow("theme_match_row", "Previous games row", m_config.themeMatchRow)
        << colorRow("theme_match_row_alt", "Previous games row (alt)", m_config.themeMatchRowAlt)
        << colorRow("theme_text", "Text", m_config.themeText)
        << colorRow("theme_accent", "Accent", m_config.themeAccent)
        << colorRow("theme_win", "Win", m_config.themeWin)
        << colorRow("theme_loss", "Loss", m_config.themeLoss)
        << colorRow("theme_dim", "Dim text", m_config.themeDim)
        << colorRow("theme_muted", "Muted info", m_config.themeMuted)
        << colorRow("theme_graph", "Graph line", m_config.themeGraphLine)
        << colorRow("theme_baseline", "Graph baseline", m_config.themeGraphBaseline)
        << SectionEnd();

    out << SectionStart("Units & Formatting")
        << SelectRow("speed_units", "Speed units", "", {{"metric", "Kilometers per hour"}, {"imperial", "Miles per hour"}}, m_config.imperial_units ? "imperial" : "metric")
        << SelectRow("crossbar_display_mode", "Crossbar hit display", "", {{"raw", "Raw Force (UU/s)"}, {"speed", "Speed (MPH/KPH)"}}, m_config.crossbar_display_mode)
        << ToggleControl("use_roman_numerals", "Use Roman numerals", "I, II, III in rank labels.", m_config.use_roman_numerals)
        << SectionEnd();
    return out.str();
}
