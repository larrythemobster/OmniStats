#pragma once
#include <memory>
#include <stop_token>
#include <string>

class TelemetryManager;

class SessionState;

namespace ExternalUpdaterLauncher {

    enum class BackgroundUpdateCheckReason {
        Initial,
        SettingsOpened,
        Periodic
    };

    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm);
    bool RunStartupUpdateCheck();
    bool StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state,
                                    BackgroundUpdateCheckReason reason = BackgroundUpdateCheckReason::Initial);
    void StartInteractiveUpdate(std::shared_ptr<SessionState> state);
    // Waits for the elevated updater; returns false if it fails, times out, or `stop` is requested.
    bool RepairStatsApiConfig(const std::string& filePath, int expectedPort, std::stop_token stop);
    bool WaitForRepairProcess(void* processHandle, std::stop_token stop, unsigned long timeoutMs, unsigned long& exitCode);
    void ShutdownBackgroundTasks();

} // namespace ExternalUpdaterLauncher
