#include "updater/StatsApiRepair.hpp"

#include "core/config/StatsApiIni.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace UpdaterStatsApiRepair {

    int FixConfigStrictHeadless(const std::string& requestedPath, int expectedPort) {
        const std::filesystem::path requested(requestedPath);
        if (!StatsApiIni::IsStatsApiFileName(requested)) {
            return 2;
        }
        const std::filesystem::path target = StatsApiIni::EffectiveConfigPath(requested);
        const std::string filePath = target.string();

        std::error_code ec;
        if (!std::filesystem::is_regular_file(target, ec)) {
            return 2;
        }

        const std::string backupPath = filePath + ".omnistats.bak";
        if (!std::filesystem::exists(backupPath, ec)) {
            std::filesystem::copy_file(target, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return 3;
            }
        }

        std::ifstream inFile(target, std::ios::binary);
        if (!inFile.is_open()) {
            return 4;
        }
        const std::string fileContent((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        inFile.close();

        const std::string lineEnding = fileContent.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < fileContent.length()) {
            const size_t nextPos = fileContent.find('\n', pos);
            std::string line = fileContent.substr(pos, nextPos == std::string::npos ? std::string::npos : nextPos - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(std::move(line));
            if (nextPos == std::string::npos) break;
            pos = nextPos + 1;
        }

        const std::vector<std::string> newLines = StatsApiIni::RewriteExporterSection(lines, expectedPort);

        std::ofstream outFile(target, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            return 5;
        }
        for (size_t i = 0; i < newLines.size(); ++i) {
            outFile.write(newLines[i].data(), static_cast<std::streamsize>(newLines[i].length()));
            if (i + 1 < newLines.size() || (!fileContent.empty() && fileContent.back() == '\n')) {
                outFile.write(lineEnding.data(), static_cast<std::streamsize>(lineEnding.length()));
            }
        }
        outFile.close();
        if (!outFile) {
            return 5;
        }

        return 0;
    }

} // namespace UpdaterStatsApiRepair
