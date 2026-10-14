#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Rocket League reads Stats API settings from <Install Dir>\TAGame\Config\TAStatsAPI.ini, or from
// DefaultStatsAPI.ini when TAStatsAPI.ini does not exist, and only under [TAGame.MatchStatsExporter_TA].
namespace StatsApiIni {

    inline constexpr std::string_view kExporterSection = "TAGame.MatchStatsExporter_TA";

    struct ExporterValues {
        std::optional<float> packetSendRate;
        std::optional<int> port;
    };

    inline std::string Trim(std::string_view text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) return {};
        const auto last = text.find_last_not_of(" \t\r\n");
        return std::string(text.substr(first, last - first + 1));
    }

    inline std::string Lower(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    inline bool IsFileName(const std::filesystem::path& path, std::string_view lowerName) {
        return Lower(path.filename().string()) == lowerName;
    }

    inline bool IsStatsApiFileName(const std::filesystem::path& path) {
        return IsFileName(path, "tastatsapi.ini") || IsFileName(path, "defaultstatsapi.ini");
    }

    // Maps a detected DefaultStatsAPI.ini to the sibling TAStatsAPI.ini when the game would read that file instead.
    inline std::filesystem::path EffectiveConfigPath(const std::filesystem::path& path) {
        if (!IsFileName(path, "defaultstatsapi.ini")) return path;
        const std::filesystem::path override = path.parent_path() / "TAStatsAPI.ini";
        std::error_code ec;
        return std::filesystem::is_regular_file(override, ec) ? override : path;
    }

    inline std::optional<std::string> SectionName(const std::string& trimmedLine) {
        if (trimmedLine.size() < 2 || trimmedLine.front() != '[' || trimmedLine.back() != ']') return std::nullopt;
        return Trim(std::string_view(trimmedLine).substr(1, trimmedLine.size() - 2));
    }

    inline bool IsExporterSection(const std::string& name) {
        return Lower(name) == Lower(std::string(kExporterSection));
    }

    inline ExporterValues ReadExporterValues(const std::vector<std::string>& lines) {
        ExporterValues values;
        bool inExporter = false;
        for (const auto& raw : lines) {
            const std::string line = Trim(raw);
            if (line.empty() || line.front() == ';' || line.front() == '#') continue;
            if (const auto section = SectionName(line)) {
                inExporter = IsExporterSection(*section);
                continue;
            }
            if (!inExporter) continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Lower(Trim(std::string_view(line).substr(0, eq)));
            const std::string value = Trim(std::string_view(line).substr(eq + 1));
            try {
                if (key == "packetsendrate")
                    values.packetSendRate = std::stof(value);
                else if (key == "port")
                    values.port = std::stoi(value);
            } catch (...) {
            }
        }
        return values;
    }

    // Sets PacketSendRate=30 and Port inside the exporter section, adding the section or keys when missing.
    // Lines outside that section, including comments, are preserved unchanged.
    inline std::vector<std::string> RewriteExporterSection(const std::vector<std::string>& lines, int port) {
        const std::string portLine = "Port=" + std::to_string(port);
        std::vector<std::string> out;
        out.reserve(lines.size() + 3);
        bool inExporter = false;
        bool sawSection = false;
        bool wroteRate = false;
        bool wrotePort = false;
        size_t insertAt = 0;

        const auto closeSection = [&] {
            std::vector<std::string> missing;
            if (!wroteRate) missing.push_back("PacketSendRate=30");
            if (!wrotePort) missing.push_back(portLine);
            out.insert(out.begin() + static_cast<std::ptrdiff_t>(insertAt), missing.begin(), missing.end());
            wroteRate = wrotePort = true;
        };

        for (const auto& raw : lines) {
            const std::string line = Trim(raw);
            if (const auto section = SectionName(line)) {
                if (inExporter) closeSection();
                inExporter = IsExporterSection(*section);
                out.push_back(raw);
                if (inExporter) {
                    sawSection = true;
                    insertAt = out.size();
                }
                continue;
            }
            if (inExporter && !line.empty() && line.front() != ';' && line.front() != '#') {
                const auto eq = line.find('=');
                const std::string key = eq == std::string::npos ? std::string{} : Lower(Trim(std::string_view(line).substr(0, eq)));
                if (key == "packetsendrate") {
                    if (!wroteRate) out.push_back("PacketSendRate=30");
                    wroteRate = true;
                    insertAt = out.size();
                    continue;
                }
                if (key == "port") {
                    if (!wrotePort) out.push_back(portLine);
                    wrotePort = true;
                    insertAt = out.size();
                    continue;
                }
                out.push_back(raw);
                insertAt = out.size();
                continue;
            }
            out.push_back(raw);
        }
        if (inExporter) closeSection();
        if (!sawSection) {
            while (!out.empty() && Trim(out.back()).empty())
                out.pop_back();
            if (!out.empty()) out.emplace_back();
            out.push_back("[" + std::string(kExporterSection) + "]");
            out.push_back("PacketSendRate=30");
            out.push_back(portLine);
        }
        return out;
    }

} // namespace StatsApiIni
