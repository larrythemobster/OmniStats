#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace SupportBundle {

    nlohmann::json RedactConfig(const nlohmann::json& config);

    std::string RedactConfigString(const std::string& jsonText);

    std::string RedactLog(const std::string& logText, const std::vector<std::string>& knownSecrets = {});

    std::string GetDatabaseSummary(const std::filesystem::path& dbPath);

    std::string GetWindowsVersionInfo();

    std::string BuildDiagnosticsSummary();

    bool CreateSupportBundle(std::string& outZipPath, std::string& outError);

} // namespace SupportBundle
