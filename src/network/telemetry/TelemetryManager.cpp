#include "network/telemetry/TelemetryManager.hpp"
#include "database/DatabaseManager.hpp"
#include "core/config/AppVersion.hpp"
#include "core/config/Config.hpp"
#include "network/omninet/OmniNet.hpp"
#include "updater/UpdaterEventLog.hpp"
#include "utils/PrivacyLog.hpp"
#include "utils/Storage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <random>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

    static uint64_t GetCurrentUnbiasedTime() {
        ULONGLONG t = 0;
        QueryUnbiasedInterruptTime(&t);
        return static_cast<uint64_t>(t);
    }

    static uint32_t LoadMaskFromJson(const std::string& jsonStr) {
        uint32_t mask = 0;
        if (jsonStr.empty()) return mask;
        try {
            const auto j = nlohmann::json::parse(jsonStr);
            if (j.is_array()) {
                for (const auto& item : j) {
                    std::string name;
                    if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                        name = item["name"].get<std::string>();
                    } else if (item.is_string()) {
                        name = item.get<std::string>();
                    }
                    if (!name.empty()) {
                        for (uint8_t i = 0; i < static_cast<uint8_t>(TelemetryManager::Milestone::Count); ++i) {
                            const auto meta = TelemetryManager::MilestoneMetadata(static_cast<TelemetryManager::Milestone>(i));
                            if (meta.name == name) {
                                mask |= (1u << i);
                                break;
                            }
                        }
                    }
                }
            }
        } catch (...) {
        }
        return mask;
    }

    static std::string DumpMaskToJson(uint32_t mask) {
        nlohmann::json arr = nlohmann::json::array();
        for (uint8_t i = 0; i < static_cast<uint8_t>(TelemetryManager::Milestone::Count); ++i) {
            if (mask & (1u << i)) {
                const auto meta = TelemetryManager::MilestoneMetadata(static_cast<TelemetryManager::Milestone>(i));
                arr.push_back({{"kind", std::string(meta.kind)}, {"name", std::string(meta.name)}});
            }
        }
        return arr.dump();
    }

} // namespace

std::string TelemetryManager::GenerateUUIDv4() {
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

TelemetryManager::TelemetryManager(std::shared_ptr<DatabaseManager> db) {
    Initialize(std::move(db));
}

TelemetryManager::~TelemetryManager() {
    Shutdown();
}

void TelemetryManager::RecordMilestone(Milestone m) {
    const auto idx = static_cast<uint8_t>(m);
    if (idx >= static_cast<uint8_t>(Milestone::Count)) return;
    const uint32_t bit = 1u << idx;
    if (m_knownMask.load(std::memory_order_relaxed) & bit) {
        return;
    }

    std::string pendingJson;
    {
        std::lock_guard<std::mutex> lock(m_milestonesMutex);
        if ((m_reportedMask | m_pendingMask) & bit) {
            return;
        }
        m_pendingMask |= bit;
        m_knownMask.store(m_reportedMask | m_pendingMask, std::memory_order_release);
        pendingJson = DumpMaskToJson(m_pendingMask);
    }

    if (m_db) {
        m_db->AsyncSetSetting("milestones_pending", pendingJson);
    }
}

void TelemetryManager::FlushPendingMilestones() {
    if (m_isTestMode || !OmniNet::Available()) {
        return;
    }

    uint32_t pendingSnapshot = 0;
    {
        std::lock_guard<std::mutex> lock(m_milestonesMutex);
        pendingSnapshot = m_pendingMask;
    }

    if (pendingSnapshot == 0) return;

    const ConfigData conf = Config::Read();
    if (conf.client_uuid.empty()) return;

    nlohmann::json items = nlohmann::json::array();
    uint32_t flushedMask = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(Milestone::Count); ++i) {
        if (pendingSnapshot & (1u << i)) {
            const auto meta = MilestoneMetadata(static_cast<Milestone>(i));
            items.push_back({{"kind", std::string(meta.kind)}, {"name", std::string(meta.name)}});
            flushedMask |= (1u << i);
            if (items.size() >= 32) break;
        }
    }

    if (items.empty()) return;

    nlohmann::json j;
    j["client_uuid"] = conf.client_uuid;
    j["version"] = AppVersion::Current;
    j["items"] = items;

    OmniNet::Request req;
    req.op = OmniNet::Op::Milestones;
    req.payload = j.dump();
    req.shouldAbort = [this]() { return !m_isRunning.load(); };

    const auto resp = OmniNet::Send(req);
    if (resp.transportError == 0 && resp.status == 200) {
        std::string reportedJson;
        std::string pendingJson;
        {
            std::lock_guard<std::mutex> lock(m_milestonesMutex);
            m_pendingMask &= ~flushedMask;
            m_reportedMask |= flushedMask;
            m_knownMask.store(m_reportedMask | m_pendingMask, std::memory_order_release);
            reportedJson = DumpMaskToJson(m_reportedMask);
            pendingJson = DumpMaskToJson(m_pendingMask);
        }
        if (m_db) {
            (void)m_db->SetSetting("milestones_reported", reportedJson);
            (void)m_db->SetSetting("milestones_pending", pendingJson);
        }
        std::cout << "[Telemetry] Flushed " << items.size() << " milestones (HTTP 200)\n";
    } else {
        std::cerr << "[Telemetry] Milestone flush failed (HTTP " << resp.status
                  << ", transportError " << resp.transportError << ")\n";
    }
}

void TelemetryManager::RecordUpdaterEvent(const std::string& event,
                                          const std::string& targetVersion,
                                          const std::string& errorCategory,
                                          const std::string& version) {
    UpdaterEventLog::Append(Storage::GetDataDirectory(), event, targetVersion, errorCategory, version);
}

void TelemetryManager::FlushPendingUpdaterEvents() {
    if (m_isTestMode || !OmniNet::Available()) {
        return;
    }

    const std::string eventsPath = Storage::GetDataDirectory() + UpdaterEventLog::kFileName;
    const std::vector<std::string> lines = UpdaterEventLog::ReadLines(eventsPath);
    if (lines.empty()) {
        std::error_code ec;
        fs::remove(eventsPath, ec);
        return;
    }

    const ConfigData conf = Config::Read();
    if (conf.client_uuid.empty()) {
        return;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        if (!m_isRunning.load()) break;

        try {
            const auto jLine = nlohmann::json::parse(lines[i]);
            nlohmann::json j;
            j["client_uuid"] = conf.client_uuid;
            j["version"] = jLine.value("version", AppVersion::Current);
            j["event"] = jLine.value("event", "");
            j["target_version"] = jLine.value("target_version", "");
            j["error_category"] = jLine.value("error_category", "");
            j["update_checks_enabled"] = true;
            j["auto_updater_enabled"] = conf.enable_auto_updates;

            OmniNet::Request req;
            req.op = OmniNet::Op::UpdaterEvent;
            req.payload = j.dump();
            req.shouldAbort = [this]() { return !m_isRunning.load(); };

            const auto resp = OmniNet::Send(req);
            if (resp.transportError != 0) {
                break;
            }
        } catch (...) {
        }
    }

    std::error_code ec;
    fs::remove(eventsPath, ec);
}

void TelemetryManager::SendHeartbeatInternal(bool isShutdown) {
    if (m_isTestMode || !OmniNet::Available()) {
        return;
    }

    const ConfigData conf = Config::Read();
    if (conf.client_uuid.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_heartbeatMutex);
    if (!isShutdown && (!m_isRunning.load() || m_shutdownHeartbeatSent.load())) {
        return;
    }

    const uint64_t now = GetCurrentUnbiasedTime();
    const uint64_t elapsed = (now >= m_lastAccountingUnbiasedTime) ? (now - m_lastAccountingUnbiasedTime) : 0;
    const auto accounting = TelemetryDetail::CalculateHeartbeatMinutes(elapsed);

    nlohmann::json j;
    j["client_uuid"] = conf.client_uuid;
    j["version"] = AppVersion::Current;
    j["open_minutes"] = accounting.open_minutes;

    OmniNet::Request req;
    req.op = OmniNet::Op::Heartbeat;
    req.payload = j.dump();
    if (isShutdown) {
        req.timeoutSeconds = 3;
    } else {
        req.shouldAbort = [this]() { return !m_isRunning.load(); };
    }

    const auto resp = OmniNet::Send(req);
    if (resp.transportError == 0 && resp.status == 200) {
        m_lastAccountingUnbiasedTime = now - accounting.remainder_100ns;
        std::cout << "[Telemetry] Heartbeat sent (HTTP 200, open_minutes " << accounting.open_minutes << ")\n";
    } else {
        std::cerr << "[Telemetry] Heartbeat failed (HTTP " << resp.status
                  << ", transportError " << resp.transportError << ")\n";
    }
}

void TelemetryManager::SendShutdownHeartbeat() {
    if (m_shutdownHeartbeatSent.exchange(true)) {
        return;
    }
    SendHeartbeatInternal(true);
}

void TelemetryManager::SendHeartbeat() {
    SendHeartbeatInternal(false);
}

void TelemetryManager::Initialize(std::shared_ptr<DatabaseManager> db) {
    if (!db)
        return;

    m_db = db;
    m_isTestMode = db->GetDatabasePath() == ":memory:";
    if (m_isTestMode) {
        std::cout << "[Telemetry] Test environment detected (:memory:). Live network transmissions disabled.\n";
    }

    {
        std::lock_guard<std::mutex> lock(m_milestonesMutex);
        m_reportedMask = LoadMaskFromJson(db->GetSetting("milestones_reported"));
        m_pendingMask = LoadMaskFromJson(db->GetSetting("milestones_pending"));
        m_knownMask.store(m_reportedMask | m_pendingMask, std::memory_order_release);
    }

    m_shutdownHeartbeatSent.store(false);
    if (m_lastAccountingUnbiasedTime == 0) {
        m_lastAccountingUnbiasedTime = GetCurrentUnbiasedTime();
    }

    bool isFreshUUID = false;
    std::string uuid = db->GetSetting("client_uuid");
    if (uuid.empty()) {
        ConfigData conf = Config::Read();
        if (!conf.client_uuid.empty()) {
            uuid = conf.client_uuid;
            (void)db->SetSetting("client_uuid", uuid);
            std::cout << "[Telemetry] Synced client UUID from config: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
        } else {
            uuid = GenerateUUIDv4();
            isFreshUUID = true;
            (void)db->SetSetting("client_uuid", uuid);
            std::cout << "[Telemetry] Generated new client UUID: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
        }
    } else {
        std::cout << "[Telemetry] Loaded existing client UUID: " << PrivacyLog::Sensitive(uuid, "client UUID") << "\n";
    }
    Config::Update([&uuid](ConfigData& c) {
        c.client_uuid = uuid;
    });

    const std::string lastRunVersion = db->GetSetting("last_run_version");
    const std::string currentVersion = AppVersion::Current;
    if (lastRunVersion.empty()) {
        (void)db->SetSetting("last_run_version", currentVersion);
        if (isFreshUUID) {
            RecordMilestone(Milestone::FirstLaunch);
        }
    } else if (lastRunVersion != currentVersion) {
        (void)db->SetSetting("last_run_version", currentVersion);
        RecordUpdaterEvent("updated_successfully", currentVersion, "", lastRunVersion);
    }

    if (m_isTestMode) {
        std::cout << "[Telemetry] Test environment detected. Skipping background telemetry worker thread.\n";
        return;
    }

    std::cout << "[Telemetry] Scheduling required startup diagnostics.\n";
    m_isRunning.store(true);
    if (!m_workerThread) {
        m_workerThread = std::make_unique<std::jthread>([this](std::stop_token stopToken) {
            try {
                SendTelemetryAsync();
                FlushPendingUpdaterEvents();
                FlushPendingMilestones();
                if (Config::Read().crash_reports_enabled) CheckAndReportCrashes();

                while (!stopToken.stop_requested() && m_isRunning.load()) {
                    std::unique_lock<std::mutex> lock(m_workerMutex);
                    if (m_workerCv.wait_for(lock, std::chrono::minutes(60), [&]() {
                            return stopToken.stop_requested() || !m_isRunning.load();
                        })) {
                        break;
                    }

                    SendHeartbeat();
                    FlushPendingMilestones();
                }
                SendShutdownHeartbeat();
            } catch (const std::exception& e) {
                std::cerr << "[Telemetry] Worker error: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[Telemetry] Worker error: unknown\n";
            }
        });
    }
}

void TelemetryManager::Shutdown() {
    m_isRunning.store(false);
    m_workerCv.notify_all();
    if (m_workerThread) {
        m_workerThread->request_stop();
        if (m_workerThread->joinable()) {
            m_workerThread->join();
        }
        m_workerThread.reset();
        std::cout << "[Telemetry] Worker thread joined.\n";
    }
    SendShutdownHeartbeat();
}

void TelemetryManager::CheckAndReportCrashes() {
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

void TelemetryManager::SendTelemetryAsync() {
    if (m_isTestMode || !OmniNet::Available()) {
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
    j["crash_reports_enabled"] = conf.crash_reports_enabled;
    OmniNet::Request req;
    req.op = OmniNet::Op::Telemetry;
    req.payload = j.dump();
    req.shouldAbort = [this]() { return !m_isRunning.load(); };

    const auto resp = OmniNet::Send(req);
    if (resp.transportError == 0) {
        std::cout << "[Telemetry] Telemetry ping sent (HTTP " << resp.status << ")\n";
    } else {
        std::cerr << "[Telemetry] Telemetry ping failed (error " << resp.transportError << ")\n";
    }
}

void TelemetryManager::SendCrashAsync(const std::string& crashFilePath) {
    if (m_isTestMode || !OmniNet::Available()) {
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
    req.shouldAbort = [this]() { return !m_isRunning.load(); };

    const auto resp = OmniNet::Send(req);
    const long code = resp.status;

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
