#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <charconv>

using namespace RmlUiDetail;

ConfigData RmlUiController::ThemeEditor::PreviewConfig() const {
    ConfigData config = m_owner.m_config;
    if (m_draft) Themes::Apply(config, *m_draft);
    return config;
}

Themes::Palette& RmlUiController::ThemeEditor::EnsureDraft() {
    if (!m_draft) m_draft = Themes::Read(m_owner.m_config);
    return *m_draft;
}

ColorRGBA* RmlUiController::ThemeEditor::Color(std::string_view key) {
    const auto fields = Themes::Fields();
    for (size_t i = 0; i < fields.size(); ++i)
        if (key == fields[i].key) return &EnsureDraft()[i];
    return nullptr;
}

bool RmlUiController::ThemeEditor::Pending() const {
    return m_draft && !Themes::Equal(*m_draft, Themes::Read(m_owner.m_config));
}

void RmlUiController::ThemeEditor::Revert() {
    const bool hadDraft = m_draft.has_value();
    m_draft.reset();
    m_resetUndo.reset();
    m_invalidColors.clear();
    m_colorPickDirty = false;
    m_colorPickerGradientHue = -1;
    m_message.clear();
    m_messageError = false;
    if (hadDraft) {
        m_owner.UpdateThemeProperties();
        RefreshThemeEditorControls();
    }
}

void RmlUiController::ThemeEditor::RefreshStatus() {
    if (!m_owner.m_document) return;
    if (m_owner.m_settingsPage != SettingsPage::Appearance) return;
    const auto setText = [&](const char* id, const std::string& text) {
        if (auto* element = m_owner.m_document->GetElementById(id)) m_owner.SetElementText(element, text);
    };
    const ConfigData config = PreviewConfig();
    const bool pending = Pending();
    setText("theme-draft-status", pending ? "Unapplied changes. Apply to keep them; closing Settings discards them." : "No unapplied changes.");
    for (const char* action : {"theme-apply", "theme-revert"}) {
        if (auto* button = m_owner.m_document->QuerySelector(std::string("[data-action='") + action + "']")) {
            if (pending || !m_invalidColors.empty())
                button->RemoveAttribute("disabled");
            else
                button->SetAttribute("disabled", "");
        }
    }
    for (const auto& [key, value] : m_invalidColors) {
        if (auto* element = m_owner.m_document->GetElementById(("theme-hex-" + key).c_str())) {
            element->SetClass("theme-invalid", true);
            if (auto* control = dynamic_cast<Rml::ElementFormControl*>(element)) control->SetValue(value);
        }
        setText(("theme-error-" + key).c_str(), "Use #RRGGBB or #RRGGBBAA.");
    }
    std::string warnings;
    if (!m_invalidColors.empty()) warnings = "Fix the invalid hex colors before applying or saving.";
    for (const auto& warning : Themes::Warnings(config)) {
        if (!warnings.empty()) warnings += " ";
        warnings += warning;
    }
    setText("theme-warnings", warnings);
    const ColorRGBA base = Themes::Composite(config.themeBg, {0, 0, 0, 1});
    const ColorRGBA backgrounds[] = {
        base, Themes::Composite(config.themeSettingsPanel, base), Themes::Composite(config.themeTopbar, base),
        Themes::Composite(config.themeGraphPanel, base), Themes::Composite(config.themeRosterCard, base),
        Themes::Composite(config.themeRosterCardSelf, base), Themes::Composite(config.themeStatBox, base),
        Themes::Composite(config.themeMatchRow, base), Themes::Composite(config.themeMatchRowAlt, base)};
    for (const auto& field : Themes::Fields()) {
        if (m_invalidColors.contains(field.key)) continue;
        const std::string id = std::string("theme-error-") + field.key;
        if (!m_owner.m_document->GetElementById(id.c_str())) continue;
        if (auto* input = m_owner.m_document->GetElementById((std::string("theme-hex-") + field.key).c_str()))
            input->SetClass("theme-invalid", false);
        const std::string_view key = field.key;
        const ColorRGBA color = config.*field.member;
        bool lowContrast = false;
        if (key == "theme_text" || key == "theme_muted" || key == "theme_dim" || key == "theme_win" || key == "theme_loss") {
            const double minimum = key == "theme_win" || key == "theme_loss" ? 3.0 : 4.5;
            lowContrast = std::any_of(std::begin(backgrounds), std::end(backgrounds), [&](auto bg) { return Themes::Contrast(color, bg) < minimum; });
        } else if (key == "theme_graph" || key == "theme_baseline") {
            lowContrast = Themes::Contrast(color, backgrounds[3]) < 3.0;
        } else if (key == "theme_primary_button" || key == "theme_button_text") {
            lowContrast = Themes::Contrast(config.themeButtonText, Themes::Composite(config.themePrimaryButton, base)) < 4.5;
        } else if (key == "theme_button_bg") {
            lowContrast = Themes::Contrast(config.themeButtonText, Themes::Composite(config.themeButtonBg, base)) < 4.5;
        } else if (key == "theme_input_bg" || key == "theme_input_text") {
            lowContrast = Themes::Contrast(config.themeInputText, Themes::Composite(config.themeInputBg, base)) < 4.5;
        } else if (key == "theme_bg" || key == "theme_panel" || key == "theme_topbar" ||
                   key == "theme_graph_panel" || key == "theme_roster_card" || key == "theme_roster_card_self" ||
                   key == "theme_stat_box" || key == "theme_match_row" || key == "theme_match_row_alt") {
            lowContrast = Themes::Contrast(config.themeText, Themes::Composite(color, base)) < 4.5;
        }
        setText(id.c_str(), lowContrast ? "Low contrast. Choose a color with more contrast against its text or background." : "");
    }
    if (auto* swatch = m_owner.m_document->GetElementById("theme-accent-swatch"))
        swatch->SetProperty("background-color", CssColor(config.themeAccent));
    for (const auto& preset : Themes::Presets()) {
        if (auto* button = m_owner.m_document->QuerySelector("[data-theme-id='" + std::string(preset.id) + "']"))
            button->SetClass("selected", Themes::Equal(Themes::Read(config), preset.colors));
    }
    m_owner.m_renderDirty = true;
}

void RmlUiController::ThemeEditor::HandleInput(std::string_view key, const std::string& value) {
    if (key == "theme_name") {
        m_nameDraft = value;
        return;
    }
    if (key == "theme_import") {
        m_importDraft = value;
        return;
    }
    std::string_view colorKey = key;
    char component = 0;
    const bool channel = ParseThemeComponentKey(key, colorKey, component);
    ColorRGBA* color = Color(colorKey);
    if (!color) return;
    if (channel) {
        if (colorKey != m_editColorKey) return;
        int number = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return;
        const float normalized = std::clamp(number, 0, 255) / 255.0f;
        switch (component) {
        case 'r':
            color->r = normalized;
            break;
        case 'g':
            color->g = normalized;
            break;
        case 'b':
            color->b = normalized;
            break;
        case 'a':
            color->a = normalized;
            break;
        }
        m_colorPickDirty = true;
        const Hsv hsv = RgbToHsv(*color);
        if (hsv.s > 0) m_editColorHue = hsv.h;
        RefreshColorPickPreview(component != 'a');
        if (auto* element = m_owner.m_document ? m_owner.m_document->GetElementById((std::string("theme-color-") + component + "-value").c_str()) : nullptr)
            m_owner.SetElementText(element, std::to_string(std::clamp(number, 0, 255)));
        RefreshStatus();
        return;
    }
    std::string_view hex = value;
    if (hex.starts_with('#')) hex.remove_prefix(1);
    uint32_t packed = 0;
    const auto parsed = std::from_chars(hex.data(), hex.data() + hex.size(), packed, 16);
    const bool valid = (hex.size() == 6 || hex.size() == 8) && parsed.ec == std::errc{} && parsed.ptr == hex.data() + hex.size();
    if (auto* input = m_owner.m_document ? m_owner.m_document->GetElementById(("theme-hex-" + std::string(colorKey)).c_str()) : nullptr)
        input->SetClass("theme-invalid", !valid);
    if (auto* error = m_owner.m_document ? m_owner.m_document->GetElementById(("theme-error-" + std::string(colorKey)).c_str()) : nullptr)
        m_owner.SetElementText(error, valid ? "" : "Use #RRGGBB or #RRGGBBAA.");
    if (!valid) {
        m_invalidColors[std::string(colorKey)] = value;
        RefreshStatus();
        return;
    }
    m_invalidColors.erase(std::string(colorKey));
    if (hex.size() == 6) packed = (packed << 8) | 255u;
    *color = {((packed >> 24) & 255u) / 255.0f, ((packed >> 16) & 255u) / 255.0f, ((packed >> 8) & 255u) / 255.0f, (packed & 255u) / 255.0f};
    if (colorKey == "theme_accent") {
        ConfigData config = PreviewConfig();
        Themes::SetAccent(config, *color);
        m_draft = Themes::Read(config);
    }
    Themes::LinkColor(EnsureDraft(), colorKey);
    m_owner.UpdateThemeProperties();
    RefreshThemeEditorControls(colorKey);
    RefreshStatus();
}

bool RmlUiController::ThemeEditor::HandleAction(std::string_view action, Rml::Element* target) {
    const bool themeAction = action.starts_with("theme-") || action == "reset-theme-colors";
    if (!themeAction) return false;
    m_message.clear();
    m_messageError = false;
    const auto error = [&](std::string message) {
        m_message = std::move(message);
        m_messageError = true;
        m_owner.ShowToast(m_message, true);
    };
    const auto preview = [&](const Themes::Palette& colors) {
        m_draft = colors;
        m_invalidColors.clear();
        m_editColorKey.clear();
        m_colorPickDirty = false;
        m_colorPickerGradientHue = -1;
        m_owner.UpdateThemeProperties();
    };
    const auto savedIndex = [&]() -> std::optional<size_t> {
        const std::string text = Attribute(target, "data-theme-index");
        size_t index = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), index);
        if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || index >= m_owner.m_config.custom_themes.size()) return {};
        return index;
    };
    if (action == "theme-preset") {
        const std::string id = Attribute(target, "data-theme-id");
        for (const auto& preset : Themes::Presets())
            if (preset.id == id) {
                preview(preset.colors);
                break;
            }
    } else if (action == "theme-apply") {
        CommitColorPick();
        if (!m_invalidColors.empty()) {
            error("Fix the invalid hex colors first.");
        } else if (m_draft) {
            const auto palette = *m_draft;
            Config::Update([&](ConfigData& config) { Themes::Apply(config, palette); });
            m_owner.m_config = Config::Read();
            m_draft.reset();
            m_resetUndo.reset();
            m_message = "Theme applied.";
            m_owner.UpdateThemeProperties();
        }
    } else if (action == "theme-revert") {
        Revert();
        m_message = "Returned to your applied theme.";
    } else if (action == "reset-theme-colors") {
        m_resetUndo = Themes::Read(PreviewConfig());
        preview(Themes::Read(ConfigData{}));
        m_message = "Default colors previewed. Undo reset or Revert to restore your colors.";
        m_owner.ShowToast("Default colors previewed. Undo reset is available in Appearance.");
    } else if (action == "theme-undo-reset") {
        if (m_resetUndo) {
            const auto colors = *m_resetUndo;
            m_resetUndo.reset();
            preview(colors);
            m_message = "Reset undone.";
        }
    } else if (action == "theme-copy") {
        CommitColorPick();
        if (!m_invalidColors.empty())
            error("Fix the invalid hex colors before copying.");
        else {
            m_owner.m_systemInterface.SetClipboardText(Themes::Encode(Themes::Read(PreviewConfig())));
            m_message = "Theme code copied.";
        }
    } else if (action == "theme-import") {
        Themes::Palette palette{};
        std::string code = m_importDraft;
        const auto first = code.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            code.clear();
        else
            code = code.substr(first, code.find_last_not_of(" \t\r\n") - first + 1);
        std::string detail;
        if (!Themes::Decode(code, palette, detail))
            error(detail);
        else {
            preview(palette);
            m_message = "Imported preview. Apply to keep it, or save it with a name.";
        }
    } else if (action == "theme-save") {
        CommitColorPick();
        std::string name = m_nameDraft;
        const auto first = name.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            name.clear();
        else
            name = name.substr(first, name.find_last_not_of(" \t\r\n") - first + 1);
        const auto& saved = m_owner.m_config.custom_themes;
        if (!m_invalidColors.empty())
            error("Fix the invalid hex colors before saving.");
        else if (m_owner.m_config.custom_themes_unreadable)
            error("Saved theme data could not be read. Back up and repair custom_themes in config.json before saving more themes.");
        else if (name.empty() || name.size() > 80 || name.find_first_of("\r\n\t") != std::string::npos)
            error("Use a theme name of 1 to 80 bytes, without line breaks.");
        else if (saved.size() >= 32)
            error("You can save up to 32 themes. Delete one before saving another.");
        else if (std::any_of(saved.begin(), saved.end(), [&](const auto& item) { return item.name == name; }))
            error("That name is already saved. Choose a different name.");
        else {
            const Themes::SavedTheme savedTheme{name, Themes::Read(PreviewConfig())};
            Config::Update([&](ConfigData& config) { config.custom_themes.push_back(savedTheme); });
            m_owner.m_config = Config::Read();
            m_nameDraft.clear();
            m_message = "Saved " + name + ". Apply to use these colors on every launch.";
        }
    } else if (action == "theme-load") {
        if (const auto index = savedIndex()) preview(m_owner.m_config.custom_themes[*index].colors);
    } else if (action == "theme-delete") {
        if (const auto index = savedIndex()) {
            if (m_deleteTheme == index) {
                Config::Update([&](ConfigData& config) { config.custom_themes.erase(config.custom_themes.begin() + *index); });
                m_owner.m_config = Config::Read();
                m_deleteTheme.reset();
                m_message = "Saved theme deleted. Your applied colors have not changed.";
            } else {
                m_deleteTheme = index;
                m_message = "Click Delete again to remove " + m_owner.m_config.custom_themes[*index].name + ".";
            }
        }
    }
    if (action != "theme-delete") m_deleteTheme.reset();
    m_owner.RebuildSettings();
    return true;
}
