#include <ctime>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include "network/UpdaterCommon.hpp"
#include "network/ShortcutUtils.hpp"
#include "updater/StatsApiRepair.hpp"
#include <shellapi.h>
#include <softpub.h>
#include <wintrust.h>

#ifdef NDEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#ifndef OMNISTATS_VERSION
#define OMNISTATS_VERSION "2.0.0"
#endif
#include "network/SigningConfig.hpp"

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
    const std::string versionUrl = serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr));
    latestVersion = UpdaterCommon::Trim(UpdaterCommon::DownloadString(versionUrl, 10));
    if (latestVersion.empty()) {
        std::cout << "[Updater] Failed to check version from " << versionUrl << "\n";
        return false;
    }

    char updaterPath[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, updaterPath, MAX_PATH);
    std::string updaterDir;
    const std::string updaterPathStr(updaterPath);
    const size_t lastSlash = updaterPathStr.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        updaterDir = updaterPathStr.substr(0, lastSlash + 1);
    }

    const std::string appNextToUpdater = updaterDir + "OmniStats.exe";
    const std::string appInLocalAppData = UpdaterCommon::GetLocalAppDataDir() + "OmniStats.exe";

    std::string localVersion;
    if (GetFileAttributesA(appNextToUpdater.c_str()) != INVALID_FILE_ATTRIBUTES) {
        localVersion = UpdaterCommon::GetFileVersion(appNextToUpdater);
    } else if (GetFileAttributesA(appInLocalAppData.c_str()) != INVALID_FILE_ATTRIBUTES) {
        localVersion = UpdaterCommon::GetFileVersion(appInLocalAppData);
    }

    if (localVersion.empty()) {
        localVersion = OMNISTATS_VERSION;
    }

    std::cout << "[Updater] Server version: " << latestVersion
              << ", Local app version: " << localVersion << "\n";
    return UpdaterCommon::IsNewerVersion(localVersion, latestVersion);
}

namespace {

    bool VerifyMsiPackage(const std::string& msiPath, const std::string& expectedSha) {
        return UpdaterCommon::VerifyMsiPackage(msiPath, expectedSha);
    }

    std::string QuoteCommandLineArg(const std::string& value) {
        std::string quoted = "\"";
        for (const char c : value) {
            if (c == '"') {
                quoted += "\\\"";
            } else {
                quoted += c;
            }
        }
        quoted += "\"";
        return quoted;
    }

    bool LaunchSilentMsiUpgrade(const std::string& msiPath) {
        const std::string logPath = UpdaterCommon::GetAppDataDir() + "omnistats_msi_update.log";
        std::string commandLine =
            "msiexec.exe /i " + QuoteCommandLineArg(msiPath) +
            " /qn /norestart REBOOT=ReallySuppress OMNISTATS_AUTOSTART=1 /L*v " + QuoteCommandLineArg(logPath);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        std::vector<char> commandLineBuffer(commandLine.begin(), commandLine.end());
        commandLineBuffer.push_back('\0');

        if (!CreateProcessA(nullptr, commandLineBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            std::cout << "[Updater] Failed to start Windows Installer. Error: " << GetLastError() << "\n";
            return false;
        }

        std::cout << "[Updater] Windows Installer started. The updater will now exit so MSI can replace it.\n";
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    bool DownloadAndLaunchMsiUpgrade(const std::string& serverUrl) {
        const std::string appDataDir = UpdaterCommon::GetAppDataDir();
        const std::string updatesDir = appDataDir + "updates\\";
        if (!UpdaterCommon::EnsureDirExists(updatesDir)) {
            std::cout << "[Updater] Failed to create updates directory.\n";
            return false;
        }

        const std::string msiPath = updatesDir + "OmniStats.msi";
        const std::string msiUrl = serverUrl + "/OmniStats.msi?t=" + std::to_string(std::time(nullptr));
        const std::string shaUrl = serverUrl + "/OmniStats.msi.sha256?t=" + std::to_string(std::time(nullptr));

        DeleteFileA(msiPath.c_str());
        std::cout << "[Updater] Downloading signed installer package...\n";
        if (!UpdaterCommon::DownloadFile(msiUrl, msiPath, 180)) {
            std::cout << "[Updater] Failed to download OmniStats.msi.\n";
            return false;
        }

        const std::string expectedSha = UpdaterCommon::DownloadString(shaUrl, 15);
        if (expectedSha.empty()) {
            std::cout << "[Updater] Failed to retrieve OmniStats.msi.sha256.\n";
            DeleteFileA(msiPath.c_str());
            return false;
        }

        if (!VerifyMsiPackage(msiPath, expectedSha)) {
            DeleteFileA(msiPath.c_str());
            return false;
        }

        return LaunchSilentMsiUpgrade(msiPath);
    }

    bool ShouldRetryStatsRepairElevated(int result) {
        return result == 3 || result == 4 || result == 5;
    }

    int RelaunchStatsRepairElevated(const std::string& filePath, int expectedPort) {
        char updaterPath[MAX_PATH] = {0};
        const DWORD length = GetModuleFileNameA(nullptr, updaterPath, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) {
            return 5;
        }

        const std::string parameters =
            "--repair-stats-api " + QuoteCommandLineArg(filePath) + " " + std::to_string(expectedPort) + " --elevated";

        SHELLEXECUTEINFOA sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = "runas";
        sei.lpFile = updaterPath;
        sei.lpParameters = parameters.c_str();
        sei.nShow = SW_SHOWNORMAL;

        if (!ShellExecuteExA(&sei)) {
            const DWORD error = GetLastError();
            std::cout << "[Updater] Elevation was not started. Error: " << error << "\n";
            return 5;
        }

        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD exitCode = 5;
        if (!GetExitCodeProcess(sei.hProcess, &exitCode)) {
            exitCode = 5;
        }
        CloseHandle(sei.hProcess);
        return static_cast<int>(exitCode);
    }

    int RunStatsApiRepair(const std::string& filePath, int expectedPort, bool alreadyElevated) {
        int result = UpdaterStatsApiRepair::FixConfigStrictHeadless(filePath, expectedPort);
        if (result == 0 || alreadyElevated || !ShouldRetryStatsRepairElevated(result)) {
            return result;
        }

        std::cout << "[Updater] Stats API config requires elevated write access; requesting UAC approval.\n";
        return RelaunchStatsRepairElevated(filePath, expectedPort);
    }

} // namespace

int main(int argc, char* argv[]) {
    const std::string appDataDir = UpdaterCommon::GetAppDataDir();
    UpdaterCommon::EnsureDirExists(appDataDir);
    const std::string logPath = appDataDir + "omnistats_updater_log.txt";

    FILE* logFile = nullptr;
    freopen_s(&logFile, logPath.c_str(), "w", stdout);
    if (logFile) {
        setvbuf(logFile, nullptr, _IONBF, 0);
    }
    FILE* errFile = nullptr;
    freopen_s(&errFile, logPath.c_str(), "a", stderr);

    std::cout << "[Updater] Started. Arguments: ";
    for (int i = 0; i < argc; ++i) {
        std::cout << argv[i] << " ";
    }
    std::cout << "\n";

    bool check = false;
    bool installUpdate = false;
    bool repairDependencies = false;
    bool repairStatsApi = false;
    bool elevatedRepair = false;
    DWORD parentPid = 0;
    std::string resultFile;
    std::string statsApiPath;
    int statsApiPort = 49123;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            g_serverOverride = argv[++i];
        } else if (arg == "--check") {
            check = true;
        } else if (arg == "--install-update" && i + 1 < argc) {
            installUpdate = true;
            try {
                parentPid = std::stoul(argv[++i]);
            } catch (...) {
                parentPid = 0;
            }
        } else if (arg == "--update-app" && i + 2 < argc) {
            // Legacy command compatibility. Older OmniStats builds pass the target EXE
            // followed by the parent PID. MSI-based updates no longer replace that EXE directly.
            ++i; // target path intentionally ignored
            installUpdate = true;
            try {
                parentPid = std::stoul(argv[++i]);
            } catch (...) {
                parentPid = 0;
            }
        } else if (arg == "--apply") {
            // Backward-compatible manual updater entry point.
            installUpdate = true;
        } else if (arg == "--repair") {
            repairDependencies = true;
        } else if (arg == "--repair-stats-api" && i + 2 < argc) {
            repairStatsApi = true;
            statsApiPath = argv[++i];
            try {
                statsApiPort = std::stoi(argv[++i]);
            } catch (...) {
                statsApiPort = 49123;
            }
        } else if (arg == "--elevated") {
            elevatedRepair = true;
        } else if (arg == "--result-file" && i + 1 < argc) {
            resultFile = argv[++i];
        }
    }

    if (repairStatsApi) {
        return RunStatsApiRepair(statsApiPath, statsApiPort, elevatedRepair);
    }

    curl_global_init(CURL_GLOBAL_ALL);
    const std::string serverUrl = GetActiveServerUrl();

    if (repairDependencies) {
        const bool success = RepairDependencies(appDataDir);
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    if (check) {
        std::string latestVersion;
        const bool updateAvailable = PerformUpdateCheck(serverUrl, latestVersion);

        if (!resultFile.empty() && !latestVersion.empty()) {
            std::ofstream result(resultFile, std::ios::trunc);
            if (result.is_open()) {
                result << latestVersion << "\n";
            } else {
                std::cout << "[Updater] Failed to write update-check result file: " << resultFile << "\n";
            }
        }

        curl_global_cleanup();
        if (updateAvailable) {
            std::cout << "[Updater] Update is available: " << latestVersion << "\n";
            return 0;
        }
        if (latestVersion.empty()) {
            std::cout << "[Updater] Update check failed.\n";
            return 2;
        }
        std::cout << "[Updater] No update available.\n";
        return 1;
    }

    if (installUpdate) {
        if (parentPid != 0) {
            std::cout << "[Updater] Waiting for OmniStats process " << parentPid << " to exit...\n";
            if (!UpdaterCommon::WaitForProcessExit(parentPid, 15000)) {
                std::cout << "[Updater] OmniStats is still running; refusing to start MSI update.\n";
                curl_global_cleanup();
                return 1;
            }
        }

        const bool success = DownloadAndLaunchMsiUpgrade(serverUrl);
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    std::cout << "[Updater] No valid command provided. Exiting.\n";
    curl_global_cleanup();
    return 1;
}
