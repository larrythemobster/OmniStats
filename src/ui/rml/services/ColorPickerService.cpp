#include "ui/rml/services/ColorPickerService.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

ColorPickerService::Hsv ColorPickerService::RgbToHsv(ColorRGBA color) {
    const auto hsv = RmlUiDetail::RgbToHsv(color);
    return Hsv{hsv.h, hsv.s, hsv.v};
}

ColorRGBA ColorPickerService::HsvToRgb(float h, float s, float v, float a) {
    return RmlUiDetail::HsvToRgb(RmlUiDetail::Hsv{h, s, v}, a);
}

void ColorPickerService::SetActiveKey(const std::string& key, const ConfigData& config) {
    if (const ColorRGBA* c = ThemeColorForKey(config, key)) {
        m_editColorKey = key;
        const auto hsv = RgbToHsv(*c);
        if (hsv.s > 0.0f) {
            m_editColorHue = hsv.h;
        }
        m_gradientHue = -1;
        m_dirty = false;
    } else {
        Close();
    }
}

void ColorPickerService::Close() {
    m_editColorKey.clear();
    m_gradientHue = -1;
    m_dirty = false;
}

void ColorPickerService::SetHue(float hue) {
    m_editColorHue = hue;
}

void ColorPickerService::SetGradientHue(int hue) {
    m_gradientHue = hue;
}

void ColorPickerService::SetDirty(bool dirty) {
    m_dirty = dirty;
}

void ColorPickerService::Commit(ConfigData& config) {
    if (!m_dirty || m_editColorKey.empty()) {
        m_dirty = false;
        return;
    }
    const ColorRGBA* current = ThemeColorForKey(config, m_editColorKey);
    if (!current) {
        m_dirty = false;
        return;
    }
    const std::string key = m_editColorKey;
    const ColorRGBA color = *current;
    Config::Update([key, color](ConfigData& c) {
        if (ColorRGBA* target = ThemeColorForKey(c, key)) {
            *target = color;
        }
    });
    m_dirty = false;
}
