#include "TelemetryManager.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "core/Storage.hpp"
#include "core/AppVersion.hpp"
#include <curl/curl.h>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>
#include <cstdlib>

namespace fs = std::filesystem;

namespace TelemetryManager {

    static bool g_isTestMode = false;
    static std::string g_testSecret = "";
    static std::unique_ptr<std::jthread> g_workerThread;
    static std::atomic<bool> g_isRunning{true};

    static int TelemetryProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
        std::atomic<bool>* running = (std::atomic<bool>*)clientp;
        if (running && !running->load()) {
            return 1; // Abort transfer
        }
        return 0;
    }

    std::string GenerateUUIDv4() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 15);
        std::uniform_int_distribution<> dis2(8, 11);

        std::stringstream ss;
        ss << std::hex;
        for (int i = 0; i < 8; i++)
            ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 4; i++)
            ss << dis(gen);
        ss << "-4";
        for (int i = 0; i < 3; i++)
            ss << dis(gen);
        ss << "-";
        ss << dis2(gen);
        for (int i = 0; i < 3; i++)
            ss << dis(gen);
        ss << "-";
        for (int i = 0; i < 12; i++)
            ss << dis(gen);
        return ss.str();
    }

    void Initialize(std::shared_ptr<DatabaseManager> db) {
        if (!db)
            return;

        if (db->GetDatabasePath() == ":memory:") {
            g_isTestMode = true;
            const char* envSecret = std::getenv("OMNISTATS_TEST_SECRET");
            if (envSecret && std::string(envSecret) != "") {
                g_testSecret = envSecret;
            } else {
                g_testSecret.clear();
            }
            std::cout << "[Telemetry] Test environment detected (:memory:). X-Test-Mode is only sent when OMNISTATS_TEST_SECRET is set.\n";
        } else {
            g_isTestMode = false;
        }

        // Load or generate the pseudonymous installation ID used for required startup diagnostics.
        std::string uuid = db->GetSetting("client_uuid");
        if (uuid.empty()) {
            ConfigData conf = Config::Read();
            if (!conf.client_uuid.empty()) {
                uuid = conf.client_uuid;
                (void)db->SetSetting("client_uuid", uuid);
                std::cout << "[Telemetry] Synced client UUID from config: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
            } else {
                uuid = GenerateUUIDv4();
                (void)db->SetSetting("client_uuid", uuid);
                std::cout << "[Telemetry] Generated new client UUID: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
            }
        } else {
            std::cout << "[Telemetry] Loaded existing client UUID: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
        }
        Config::Update([&uuid](ConfigData& c) {
            c.client_uuid = uuid;
        });

        std::cout << "[Telemetry] Scheduling required startup diagnostics.\n";
        g_isRunning.store(true);
        if (!g_workerThread) {
            g_workerThread = std::make_unique<std::jthread>([]() {
                try {
                    SendTelemetryAsync();
                    if (Config::Read().crash_reports_enabled) CheckAndReportCrashes();
                } catch (const std::exception& e) {
                    std::cerr << "[Telemetry] Worker error: " << e.what() << "\n";
                } catch (...) {
                    std::cerr << "[Telemetry] Worker error: unknown\n";
                }
            });
        }
    }

    void Shutdown() {
        g_isRunning.store(false);
        if (g_workerThread && g_workerThread->joinable()) {
            g_workerThread->join();
            g_workerThread.reset();
            std::cout << "[Telemetry] Worker thread joined.\n";
        }
    }

    void CheckAndReportCrashes() {
        std::string crashFile = Storage::GetDataDirectory() + "crash_pending.dmp";
        if (fs::exists(crashFile)) {
            if (!Config::Read().crash_reports_enabled) {
                std::cout << "[Telemetry] Pending crash file detected, but crash reports are disabled. Leaving local dump in place.\n";
                return;
            }
            std::cout << "[Telemetry] Pending crash file detected on startup!\n";
            SendCrashAsync(crashFile);
        }
    }

    void SendTelemetryAsync() {
        ConfigData conf = Config::Read();

        nlohmann::json j;
        j["client_uuid"] = conf.client_uuid;
        j["version"] = AppVersion::Current;
        j["ballchasing_enabled"] = conf.auto_upload_replays;
        j["discord_rpc_enabled"] = conf.discord_rpc_enabled;
        j["update_checks_enabled"] = conf.check_for_updates;
        j["auto_updater_enabled"] = conf.enable_auto_updates;

        std::string payload = j.dump();

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        if (g_isTestMode && !g_testSecret.empty()) {
            std::string h = "X-Test-Mode: " + g_testSecret;
            headers = curl_slist_append(headers, h.c_str());
        }
        std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;

        CURL* curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL,
                             "https://api.omnistats.org/api/v1/telemetry");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TelemetryProgressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &g_isRunning);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                long code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
                std::cout << "[Telemetry] Telemetry ping sent successfully (HTTP " << code
                          << ")\n";
            } else {
                std::cerr << "[Telemetry] Telemetry ping failed: "
                          << curl_easy_strerror(res) << "\n";
            }
            curl_easy_cleanup(curl);
        }
        curl_slist_free_all(headers);
    }

    void SendCrashAsync(const std::string& crashFilePath) {
        ConfigData conf = Config::Read();
        if (!conf.crash_reports_enabled) {
            std::cout << "[Telemetry] Crash report upload skipped because crash reports are disabled.\n";
            return;
        }

        std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;

        CURL* curl = curl_easy_init();
        if (curl) {
            curl_mime* mime = curl_mime_init(curl);
            curl_mimepart* part;

            // Attach the crash dump
            part = curl_mime_addpart(mime);
            curl_mime_filedata(part, crashFilePath.c_str());
            curl_mime_name(part, "crash_dump");

            // Attach the client identifier
            part = curl_mime_addpart(mime);
            curl_mime_data(part, conf.client_uuid.c_str(), CURL_ZERO_TERMINATED);
            curl_mime_name(part, "client_uuid");

            // Attach the app version
            part = curl_mime_addpart(mime);
            curl_mime_data(part, AppVersion::Current, CURL_ZERO_TERMINATED);
            curl_mime_name(part, "version");

            struct curl_slist* headers = nullptr;
            if (g_isTestMode && !g_testSecret.empty()) {
                std::string h = "X-Test-Mode: " + g_testSecret;
                headers = curl_slist_append(headers, h.c_str());
            }

            curl_easy_setopt(curl, CURLOPT_URL, "https://api.omnistats.org/api/v1/crash");
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            if (headers) {
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            }
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, TelemetryProgressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &g_isRunning);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

            CURLcode res = curl_easy_perform(curl);
            long code = 0;
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
            }

            curl_easy_cleanup(curl);
            curl_mime_free(mime);
            if (headers) {
                curl_slist_free_all(headers);
            }

            if (code == 200) {
                std::cout << "[Telemetry] Crash report processed successfully by server. "
                             "Clearing local log.\n";
                std::error_code ec;
                fs::remove(crashFilePath, ec);
            } else {
                std::cerr
                    << "[Telemetry] Crash report rejected or server unavailable (HTTP "
                    << code << ")\n";
            }
        }
    }
} // namespace TelemetryManager
