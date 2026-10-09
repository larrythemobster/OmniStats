#pragma once

#include <filesystem>
#include <string>

namespace LogRotation {

    // Keeps up to maxBackups previous logs as <stem>.1<ext> (newest) .. <stem>.N<ext>; never throws.
    bool RotateLogs(const std::filesystem::path& directory, const std::string& baseFilename, int maxBackups = 5);

} // namespace LogRotation
