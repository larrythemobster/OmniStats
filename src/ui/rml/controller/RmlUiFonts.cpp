#include "ui/rml/controller/RmlUiController.hpp"

#include "ui/rml/services/FontService.hpp"

bool RmlUiController::LoadBundledFonts() {
    return m_fontService ? m_fontService->LoadBundledFonts() : false;
}

std::string RmlUiController::ResolveFontFamily(const std::string& choice, std::string_view defaultFamily) {
    return m_fontService ? m_fontService->ResolveFontFamily(choice, defaultFamily) : std::string(defaultFamily);
}

std::string RmlUiController::FontOverrideCss() {
    return m_fontService ? m_fontService->FontOverrideCss(m_config) : std::string{};
}

bool RmlUiController::FontsDifferFromApplied(const ConfigData& config) const {
    return m_fontService ? m_fontService->FontsDifferFromApplied(config) : false;
}

const std::vector<std::string>& RmlUiController::SystemFontFamilies() {
    return FontService::SystemFontFamilies();
}
