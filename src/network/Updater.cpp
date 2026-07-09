#include "Updater.hpp"
#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include "network/TelemetryManager.hpp"
#include "network/UpdaterCommon.hpp"
#include <iostream>
#include <thread>
#include <mutex>
#include <sstream>
#include <vector>
#include <utility>
#include <ctime>
#include <windows.h>
#include "core/FileHash.hpp"
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <shellapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace Updater {
    std::atomic<bool> RestartRequested{false};
    std::atomic<bool> InstallerLaunched{false};
    static std::string g_updateServerOverride;
    static bool g_updaterEventsDisabled = false;
    static std::mutex g_updateThreadsMutex;
    static std::vector<std::thread> g_updateThreads;

    static std::string Trim(const std::string& str);

    static void StoreUpdateThread(std::thread thread) {
        std::lock_guard<std::mutex> lock(g_updateThreadsMutex);
        g_updateThreads.push_back(std::move(thread));
    }

    static std::string NormalizeServerUrl(std::string serverUrl) {
        serverUrl = Trim(serverUrl);
        while (!serverUrl.empty() && serverUrl.back() == '/') {
            serverUrl.pop_back();
        }
        return serverUrl;
    }

    static std::string ActiveUpdateServerUrl() {
        if (!g_updateServerOverride.empty()) {
            return g_updateServerOverride;
        }
        return UPDATE_SERVER_URL;
    }

    static std::string EnsureClientUUID() {
        ConfigData conf = Config::Read();
        if (!conf.client_uuid.empty()) {
            return conf.client_uuid;
        }

        std::string uuid = TelemetryManager::GenerateUUIDv4();
        Config::Update([&uuid](ConfigData& c) { c.client_uuid = uuid; }, false);
        Config::Save();
        return uuid;
    }

    static void SendUpdaterEvent(const std::string& event,
                                 const std::string& targetVersion = "",
                                 const std::string& errorCategory = "") {
#ifdef OMNISTATS_TEST_ENVIRONMENT
        (void)event;
        (void)targetVersion;
        (void)errorCategory;
        return;
#else
        ConfigData conf = Config::Read();
        if (g_updaterEventsDisabled) {
            return;
        }

        nlohmann::json j;
        j["client_uuid"] = EnsureClientUUID();
        j["version"] = CURRENT_VERSION;
        j["event"] = event;
        j["target_version"] = targetVersion;
        j["error_category"] = errorCategory;
        j["update_checks_enabled"] = conf.check_for_updates;
        j["auto_updater_enabled"] = conf.enable_auto_updates;
        std::string payload = j.dump();

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        std::string userAgent = "OmniStats-Client/" + CURRENT_VERSION;

        CURL* curl = curl_easy_init();
        if (!curl) {
            g_updaterEventsDisabled = true;
            if (headers) {
                curl_slist_free_all(headers);
            }
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, "https://api.omnistats.org/api/v1/updater-event");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1500L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 750L);
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        }

        curl_easy_cleanup(curl);
        if (headers) {
            curl_slist_free_all(headers);
        }

        if (res != CURLE_OK) {
            g_updaterEventsDisabled = true;
            std::cout << "[Updater] Updater health event failed: " << curl_easy_strerror(res) << "\n";
        } else if (httpCode < 200 || httpCode >= 300) {
            std::cout << "[Updater] Updater health event rejected (HTTP " << httpCode << ")\n";
        }
#endif
    }

    // Helper: Trim whitespace and control characters from string
    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    // Helper: Compare semantic versions (Major.Minor.Patch)
    static bool IsNewerVersion(const std::string& current, const std::string& latest) {
        std::vector<int> curParts, latParts;
        std::stringstream curSS(current), latSS(latest);
        std::string item;
        
        while (std::getline(curSS, item, '.')) {
            try { curParts.push_back(std::stoi(item)); } catch (...) { curParts.push_back(0); }
        }
        while (std::getline(latSS, item, '.')) {
            try { latParts.push_back(std::stoi(item)); } catch (...) { latParts.push_back(0); }
        }
        
        while (curParts.size() < 3) curParts.push_back(0);
        while (latParts.size() < 3) latParts.push_back(0);
        
        for (size_t i = 0; i < 3; ++i) {
            if (latParts[i] > curParts[i]) return true;
            if (latParts[i] < curParts[i]) return false;
        }
        return false;
    }

    // Libcurl callback: writes retrieved text into std::string
    static size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        std::string* s = static_cast<std::string*>(userp);
        s->append(static_cast<char*>(contents), total);
        return total;
    }

    // Libcurl callback: writes downloaded binary bytes directly into open std::ofstream stream
    static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t total = size * nmemb;
        std::ofstream* out = static_cast<std::ofstream*>(userp);
        out->write(static_cast<char*>(contents), total);
        return total;
    }

    static std::string CalculateFileSHA256(const std::string& filepath) {
        return CalculateSHA256(filepath);
    }

    void SetUpdateServerOverride(const std::string& serverUrl) {
        g_updateServerOverride = NormalizeServerUrl(serverUrl);
    }

    void ReportUpdatedSuccessfully() {
        SendUpdaterEvent("updated_successfully");
    }

    void CleanupOldBinary() {
        char currentExePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, currentExePath, MAX_PATH) != 0) {
            std::string exeStr(currentExePath);
            size_t lastSlash = exeStr.find_last_of("\\/");
            std::string dirPath = (lastSlash != std::string::npos) ? exeStr.substr(0, lastSlash) : ".";
            std::string oldExePath = dirPath + "\\OmniStats_old.exe";
            
            if (DeleteFileA(oldExePath.c_str())) {
                std::cout << "[Updater] Leftover backup 'OmniStats_old.exe' deleted.\n";
            }
        }
    }

    UpdateCheckResult CheckForUpdate(bool forceCheck) {
        UpdateCheckResult update;

        ConfigData conf = Config::Read();
        if (!forceCheck && !conf.check_for_updates && !conf.enable_auto_updates) {
            std::cout << "[Updater] Startup update checks are currently disabled.\n";
            return update;
        }

        update.serverUrl = ActiveUpdateServerUrl();
        if (update.serverUrl.empty()) {
            std::cout << "[Updater] Update server URL is empty.\n";
            SendUpdaterEvent("update_check_failed", "", "empty_server_url");
            return update;
        }

        std::cout << "[Updater] Update check started. Checking server: " << update.serverUrl << "\n";
        SendUpdaterEvent("update_check_started");

        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cout << "[Updater] Failed to initialize curl easy handle for version check.\n";
            SendUpdaterEvent("update_check_failed", "", "curl_init");
            return update;
        }

        std::string versionUrl = update.serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr));
        std::string responseString;

        curl_easy_setopt(curl, CURLOPT_URL, versionUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseString);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L); // 3s max timeout to prevent hangs when offline/slow server
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != 200) {
            std::cout << "[Updater] Failed to check for updates (CURL: " << res << ", HTTP: " << http_code << ")\n";
            SendUpdaterEvent("update_check_failed", "", res != CURLE_OK ? "curl_error" : "http_error");
            return update;
        }

        update.latestVersion = Trim(responseString);
        std::cout << "[Updater] Latest version on server: '" << update.latestVersion << "' (Current: '" << CURRENT_VERSION << "')\n";

        if (!IsNewerVersion(CURRENT_VERSION, update.latestVersion)) {
            std::cout << "[Updater] OmniStats is up to date.\n";
            return update;
        }

        update.available = true;
        SendUpdaterEvent("update_available", update.latestVersion);
        return update;
    }

    bool DownloadAndApplyUpdate(const UpdateCheckResult& update) {
        if (!update.available) {
            return false;
        }

        const std::string serverUrl = update.serverUrl.empty() ? ActiveUpdateServerUrl() : update.serverUrl;
        
        // Check for external updater first
        std::string appDataDir = UpdaterCommon::GetAppDataDir();
        std::string updaterPath = appDataDir + "OmniStatsUpdater.exe";
        DWORD attrib = GetFileAttributesA(updaterPath.c_str());
        bool updaterExists = (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY));
        if (updaterExists) {
            std::cout << "[Updater] External updater found at: " << updaterPath << ". Spawning updater process.\n";
            STARTUPINFOA si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            ZeroMemory(&pi, sizeof(pi));

            char currentExePath[MAX_PATH];
            GetModuleFileNameA(NULL, currentExePath, MAX_PATH);

            std::string cmdLine = "\"" + updaterPath + "\" --update-app \"" + std::string(currentExePath) + "\" " + std::to_string(GetCurrentProcessId());
            if (!g_updateServerOverride.empty()) {
                cmdLine += " --server \"" + g_updateServerOverride + "\"";
            }
            std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
            cmdLineBuf.push_back('\0');

            if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, appDataDir.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                InstallerLaunched.store(true);
                std::cout << "[Updater] Launched external updater. Closing main application.\n";
                return true;
            } else {
                std::cout << "[Updater] Failed to spawn external updater. Error: " << GetLastError() << ". Falling back to self-updater.\n";
            }
        }

        std::cout << "[Updater] Update available. Applying before startup continues.\n";
        SendUpdaterEvent("download_started", update.latestVersion);

        char currentExePath[MAX_PATH];
        if (GetModuleFileNameA(NULL, currentExePath, MAX_PATH) == 0) {
            std::cout << "[Updater] Error querying current module filepath.\n";
            SendUpdaterEvent("apply_failed", update.latestVersion, "module_path");
            return false;
        }

        std::string exeStr(currentExePath);
        size_t lastSlash = exeStr.find_last_of("\\/");
        std::string dirPath = (lastSlash != std::string::npos) ? exeStr.substr(0, lastSlash) : ".";

        std::string newExePath = dirPath + "\\OmniStats_new.exe";

        std::ofstream newExeFile(newExePath, std::ios::binary);
        if (!newExeFile.is_open()) {
            std::cout << "[Updater] Failed to open local temporary binary for write.\n";
            SendUpdaterEvent("download_failed", update.latestVersion, "local_file");
            return false;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            newExeFile.close();
            std::cout << "[Updater] Failed to initialize curl easy handle for download.\n";
            SendUpdaterEvent("download_failed", update.latestVersion, "curl_init");
            return false;
        }

        std::string downloadUrl = serverUrl + "/OmniStats.exe?t=" + std::to_string(std::time(nullptr));
        curl_easy_setopt(curl, CURLOPT_URL, downloadUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &newExeFile);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L); // 2 minutes max download window
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        newExeFile.close();
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != 200) {
            std::cout << "[Updater] Binary download failed (CURL: " << res << ", HTTP: " << http_code << "). Cleaning up.\n";
            DeleteFileA(newExePath.c_str());
            SendUpdaterEvent("download_failed", update.latestVersion, res != CURLE_OK ? "curl_error" : "http_error");
            return false;
        }

        // Reject obviously invalid downloads before checksum verification.
        HANDLE hFile = CreateFileA(newExePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSize;
            if (GetFileSizeEx(hFile, &fileSize)) {
                CloseHandle(hFile);
                if (fileSize.QuadPart < 500000) {
                    std::cout << "[Updater] Downloaded file is too small (" << fileSize.QuadPart << " bytes). Aborting update.\n";
                    DeleteFileA(newExePath.c_str());
                    SendUpdaterEvent("download_failed", update.latestVersion, "file_too_small");
                    return false;
                }
            } else {
                CloseHandle(hFile);
            }
        }

        std::cout << "[Updater] Downloading SHA-256 checksum from server...\n";
        std::string checksumUrl = serverUrl + "/OmniStats.exe.sha256?t=" + std::to_string(std::time(nullptr));
        std::string serverChecksum;

        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, checksumUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &serverChecksum);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

            res = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_cleanup(curl);

            if (res == CURLE_OK && http_code == 200) {
                serverChecksum = Trim(serverChecksum);
                // Accept common .sha256 formats by keeping only the hash prefix.
                if (serverChecksum.length() >= 64) {
                    serverChecksum = serverChecksum.substr(0, 64);
                }
                std::transform(serverChecksum.begin(), serverChecksum.end(), serverChecksum.begin(), ::tolower);
                std::cout << "[Updater] Server SHA-256 checksum: '" << serverChecksum << "'\n";

                std::string localChecksum = CalculateFileSHA256(newExePath);
                std::cout << "[Updater] Local SHA-256 checksum:  '" << localChecksum << "'\n";

                if (localChecksum.empty() || localChecksum != serverChecksum) {
                    std::cout << "[Updater] Checksum verification failed. Aborting update.\n";
                    DeleteFileA(newExePath.c_str());
                    SendUpdaterEvent("checksum_failed", update.latestVersion, "mismatch");
                    return false;
                }
                std::cout << "[Updater] Checksum verification passed.\n";
                SendUpdaterEvent("checksum_passed", update.latestVersion);
            } else {
                std::cout << "[Updater] Failed to retrieve server checksum. Aborting update for safety.\n";
                DeleteFileA(newExePath.c_str());
                SendUpdaterEvent("checksum_failed", update.latestVersion, res != CURLE_OK ? "curl_error" : "http_error");
                return false;
            }
        } else {
            std::cout << "[Updater] Failed to initialize curl for checksum query. Aborting update for safety.\n";
            DeleteFileA(newExePath.c_str());
            SendUpdaterEvent("checksum_failed", update.latestVersion, "curl_init");
            return false;
        }

        std::cout << "[Updater] Binary download complete and verified. Launching installer child process.\n";
        SendUpdaterEvent("apply_started", update.latestVersion);

        // Spawn replacement process with apply-update arguments.
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        ZeroMemory(&pi, sizeof(pi));

        std::string cmdLine = "\"" + newExePath + "\" --apply-update \"" +
                              std::string(currentExePath) + "\" " + std::to_string(GetCurrentProcessId());
        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
        cmdLineBuf.push_back('\0');

        if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL, dirPath.c_str(), &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            InstallerLaunched.store(true);
            std::cout << "[Updater] Launched child installer process. Closing parent instance.\n";
            return true;
        } else {
            std::cout << "[Updater] Failed to spawn child installer process. Error: " << GetLastError() << "\n";
            DeleteFileA(newExePath.c_str());
            SendUpdaterEvent("apply_failed", update.latestVersion, "create_process");
            return false;
        }
    }

    bool RunStartupUpdateFlow(bool forceCheck) {
        CleanupOldBinary();
        UpdateCheckResult update = CheckForUpdate(forceCheck);
        if (!update.available) {
            return false;
        }
        if (!forceCheck && !Config::Read().enable_auto_updates) {
            return false;
        }
        return DownloadAndApplyUpdate(update);
    }
    bool Initialize() {
        return RunStartupUpdateFlow(false);
    }

    void StartBackgroundUpdateCheck(std::shared_ptr<SessionState> state) {
        StoreUpdateThread(std::thread([state]() {
            std::cout << "[Updater] Background update check thread started.\n";
            UpdateCheckResult res = CheckForUpdate(true);
            if (res.available) {
                std::lock_guard<std::mutex> lock(state->ui.updateMutex);
                state->ui.updateAvailableVersion = res.latestVersion;
                state->ui.updateServerUrl = res.serverUrl;
                state->ui.updateAvailable.store(true);
            }
            state->ui.updateChecked.store(true);
            std::cout << "[Updater] Background update check complete. Available: "
                      << (res.available ? "yes (" + res.latestVersion + ")" : "no") << "\n";
        }));
    }

    void StartInteractiveUpdate(std::shared_ptr<SessionState> state) {
        bool expected = false;
        if (!state->ui.updateDownloading.compare_exchange_strong(expected, true)) {
            std::cout << "[Updater] Interactive update already in progress. Ignoring duplicate request.\n";
            return;
        }
        state->ui.updateDownloadFailed.store(false);

        StoreUpdateThread(std::thread([state]() {
            std::cout << "[Updater] Interactive manual update thread started.\n";
            UpdateCheckResult update;
            update.available = true;
            {
                std::lock_guard<std::mutex> lock(state->ui.updateMutex);
                update.latestVersion = state->ui.updateAvailableVersion;
                update.serverUrl = state->ui.updateServerUrl;
            }

            bool success = DownloadAndApplyUpdate(update);
            if (success) {
                std::cout << "[Updater] Interactive manual update succeeded! Requesting restart.\n";
                RestartRequested.store(true);
            } else {
                std::cout << "[Updater] Interactive manual update download/apply failed.\n";
                state->ui.updateDownloading.store(false);
                state->ui.updateDownloadFailed.store(true);
            }
        }));
    }

    void ShutdownBackgroundTasks() {
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

    void BootstrapUpdater() {
        std::cout << "[Bootstrap] Checking for updater executable...\n";
        std::string appDataDir = UpdaterCommon::GetAppDataDir();
        if (!UpdaterCommon::EnsureDirExists(appDataDir)) {
            std::cout << "[Bootstrap] Failed to ensure app data directory exists: " << appDataDir << "\n";
            return;
        }

        std::string updaterPath = appDataDir + "OmniStatsUpdater.exe";
        std::string tempUpdaterPath = appDataDir + "OmniStatsUpdater_new.exe";

        std::string serverUrl = ActiveUpdateServerUrl();
        if (serverUrl.empty()) {
            std::cout << "[Bootstrap] Update server URL is empty. Skipping bootstrap.\n";
            return;
        }

        // Step 1: Check server version
        std::string versionUrl = serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr));
        std::string serverVersion = UpdaterCommon::DownloadString(versionUrl, 5); // 5s timeout
        if (serverVersion.empty()) {
            std::cout << "[Bootstrap] Failed to fetch server version for updater bootstrap. Skipping.\n";
            return;
        }

        // Step 2: Check local version
        bool needsDownload = false;
        DWORD attrib = GetFileAttributesA(updaterPath.c_str());
        if (attrib == INVALID_FILE_ATTRIBUTES || (attrib & FILE_ATTRIBUTE_DIRECTORY)) {
            std::cout << "[Bootstrap] OmniStatsUpdater.exe is missing.\n";
            needsDownload = true;
        } else {
            std::string localVersion = UpdaterCommon::GetFileVersion(updaterPath);
            std::cout << "[Bootstrap] Local OmniStatsUpdater.exe version: " << localVersion << ", Server: " << serverVersion << "\n";
            if (UpdaterCommon::IsNewerVersion(localVersion, serverVersion)) {
                needsDownload = true;
            }
        }

        if (!needsDownload) {
            std::cout << "[Bootstrap] Updater is up to date.\n";
            return;
        }

        std::cout << "[Bootstrap] Downloading OmniStatsUpdater.exe...\n";
        std::string updaterUrl = serverUrl + "/OmniStatsUpdater.exe?t=" + std::to_string(std::time(nullptr));
        if (!UpdaterCommon::DownloadFile(updaterUrl, tempUpdaterPath, 30)) { // 30s timeout
            std::cout << "[Bootstrap] Failed to download OmniStatsUpdater.exe.\n";
            return;
        }

        std::cout << "[Bootstrap] Downloading OmniStatsUpdater.exe.sha256...\n";
        std::string shaUrl = serverUrl + "/OmniStatsUpdater.exe.sha256?t=" + std::to_string(std::time(nullptr));
        std::string expectedSha = UpdaterCommon::DownloadString(shaUrl, 5);
        if (expectedSha.empty()) {
            std::cout << "[Bootstrap] Failed to download updater checksum. Cleaning up.\n";
            DeleteFileA(tempUpdaterPath.c_str());
            return;
        }

        if (!UpdaterCommon::VerifyFileSHA256(tempUpdaterPath, expectedSha)) {
            std::cout << "[Bootstrap] SHA-256 verification failed for downloaded updater. Cleaning up.\n";
            DeleteFileA(tempUpdaterPath.c_str());
            return;
        }

        std::cout << "[Bootstrap] Checksum verification passed. Installing OmniStatsUpdater.exe...\n";
        // Replace existing updater
        DeleteFileA(updaterPath.c_str());
        if (!MoveFileExA(tempUpdaterPath.c_str(), updaterPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            std::cout << "[Bootstrap] Failed to rename temp updater to final destination. Error: " << GetLastError() << "\n";
            DeleteFileA(tempUpdaterPath.c_str());
            return;
        }

        std::cout << "[Bootstrap] Updater bootstrap succeeded!\n";
    }

    bool HandleCommandLineFlags(int argc, char* argv[], int& exitCode) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];

            if (arg == "--test-updater-server" && i + 1 < argc) {
                std::string serverUrl = argv[++i];
                SetUpdateServerOverride(serverUrl);
                continue;
            }

            if (arg == "--test-update") {
                char currentExePath[MAX_PATH];
                if (GetModuleFileNameA(NULL, currentExePath, MAX_PATH) != 0) {
                    std::string exeStr(currentExePath);
                    size_t lastSlash = exeStr.find_last_of("\\/");
                    std::string dirPath = (lastSlash != std::string::npos) ? exeStr.substr(0, lastSlash) : ".";
                    std::string newExePath = dirPath + "\\OmniStats_new.exe";

                    std::cout << "[Test] Simulating update process locally...\n";
                    if (CopyFileA(currentExePath, newExePath.c_str(), FALSE)) {
                        STARTUPINFOA si = {sizeof(si)};
                        PROCESS_INFORMATION pi;
                        ZeroMemory(&si, sizeof(si));
                        si.cb = sizeof(si);
                        ZeroMemory(&pi, sizeof(pi));

                        std::string cmdLine = "\"" + newExePath + "\" --apply-update \"" +
                                              std::string(currentExePath) + "\" " +
                                              std::to_string(GetCurrentProcessId());
                        std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
                        cmdLineBuf.push_back('\0');

                        if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0,
                                           NULL, dirPath.c_str(), &si, &pi)) {
                            CloseHandle(pi.hProcess);
                            CloseHandle(pi.hThread);
                            std::cout << "[Test] Child updater spawned. Exiting old parent instance.\n";
                            exitCode = 0;
                            return true;
                        }
                    }
                }
                std::cout << "[Test] Failed to copy self or spawn child updater.\n";
                exitCode = 1;
                return true;
            }

            if (arg == "--apply-update" && i + 1 < argc) {
                std::string targetPath = argv[i + 1];
                DWORD parentProcessId = 0;
                if (i + 2 < argc) {
                    try {
                        parentProcessId = static_cast<DWORD>(std::stoul(argv[i + 2]));
                    } catch (...) {
                        parentProcessId = 0;
                    }
                }
                std::cout << "[Installer] Installation process started for target: " << targetPath << "\n";

                if (parentProcessId != 0) {
                    if (!UpdaterCommon::WaitForProcessExit(parentProcessId, 10000) &&
                        UpdaterCommon::ProcessImageMatches(parentProcessId, targetPath)) {
                        std::cout << "[Installer] Parent process did not exit. Terminating stale parent.\n";
                        UpdaterCommon::TerminateProcessById(parentProcessId);
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                }

                char currentExePath[MAX_PATH];
                GetModuleFileNameA(NULL, currentExePath, MAX_PATH);

                bool success = false;
                DWORD lastCopyError = ERROR_SUCCESS;
                bool terminatedLockedTarget = false;
                for (int attempt = 0; attempt < 30; ++attempt) {
                    if (CopyFileA(currentExePath, targetPath.c_str(), FALSE)) {
                        success = true;
                        break;
                    }
                    lastCopyError = GetLastError();
                    if (!terminatedLockedTarget && attempt >= 10 &&
                        (lastCopyError == ERROR_SHARING_VIOLATION || lastCopyError == ERROR_ACCESS_DENIED)) {
                        std::cout << "[Installer] Target remained locked. Terminating stale target process.\n";
                        terminatedLockedTarget = UpdaterCommon::TerminateProcessesRunningFromPath(targetPath);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                if (success) {
                    std::cout << "[Installer] Copy succeeded! Spawning newly updated program...\n";
                    STARTUPINFOA si = {sizeof(si)};
                    PROCESS_INFORMATION pi;
                    ZeroMemory(&si, sizeof(si));
                    si.cb = sizeof(si);
                    ZeroMemory(&pi, sizeof(pi));

                    std::string cmdLine = "\"" + targetPath + "\" --updated";
                    std::vector<char> cmdLineBuf(cmdLine.begin(), cmdLine.end());
                    cmdLineBuf.push_back('\0');

                    size_t lastSlash = targetPath.find_last_of("\\/");
                    std::string dirPath = (lastSlash != std::string::npos) ? targetPath.substr(0, lastSlash) : ".";

                    if (CreateProcessA(NULL, cmdLineBuf.data(), NULL, NULL, FALSE, 0, NULL,
                                       dirPath.c_str(), &si, &pi)) {
                        CloseHandle(pi.hProcess);
                        CloseHandle(pi.hThread);
                    } else {
                        std::cout << "[Installer] Failed to spawn updated program. Error: " << GetLastError() << "\n";
                    }
                } else {
                    std::cout << "[Installer] Copy failed after 30 attempts. Error: " << lastCopyError << "\n";
                }
                exitCode = 0;
                return true;
            }
        }
        return false;
    }
}
