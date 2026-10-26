#pragma once

#include <RmlUi/Core/Types.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "core/config/Config.hpp"
class FontService {
  public:
    FontService() = default;

    bool LoadBundledFonts();
    std::string ResolveFontFamily(const std::string& choice, std::string_view defaultFamily);
    std::string FontOverrideCss(const ConfigData& config);
    bool FontsDifferFromApplied(const ConfigData& config) const;
    static const std::vector<std::string>& SystemFontFamilies();

  private:
    std::unordered_map<std::string, std::string> m_loadedSystemFonts;
    std::string m_appliedUiFont;
    std::string m_appliedMonoFont;
    std::string m_appliedDisplayFont;
    std::vector<std::vector<Rml::byte>> m_fontBlobs;
};
