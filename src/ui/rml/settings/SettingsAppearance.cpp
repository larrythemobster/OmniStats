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

    std::ostringstream out;
    out << SectionStart("Scale") << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>App-wide text size</div></div><select data-setting='ui_scale'>"
        << "<option value='0.75'" << Selected(std::fabs(m_config.ui_scale - 0.75f) < .01f) << ">75%</option>"
        << "<option value='0.8'" << Selected(std::fabs(m_config.ui_scale - 0.8f) < .01f) << ">80%</option>"
        << "<option value='0.85'" << Selected(std::fabs(m_config.ui_scale - 0.85f) < .01f) << ">85%</option>"
        << "<option value='0.9'" << Selected(std::fabs(m_config.ui_scale - 0.9f) < .01f) << ">90%</option>"
        << "<option value='0.95'" << Selected(std::fabs(m_config.ui_scale - 0.95f) < .01f) << ">95%</option>"
        << "<option value='1.0'" << Selected(std::fabs(m_config.ui_scale - 1.0f) < .01f) << ">100%</option>"
        << "<option value='1.1'" << Selected(std::fabs(m_config.ui_scale - 1.1f) < .01f) << ">110%</option>"
        << "<option value='1.25'" << Selected(std::fabs(m_config.ui_scale - 1.25f) < .01f) << ">125%</option>"
        << "<option value='1.5'" << Selected(std::fabs(m_config.ui_scale - 1.5f) < .01f) << ">150%</option></select></div>"
        << "<div class='setting-help'>Changes text size throughout the dashboard and overlay.</div>" << SectionEnd();
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
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Speed units</div></div><select data-setting='speed_units'><option value='metric'" << Selected(!m_config.imperial_units) << ">Kilometers per hour</option><option value='imperial'" << Selected(m_config.imperial_units) << ">Miles per hour</option></select></div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Crossbar hit display</div></div><select data-setting='crossbar_display_mode'><option value='raw'" << Selected(m_config.crossbar_display_mode == "raw") << ">Raw Force (UU/s)</option><option value='speed'" << Selected(m_config.crossbar_display_mode == "speed") << ">Speed (MPH/KPH)</option></select></div>"
        << ToggleControl("use_roman_numerals", "Use Roman numerals", "I, II, III in rank labels.", m_config.use_roman_numerals)
        << SectionEnd();
    return out.str();
}
