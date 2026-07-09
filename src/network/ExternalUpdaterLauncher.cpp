#include "ExternalUpdaterLauncher.hpp"
#include "core/SessionState.hpp"
#include "core/Storage.hpp"
#include "core/FileHash.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>
#include <curl/curl.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace {

constexpr const char* kUpdaterExeName = "OmniStatsUpdater.exe";
constexpr const char* kUpdaterMissingMessage = "Updater is not installed. Please reinstall OmniStats.";
constexpr UINT kUpdaterMessageBoxFlags = MB_OK | MB_ICONWARNING | MB_SETFOREGROUND;

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

bool FindUpdaterExecutable(std::string& updaterPath) {
    const std::string currentExePath = GetCurrentExecutablePath();
    if (!currentExePath.empty()) {
        const std::string candidate = GetDirectoryForPath(currentExePath) + "\\" + kUpdaterExeName;
        if (FileExists(candidate)) {
            updaterPath = candidate;
            return true;
        }
    }

    const std::string stagedCandidate = Storage::GetDataDirectory() + kUpdaterExeName;
    if (FileExists(stagedCandidate)) {
        updaterPath = stagedCandidate;
        return true;
    }

    updaterPath.clear();
    return false;
}

void ShowUpdaterMissingMessage() {
    MessageBoxA(NULL, kUpdaterMissingMessage, "OmniStats Update", kUpdaterMessageBoxFlags);
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
        std::cout << "[UpdaterLauncher] Failed to launch updater. Error: " << GetLastError() << "\n";
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

    const std::string arguments = "--update-app \"" + currentExePath + "\" " + std::to_string(GetCurrentProcessId());
    return LaunchUpdater(arguments);
}

size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::ofstream* out = static_cast<std::ofstream*>(userp);
    out->write(static_cast<char*>(contents), total);
    return total;
}

std::string Trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

std::string DownloadString(const std::string& url, long timeoutSecs = 15) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSecs);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        return "";
    }
    return Trim(response);
}

bool DownloadFile(const std::string& url, const std::string& outputPath, long timeoutSecs = 60) {
    try {
        std::filesystem::path outPath(outputPath);
        if (outPath.has_parent_path()) {
            std::filesystem::create_directories(outPath.parent_path());
        }
    } catch (...) {
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        outFile.close();
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outFile);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSecs);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    outFile.close();
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        DeleteFileA(outputPath.c_str());
        return false;
    }
    return true;
}

std::string GetFileVersion(const std::string& filePath) {
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeA(filePath.c_str(), &dummy);
    if (size == 0) {
        return "";
    }
    std::vector<char> buffer(size);
    if (!GetFileVersionInfoA(filePath.c_str(), 0, size, buffer.data())) {
        return "";
    }
    VS_FIXEDFILEINFO* fileInfo = nullptr;
    UINT len = 0;
    if (!VerQueryValueA(buffer.data(), "\\", reinterpret_cast<LPVOID*>(&fileInfo), &len) || len == 0) {
        return "";
    }
    std::stringstream ss;
    ss << ((fileInfo->dwFileVersionMS >> 16) & 0xffff) << "."
       << (fileInfo->dwFileVersionMS & 0xffff) << "."
       << ((fileInfo->dwFileVersionLS >> 16) & 0xffff);
    return ss.str();
}

bool IsNewerVersion(const std::string& current, const std::string& latest) {
    std::vector<int> curParts, latParts;
    std::stringstream curSS(current), latSS(latest);
    std::string item;
    
    while (std::getline(curSS, item, '.')) {
        try {
            curParts.push_back(std::stoi(item));
        } catch (...) {
            curParts.push_back(0);
        }
    }
    while (std::getline(latSS, item, '.')) {
        try {
            latParts.push_back(std::stoi(item));
        } catch (...) {
            latParts.push_back(0);
        }
    }
    
    while (curParts.size() < 3) {
        curParts.push_back(0);
    }
    while (latParts.size() < 3) {
        latParts.push_back(0);
    }
    
    for (size_t i = 0; i < 3; ++i) {
        if (latParts[i] > curParts[i]) {
            return true;
        }
        if (latParts[i] < curParts[i]) {
            return false;
        }
    }
    return false;
}

bool VerifyFileSHA256(const std::string& filePath, const std::string& expectedHash) {
    std::string localHash = CalculateSHA256(filePath);
    if (localHash.empty()) {
        return false;
    }
    std::string cleanExpected = Trim(expectedHash);
    if (cleanExpected.length() >= 64) {
        cleanExpected = cleanExpected.substr(0, 64);
    }
    std::transform(cleanExpected.begin(), cleanExpected.end(), cleanExpected.begin(), ::tolower);
    std::transform(localHash.begin(), localHash.end(), localHash.begin(), ::tolower);
    return localHash == cleanExpected;
}

bool RefreshUpdaterExecutable(const std::string& updaterPath) {
    if (updaterPath.empty()) {
        return false;
    }

    const std::string serverUrl = "https://omnistats.org";
    const std::string updaterUrl = serverUrl + "/OmniStatsUpdater.exe?t=" + std::to_string(std::time(nullptr));
    const std::string shaUrl = serverUrl + "/OmniStatsUpdater.exe.sha256?t=" + std::to_string(std::time(nullptr));

    const std::string tempUpdaterPath = updaterPath + ".download";
    
    // Download to temp file
    if (!DownloadFile(updaterUrl, tempUpdaterPath, 60)) {
        std::cout << "[UpdaterLauncher] Failed to download refreshed updater to temp path: " << tempUpdaterPath << "\n";
        return false;
    }

    // Verify SHA-256
    std::string expectedSha = DownloadString(shaUrl, 10);
    if (expectedSha.empty()) {
        std::cout << "[UpdaterLauncher] Failed to download updater SHA-256. Bypassing replacement for safety.\n";
        DeleteFileA(tempUpdaterPath.c_str());
        return false;
    }

    if (!VerifyFileSHA256(tempUpdaterPath, expectedSha)) {
        std::cout << "[UpdaterLauncher] Refreshed updater SHA-256 verification failed.\n";
        DeleteFileA(tempUpdaterPath.c_str());
        return false;
    }

    // Try to replace the existing updater with retry loop (in case it is locked)
    bool replaced = false;
    for (int retry = 0; retry < 10; ++retry) {
        if (MoveFileExA(tempUpdaterPath.c_str(), updaterPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            replaced = true;
            break;
        }
        std::cout << "[UpdaterLauncher] Updater locked, retrying replacement in 500ms (attempt " << (retry + 1) << ")...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!replaced) {
        std::cout << "[UpdaterLauncher] Failed to replace updater after retries.\n";
        DeleteFileA(tempUpdaterPath.c_str());
        return false;
    }

    std::cout << "[UpdaterLauncher] Updater successfully refreshed!\n";
    return true;
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
    const std::string currentExePath = GetCurrentExecutablePath();
    std::string currentAppVersion = GetFileVersion(currentExePath);
    if (currentAppVersion.empty()) {
        currentAppVersion = OMNISTATS_VERSION;
    }

    const std::string serverUrl = "https://omnistats.org";
    std::string serverVersion = DownloadString(serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr)), 10);

    if (serverVersion.empty() || !IsNewerVersion(currentAppVersion, serverVersion)) {
        return false;
    }

    // Update is available!
    std::string updaterPath;
    if (!FindUpdaterExecutable(updaterPath)) {
        if (!currentExePath.empty()) {
            updaterPath = GetDirectoryForPath(currentExePath) + "\\" + kUpdaterExeName;
        } else {
            updaterPath = Storage::GetDataDirectory() + kUpdaterExeName;
        }
    }

    // Always attempt refresh with hash verification before launching --update-app
    if (!RefreshUpdaterExecutable(updaterPath)) {
        std::cout << "[UpdaterLauncher] Warning: Failed to refresh updater executable. Continuing with existing one.\n";
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
        const std::string currentExePath = GetCurrentExecutablePath();
        std::string currentAppVersion = GetFileVersion(currentExePath);
        if (currentAppVersion.empty()) {
            currentAppVersion = OMNISTATS_VERSION;
        }

        const std::string serverUrl = "https://omnistats.org";
        std::string serverVersion = DownloadString(serverUrl + "/version.txt?t=" + std::to_string(std::time(nullptr)), 10);

        if (!serverVersion.empty() && IsNewerVersion(currentAppVersion, serverVersion)) {
            {
                std::lock_guard<std::mutex> lock(state->ui.updateMutex);
                state->ui.updateAvailableVersion = serverVersion;
                state->ui.updateServerUrl = serverUrl;
            }
            state->ui.updateAvailable.store(true);
        }
        state->ui.updateChecked.store(true);
        std::cout << "[UpdaterLauncher] Background update check finished. Server version: " << serverVersion << "\n";
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

    std::string updaterPath;
    if (!FindUpdaterExecutable(updaterPath)) {
        const std::string currentExePath = GetCurrentExecutablePath();
        if (!currentExePath.empty()) {
            updaterPath = GetDirectoryForPath(currentExePath) + "\\" + kUpdaterExeName;
        } else {
            updaterPath = Storage::GetDataDirectory() + kUpdaterExeName;
        }
    }

    // Always attempt refresh with hash verification before launching --update-app
    if (!RefreshUpdaterExecutable(updaterPath)) {
        std::cout << "[UpdaterLauncher] Warning: Failed to refresh updater executable. Continuing with existing one.\n";
    }

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
