#include "core/config/StatsApiConfig.hpp"
#include "core/config/StatsApiIni.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include "nlohmann/json.hpp"

namespace StatsApiConfig {

    static bool PathExists(const std::filesystem::path& path) {
        std::error_code error;
        return std::filesystem::exists(path, error);
    }

    std::string GetStatusMessage(Status status) {
        switch (status) {
        case Status::Valid:
            return "Valid";
        case Status::NotFound:
            return "Config file not found.";
        case Status::DisabledPacketSendRate:
            return "PacketSendRate is 0 or disabled.";
        case Status::WrongPacketSendRate:
            return "PacketSendRate is not set to 30.";
        case Status::MissingPacketSendRate:
            return "PacketSendRate is missing.";
        case Status::MissingPort:
            return "Port is missing.";
        case Status::WrongPort:
            return "Port is misconfigured.";
        case Status::ReadError:
            return "Read error: could not open config file.";
        case Status::WriteError:
            return "Write error: permission denied or file locked.";
        }
        return "Unknown status.";
    }

    static std::string GetUninstallInstallLocation(const std::string& appId) {
        std::string subKey = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App " + appId;
        HKEY hKey;
        char szValue[512];
        DWORD dwSize = sizeof(szValue);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallLocation", nullptr, nullptr, (LPBYTE)szValue, &dwSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                return szValue;
            }
            RegCloseKey(hKey);
        }
        dwSize = sizeof(szValue);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subKey.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "InstallLocation", nullptr, nullptr, (LPBYTE)szValue, &dwSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                return szValue;
            }
            RegCloseKey(hKey);
        }
        return "";
    }

    static std::string EnvVar(const char* name) {
        std::string value;
        char* buf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&buf, &len, name) == 0 && buf != nullptr) {
            value = buf;
            free(buf);
        }
        return value;
    }

    std::string FindConfigPathInLegendaryInstalled(const std::filesystem::path& installedJson) {
        std::ifstream file(installedJson);
        if (!file.is_open()) return "";
        try {
            const nlohmann::json installed = nlohmann::json::parse(file);
            if (!installed.is_object()) return "";
            for (const auto& [appName, game] : installed.items()) {
                if (appName != "Sugar" || !game.is_object()) continue;
                const auto installPath = game.find("install_path");
                if (installPath == game.end() || !installPath->is_string()) continue;
                const std::string utf8 = installPath->get<std::string>();
                const std::filesystem::path iniPath = std::filesystem::path(std::u8string(utf8.begin(), utf8.end())) / "TAGame" / "Config" / "DefaultStatsAPI.ini";
                if (PathExists(iniPath)) return iniPath.string();
            }
        } catch (...) {
        }
        return "";
    }

    static HWND FindRocketLeagueWindow() {
        HWND rlHwnd = ::FindWindowW(L"LaunchUnrealUWindowsClient", nullptr);
        if (!rlHwnd) rlHwnd = ::FindWindowW(nullptr, L"Rocket League (64-bit, DX11)");
        if (!rlHwnd) rlHwnd = ::FindWindowW(nullptr, L"Rocket League (32-bit, DX11)");
        if (!rlHwnd) rlHwnd = ::FindWindowW(nullptr, L"Rocket League");
        return rlHwnd;
    }

    bool IsRocketLeagueRunning() {
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe32;
            pe32.dwSize = sizeof(pe32);
            if (Process32FirstW(hSnap, &pe32)) {
                do {
                    if (_wcsicmp(pe32.szExeFile, L"RocketLeague.exe") == 0) {
                        CloseHandle(hSnap);
                        return true;
                    }
                } while (Process32NextW(hSnap, &pe32));
            }
            CloseHandle(hSnap);
        }

        return FindRocketLeagueWindow() != nullptr;
    }

    std::string FindConfigPathFromExecutable(const std::filesystem::path& exePath) {
        if (exePath.empty()) {
            return "";
        }

        std::error_code ec;
        std::filesystem::path current;
        try {
            current = exePath.parent_path();
        } catch (...) {
            return "";
        }

        std::filesystem::path candidate;

        for (int i = 0; i < 5 && !current.empty(); ++i) {
            std::filesystem::path iniPath = current / "TAGame" / "Config" / "DefaultStatsAPI.ini";
            if (std::filesystem::exists(iniPath, ec) && !std::filesystem::is_directory(iniPath, ec)) {
                return iniPath.string();
            }

            if (candidate.empty()) {
                std::filesystem::path configDir = current / "TAGame" / "Config";
                if (std::filesystem::is_directory(configDir, ec)) {
                    candidate = iniPath;
                } else {
                    std::filesystem::path tagameDir = current / "TAGame";
                    if (std::filesystem::is_directory(tagameDir, ec)) {
                        candidate = iniPath;
                    }
                }
            }

            std::filesystem::path parent = current.parent_path();
            if (parent == current) {
                break;
            }
            current = parent;
        }

        if (!candidate.empty()) {
            return candidate.string();
        }

        return "";
    }

    static std::string QueryProcessPath(DWORD pid) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) {
            hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        }
        if (!hProc) {
            return "";
        }

        wchar_t pathBuf[MAX_PATH * 4] = {0};
        DWORD size = static_cast<DWORD>(std::size(pathBuf));
        if (!QueryFullProcessImageNameW(hProc, 0, pathBuf, &size) || size == 0) {
            size = GetModuleFileNameExW(hProc, NULL, pathBuf, static_cast<DWORD>(std::size(pathBuf)));
        }
        CloseHandle(hProc);

        if (size > 0 && pathBuf[0] != L'\0') {
            std::error_code ec;
            std::filesystem::path p(pathBuf);
            if (std::filesystem::exists(p, ec)) {
                return p.string();
            }
        }
        return "";
    }

    std::string DetectConfigPathFromRunningProcess() {
        std::string candidatePath;

        // 1. Enumerate processes via Toolhelp snapshot
        HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe32;
            pe32.dwSize = sizeof(pe32);
            if (Process32FirstW(hSnap, &pe32)) {
                do {
                    if (_wcsicmp(pe32.szExeFile, L"RocketLeague.exe") == 0) {
                        std::string exePath = QueryProcessPath(pe32.th32ProcessID);
                        if (!exePath.empty()) {
                            std::string found = FindConfigPathFromExecutable(exePath);
                            if (!found.empty()) {
                                std::error_code ec;
                                if (std::filesystem::exists(found, ec)) {
                                    CloseHandle(hSnap);
                                    return found;
                                }
                                if (candidatePath.empty()) {
                                    candidatePath = found;
                                }
                            }
                        }
                    }
                } while (Process32NextW(hSnap, &pe32));
            }
            CloseHandle(hSnap);
        }

        // 2. Fallback: Window lookup
        HWND rlHwnd = FindRocketLeagueWindow();
        if (rlHwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(rlHwnd, &pid);
            if (pid != 0) {
                std::string exePath = QueryProcessPath(pid);
                if (!exePath.empty()) {
                    std::string found = FindConfigPathFromExecutable(exePath);
                    if (!found.empty()) {
                        std::error_code ec;
                        if (std::filesystem::exists(found, ec)) {
                            return found;
                        }
                        if (candidatePath.empty()) {
                            candidatePath = found;
                        }
                    }
                }
            }
        }

        if (!candidatePath.empty()) {
            return candidatePath;
        }

        return "";
    }

    std::string DetectConfigPath() {
        std::error_code error;
        // 0. Try currently running Rocket League process first
        std::string runningPath = DetectConfigPathFromRunningProcess();
        if (!runningPath.empty()) {
            return runningPath;
        }

        // 1. Try Steam Uninstall Registry Key
        std::string steamInstall = GetUninstallInstallLocation("252950");
        if (!steamInstall.empty()) {
            std::string fullPath = steamInstall + "\\TAGame\\Config\\DefaultStatsAPI.ini";
            if (PathExists(fullPath)) {
                return fullPath;
            }
        }

        // 2. Try Steam Path Registry Key + libraryfolders.vdf
        std::string steamPath = "";
        HKEY hKey;
        char szValue[512];
        DWORD dwSize = sizeof(szValue);
        if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            if (RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr, (LPBYTE)szValue, &dwSize) == ERROR_SUCCESS) {
                steamPath = szValue;
            }
            RegCloseKey(hKey);
        }
        if (steamPath.empty()) {
            dwSize = sizeof(szValue);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", 0, KEY_READ | KEY_WOW64_32KEY, &hKey) == ERROR_SUCCESS) {
                if (RegQueryValueExA(hKey, "InstallPath", nullptr, nullptr, (LPBYTE)szValue, &dwSize) == ERROR_SUCCESS) {
                    steamPath = szValue;
                }
                RegCloseKey(hKey);
            }
        }

        if (!steamPath.empty()) {
            std::string libFoldersPath = steamPath + "\\steamapps\\libraryfolders.vdf";
            if (PathExists(libFoldersPath)) {
                std::ifstream file(libFoldersPath);
                std::string line;
                while (std::getline(file, line)) {
                    size_t p = line.find("\"path\"");
                    if (p != std::string::npos) {
                        size_t firstQuote = line.find('"', p + 6);
                        if (firstQuote != std::string::npos) {
                            size_t secondQuote = line.find('"', firstQuote + 1);
                            if (secondQuote != std::string::npos) {
                                std::string pathVal = line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                                std::string cleanPath = "";
                                for (size_t k = 0; k < pathVal.length(); k++) {
                                    if (pathVal[k] == '\\' && k + 1 < pathVal.length() && pathVal[k + 1] == '\\') {
                                        cleanPath += '\\';
                                        k++;
                                    } else {
                                        cleanPath += pathVal[k];
                                    }
                                }
                                std::string fullPath = cleanPath + "\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini";
                                if (PathExists(fullPath)) {
                                    return fullPath;
                                }
                            }
                        }
                    }
                }
            }
        }

        // 3. Try Epic Launcher Manifests
        std::string programData = EnvVar("ProgramData");
        if (programData.empty()) programData = "C:\\ProgramData";
        std::string manifestsPath = programData + "\\Epic\\EpicGamesLauncher\\Data\\Manifests";
        if (PathExists(manifestsPath)) {
            for (auto it = std::filesystem::directory_iterator(manifestsPath, error);
                 !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
                const auto& entry = *it;
                if (entry.path().extension() == ".item") {
                    try {
                        std::ifstream file(entry.path());
                        nlohmann::json j;
                        file >> j;
                        bool isRL = false;
                        if (j.contains("MandatoryAppFolderName") && j["MandatoryAppFolderName"] == "rocketleague") {
                            isRL = true;
                        } else if (j.contains("AppName") && j["AppName"] == "Sugar") {
                            isRL = true;
                        }
                        if (isRL && j.contains("InstallLocation")) {
                            std::string loc = j["InstallLocation"];
                            std::string fullPath = loc + "\\TAGame\\Config\\DefaultStatsAPI.ini";
                            if (PathExists(fullPath)) {
                                return fullPath;
                            }
                        }
                    } catch (...) {
                    }
                }
            }
        }

        // 4. Heroic Games Launcher and standalone Legendary
        const std::string appData = EnvVar("APPDATA");
        const std::string userProfile = EnvVar("USERPROFILE");
        std::vector<std::filesystem::path> legendaryManifests;
        if (!appData.empty()) legendaryManifests.push_back(std::filesystem::path(appData) / "heroic" / "legendaryConfig" / "legendary" / "installed.json");
        if (!userProfile.empty()) legendaryManifests.push_back(std::filesystem::path(userProfile) / ".config" / "legendary" / "installed.json");
        for (const auto& manifest : legendaryManifests) {
            std::string found = FindConfigPathInLegendaryInstalled(manifest);
            if (!found.empty()) return found;
        }

        // 5. Fallbacks
        std::vector<std::string> fallbacks = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "C:\\Program Files\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "C:\\Program Files\\Steam\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "D:\\SteamLibrary\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "E:\\SteamLibrary\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "D:\\Games\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "E:\\Games\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini"};
        if (!userProfile.empty()) fallbacks.push_back(userProfile + "\\Games\\Heroic\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini");

        for (const auto& path : fallbacks) {
            if (PathExists(path)) {
                return path;
            }
        }

        return "";
    }

    CheckResult VerifyConfig(const std::string& requestedPath, int expectedPort) {
        CheckResult result;
        result.expectedPort = expectedPort;
        result.rlRunning = IsRocketLeagueRunning();
        if (requestedPath.empty()) {
            result.status = Status::NotFound;
            result.message = "Stats API config file path is empty.";
            return result;
        }
        const std::string filePath = StatsApiIni::EffectiveConfigPath(std::filesystem::path(requestedPath)).string();
        result.path = filePath;

        if (!PathExists(filePath)) {
            result.status = Status::NotFound;
            result.message = "Stats API config file not found.";
            return result;
        }

        std::ifstream file{std::filesystem::path(filePath)};
        if (!file.is_open()) {
            result.status = Status::ReadError;
            result.message = "Could not open Stats API config file for reading.";
            return result;
        }
        std::vector<std::string> lines;
        for (std::string line; std::getline(file, line);)
            lines.push_back(std::move(line));
        file.close();

        const StatsApiIni::ExporterValues values = StatsApiIni::ReadExporterValues(lines);
        const bool hasPacketSendRate = values.packetSendRate.has_value();
        const bool hasPort = values.port.has_value();
        if (hasPacketSendRate) result.packetSendRate = *values.packetSendRate;
        if (hasPort) result.actualPort = *values.port;

        if (!hasPacketSendRate) {
            result.status = Status::MissingPacketSendRate;
            result.message = "PacketSendRate setting is missing in configuration.";
        } else if (result.packetSendRate <= 0.0f) {
            result.status = Status::DisabledPacketSendRate;
            result.message = "PacketSendRate is set to 0 (disabled).";
        } else if (result.packetSendRate != 30.0f) {
            result.status = Status::WrongPacketSendRate;
            result.message = "PacketSendRate is set to " + std::to_string(result.packetSendRate) + " (expected 30).";
        } else if (!hasPort) {
            result.status = Status::MissingPort;
            result.message = "Port setting is missing in configuration.";
        } else if (result.actualPort != expectedPort) {
            result.status = Status::WrongPort;
            result.message = "Port is misconfigured (found " + std::to_string(result.actualPort) + ", expected " + std::to_string(expectedPort) + ").";
        } else {
            result.status = Status::Valid;
            result.message = "Stats API is correctly configured.";
        }

        return result;
    }

} // namespace StatsApiConfig
