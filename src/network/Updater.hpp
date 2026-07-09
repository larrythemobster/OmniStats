#pragma once
#include <atomic>
#include <string>
#include <memory>


class SessionState;

#ifndef OMNISTATS_VERSION
#define OMNISTATS_VERSION "2.0.0"
#endif

namespace Updater {
// Current local version
const std::string CURRENT_VERSION = OMNISTATS_VERSION;

// Update server address (no trailing slash)
const std::string UPDATE_SERVER_URL = "https://omnistats.org";

extern std::atomic<bool> RestartRequested;
extern std::atomic<bool> InstallerLaunched;

struct UpdateCheckResult {
    bool available = false;
    std::string latestVersion;
    std::string serverUrl;
};

void CleanupOldBinary();
void SetUpdateServerOverride(const std::string& serverUrl);
void ReportUpdatedSuccessfully();
UpdateCheckResult CheckForUpdate(bool forceCheck = false);
bool DownloadAndApplyUpdate(const UpdateCheckResult& update);
bool RunStartupUpdateFlow(bool forceCheck = false);
void StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state);
void StartInteractiveUpdate(std::shared_ptr<SessionState> state);
void ShutdownBackgroundTasks();

void BootstrapUpdater();
bool HandleCommandLineFlags(int argc, char* argv[], int& exitCode);

// Preserved as the startup entry point wrapper. Returns true if an update was
// successfully applied and a replacement process was launched.
bool Initialize();
} // namespace Updater
