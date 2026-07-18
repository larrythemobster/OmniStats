#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include "network/UpdaterCommon.hpp"
#include "network/ShortcutUtils.hpp"

#ifndef OMNISTATS_VERSION
#define OMNISTATS_VERSION "2.0.0"
#endif

// Global override for test server URL
static std::string g_serverOverride = "";

std::string GetActiveServerUrl() {
    if (!g_serverOverride.empty()) {
        return g_serverOverride;
    }
    return "https://omnistats.org";
}

// Tar and gzip extraction helpers
#include <zlib.h>
#include <fstream>

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

static long long parseTarOctal(const char* s, int len) {
    long long val = 0;
    for (int i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        val = val * 8 + (s[i] - '0');
    return val;
}

static bool gzipDecompress(const std::vector<unsigned char>& compressed, std::vector<unsigned char>& out) {
    z_stream strm = {};
    if (inflateInit2(&strm, 15 + 16) != Z_OK) return false;

    strm.next_in = const_cast<unsigned char*>(compressed.data());
    strm.avail_in = (uInt)compressed.size();

    out.clear();
    out.resize(compressed.size() * 4); // initial guess
    int ret;
    do {
        if (strm.total_out >= out.size())
            out.resize(out.size() * 2);
        strm.next_out = out.data() + strm.total_out;
        strm.avail_out = (uInt)(out.size() - strm.total_out);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_MEM_ERROR || ret == Z_DATA_ERROR || ret == Z_STREAM_ERROR) {
            inflateEnd(&strm);
            return false;
        }
    } while (ret != Z_STREAM_END);

    out.resize(strm.total_out);
    inflateEnd(&strm);
    return true;
}

static bool extractFromTar(const std::vector<unsigned char>& tarData,
                           const std::string& targetName,
                           const std::string& outputPath) {
    size_t pos = 0;
    while (pos + 512 <= tarData.size()) {
        const TarHeader* hdr = reinterpret_cast<const TarHeader*>(&tarData[pos]);
        if (hdr->name[0] == '\0') break;

        long long fileSize = parseTarOctal(hdr->size, 12);
        pos += 512; // skip header

        std::string name(hdr->name);
        if (name.substr(0, 2) == "./") name = name.substr(2);

        if (name == targetName && hdr->typeflag == '0') {
            std::ofstream out(outputPath, std::ios::binary);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(&tarData[pos]), fileSize);
            out.close();
            std::cout << "[Updater] Extracted " << targetName << " to " << outputPath << "\n";
            return true;
        }

        pos += ((fileSize + 511) / 512) * 512;
    }
    return false;
}

bool RepairDependencies(const std::string& appDataDir) {
    std::string dllPath = appDataDir + "libcurl-impersonate.dll";
    std::string zlibPath = appDataDir + "zlib.dll";

    bool dllExists = (GetFileAttributesA(dllPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    bool zlibExists = (GetFileAttributesA(zlibPath.c_str()) != INVALID_FILE_ATTRIBUTES);

    static const std::string EXPECTED_DLL_HASH = "97f1e2988e2edd296d902a4bdc4f12d61b3b565ff2a320fff72d65075afb9d33";
    if (dllExists && zlibExists) {
        if (UpdaterCommon::VerifyFileSHA256(dllPath, EXPECTED_DLL_HASH)) {
            std::cout << "[Updater] DLL dependencies are already present and valid.\n";
            return true;
        }
        std::cout << "[Updater] Hash mismatch on existing libcurl-impersonate.dll. Repairing...\n";
    } else {
        std::cout << "[Updater] DLL dependencies missing. Repairing...\n";
    }

    std::string tempArchive = appDataDir + "updates\\libcurl-impersonate.tar.gz";
    std::string archiveUrl = "https://github.com/lexiforest/curl-impersonate/releases/download/v1.5.6/libcurl-impersonate-v1.5.6.x86_64-win32.tar.gz";

    std::cout << "[Updater] Downloading official curl-impersonate archive...\n";
    if (!UpdaterCommon::DownloadFile(archiveUrl, tempArchive, 120)) {
        std::cout << "[Updater] Failed to download archive.\n";
        return false;
    }

    static const std::string EXPECTED_ARCHIVE_HASH = "fe8ce2488d5467fda6061b8b130b5834bc30cdfff40712692e8c5685dbbda6c7";
    if (!UpdaterCommon::VerifyFileSHA256(tempArchive, EXPECTED_ARCHIVE_HASH)) {
        std::cout << "[Updater] Archive hash verification failed.\n";
        DeleteFileA(tempArchive.c_str());
        return false;
    }
    std::cout << "[Updater] Archive verification passed. Extracting...\n";

    // Read archive
    std::ifstream file(tempArchive, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cout << "[Updater] Failed to open temp archive.\n";
        DeleteFileA(tempArchive.c_str());
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> downloadBuffer(size);
    if (!file.read(reinterpret_cast<char*>(downloadBuffer.data()), size)) {
        std::cout << "[Updater] Failed to read temp archive.\n";
        file.close();
        DeleteFileA(tempArchive.c_str());
        return false;
    }
    file.close();

    // Decompress gzip
    std::vector<unsigned char> tarData;
    if (!gzipDecompress(downloadBuffer, tarData)) {
        std::cout << "[Updater] Gzip decompression failed.\n";
        DeleteFileA(tempArchive.c_str());
        return false;
    }

    // Extract DLLs
    bool ok1 = extractFromTar(tarData, "bin/libcurl-impersonate.dll", dllPath);
    bool ok2 = extractFromTar(tarData, "bin/zlib.dll", zlibPath);

    DeleteFileA(tempArchive.c_str());

    if (!ok1 || !ok2) {
        std::cout << "[Updater] Failed to extract runtime DLLs from archive.\n";
        return false;
    }

    std::cout << "[Updater] Dependency repair complete. DLLs successfully restored.\n";
    return true;
}

bool PerformUpdateCheck(const std::string& serverUrl, std::string& latestVersion) {
    std::string versionUrl = serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr));
    latestVersion = UpdaterCommon::DownloadString(versionUrl, 10);
    if (latestVersion.empty()) {
        std::cout << "[Updater] Failed to check version from " << versionUrl << "\n";
        return false;
    }

    char updaterPath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, updaterPath, MAX_PATH);
    std::string updaterDir = "";
    std::string updaterPathStr(updaterPath);
    size_t lastSlash = updaterPathStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        updaterDir = updaterPathStr.substr(0, lastSlash + 1);
    }
    std::string appNextToUpdater = updaterDir + "OmniStats.exe";
    std::string appInLocalAppData = UpdaterCommon::GetLocalAppDataDir() + "OmniStats.exe";

    std::string localVersion = "";
    if (GetFileAttributesA(appNextToUpdater.c_str()) != INVALID_FILE_ATTRIBUTES) {
        localVersion = UpdaterCommon::GetFileVersion(appNextToUpdater);
    } else if (GetFileAttributesA(appInLocalAppData.c_str()) != INVALID_FILE_ATTRIBUTES) {
        localVersion = UpdaterCommon::GetFileVersion(appInLocalAppData);
    }

    if (localVersion.empty()) {
        localVersion = OMNISTATS_VERSION; // Fallback
    }

    std::cout << "[Updater] Server version: " << latestVersion << ", Local app version: " << localVersion << "\n";
    return UpdaterCommon::IsNewerVersion(localVersion, latestVersion);
}

bool DownloadAndInstall(const std::string& serverUrl, const std::string& targetPath) {
    std::string appDataDir = UpdaterCommon::GetAppDataDir();
    std::string updatesDir = appDataDir + "updates\\";
    std::string backupDir = updatesDir + "backup\\";

    if (!UpdaterCommon::EnsureDirExists(updatesDir) || !UpdaterCommon::EnsureDirExists(backupDir)) {
        std::cout << "[Updater] Failed to create directories for updates.\n";
        return false;
    }

    std::string tempExePath = updatesDir + "OmniStats.exe";
    std::cout << "[Updater] Downloading OmniStats.exe...\n";
    std::string downloadUrl = serverUrl + "/OmniStats.exe?t=" + std::to_string(std::time(nullptr));
    if (!UpdaterCommon::DownloadFile(downloadUrl, tempExePath, 120)) {
        std::cout << "[Updater] Download failed.\n";
        return false;
    }

    std::cout << "[Updater] Downloading OmniStats.exe.sha256...\n";
    std::string shaUrl = serverUrl + "/OmniStats.exe.sha256?t=" + std::to_string(std::time(nullptr));
    std::string expectedSha = UpdaterCommon::DownloadString(shaUrl, 10);
    if (expectedSha.empty()) {
        std::cout << "[Updater] Failed to retrieve expected SHA-256.\n";
        DeleteFileA(tempExePath.c_str());
        return false;
    }

    if (!UpdaterCommon::VerifyFileSHA256(tempExePath, expectedSha)) {
        std::cout << "[Updater] SHA-256 checksum verification failed.\n";
        DeleteFileA(tempExePath.c_str());
        return false;
    }
    std::cout << "[Updater] SHA-256 verification passed.\n";

    // Backup current target
    std::string backupExePath = backupDir + "OmniStats.exe";
    bool hasBackup = false;
    if (GetFileAttributesA(targetPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteFileA(backupExePath.c_str());
        if (MoveFileExA(targetPath.c_str(), backupExePath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            hasBackup = true;
            std::cout << "[Updater] Backup of current executable created.\n";
        } else {
            std::cout << "[Updater] Warning: Failed to backup current executable. Error: " << GetLastError() << "\n";
        }
    }

    // Move new file into target path
    if (MoveFileExA(tempExePath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::cout << "[Updater] Target replacement succeeded!\n";

        // Spawn target
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        std::string cmdLine = "\"" + targetPath + "\"";
        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        size_t lastSlash = targetPath.find_last_of("\\/");
        std::string dirPath = (lastSlash != std::string::npos) ? targetPath.substr(0, lastSlash) : ".";

        if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, dirPath.c_str(), &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            std::cout << "[Updater] Spawned updated executable: " << cmdLine << "\n";
            return true;
        } else {
            std::cout << "[Updater] Failed to spawn updated executable. Error: " << GetLastError() << "\n";
            // Rollback
            if (hasBackup) {
                std::cout << "[Updater] Attempting rollback...\n";
                MoveFileExA(backupExePath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING);
            }
            return false;
        }
    } else {
        std::cout << "[Updater] Failed to move new executable to target: " << targetPath << ". Error: " << GetLastError() << "\n";
        // Rollback
        if (hasBackup) {
            std::cout << "[Updater] Attempting rollback...\n";
            MoveFileExA(backupExePath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Redirect stdout and stderr to the updater log file in AppData
    std::string appDataDir = UpdaterCommon::GetAppDataDir();
    UpdaterCommon::EnsureDirExists(appDataDir);
    std::string logPath = appDataDir + "omnistats_updater_log.txt";

    FILE* logFile = nullptr;
    freopen_s(&logFile, logPath.c_str(), "w", stdout);
    if (logFile) {
        setvbuf(logFile, NULL, _IONBF, 0);
    }
    FILE* errFile = nullptr;
    freopen_s(&errFile, logPath.c_str(), "a", stderr);

    std::cout << "[Updater] Started. Arguments: ";
    for (int i = 0; i < argc; ++i) {
        std::cout << argv[i] << " ";
    }
    std::cout << "\n";

    // Initialize Curl globally
    curl_global_init(CURL_GLOBAL_ALL);

    // Command flags
    bool check = false;
    bool apply = false;
    std::string targetPath = "";
    DWORD parentPid = 0;
    bool repair = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            g_serverOverride = argv[++i];
        } else if (arg == "--check") {
            check = true;
        } else if (arg == "--apply") {
            apply = true;
        } else if (arg == "--update-app" && i + 2 < argc) {
            targetPath = argv[i + 1];
            try {
                parentPid = std::stoul(argv[i + 2]);
            } catch (...) {
                parentPid = 0;
            }
            i += 2;
        } else if (arg == "--repair") {
            repair = true;
        }
    }

    std::string serverUrl = GetActiveServerUrl();

    if (repair) {
        bool success = RepairDependencies(appDataDir);
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    if (check) {
        std::string latestVersion;
        bool updateAvailable = PerformUpdateCheck(serverUrl, latestVersion);
        if (updateAvailable) {
            std::cout << "[Updater] Update is available: " << latestVersion << "\n";
            curl_global_cleanup();
            return 0; // 0 means update available
        } else {
            std::cout << "[Updater] No update available.\n";
            curl_global_cleanup();
            return 1; // 1 means no update
        }
    }

    if (!targetPath.empty()) {
        std::cout << "[Updater] Target path: " << targetPath << ", Parent PID: " << parentPid << "\n";

        // Ensure dependencies are present/repaired for bridge users
        RepairDependencies(appDataDir);

        // Step 1: Wait for parent process to exit
        if (parentPid != 0) {
            std::cout << "[Updater] Waiting for parent process " << parentPid << " to exit...\n";
            if (!UpdaterCommon::WaitForProcessExit(parentPid, 10000)) {
                std::cout << "[Updater] Parent process did not exit. Terminating parent process.\n";
                UpdaterCommon::TerminateProcessById(parentPid);
            }
        }

        // Step 2: Terminate any other instances locked on target path
        std::cout << "[Updater] Terminating any locks on target path...\n";
        UpdaterCommon::TerminateProcessesRunningFromPath(targetPath);

        // Step 3: Run download and installation
        bool success = DownloadAndInstall(serverUrl, targetPath);
        if (success) {
            UpdaterCommon::RepairExistingOmniStatsShortcuts(targetPath);
        }
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    if (apply) {
        // Ensure dependencies are present/repaired
        RepairDependencies(appDataDir);

        // Apply updates to the default install location
        std::string defaultTarget = UpdaterCommon::GetLocalAppDataDir() + "OmniStats.exe";
        std::cout << "[Updater] Running apply on default path: " << defaultTarget << "\n";
        bool success = DownloadAndInstall(serverUrl, defaultTarget);
        if (success) {
            UpdaterCommon::RepairExistingOmniStatsShortcuts(defaultTarget);
        }
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    std::cout << "[Updater] No valid command provided. Exiting.\n";
    curl_global_cleanup();
    return 1;
}
