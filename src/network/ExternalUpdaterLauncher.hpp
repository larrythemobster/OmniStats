#pragma once
#include <memory>
#include <string>

class SessionState;

namespace ExternalUpdaterLauncher {

    bool RunStartupUpdateCheck();
    void StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state);
    void StartInteractiveUpdate(std::shared_ptr<SessionState> state);
    bool RepairStatsApiConfig(const std::string& filePath, int expectedPort);
    void ShutdownBackgroundTasks();

} // namespace ExternalUpdaterLauncher
