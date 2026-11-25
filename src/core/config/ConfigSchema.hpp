#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

struct ConfigData;
class SecretStore;

namespace ConfigSchema {
    void Decode(const nlohmann::json& json, ConfigData& config, SecretStore& secrets, bool& needsSave, bool& needsLegacyMigration);
    nlohmann::json Encode(const ConfigData& config, SecretStore& secrets);
    void ResetPreservedThemes();
    bool IsKnownMmrCategory(const std::string& category);
    bool IsGraphMmrCategory(const std::string& category);
    bool IsExtraMmrCategory(const std::string& category);
    void NormalizeLobbyRanks(ConfigData& config);
}
