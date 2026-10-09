#include "core/LogRotation.hpp"
#include <system_error>

namespace LogRotation {

    bool RotateLogs(const std::filesystem::path& directory, const std::string& baseFilename, int maxBackups) {
        if (directory.empty() || baseFilename.empty() || maxBackups <= 0) {
            return false;
        }

        std::error_code ec;
        if (!std::filesystem::exists(directory, ec)) {
            return false;
        }

        const std::filesystem::path basePath(baseFilename);
        const std::string stem = basePath.stem().string();
        const std::string ext = basePath.extension().string();

        auto makeRotatedPath = [&](int index) -> std::filesystem::path {
            return directory / (stem + "." + std::to_string(index) + ext);
        };

        const std::filesystem::path oldest = makeRotatedPath(maxBackups);
        if (std::filesystem::exists(oldest, ec)) {
            std::filesystem::remove(oldest, ec);
        }

        for (int i = maxBackups - 1; i >= 1; --i) {
            const std::filesystem::path src = makeRotatedPath(i);
            if (std::filesystem::exists(src, ec)) {
                const std::filesystem::path dst = makeRotatedPath(i + 1);
                std::filesystem::remove(dst, ec);
                std::filesystem::rename(src, dst, ec);
            }
        }

        const std::filesystem::path current = directory / baseFilename;
        if (std::filesystem::exists(current, ec)) {
            const std::filesystem::path first = makeRotatedPath(1);
            std::filesystem::remove(first, ec);
            std::filesystem::rename(current, first, ec);
        }

        return true;
    }

} // namespace LogRotation
