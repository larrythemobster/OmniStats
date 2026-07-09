#pragma once
#include <nlohmann/json.hpp>
#include <string>

namespace Storage {
    constexpr const char* APP_NAME = "omnistats";

    // Gets the path to %APPDATA%/omnistats/
    std::string GetDataDirectory();

    // Ensures the data directory exists
    void InitializeEnvironment();

    // Appends a single JSON object to matches.jsonl synchronously
    void AppendMatchSync(const nlohmann::json& record);

    // Appends a single JSON object to mmr_history.jsonl
    void AppendMMRHistory(const nlohmann::json& entry);
}