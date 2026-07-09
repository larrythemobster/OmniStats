#pragma once
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
CheckResult VerifyConfig(const std::string& filePath, int expectedPort);
Status FixConfig(const std::string& filePath, int expectedPort);
int FixConfigStrictHeadless(const std::string& filePath, int expectedPort);

} // namespace StatsApiConfig
