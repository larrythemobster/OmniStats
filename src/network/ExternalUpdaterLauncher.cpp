#include "ExternalUpdaterLauncher.hpp"
#include "core/SessionState.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

    constexpr const char* kUpdaterExeName = "OmniStatsUpdater.exe";
    constexpr const char* kUpdaterMissingMessage =
        "OmniStatsUpdater.exe is missing from the OmniStats installation. Please reinstall OmniStats.";
    constexpr UINT kUpdaterMessageBoxFlags = MB_OK | MB_ICONWARNING | MB_SETFOREGROUND;
    constexpr DWORD kUpdateCheckTimeoutMs = 20000;
    constexpr DWORD kUpdateAvailableExitCode = 0;
    constexpr DWORD kNoUpdateExitCode = 1;
    constexpr ULONGLONG kSettingsUpdateCheckIntervalMs = 5ULL * 60ULL * 1000ULL;
    constexpr ULONGLONG kPeriodicUpdateCheckIntervalMs = 60ULL * 60ULL * 1000ULL;
    constexpr ULONGLONG kFailedUpdateCheckRetryMs = 5ULL * 60ULL * 1000ULL;

    enum class UpdateCheckResult {
        UpdateAvailable,
        UpToDate,
        Failed
    };

    std::mutex g_updateThreadsMutex;
    std::vector<std::thread> g_updateThreads;
    std::atomic<bool> g_updateCheckInProgress{false};
    std::atomic<ULONGLONG> g_lastUpdateCheckAttemptMs{0};
    std::atomic<ULONGLONG> g_lastSuccessfulUpdateCheckMs{0};

    std::string GetCurrentExecutablePath() {
        char path[MAX_PATH] = {0};
        DWORD length = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return "";
        }
        return std::string(path, length);
    }

    std::string GetDirectoryForPath(const std::string& path) {
        size_t lastSlash = path.find_last_of("\\/");
        if (lastSlash == std::string::npos) {
            return ".";
        }
        return path.substr(0, lastSlash);
    }

    bool FileExists(const std::string& path) {
        DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    // The main application never downloads, refreshes, stages, or replaces the updater.
    // It only launches the side-by-side updater provisioned by the OmniStats installer.
    bool FindUpdaterExecutable(std::string& updaterPath) {
        const std::string currentExePath = GetCurrentExecutablePath();
        if (currentExePath.empty()) {
            updaterPath.clear();
            return false;
        }

        const std::string candidate = GetDirectoryForPath(currentExePath) + "\\" + kUpdaterExeName;
        if (!FileExists(candidate)) {
            updaterPath.clear();
            return false;
        }

        updaterPath = candidate;
        return true;
    }

    void ShowUpdaterMissingMessage() {
        MessageBoxA(NULL, kUpdaterMissingMessage, "OmniStats Update", kUpdaterMessageBoxFlags);
    }

    std::string GetUpdateCheckResultPath() {
        char tempPath[MAX_PATH] = {0};
        DWORD length = GetTempPathA(MAX_PATH, tempPath);
        if (length == 0 || length >= MAX_PATH) {
            return "";
        }

        return std::string(tempPath, length) + "OmniStats-update-check-" +
               std::to_string(GetCurrentProcessId()) + ".txt";
    }

    std::string ReadFirstLine(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) {
            return "";
        }

        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        return line;
    }

    bool LaunchUpdater(const std::string& arguments, HANDLE* processHandle = nullptr) {
        std::string updaterPath;
        if (!FindUpdaterExecutable(updaterPath)) {
            return false;
        }

        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        std::string commandLine = "\"" + updaterPath + "\"";
        if (!arguments.empty()) {
            commandLine += " ";
            commandLine += arguments;
        }
        std::vector<char> commandLineBuffer(commandLine.begin(), commandLine.end());
        commandLineBuffer.push_back('\0');

        const std::string workingDirectory = GetDirectoryForPath(updaterPath);
        if (!CreateProcessA(NULL, commandLineBuffer.data(), NULL, NULL, FALSE, 0, NULL,
                            workingDirectory.c_str(), &si, &pi)) {
            std::cout << "[UpdaterLauncher] Failed to launch installed updater. Error: " << GetLastError() << "\n";
            return false;
        }

        CloseHandle(pi.hThread);
        if (processHandle) {
            *processHandle = pi.hProcess;
        } else {
            CloseHandle(pi.hProcess);
        }
        return true;
    }

    bool LaunchUpdateForCurrentApp() {
        const std::string currentExePath = GetCurrentExecutablePath();
        if (currentExePath.empty()) {
            std::cout << "[UpdaterLauncher] Failed to resolve current executable path.\n";
            return false;
        }

        // Keep the legacy --update-app command shape so older installed updaters do not
        // break during the one-time migration to MSI-based updater replacement. New updaters
        // interpret the same command as an MSI upgrade and ignore the target path.
        const std::string arguments = "--update-app \"" + currentExePath + "\" " +
                                      std::to_string(GetCurrentProcessId());
        return LaunchUpdater(arguments);
    }

    UpdateCheckResult RunInstalledUpdaterCheck(std::string& latestVersion) {
        latestVersion.clear();

        std::string updaterPath;
        if (!FindUpdaterExecutable(updaterPath)) {
            std::cout << "[UpdaterLauncher] Installed updater not found; skipping update check.\n";
            return UpdateCheckResult::Failed;
        }

        const std::string resultPath = GetUpdateCheckResultPath();
        if (resultPath.empty()) {
            std::cout << "[UpdaterLauncher] Failed to create updater check result path.\n";
            return UpdateCheckResult::Failed;
        }
        DeleteFileA(resultPath.c_str());

        HANDLE processHandle = nullptr;
        const std::string arguments = "--check --result-file \"" + resultPath + "\"";
        if (!LaunchUpdater(arguments, &processHandle) || !processHandle) {
            DeleteFileA(resultPath.c_str());
            return UpdateCheckResult::Failed;
        }

        const DWORD waitResult = WaitForSingleObject(processHandle, kUpdateCheckTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            std::cout << "[UpdaterLauncher] Installed updater check timed out.\n";
            CloseHandle(processHandle);
            DeleteFileA(resultPath.c_str());
            return UpdateCheckResult::Failed;
        }

        DWORD exitCode = static_cast<DWORD>(-1);
        if (!GetExitCodeProcess(processHandle, &exitCode)) {
            std::cout << "[UpdaterLauncher] Failed to read updater check exit code. Error: " << GetLastError() << "\n";
            CloseHandle(processHandle);
            DeleteFileA(resultPath.c_str());
            return UpdateCheckResult::Failed;
        }
        CloseHandle(processHandle);

        latestVersion = ReadFirstLine(resultPath);
        DeleteFileA(resultPath.c_str());

        if (exitCode == kUpdateAvailableExitCode) {
            return UpdateCheckResult::UpdateAvailable;
        }
        if (exitCode == kNoUpdateExitCode && !latestVersion.empty()) {
            return UpdateCheckResult::UpToDate;
        }

        std::cout << "[UpdaterLauncher] Installed updater check failed with exit code " << exitCode << ".\n";
        return UpdateCheckResult::Failed;
    }

    ULONGLONG MinimumIntervalFor(ExternalUpdaterLauncher::BackgroundUpdateCheckReason reason) {
        switch (reason) {
        case ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Initial:
            return 0;
        case ExternalUpdaterLauncher::BackgroundUpdateCheckReason::SettingsOpened:
            return kSettingsUpdateCheckIntervalMs;
        case ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Periodic:
            return kPeriodicUpdateCheckIntervalMs;
        }
        return kPeriodicUpdateCheckIntervalMs;
    }

    bool UpdateCheckAllowed(ExternalUpdaterLauncher::BackgroundUpdateCheckReason reason, ULONGLONG now) {
        const ULONGLONG lastAttempt = g_lastUpdateCheckAttemptMs.load(std::memory_order_relaxed);
        const ULONGLONG lastSuccess = g_lastSuccessfulUpdateCheckMs.load(std::memory_order_relaxed);

        // A failed request should not turn an always-on check into a tight retry loop.
        if (lastAttempt > lastSuccess && lastAttempt != 0 && now - lastAttempt < kFailedUpdateCheckRetryMs) {
            return false;
        }

        const ULONGLONG minimumInterval = MinimumIntervalFor(reason);
        return minimumInterval == 0 || lastSuccess == 0 || now - lastSuccess >= minimumInterval;
    }

    bool RunStatsApiRepair(const std::string& filePath, int expectedPort) {
        if (filePath.empty()) {
            return false;
        }

        HANDLE processHandle = nullptr;
        const std::string arguments = "--repair-stats-api \"" + filePath + "\" " +
                                      std::to_string(expectedPort);
        if (!LaunchUpdater(arguments, &processHandle) || !processHandle) {
            return false;
        }

        const DWORD waitResult = WaitForSingleObject(processHandle, INFINITE);
        if (waitResult != WAIT_OBJECT_0) {
            CloseHandle(processHandle);
            return false;
        }

        DWORD exitCode = static_cast<DWORD>(-1);
        const bool readExitCode = GetExitCodeProcess(processHandle, &exitCode) != FALSE;
        CloseHandle(processHandle);
        return readExitCode && exitCode == 0;
    }

    void JoinBackgroundThreads() {
        std::vector<std::thread> threads;
        {
            std::lock_guard<std::mutex> lock(g_updateThreadsMutex);
            threads.swap(g_updateThreads);
        }

        for (auto& thread : threads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

    void StoreBackgroundThread(std::thread thread) {
        std::lock_guard<std::mutex> lock(g_updateThreadsMutex);
        g_updateThreads.push_back(std::move(thread));
    }
    struct BackgroundThreadGuard {
        ~BackgroundThreadGuard() {
            JoinBackgroundThreads();
        }
    } g_backgroundThreadGuard;

} // namespace

namespace ExternalUpdaterLauncher {

    bool RunStartupUpdateCheck() {
        const ULONGLONG now = GetTickCount64();
        g_lastUpdateCheckAttemptMs.store(now, std::memory_order_relaxed);

        std::string latestVersion;
        const UpdateCheckResult result = RunInstalledUpdaterCheck(latestVersion);
        if (result != UpdateCheckResult::Failed) {
            g_lastSuccessfulUpdateCheckMs.store(GetTickCount64(), std::memory_order_relaxed);
        }
        if (result != UpdateCheckResult::UpdateAvailable) {
            return false;
        }

        if (!LaunchUpdateForCurrentApp()) {
            ShowUpdaterMissingMessage();
            return false;
        }

        return true;
    }

    bool StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state, BackgroundUpdateCheckReason reason) {
        if (!state) {
            return false;
        }

        const ULONGLONG now = GetTickCount64();
        if (!UpdateCheckAllowed(reason, now)) {
            return false;
        }

        bool expected = false;
        if (!g_updateCheckInProgress.compare_exchange_strong(expected, true)) {
            return false;
        }

        // Re-check after claiming the slot so two callers racing from the render
        // thread and Settings cannot both pass the interval gate.
        const ULONGLONG claimedAt = GetTickCount64();
        if (!UpdateCheckAllowed(reason, claimedAt)) {
            g_updateCheckInProgress.store(false);
            return false;
        }
        g_lastUpdateCheckAttemptMs.store(claimedAt, std::memory_order_relaxed);

        StoreBackgroundThread(std::thread([state = std::move(state)]() {
            std::string latestVersion;
            const UpdateCheckResult result = RunInstalledUpdaterCheck(latestVersion);

            if (result != UpdateCheckResult::Failed) {
                g_lastSuccessfulUpdateCheckMs.store(GetTickCount64(), std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lock(state->ui.updateMutex);
                    state->ui.updateAvailableVersion =
                        result == UpdateCheckResult::UpdateAvailable ? latestVersion : std::string{};
                    state->ui.updateServerUrl.clear();
                }
                const bool updateAvailable = result == UpdateCheckResult::UpdateAvailable;
                state->ui.updateAvailable.store(updateAvailable);
                state->ui.updateChecked.store(true);
                if (!updateAvailable) {
                    state->ui.updatePromptShown.store(false);
                    state->ui.updateDownloadFailed.store(false);
                }
            }

            g_updateCheckInProgress.store(false);
            std::cout << "[UpdaterLauncher] Background updater check finished. Result: "
                      << (result == UpdateCheckResult::UpdateAvailable ? "update available"
                          : result == UpdateCheckResult::UpToDate      ? "up to date"
                                                                       : "failed")
                      << (latestVersion.empty() ? "" : ", latest version: ") << latestVersion << "\n";
        }));
        return true;
    }

    void StartInteractiveUpdate(std::shared_ptr<SessionState> state) {
        if (!state) {
            return;
        }

        bool expected = false;
        if (!state->ui.updateDownloading.compare_exchange_strong(expected, true)) {
            std::cout << "[UpdaterLauncher] Update request ignored because one is already active.\n";
            return;
        }
        state->ui.updateDownloadFailed.store(false);

        if (!LaunchUpdateForCurrentApp()) {
            state->ui.updateDownloading.store(false);
            state->ui.updateDownloadFailed.store(true);
            ShowUpdaterMissingMessage();
            return;
        }

        state->ui.appExitRequested.store(true);
    }

    bool RepairStatsApiConfig(const std::string& filePath, int expectedPort) {
        return RunStatsApiRepair(filePath, expectedPort);
    }

    void ShutdownBackgroundTasks() {
        JoinBackgroundThreads();
    }

} // namespace ExternalUpdaterLauncher
