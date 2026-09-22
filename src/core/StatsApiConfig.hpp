#pragma once
#include <filesystem>
#include <string>
namespace StatsApiConfig {

    enum class Status {
        Valid = 0,
        NotFound,
        DisabledPacketSendRate,
        WrongPacketSendRate,
        MissingPacketSendRate,
        MissingPort,
        WrongPort,
        ReadError,
        WriteError
    };

    struct CheckResult {
        Status status = Status::NotFound;
        std::string path;
        std::string message;
        int expectedPort = 0;
        int actualPort = 0;
        float packetSendRate = 0.0f;
        bool rlRunning = false;
    };

    std::string GetStatusMessage(Status status);
    std::string DetectConfigPath();
    std::string DetectConfigPathFromRunningProcess();
    std::string FindConfigPathFromExecutable(const std::filesystem::path& exePath);
    bool IsRocketLeagueRunning();
    CheckResult VerifyConfig(const std::string& filePath, int expectedPort);

} // namespace StatsApiConfig
