#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TelemetryRecord {
    int64_t timestampMs = 0;
    std::string line;
};

std::optional<TelemetryRecord> ParseLine(std::string_view line);
std::vector<TelemetryRecord> ParseRecordingStream(std::istream& is);
std::vector<TelemetryRecord> ReadRecording(const std::filesystem::path& path);

class TelemetryRecorder {
  public:
    explicit TelemetryRecorder(std::filesystem::path file);
    ~TelemetryRecorder();

    TelemetryRecorder(const TelemetryRecorder&) = delete;
    TelemetryRecorder& operator=(const TelemetryRecorder&) = delete;
    TelemetryRecorder(TelemetryRecorder&&) = delete;
    TelemetryRecorder& operator=(TelemetryRecorder&&) = delete;

    // Append a raw Stats API line with a monotonic ms timestamp.
    // Safe, non-blocking / no-throw, never crashes if the file cannot be opened.
    void Record(std::string_view line) noexcept;

    // Resolves recording destination path from OMNISTATS_RECORD_TELEMETRY.
    // Returns std::nullopt if the variable is unset, empty, "0", or "false".
    static std::optional<std::filesystem::path> PathFromEnvironment();

    static std::optional<TelemetryRecord> ParseLine(std::string_view line) {
        return ::ParseLine(line);
    }
    static std::vector<TelemetryRecord> ParseRecordingStream(std::istream& is) {
        return ::ParseRecordingStream(is);
    }
    static std::vector<TelemetryRecord> ReadRecording(const std::filesystem::path& path) {
        return ::ReadRecording(path);
    }

  private:
    std::filesystem::path m_path;
    std::ofstream m_file;
    std::mutex m_mutex;
};
