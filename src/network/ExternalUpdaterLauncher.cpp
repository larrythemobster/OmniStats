#include "ExternalUpdaterLauncher.hpp"
#include "core/SessionState.hpp"
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

    std::mutex g_updateThreadsMutex;
    std::vector<std::thread> g_updateThreads;

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
        if (!CreateProcessA(NULL, commandLineBuffer.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
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

        const std::string arguments = "--update-app \"" + currentExePath + "\" " +
                                      std::to_string(GetCurrentProcessId());
        return LaunchUpdater(arguments);
    }

    bool RunInstalledUpdaterCheck(std::string& latestVersion) {
        latestVersion.clear();

        std::string updaterPath;
        if (!FindUpdaterExecutable(updaterPath)) {
            std::cout << "[UpdaterLauncher] Installed updater not found; skipping update check.\n";
            return false;
        }

        const std::string resultPath = GetUpdateCheckResultPath();
        if (resultPath.empty()) {
            std::cout << "[UpdaterLauncher] Failed to create updater check result path.\n";
            return false;
        }
        DeleteFileA(resultPath.c_str());

        HANDLE processHandle = nullptr;
        const std::string arguments = "--check --result-file \"" + resultPath + "\"";
        if (!LaunchUpdater(arguments, &processHandle) || !processHandle) {
            DeleteFileA(resultPath.c_str());
            return false;
        }

        const DWORD waitResult = WaitForSingleObject(processHandle, kUpdateCheckTimeoutMs);
        if (waitResult != WAIT_OBJECT_0) {
            std::cout << "[UpdaterLauncher] Installed updater check timed out.\n";
            CloseHandle(processHandle);
            DeleteFileA(resultPath.c_str());
            return false;
        }

        DWORD exitCode = static_cast<DWORD>(-1);
        if (!GetExitCodeProcess(processHandle, &exitCode)) {
            std::cout << "[UpdaterLauncher] Failed to read updater check exit code. Error: " << GetLastError() << "\n";
            CloseHandle(processHandle);
            DeleteFileA(resultPath.c_str());
            return false;
        }
        CloseHandle(processHandle);

        latestVersion = ReadFirstLine(resultPath);
        DeleteFileA(resultPath.c_str());

        if (exitCode == kUpdateAvailableExitCode) {
            return true;
        }
        if (exitCode != kNoUpdateExitCode) {
            std::cout << "[UpdaterLauncher] Installed updater check failed with exit code " << exitCode << ".\n";
        }
        return false;
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

} // namespace

namespace ExternalUpdaterLauncher {

    bool RunStartupUpdateCheck() {
        std::string latestVersion;
        if (!RunInstalledUpdaterCheck(latestVersion)) {
            return false;
        }

        if (!LaunchUpdateForCurrentApp()) {
            ShowUpdaterMissingMessage();
            return false;
        }

        return true;
    }

    void StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state) {
        if (!state) {
            return;
        }

        StoreBackgroundThread(std::thread([state = std::move(state)]() {
            std::string latestVersion;
            const bool updateAvailable = RunInstalledUpdaterCheck(latestVersion);

            if (updateAvailable) {
                {
                    std::lock_guard<std::mutex> lock(state->ui.updateMutex);
                    state->ui.updateAvailableVersion = latestVersion;
                    state->ui.updateServerUrl.clear();
                }
                state->ui.updateAvailable.store(true);
            }
            state->ui.updateChecked.store(true);
            std::cout << "[UpdaterLauncher] Background updater check finished. Latest version: "
                      << latestVersion << "\n";
        }));
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

    void ShutdownBackgroundTasks() {
        JoinBackgroundThreads();
    }

} // namespace ExternalUpdaterLauncher
