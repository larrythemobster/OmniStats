#include "StatsApiConfig.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <windows.h>
#include "nlohmann/json.hpp"

namespace StatsApiConfig {

    static std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, last - first + 1);
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

    std::string DetectConfigPath() {
        // 1. Try Steam Uninstall Registry Key
        std::string steamInstall = GetUninstallInstallLocation("252950");
        if (!steamInstall.empty()) {
            std::string fullPath = steamInstall + "\\TAGame\\Config\\DefaultStatsAPI.ini";
            if (std::filesystem::exists(fullPath)) {
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
            if (std::filesystem::exists(libFoldersPath)) {
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
                                if (std::filesystem::exists(fullPath)) {
                                    return fullPath;
                                }
                            }
                        }
                    }
                }
            }
        }

        // 3. Try Epic Launcher Manifests
        std::string programData = "";
        char* pBuf = nullptr;
        size_t len = 0;
        if (_dupenv_s(&pBuf, &len, "ProgramData") == 0 && pBuf != nullptr) {
            programData = pBuf;
            free(pBuf);
        }
        if (programData.empty()) {
            programData = "C:\\ProgramData";
        }
        std::string manifestsPath = programData + "\\Epic\\EpicGamesLauncher\\Data\\Manifests";
        if (std::filesystem::exists(manifestsPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(manifestsPath)) {
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
                            if (std::filesystem::exists(fullPath)) {
                                return fullPath;
                            }
                        }
                    } catch (...) {
                    }
                }
            }
        }

        // 4. Fallbacks
        std::vector<std::string> fallbacks = {
            "C:\\Program Files (x86)\\Steam\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "C:\\Program Files\\Epic Games\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "C:\\Program Files\\Steam\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "D:\\SteamLibrary\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "E:\\SteamLibrary\\steamapps\\common\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "D:\\Games\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini",
            "E:\\Games\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini"};

        for (const auto& path : fallbacks) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }

        return "";
    }

    CheckResult VerifyConfig(const std::string& filePath, int expectedPort) {
        CheckResult result;
        result.path = filePath;
        result.expectedPort = expectedPort;

        if (filePath.empty()) {
            result.status = Status::NotFound;
            result.message = "Stats API config file path is empty.";
            return result;
        }

        if (!std::filesystem::exists(filePath)) {
            result.status = Status::NotFound;
            result.message = "Stats API config file not found.";
            return result;
        }

        std::ifstream file(filePath);
        if (!file.is_open()) {
            result.status = Status::ReadError;
            result.message = "Could not open Stats API config file for reading.";
            return result;
        }

        bool hasPacketSendRate = false;
        bool hasPort = false;

        std::string line;
        while (std::getline(file, line)) {
            std::string trimmed = Trim(line);
            if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
                continue;
            }
            if (trimmed[0] == '[' && trimmed[trimmed.length() - 1] == ']') {
                continue;
            }

            size_t eq = trimmed.find('=');
            if (eq != std::string::npos) {
                std::string key = Trim(trimmed.substr(0, eq));
                std::string val = Trim(trimmed.substr(eq + 1));

                std::string keyLower = key;
                std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);

                if (keyLower == "packetsendrate") {
                    try {
                        result.packetSendRate = std::stof(val);
                        hasPacketSendRate = true;
                    } catch (...) {
                    }
                } else if (keyLower == "port") {
                    try {
                        result.actualPort = std::stoi(val);
                        hasPort = true;
                    } catch (...) {
                    }
                }
            }
        }
        file.close();

        // Check if Rocket League window exists
        HWND rlHwnd = ::FindWindowA("LaunchUnrealUWindowsClient", nullptr);
        if (!rlHwnd) rlHwnd = ::FindWindowA(nullptr, "Rocket League (64-bit, DX11)");
        if (!rlHwnd) rlHwnd = ::FindWindowA(nullptr, "Rocket League (32-bit, DX11)");
        if (!rlHwnd) rlHwnd = ::FindWindowA(nullptr, "Rocket League");
        result.rlRunning = (rlHwnd != nullptr);

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
