#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace Storage {
    constexpr const char* APP_NAME = "omnistats";

    // Gets the path to %APPDATA%/omnistats/
    std::string GetDataDirectory();
    void SetDataDirectoryOverride(std::string path);

    // Ensures the data directory exists
    void InitializeEnvironment();

    // Documents\My Games\Rocket League\TAGame\ with a trailing separator, or empty.
    std::string GetRocketLeagueTAGameDirectory();

    // Writes to "<path>.tmp", flushes it to disk, then replaces path. GetLastError() holds the failure.
    bool WriteFileAtomically(const std::filesystem::path& path, std::string_view contents);
}