#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <windows.h>
#include <shlobj.h>
#include "network/UpdaterCommon.hpp"

// Global override for test server URL
static std::string g_serverOverride = "";

std::string GetActiveServerUrl() {
    if (!g_serverOverride.empty()) {
        return g_serverOverride;
    }
    return "https://omnistats.org";
}

int main(int argc, char* argv[]) {
    std::cout << "OmniStats Installer Starting...\n";

    std::string installDir = UpdaterCommon::GetLocalAppDataDir();

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            g_serverOverride = argv[++i];
        } else if (arg == "--dir" && i + 1 < argc) {
            installDir = argv[++i];
            if (installDir.back() != '\\' && installDir.back() != '/') {
                installDir += "\\";
            }
        }
    }

    std::cout << "Installing app to: " << installDir << "\n";
    std::cout << "Installing updater to: " << installDir << "\n";

    // Ensure folders exist
    if (!UpdaterCommon::EnsureDirExists(installDir)) {
        std::cout << "Failed to create installation directories.\n";
        return 1;
    }

    // Check if we can find binaries beside the installer first
    char currentExePath[MAX_PATH];
    GetModuleFileNameA(NULL, currentExePath, MAX_PATH);
    std::string exeStr(currentExePath);
    size_t lastSlash = exeStr.find_last_of("\\/");
    std::string currentDir = (lastSlash != std::string::npos) ? exeStr.substr(0, lastSlash) : ".";

    std::string localAppSrc = currentDir + "\\OmniStats.exe";
    std::string localUpdaterSrc = currentDir + "\\OmniStatsUpdater.exe";

    std::string destAppPath = installDir + "OmniStats.exe";
    std::string destUpdaterPath = installDir + "OmniStatsUpdater.exe";

    bool hasLocalApp = (GetFileAttributesA(localAppSrc.c_str()) != INVALID_FILE_ATTRIBUTES);
    bool hasLocalUpdater = (GetFileAttributesA(localUpdaterSrc.c_str()) != INVALID_FILE_ATTRIBUTES);

    // Initialize Curl
    curl_global_init(CURL_GLOBAL_ALL);
    std::string serverUrl = GetActiveServerUrl();

    if (hasLocalApp && hasLocalUpdater) {
        std::cout << "Found local binaries beside installer. Copying...\n";
        
        // Terminate any running instances first
        UpdaterCommon::TerminateProcessesRunningFromPath(destAppPath);

        if (!CopyFileA(localAppSrc.c_str(), destAppPath.c_str(), FALSE)) {
            std::cout << "Failed to copy OmniStats.exe. Error: " << GetLastError() << "\n";
            curl_global_cleanup();
            return 1;
        }
        if (!CopyFileA(localUpdaterSrc.c_str(), destUpdaterPath.c_str(), FALSE)) {
            std::cout << "Failed to copy OmniStatsUpdater.exe. Error: " << GetLastError() << "\n";
            curl_global_cleanup();
            return 1;
        }
    } else {
        std::cout << "Binaries not found beside installer. Fetching from server: " << serverUrl << "\n";

        // Terminate any running instances first
        UpdaterCommon::TerminateProcessesRunningFromPath(destAppPath);

        // Download OmniStats.exe
        std::cout << "Downloading OmniStats.exe...\n";
        std::string appUrl = serverUrl + "/OmniStats.exe?t=" + std::to_string(std::time(nullptr));
        if (!UpdaterCommon::DownloadFile(appUrl, destAppPath, 120)) {
            std::cout << "Failed to download OmniStats.exe.\n";
            curl_global_cleanup();
            return 1;
        }

        // Download checksum and verify
        std::string appShaUrl = serverUrl + "/OmniStats.exe.sha256?t=" + std::to_string(std::time(nullptr));
        std::string appExpectedSha = UpdaterCommon::DownloadString(appShaUrl, 10);
        if (appExpectedSha.empty() || !UpdaterCommon::VerifyFileSHA256(destAppPath, appExpectedSha)) {
            std::cout << "SHA-256 validation failed for OmniStats.exe.\n";
            DeleteFileA(destAppPath.c_str());
            curl_global_cleanup();
            return 1;
        }

        // Download OmniStatsUpdater.exe
        std::cout << "Downloading OmniStatsUpdater.exe...\n";
        std::string updaterUrl = serverUrl + "/OmniStatsUpdater.exe?t=" + std::to_string(std::time(nullptr));
        if (!UpdaterCommon::DownloadFile(updaterUrl, destUpdaterPath, 60)) {
            std::cout << "Failed to download OmniStatsUpdater.exe.\n";
            curl_global_cleanup();
            return 1;
        }

        // Download checksum and verify
        std::string updaterShaUrl = serverUrl + "/OmniStatsUpdater.exe.sha256?t=" + std::to_string(std::time(nullptr));
        std::string updaterExpectedSha = UpdaterCommon::DownloadString(updaterShaUrl, 10);
        if (updaterExpectedSha.empty() || !UpdaterCommon::VerifyFileSHA256(destUpdaterPath, updaterExpectedSha)) {
            std::cout << "SHA-256 validation failed for OmniStatsUpdater.exe.\n";
            DeleteFileA(destUpdaterPath.c_str());
            curl_global_cleanup();
            return 1;
        }
    }

    // Shortcuts creation
    char szPath[MAX_PATH];
    
    std::cout << "Creating Start Menu shortcut...\n";
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, szPath))) {
        std::string startMenuLnk = std::string(szPath) + "\\OmniStats.lnk";
        if (!UpdaterCommon::CreateShortcut(destAppPath, startMenuLnk, "OmniStats Rocket League live telemetry companion")) {
            std::cout << "Warning: Failed to create Start Menu shortcut.\n";
        }
    } else {
        std::cout << "Warning: Failed to locate Start Menu programs folder.\n";
    }

    std::cout << "Creating Desktop shortcut...\n";
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, szPath))) {
        std::string desktopLnk = std::string(szPath) + "\\OmniStats.lnk";
        if (!UpdaterCommon::CreateShortcut(destAppPath, desktopLnk, "OmniStats Rocket League live telemetry companion")) {
            std::cout << "Warning: Failed to create Desktop shortcut.\n";
        }
    } else {
        std::cout << "Warning: Failed to locate Desktop folder.\n";
    }

    std::cout << "Launch OmniStats application...\n";
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::string cmdLine = "\"" + destAppPath + "\"";
    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back('\0');

    if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, installDir.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        std::cout << "Installation complete. Launched OmniStats.\n";
    } else {
        std::cout << "Installation complete, but failed to launch app. Error: " << GetLastError() << "\n";
    }

    curl_global_cleanup();
    return 0;
}
