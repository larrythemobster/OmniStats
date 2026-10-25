#include "network/telemetry/TelemetryRecorder.hpp"
#include "utils/Env.hpp"
#include "utils/Storage.hpp"

#include <cctype>
#include <iostream>
#include <sstream>

static int64_t CurrentMonotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::optional<std::filesystem::path> TelemetryRecorder::PathFromEnvironment() {
    const auto envOpt = Env::Get("OMNISTATS_RECORD_TELEMETRY");
    if (!envOpt.has_value() || envOpt->empty() || *envOpt == "0" || *envOpt == "false") {
        return std::nullopt;
    }

    const std::string& val = *envOpt;
    try {
        if (val == "1" || val == "true") {
            const std::filesystem::path dataDir(Storage::GetDataDirectory());
            return dataDir / "telemetry_recordings" / "telemetry_recording.log";
        }
        std::filesystem::path p(val);
        std::error_code ec;
        if (std::filesystem::is_directory(p, ec) || !p.has_extension()) {
            return p / "telemetry_recording.log";
        }
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

TelemetryRecorder::TelemetryRecorder(std::filesystem::path file)
    : m_path(std::move(file)) {}

TelemetryRecorder::~TelemetryRecorder() {
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_file.is_open()) {
            m_file.flush();
            m_file.close();
        }
    } catch (...) {
    }
}

void TelemetryRecorder::Record(std::string_view line) noexcept {
    if (line.empty()) return;

    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_path.empty()) return;

        if (!m_file.is_open()) {
            std::error_code ec;
            if (m_path.has_parent_path()) {
                std::filesystem::create_directories(m_path.parent_path(), ec);
            }
            m_file.open(m_path, std::ios::out | std::ios::app);
        }

        if (m_file.is_open()) {
            const int64_t timestampMs = CurrentMonotonicMs();
            m_file << timestampMs << '\t' << line << '\n';
            m_file.flush();
        }
    } catch (...) {
        // Never affect parsing or crash the client if the file cannot be opened.
    }
}

std::optional<TelemetryRecord> ParseLine(std::string_view line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r' || line.front() == '\n')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return std::nullopt;

    if (line.front() == '{') {
        return TelemetryRecord{0, std::string(line)};
    }

    if (std::isdigit(static_cast<unsigned char>(line.front()))) {
        const size_t sepPos = line.find_first_of(" \t");
        if (sepPos != std::string_view::npos) {
            try {
                const int64_t ts = std::stoll(std::string(line.substr(0, sepPos)));
                std::string_view payload = line.substr(sepPos + 1);
                while (!payload.empty() && (payload.front() == ' ' || payload.front() == '\t')) {
                    payload.remove_prefix(1);
                }
                return TelemetryRecord{ts, std::string(payload)};
            } catch (...) {
                // fall through
            }
        }
    }

    return TelemetryRecord{0, std::string(line)};
}

std::vector<TelemetryRecord> ParseRecordingStream(std::istream& is) {
    std::vector<TelemetryRecord> records;
    std::string line;
    while (std::getline(is, line)) {
        auto rec = ParseLine(line);
        if (rec.has_value()) {
            records.push_back(std::move(*rec));
        }
    }
    return records;
}

std::vector<TelemetryRecord> ReadRecording(const std::filesystem::path& path) {
    std::vector<TelemetryRecord> records;
    std::ifstream file(path);
    if (!file.is_open()) return records;
    return ParseRecordingStream(file);
}
