#pragma once
#include <string>
#include <memory>
#include "database/DatabaseManager.hpp"

namespace TelemetryManager {
    // Generates a random standard UUIDv4 string
    std::string GenerateUUIDv4();

    // Initializes telemetry: loads/creates client_uuid and enables telemetry.
    void Initialize(std::shared_ptr<DatabaseManager> db);

    // Checks and reports any pending crashes asynchronously
    void CheckAndReportCrashes();

    // Asynchronously sends a telemetry ping
    void SendTelemetryAsync();

    // Asynchronously sends a crash report ping with error details
    void SendCrashAsync(const std::string& filePath);

    // Gracefully stops telemetry background worker
    void Shutdown();
}
