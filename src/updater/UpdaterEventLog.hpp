#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

// Shared by OmniStatsUpdater.exe (writer) and OmniStats.exe (writer + sender); the main app uploads and clears it.
namespace UpdaterEventLog {

    inline constexpr const char* kFileName = "updater_events.jsonl";
    inline constexpr size_t kMaxLines = 20;

    inline std::vector<std::string> ReadLines(const std::string& path) {
        std::vector<std::string> lines;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty()) lines.push_back(std::move(line));
        }
        return lines;
    }

    inline void WriteLines(const std::string& path, const std::vector<std::string>& lines, size_t first = 0) {
        std::ofstream out(path, std::ios::trunc);
        for (size_t i = first; i < lines.size(); ++i)
            out << lines[i] << "\n";
    }

    inline void Append(const std::string& dir,
                       std::string_view event,
                       std::string_view targetVersion,
                       std::string_view errorCategory,
                       std::string_view version) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + kFileName;

        auto escape = [](std::string& out, std::string_view s) {
            for (const char c : s) {
                if (c == '"' || c == '\\') out += '\\';
                if (static_cast<unsigned char>(c) < 0x20) continue;
                out += c;
            }
        };
        std::string line = "{\"event\":\"";
        escape(line, event);
        line += "\",\"target_version\":\"";
        escape(line, targetVersion);
        line += "\",\"error_category\":\"";
        escape(line, errorCategory);
        line += "\",\"version\":\"";
        escape(line, version);
        line += "\"}";

        std::vector<std::string> lines = ReadLines(path);
        lines.push_back(std::move(line));
        WriteLines(path, lines, lines.size() > kMaxLines ? lines.size() - kMaxLines : 0);
    }

} // namespace UpdaterEventLog
