#pragma once
#include <memory>
#include <string>

class SessionState;

namespace ExternalUpdaterLauncher {

    enum class BackgroundUpdateCheckReason {
        Initial,
        SettingsOpened,
        Periodic
    };

    bool RunStartupUpdateCheck();
    bool StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state,
                                    BackgroundUpdateCheckReason reason = BackgroundUpdateCheckReason::Initial);
    void StartInteractiveUpdate(std::shared_ptr<SessionState> state);
    bool RepairStatsApiConfig(const std::string& filePath, int expectedPort);
    void ShutdownBackgroundTasks();

} // namespace ExternalUpdaterLauncher
