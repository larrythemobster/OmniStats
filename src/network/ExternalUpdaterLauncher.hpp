#pragma once
#include <memory>

class SessionState;

namespace ExternalUpdaterLauncher {

bool RunStartupUpdateCheck();
void StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state);
void StartInteractiveUpdate(std::shared_ptr<SessionState> state);
void ShutdownBackgroundTasks();

} // namespace ExternalUpdaterLauncher
