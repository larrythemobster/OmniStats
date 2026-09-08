#include "updater/StatsApiRepair.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace UpdaterStatsApiRepair {
    namespace {

        std::string Trim(const std::string& str) {
            const size_t first = str.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                return "";
            }
            const size_t last = str.find_last_not_of(" \t\r\n");
            return str.substr(first, last - first + 1);
        }

    } // namespace

    int FixConfigStrictHeadless(const std::string& filePath, int expectedPort) {
        std::filesystem::path p(filePath);
        std::string filename = p.filename().string();
        std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
        if (filename != "defaultstatsapi.ini") {
            return 2;
        }

        if (!std::filesystem::exists(filePath)) {
            return 2;
        }
        const auto parent = p.parent_path();
        if (!std::filesystem::exists(parent)) {
            return 2;
        }

        const std::string backupPath = filePath + ".omnistats.bak";
        if (!std::filesystem::exists(backupPath)) {
            std::error_code ec;
            std::filesystem::copy_file(filePath, backupPath, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                return 3;
            }
        }

        std::ifstream inFile(filePath, std::ios::binary);
        if (!inFile.is_open()) {
            return 4;
        }

        const std::string fileContent((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
        inFile.close();

        std::string lineEnding = "\n";
        if (fileContent.find("\r\n") != std::string::npos) {
            lineEnding = "\r\n";
        }

        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos < fileContent.length()) {
            const size_t nextPos = fileContent.find('\n', pos);
            if (nextPos == std::string::npos) {
                lines.push_back(fileContent.substr(pos));
                break;
            }
            std::string line = fileContent.substr(pos, nextPos - pos + 1);
            if (!line.empty() && line.back() == '\n') {
                line.pop_back();
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
            pos = nextPos + 1;
        }

        bool hasStatsApiSection = false;
        bool hasPacketSendRate = false;
        bool hasPort = false;

        std::vector<std::string> newLines;
        for (const auto& line : lines) {
            const std::string trimmed = Trim(line);
            if (trimmed.empty()) {
                newLines.push_back(line);
                continue;
            }
            if (trimmed[0] == ';' || trimmed[0] == '#') {
                newLines.push_back(line);
                continue;
            }
            if (trimmed[0] == '[' && trimmed[trimmed.length() - 1] == ']') {
                std::string sectionLower = trimmed;
                std::transform(sectionLower.begin(), sectionLower.end(), sectionLower.begin(), ::tolower);
                if (sectionLower == "[statsapi]") {
                    hasStatsApiSection = true;
                }
                newLines.push_back(line);
                continue;
            }

            const size_t eq = trimmed.find('=');
            if (eq != std::string::npos) {
                const std::string key = Trim(trimmed.substr(0, eq));
                std::string keyLower = key;
                std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

                if (keyLower == "packetsendrate") {
                    newLines.push_back("PacketSendRate=30");
                    hasPacketSendRate = true;
                } else if (keyLower == "port") {
                    newLines.push_back("Port=" + std::to_string(expectedPort));
                    hasPort = true;
                } else {
                    newLines.push_back(line);
                }
            } else {
                newLines.push_back(line);
            }
        }

        if (!hasPacketSendRate || !hasPort) {
            if (hasStatsApiSection) {
                std::vector<std::string> temp;
                for (const auto& line : newLines) {
                    temp.push_back(line);
                    const std::string trimmed = Trim(line);
                    if (!trimmed.empty() && trimmed[0] == '[' && trimmed[trimmed.length() - 1] == ']') {
                        std::string sectionLower = trimmed;
                        std::transform(sectionLower.begin(), sectionLower.end(), sectionLower.begin(), ::tolower);
                        if (sectionLower == "[statsapi]") {
                            if (!hasPacketSendRate) {
                                temp.push_back("PacketSendRate=30");
                                hasPacketSendRate = true;
                            }
                            if (!hasPort) {
                                temp.push_back("Port=" + std::to_string(expectedPort));
                                hasPort = true;
                            }
                        }
                    }
                }
                newLines = temp;
            } else {
                newLines.push_back("[StatsAPI]");
                newLines.push_back("PacketSendRate=30");
                newLines.push_back("Port=" + std::to_string(expectedPort));
            }
        }

        std::ofstream outFile(filePath, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            return 5;
        }

        for (size_t i = 0; i < newLines.size(); ++i) {
            outFile.write(newLines[i].data(), newLines[i].length());
            if (i + 1 < newLines.size() || (!fileContent.empty() && fileContent.back() == '\n')) {
                outFile.write(lineEnding.data(), lineEnding.length());
            }
        }
        outFile.close();
        if (!outFile) {
            return 5;
        }

        return 0;
    }

} // namespace UpdaterStatsApiRepair
