#include "network/telemetry/TelemetryManager.hpp"
#include "core/config/Config.hpp"
#include "utils/PrivacyLog.hpp"
#include "utils/Storage.hpp"
#include "core/config/AppVersion.hpp"
#include <curl/curl.h>
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace TelemetryManager {

    static bool g_isTestMode = false;
    static std::unique_ptr<std::jthread> g_workerThread;
    static std::atomic<bool> g_isRunning{true};

    namespace {
        struct CurlEasyDeleter {
            void operator()(CURL* curl) const noexcept {
                curl_easy_cleanup(curl);
            }
        };
        struct CurlSlistDeleter {
            void operator()(curl_slist* list) const noexcept {
                curl_slist_free_all(list);
            }
        };
        struct CurlMimeDeleter {
            void operator()(curl_mime* mime) const noexcept {
                curl_mime_free(mime);
            }
        };
        using UniqueCurl = std::unique_ptr<CURL, CurlEasyDeleter>;
        using UniqueSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;
        using UniqueMime = std::unique_ptr<curl_mime, CurlMimeDeleter>;

        void ApplyCommonOptions(CURL* curl, const std::string& userAgent, long timeoutSeconds) {
            curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        }
    } // namespace

    static int TelemetryProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto* running = static_cast<std::atomic<bool>*>(clientp);
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

        g_isTestMode = db->GetDatabasePath() == ":memory:";
        if (g_isTestMode) {
            std::cout << "[Telemetry] Test environment detected (:memory:). Live network transmissions disabled.\n";
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

        if (g_isTestMode) {
            std::cout << "[Telemetry] Test environment detected. Skipping background telemetry worker thread.\n";
            return;
        }

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
        if (g_isTestMode) {
            std::cout << "[Telemetry] Test environment detected. Skipping live telemetry ping.\n";
            return;
        }

        ConfigData conf = Config::Read();

        nlohmann::json j;
        j["client_uuid"] = conf.client_uuid;
        j["version"] = AppVersion::Current;
        j["ballchasing_enabled"] = conf.auto_upload_replays;
        j["discord_rpc_enabled"] = conf.discord_rpc_enabled;
        j["update_checks_enabled"] = true;
        j["auto_updater_enabled"] = conf.enable_auto_updates;

        const std::string payload = j.dump();
        const std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;

        UniqueCurl curl(curl_easy_init());
        if (!curl) return;
        UniqueSlist headers(curl_slist_append(nullptr, "Content-Type: application/json"));

        curl_easy_setopt(curl.get(), CURLOPT_URL, "https://api.omnistats.org/api/v1/telemetry");
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
        ApplyCommonOptions(curl.get(), userAgent, 10L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, TelemetryProgressCallback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &g_isRunning);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);

        const CURLcode res = curl_easy_perform(curl.get());
        if (res == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
            std::cout << "[Telemetry] Telemetry ping sent (HTTP " << code << ")\n";
        } else {
            std::cerr << "[Telemetry] Telemetry ping failed: " << curl_easy_strerror(res) << "\n";
        }
    }

    void SendCrashAsync(const std::string& crashFilePath) {
        if (g_isTestMode) {
            std::cout << "[Telemetry] Test environment detected. Skipping live crash report upload.\n";
            return;
        }

        ConfigData conf = Config::Read();
        if (!conf.crash_reports_enabled) {
            std::cout << "[Telemetry] Crash report upload skipped because crash reports are disabled.\n";
            return;
        }

        const std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;

        UniqueCurl curl(curl_easy_init());
        if (!curl) return;
        UniqueMime mime(curl_mime_init(curl.get()));
        if (!mime) return;

        curl_mimepart* part = curl_mime_addpart(mime.get());
        curl_mime_filedata(part, crashFilePath.c_str());
        curl_mime_name(part, "crash_dump");

        part = curl_mime_addpart(mime.get());
        curl_mime_data(part, conf.client_uuid.c_str(), CURL_ZERO_TERMINATED);
        curl_mime_name(part, "client_uuid");

        part = curl_mime_addpart(mime.get());
        curl_mime_data(part, AppVersion::Current, CURL_ZERO_TERMINATED);
        curl_mime_name(part, "version");

        curl_easy_setopt(curl.get(), CURLOPT_URL, "https://api.omnistats.org/api/v1/crash");
        curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
        ApplyCommonOptions(curl.get(), userAgent, 15L);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, TelemetryProgressCallback);
        curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &g_isRunning);
        curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);

        long code = 0;
        if (curl_easy_perform(curl.get()) == CURLE_OK) {
            curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
        }
        curl.reset();
        mime.reset();

        // 4xx other than 429 means the server will never accept this dump (e.g. oversized), so stop retrying it every launch.
        const bool accepted = code == 200;
        const bool permanentlyRejected = code >= 400 && code < 500 && code != 429;
        if (accepted || permanentlyRejected) {
            if (accepted) {
                std::cout << "[Telemetry] Crash report processed by server. Clearing local dump.\n";
            } else {
                std::cerr << "[Telemetry] Crash report rejected (HTTP " << code << "). Discarding local dump.\n";
            }
            std::error_code ec;
            fs::remove(crashFilePath, ec);
        } else {
            std::cerr << "[Telemetry] Crash report not delivered (HTTP " << code << "); will retry next launch.\n";
        }
    }
} // namespace TelemetryManager
