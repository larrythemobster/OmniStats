#pragma once

#include <string>
#include "core/config/Config.hpp"
#include "ui/Formatting.hpp"

class ColorPickerService {
  public:
    struct Hsv {
        float h = 0.0f; // [0, 360)
        float s = 0.0f; // [0, 1]
        float v = 0.0f; // [0, 1]
    };

    static Hsv RgbToHsv(ColorRGBA color);
    static ColorRGBA HsvToRgb(float h, float s, float v, float a);

    ColorPickerService() = default;

    const std::string& ActiveKey() const noexcept {
        return m_editColorKey;
    }
    bool IsEditing() const noexcept {
        return !m_editColorKey.empty();
    }
    float ActiveHue() const noexcept {
        return m_editColorHue;
    }
    int GradientHue() const noexcept {
        return m_gradientHue;
    }
    bool IsDirty() const noexcept {
        return m_dirty;
    }

    void SetActiveKey(const std::string& key, const ConfigData& config);
    void Close();
    void SetHue(float hue);
    void SetGradientHue(int hue);
    void SetDirty(bool dirty);

    void Commit(ConfigData& config);

  private:
    std::string m_editColorKey;
    float m_editColorHue = 0.0f;
    int m_gradientHue = -1;
    bool m_dirty = false;
};
