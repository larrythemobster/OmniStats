#pragma once
#include <string>

namespace Storage {
    constexpr const char* APP_NAME = "omnistats";

    // Gets the path to %APPDATA%/omnistats/
    std::string GetDataDirectory();

    // Ensures the data directory exists
    void InitializeEnvironment();
}