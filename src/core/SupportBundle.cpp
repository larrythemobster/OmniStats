#include "core/SupportBundle.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#ifdef small
#undef small
#endif

#include <sqlite3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "core/StatsApiConfig.hpp"
#include "core/Storage.hpp"
#include "core/ZipWriter.hpp"
#include "network/RemoteConfig.hpp"

namespace SupportBundle {

    namespace {

        const std::unordered_set<std::string> kSecretConfigKeys = {
            "ballchasing_token",
            "custom_api_key",
            "pro_api_key",
            "account_device_key",
            "account_device_public_id",
            "account_refresh_token",
            "account_access_token",
            "account_signed_in_name",
            "client_uuid",
            "last_primary_id",
            "known_primary_ids"};

        bool IsDpapiHexBlob(const std::string& str) {
            if (str.size() < 32 || (str.size() % 2) != 0) {
                return false;
            }
            for (char c : str) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }
            std::string bytes;
            bytes.reserve(str.size() / 2);
            for (size_t i = 0; i < str.size(); i += 2) {
                const char pair[3] = {str[i], str[i + 1], '\0'};
                bytes.push_back(static_cast<char>(std::strtoul(pair, nullptr, 16)));
            }

            DATA_BLOB inBlob;
            inBlob.pbData = reinterpret_cast<BYTE*>(bytes.data());
            inBlob.cbData = static_cast<DWORD>(bytes.size());

            DATA_BLOB outBlob{};
            if (CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, 0, &outBlob)) {
                LocalFree(outBlob.pbData);
                return true;
            }
            return false;
        }

        std::string ReadFileShared(const std::filesystem::path& path) {
            fflush(stdout);
            HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) {
                return "";
            }
            LARGE_INTEGER size;
            if (!GetFileSizeEx(hFile, &size) || size.QuadPart <= 0) {
                CloseHandle(hFile);
                return "";
            }

            // Caps memory for oversized logs.
            const DWORD bytesToRead = static_cast<DWORD>(std::min<int64_t>(size.QuadPart, 10 * 1024 * 1024));
            std::string buffer(bytesToRead, '\0');
            DWORD bytesRead = 0;
            ReadFile(hFile, buffer.data(), bytesToRead, &bytesRead, nullptr);
            CloseHandle(hFile);
            buffer.resize(bytesRead);
            return buffer;
        }

        std::string GetTimestampString() {
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            std::ostringstream ss;
            ss << std::put_time(&tm, "%Y%m%d-%H%M%S");
            return ss.str();
        }

        void ReplaceAll(std::string& str, const std::string& from, const std::string& to) {
            if (from.empty()) return;
            size_t startPos = 0;
            while ((startPos = str.find(from, startPos)) != std::string::npos) {
                str.replace(startPos, from.length(), to);
                startPos += to.length();
            }
        }

    } // namespace

    nlohmann::json RedactConfig(const nlohmann::json& config) {
        if (!config.is_object()) {
            return config;
        }
        nlohmann::json redacted = config;

        std::vector<std::string> keysToErase;
        for (auto it = redacted.begin(); it != redacted.end(); ++it) {
            const std::string& key = it.key();

            if (kSecretConfigKeys.contains(key)) {
                keysToErase.push_back(key);
                continue;
            }

            std::string lowerKey = key;
            std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

            if (lowerKey.find("token") != std::string::npos ||
                lowerKey.find("secret") != std::string::npos ||
                lowerKey.find("password") != std::string::npos ||
                lowerKey.find("api_key") != std::string::npos ||
                lowerKey.find("private_key") != std::string::npos) {
                keysToErase.push_back(key);
                continue;
            }

            if (it->is_string()) {
                const std::string val = it->get<std::string>();
                if (IsDpapiHexBlob(val)) {
                    keysToErase.push_back(key);
                }
            }
        }

        for (const auto& key : keysToErase) {
            redacted.erase(key);
        }

        return redacted;
    }

    std::string RedactConfigString(const std::string& jsonText) {
        try {
            nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, true, true);
            return RedactConfig(j).dump(2);
        } catch (...) {
            return "{}";
        }
    }

    std::string RedactLog(const std::string& logText, const std::vector<std::string>& knownSecrets) {
        std::string result = logText;

        for (const auto& secret : knownSecrets) {
            if (secret.size() >= 4) {
                ReplaceAll(result, secret, "[redacted]");
            }
        }

        try {
            const std::regex bearerPattern(R"(Bearer\s+[A-Za-z0-9\-._~+/]+=*)");
            result = std::regex_replace(result, bearerPattern, "Bearer [redacted]");
        } catch (...) {
        }

        try {
            const std::regex apiKeyPattern(R"((?:X-API-Key|x-api-key|X-Api-Key)\s*[:=]\s*[^\r\n\s]+)");
            result = std::regex_replace(result, apiKeyPattern, "X-API-Key: [redacted]");
        } catch (...) {
        }

        try {
            const std::regex clientUuidPattern(R"((?:client_uuid|Client_UUID|clientUuid)\s*[:=]\s*["']?([0-9a-fA-F-]{32,36})["']?)");
            result = std::regex_replace(result, clientUuidPattern, "client_uuid: [redacted]");
        } catch (...) {
        }

        try {
            const std::regex authPattern(R"((Authorization\s*:\s*)(?:Basic\s+[A-Za-z0-9+/=]+))");
            result = std::regex_replace(result, authPattern, "$1[redacted]");
        } catch (...) {
        }

        return result;
    }

    std::string GetDatabaseSummary(const std::filesystem::path& dbPath) {
        std::error_code ec;
        if (!std::filesystem::exists(dbPath, ec)) {
            return "Database file not found: " + dbPath.string() + "\n";
        }

        sqlite3* db = nullptr;
        const int rc = sqlite3_open_v2(dbPath.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK || !db) {
            const std::string msg = db ? sqlite3_errmsg(db) : "cannot open";
            if (db) sqlite3_close(db);
            return "Failed to open database read-only: " + msg + "\n";
        }

        int userVersion = 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                userVersion = sqlite3_column_int(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }

        std::vector<std::string> tableNames;
        if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name;", -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                if (name) tableNames.push_back(name);
            }
            sqlite3_finalize(stmt);
        }

        std::ostringstream out;
        out << "Schema Version (user_version): " << userVersion << "\n";
        out << "Tables (" << tableNames.size() << "):\n";
        for (const auto& tbl : tableNames) {
            bool safeName = !tbl.empty();
            for (char c : tbl) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') {
                    safeName = false;
                    break;
                }
            }
            if (!safeName) continue;

            const std::string countSql = "SELECT COUNT(*) FROM \"" + tbl + "\";";
            int64_t rowCount = 0;
            if (sqlite3_prepare_v2(db, countSql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    rowCount = sqlite3_column_int64(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
            out << "  " << tbl << ": " << rowCount << " rows\n";
        }

        sqlite3_close(db);
        return out.str();
    }

    std::string GetWindowsVersionInfo() {
        typedef LONG(NTAPI * RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        std::string verString = "Windows (unknown version)";

        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        if (hNtdll) {
            auto pRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(hNtdll, "RtlGetVersion"));
            if (pRtlGetVersion) {
                RTL_OSVERSIONINFOW rovi{};
                rovi.dwOSVersionInfoSize = sizeof(rovi);
                if (pRtlGetVersion(&rovi) == 0) {
                    std::ostringstream ss;
                    ss << "Windows " << rovi.dwMajorVersion << "." << rovi.dwMinorVersion << " (Build " << rovi.dwBuildNumber << ")";
                    verString = ss.str();
                }
            }
        }

        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t prodName[256] = {0};
            DWORD prodSize = sizeof(prodName);
            if (RegQueryValueExW(hKey, L"ProductName", nullptr, nullptr, reinterpret_cast<LPBYTE>(prodName), &prodSize) == ERROR_SUCCESS) {
                wchar_t dispVer[64] = {0};
                DWORD dispSize = sizeof(dispVer);
                std::string nameUtf8;
                const int nLen = WideCharToMultiByte(CP_UTF8, 0, prodName, -1, nullptr, 0, nullptr, nullptr);
                if (nLen > 0) {
                    nameUtf8.resize(nLen - 1);
                    WideCharToMultiByte(CP_UTF8, 0, prodName, -1, nameUtf8.data(), nLen, nullptr, nullptr);
                }
                if (RegQueryValueExW(hKey, L"DisplayVersion", nullptr, nullptr, reinterpret_cast<LPBYTE>(dispVer), &dispSize) == ERROR_SUCCESS) {
                    std::string verUtf8;
                    const int vLen = WideCharToMultiByte(CP_UTF8, 0, dispVer, -1, nullptr, 0, nullptr, nullptr);
                    if (vLen > 0) {
                        verUtf8.resize(vLen - 1);
                        WideCharToMultiByte(CP_UTF8, 0, dispVer, -1, verUtf8.data(), vLen, nullptr, nullptr);
                    }
                    if (!nameUtf8.empty()) {
                        verString = nameUtf8 + " " + verUtf8 + " (" + verString + ")";
                    }
                } else if (!nameUtf8.empty()) {
                    verString = nameUtf8 + " (" + verString + ")";
                }
            }
            RegCloseKey(hKey);
        }

        return verString;
    }

    std::string BuildDiagnosticsSummary() {
        std::ostringstream out;
        out << "OmniStats Diagnostics Summary\n";
        out << "=============================\n";
        out << "Generated: " << GetTimestampString() << "\n\n";

        out << "[Application]\n";
        out << "Version: " << AppVersion::Current << "\n";
        out << "OS: " << GetWindowsVersionInfo() << "\n\n";

        out << "[Rocket League Connection]\n";
        const ConfigData conf = Config::Read();
        std::string statsPath = conf.rocket_league_stats_api_config_path;
        if (statsPath.empty()) {
            statsPath = StatsApiConfig::DetectConfigPath();
        }
        const auto statsCheck = StatsApiConfig::VerifyConfig(statsPath, conf.port);
        out << "Status: " << StatsApiConfig::GetStatusMessage(statsCheck.status) << "\n";
        out << "Detected Path: " << (statsCheck.path.empty() ? "(none)" : statsCheck.path) << "\n";
        if (!statsCheck.message.empty()) {
            out << "Message: " << statsCheck.message << "\n";
        }
        out << "Expected Port: " << statsCheck.expectedPort << "\n";
        out << "Actual Port: " << statsCheck.actualPort << "\n";
        out << "Packet Send Rate: " << statsCheck.packetSendRate << "\n";
        out << "Rocket League Running: " << (statsCheck.rlRunning ? "Yes" : "No") << "\n\n";

        out << "[Database]\n";
        const std::string dbPath = Storage::GetDataDirectory() + Storage::APP_NAME + ".db";
        out << GetDatabaseSummary(dbPath) << "\n";

        out << "[Remote Config & Announcements]\n";
        const RemoteConfigData remoteSnap = RemoteConfig::Instance().GetSnapshot();
        out << "Schema: " << remoteSnap.schema << "\n";
        out << "TTL Seconds: " << remoteSnap.ttl_seconds << "\n";
        out << "Min Supported Version: " << (remoteSnap.min_supported_version.empty() ? "(none)" : remoteSnap.min_supported_version) << "\n";
        const auto announcements = RemoteConfig::Instance().ActiveAnnouncements();
        out << "Active Announcements: " << announcements.size() << "\n";
        for (const auto& ann : announcements) {
            out << "  - [" << ann.severity << "] " << ann.id << ": " << ann.title << "\n";
        }

        return out.str();
    }

    bool CreateSupportBundle(std::string& outZipPath, std::string& outError) {
        PWSTR desktopPath = nullptr;
        std::filesystem::path targetDir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPath)) && desktopPath) {
            targetDir = desktopPath;
            CoTaskMemFree(desktopPath);
        } else {
            targetDir = Storage::GetDataDirectory();
        }

        std::error_code ec;
        if (!std::filesystem::exists(targetDir, ec)) {
            targetDir = Storage::GetDataDirectory();
        }

        const std::string timestamp = GetTimestampString();
        std::filesystem::path zipPath = targetDir / ("omnistats-support-" + timestamp + ".zip");
        int counter = 1;
        while (std::filesystem::exists(zipPath, ec)) {
            zipPath = targetDir / ("omnistats-support-" + timestamp + "_" + std::to_string(counter++) + ".zip");
        }

        const ConfigData conf = Config::Read();
        std::vector<std::string> secrets;
        if (!conf.ballchasing_token.empty()) secrets.push_back(conf.ballchasing_token);
        if (!conf.custom_api_key.empty()) secrets.push_back(conf.custom_api_key);
        if (!conf.account_device_key.empty()) secrets.push_back(conf.account_device_key);
        if (!conf.account_refresh_token.empty()) secrets.push_back(conf.account_refresh_token);
        if (!conf.account_device_public_id.empty()) secrets.push_back(conf.account_device_public_id);
        if (!conf.account_signed_in_name.empty()) secrets.push_back(conf.account_signed_in_name);
        if (!conf.client_uuid.empty()) secrets.push_back(conf.client_uuid);
        for (const auto& id : conf.known_primary_ids) {
            if (!id.empty()) secrets.push_back(id);
        }
        if (!conf.last_primary_id.empty()) secrets.push_back(conf.last_primary_id);

        Core::ZipWriter zip;

        const std::string summaryText = BuildDiagnosticsSummary();
        zip.AddFile("summary.txt", summaryText);

        const std::filesystem::path configPath = Storage::GetDataDirectory() + "config.json";
        std::string configContent = ReadFileShared(configPath);
        if (configContent.empty()) {
            configContent = "{}";
        }
        const std::string redactedConfig = RedactConfigString(configContent);
        zip.AddFile("config.json", redactedConfig);

        const std::string dataDir = Storage::GetDataDirectory();
        const std::filesystem::path currentLogPath = dataDir + Storage::APP_NAME + "_log.txt";
        if (std::filesystem::exists(currentLogPath, ec)) {
            const std::string rawCurrent = ReadFileShared(currentLogPath);
            const std::string redactedCurrent = RedactLog(rawCurrent, secrets);
            zip.AddFile(Storage::APP_NAME + std::string("_log.txt"), redactedCurrent);
        }

        for (int i = 1; i <= 5; ++i) {
            const std::filesystem::path rotatedLogPath = dataDir + Storage::APP_NAME + "_log." + std::to_string(i) + ".txt";
            if (std::filesystem::exists(rotatedLogPath, ec)) {
                const std::string rawRotated = ReadFileShared(rotatedLogPath);
                const std::string redactedRotated = RedactLog(rawRotated, secrets);
                zip.AddFile(Storage::APP_NAME + std::string("_log.") + std::to_string(i) + ".txt", redactedRotated);
            }
        }

        if (!zip.WriteToFile(zipPath)) {
            outError = "Failed to write support bundle zip archive to " + zipPath.string();
            return false;
        }

        outZipPath = zipPath.string();
        return true;
    }

} // namespace SupportBundle
