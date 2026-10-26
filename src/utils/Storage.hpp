#pragma once
#include <string>

namespace Storage {
    constexpr const char* APP_NAME = "omnistats";

    // Gets the path to %APPDATA%/omnistats/
    std::string GetDataDirectory();
    void SetDataDirectoryOverride(std::string path);

    // Ensures the data directory exists
    void InitializeEnvironment();

    // Documents\My Games\Rocket League\TAGame\ with a trailing separator, or empty.
    std::string GetRocketLeagueTAGameDirectory();
}