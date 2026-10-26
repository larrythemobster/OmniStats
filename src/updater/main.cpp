#include <io.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>
#include <windows.h>
#include "updater/UpdaterCommon.hpp"
#include "updater/UpdaterDownload.hpp"
#include "updater/UpdaterPaths.hpp"
#include "updater/UpdaterProcess.hpp"
#include "updater/ReleaseManifest.hpp"
#include "updater/ShortcutUtils.hpp"
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
#include "network/http/SigningConfig.hpp"
#include "network/http/CurlRuntime.hpp"

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

        const long long fileSize = parseTarOctal(hdr->size, 12);
        pos += 512; // skip header
        if (fileSize < 0 || static_cast<unsigned long long>(fileSize) > tarData.size() - pos) {
            return false;
        }

        std::string name(hdr->name, strnlen(hdr->name, sizeof(hdr->name)));
        if (name.substr(0, 2) == "./") name = name.substr(2);

        if (name == targetName && hdr->typeflag == '0') {
            std::ofstream out(outputPath, std::ios::binary);
            if (!out) return false;
            out.write(reinterpret_cast<const char*>(&tarData[pos]), static_cast<std::streamsize>(fileSize));
            out.close();
            if (!out) return false;
            std::cout << "[Updater] Extracted " << targetName << " to " << outputPath << "\n";
            return true;
        }

        pos += ((fileSize + 511) / 512) * 512;
    }
    return false;
}

bool RepairDependencies(const std::string& appDataDir) {
    const std::string dllPath = appDataDir + std::string(CurlRuntime::kLibcurlDllName);
    const std::string zlibPath = appDataDir + std::string(CurlRuntime::kZlibDllName);
    const auto dependenciesValid = [&]() {
        return UpdaterCommon::VerifyFileSHA256(dllPath, std::string(CurlRuntime::kLibcurlDllSha256)) &&
               UpdaterCommon::VerifyFileSHA256(zlibPath, std::string(CurlRuntime::kZlibDllSha256));
    };

    if (dependenciesValid()) {
        std::cout << "[Updater] DLL dependencies are already present and valid.\n";
        return true;
    }
    std::cout << "[Updater] DLL dependencies missing or modified. Repairing...\n";

    std::string tempArchive = appDataDir + "updates\\libcurl-impersonate.tar.gz";
    const std::string archiveUrl(CurlRuntime::kArchiveUrl);

    std::cout << "[Updater] Downloading official curl-impersonate archive...\n";
    if (!UpdaterCommon::DownloadFile(archiveUrl, tempArchive, 120)) {
        std::cout << "[Updater] Failed to download archive.\n";
        return false;
    }

    if (!UpdaterCommon::VerifyFileSHA256(tempArchive, std::string(CurlRuntime::kArchiveSha256))) {
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
    if (!dependenciesValid()) {
        std::cout << "[Updater] Extracted runtime DLLs do not match the pinned hashes.\n";
        return false;
    }

    std::cout << "[Updater] Dependency repair complete. DLLs successfully restored.\n";
    return true;
}

bool PerformUpdateCheck(const std::string& serverUrl, std::string& latestVersion) {
    const std::string localVersion = UpdaterCommon::GetInstalledAppVersion();
    ReleaseManifest manifest;
    ManifestVerifyResult result = ManifestVerifyResult::ManifestMissing;

    const bool ok = ReleaseManifestVerifier::FetchAndVerify(
        serverUrl, localVersion, manifest, result,
        SigningConfig::CURRENT_RELEASE_PUBLIC_KEY_HEX,
        SigningConfig::NEXT_RELEASE_PUBLIC_KEY_HEX);

    if (!ok) {
        std::cout << "[Updater] Manifest verification failed from " << serverUrl << "\n";
        std::string category = "manifest_missing";
        if (result == ManifestVerifyResult::SignatureInvalid) {
            category = "signature_invalid";
        } else if (result == ManifestVerifyResult::DowngradeRefused) {
            category = "downgrade_refused";
            latestVersion.clear();
        } else if (result == ManifestVerifyResult::ParseError) {
            category = "parse_error";
        }
        UpdaterCommon::RecordUpdaterEvent("update_check_failed", "", category, localVersion);
        return false;
    }

    latestVersion = manifest.version;
    const bool forced = ReleaseManifestVerifier::IsUpdateForced(localVersion, manifest.min_version);
    std::cout << "[Updater] Verified server release: " << latestVersion
              << ", local app version: " << localVersion
              << (forced ? " (mandatory update required)" : "") << "\n";
    return UpdaterCommon::IsNewerVersion(localVersion, latestVersion);
}

namespace {

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

    bool LaunchSilentMsiUpgrade(const std::string& msiPath, const std::string& targetVersion, const std::string& localVersion) {
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
            UpdaterCommon::RecordUpdaterEvent("msi_launch_failed", targetVersion, "create_process_failed", localVersion);
            return false;
        }

        std::cout << "[Updater] Windows Installer started. The updater will now exit so MSI can replace it.\n";
        UpdaterCommon::RecordUpdaterEvent("msi_launched", targetVersion, "", localVersion);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }

    bool DownloadAndLaunchMsiUpgrade(const std::string& serverUrl, const std::string& targetVersionParam = "") {
        const std::string localVersion = UpdaterCommon::GetInstalledAppVersion();

        ReleaseManifest manifest;
        ManifestVerifyResult manifestRes = ManifestVerifyResult::ManifestMissing;
        const bool manifestOk = ReleaseManifestVerifier::FetchAndVerify(
            serverUrl, localVersion, manifest, manifestRes,
            SigningConfig::CURRENT_RELEASE_PUBLIC_KEY_HEX,
            SigningConfig::NEXT_RELEASE_PUBLIC_KEY_HEX);

        if (!manifestOk) {
            std::cout << "[Updater] Failed to verify release manifest from " << serverUrl << "\n";
            std::string cat = "manifest_missing";
            if (manifestRes == ManifestVerifyResult::SignatureInvalid) {
                cat = "signature_invalid";
            } else if (manifestRes == ManifestVerifyResult::DowngradeRefused) {
                cat = "downgrade_refused";
            }
            UpdaterCommon::RecordUpdaterEvent("download_failed", targetVersionParam, cat, localVersion);
            return false;
        }

        const std::string targetVersion = manifest.version;
        UpdaterCommon::RecordUpdaterEvent("update_started", targetVersion, "", localVersion);

        const std::string appDataDir = UpdaterCommon::GetAppDataDir();
        const std::string updatesDir = appDataDir + "updates\\";
        if (!UpdaterCommon::EnsureDirExists(updatesDir)) {
            std::cout << "[Updater] Failed to create updates directory.\n";
            UpdaterCommon::RecordUpdaterEvent("download_failed", targetVersion, "io_error", localVersion);
            return false;
        }

        const std::string msiPath = updatesDir + "OmniStats.msi";
        std::string msiUrl = ReleaseManifestVerifier::ResolveMsiUrl(serverUrl, manifest.msi_url);
        if (msiUrl.find('?') == std::string::npos) {
            msiUrl += "?t=" + std::to_string(std::time(nullptr));
        }

        DeleteFileA(msiPath.c_str());
        std::cout << "[Updater] Downloading signed installer package from " << msiUrl << "...\n";
        long httpCode = 0;
        CURLcode curlCode = CURLE_OK;
        if (!UpdaterCommon::DownloadFile(msiUrl, msiPath, 180, &httpCode, &curlCode)) {
            std::cout << "[Updater] Failed to download OmniStats.msi.\n";
            std::string cat = "network";
            if (httpCode != 200 && httpCode != 0) cat = "http_" + std::to_string(httpCode);
            UpdaterCommon::RecordUpdaterEvent("download_failed", targetVersion, cat, localVersion);
            return false;
        }

        const auto verifyRes = UpdaterCommon::VerifyMsiPackage(msiPath, manifest.msi_sha256);
        if (verifyRes != UpdaterCommon::MsiVerifyResult::Ok) {
            DeleteFileA(msiPath.c_str());
            if (verifyRes == UpdaterCommon::MsiVerifyResult::ChecksumMismatch) {
                UpdaterCommon::RecordUpdaterEvent("checksum_failed", targetVersion, "checksum_mismatch", localVersion);
            } else {
                UpdaterCommon::RecordUpdaterEvent("signature_failed", targetVersion, "signature_invalid", localVersion);
            }
            return false;
        }

        return LaunchSilentMsiUpgrade(msiPath, targetVersion, localVersion);
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

    // freopen_s opens without sharing, so a concurrent updater could not open the log and failed fast on its first write.
#pragma warning(suppress : 4996)
    FILE* logFile = std::freopen(logPath.c_str(), "w", stdout);
    if (logFile) {
        setvbuf(logFile, nullptr, _IONBF, 0);
    } else {
#pragma warning(suppress : 4996)
        (void)std::freopen("NUL", "w", stdout);
    }
#pragma warning(suppress : 4996)
    if (std::freopen("NUL", "w", stderr) && logFile) _dup2(_fileno(stdout), _fileno(stderr));

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
    std::string targetVersion;

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
        } else if (arg == "--target-version" && i + 1 < argc) {
            targetVersion = argv[++i];
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

        const bool success = DownloadAndLaunchMsiUpgrade(serverUrl, targetVersion);
        curl_global_cleanup();
        return success ? 0 : 1;
    }

    std::cout << "[Updater] No valid command provided. Exiting.\n";
    curl_global_cleanup();
    return 1;
}
