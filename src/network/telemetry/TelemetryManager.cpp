#include "network/telemetry/TelemetryManager.hpp"
#include "core/config/Config.hpp"
#include "utils/PrivacyLog.hpp"
#include "utils/Storage.hpp"
#include "core/config/AppVersion.hpp"
#include "network/omninet/OmniNet.hpp"
#include <fstream>
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
        if (g_isTestMode || !OmniNet::Available()) {
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

        OmniNet::Request req;
        req.op = OmniNet::Op::Telemetry;
        req.payload = j.dump();
        req.shouldAbort = []() { return !g_isRunning.load(); };

        const auto resp = OmniNet::Send(req);
        if (resp.transportError == 0) {
            std::cout << "[Telemetry] Telemetry ping sent (HTTP " << resp.status << ")\n";
        } else {
            std::cerr << "[Telemetry] Telemetry ping failed (error " << resp.transportError << ")\n";
        }
    }

    void SendCrashAsync(const std::string& crashFilePath) {
        if (g_isTestMode || !OmniNet::Available()) {
            return;
        }

        ConfigData conf = Config::Read();
        if (!conf.crash_reports_enabled) {
            std::cout << "[Telemetry] Crash report upload skipped because crash reports are disabled.\n";
            return;
        }

        std::ifstream file(crashFilePath, std::ios::binary);
        if (!file) {
            std::cerr << "[Telemetry] Failed to open crash dump file.\n";
            return;
        }
        std::string dumpBytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        OmniNet::Request req;
        req.op = OmniNet::Op::Crash;
        req.payload = std::move(dumpBytes);
        req.fields["client_uuid"] = conf.client_uuid;
        req.fields["version"] = AppVersion::Current;
        req.timeoutSeconds = 15;
        req.shouldAbort = []() { return !g_isRunning.load(); };

        const auto resp = OmniNet::Send(req);
        const long code = resp.status;

        // 4xx other than 429 means the server will never accept this dump (e.g. oversized), so stop retrying it every launch.
        const bool accepted = (resp.transportError == 0 && code == 200);
        const bool permanentlyRejected = (code >= 400 && code < 500 && code != 429);
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
