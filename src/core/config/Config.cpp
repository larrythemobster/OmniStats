#include "core/config/Config.hpp"
#include "core/config/ConfigSchema.hpp"
#include "core/config/SecretStore.hpp"
#include "utils/Storage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;

namespace Config {
    static ConfigData Current;
    static std::shared_mutex Mutex;
    static SecretStore s_secretStore;

    static std::mutex s_saverMutex;
    static std::condition_variable s_saverCv;
    static std::atomic<bool> s_pendingSave{false};
    static std::atomic<uint64_t> s_revision{1};
    static std::atomic<bool> s_saverRunning{false};
    static std::jthread s_saveThread;
    // Set when config.json exists but cannot be read, so defaults never overwrite it.
    static bool s_writeBlocked = false;

    ConfigData Read() {
        std::shared_lock<std::shared_mutex> lock(Mutex);
        return Current;
    }

    uint64_t Revision() {
        return s_revision.load(std::memory_order_acquire);
    }

    struct SerializedConfig {
        uint64_t sequence = 0;
        std::string path;
        std::string text;
    };
    static std::optional<SerializedConfig> SerializeLocked(SecretStore& secrets = s_secretStore);
    static void WriteSerialized(const SerializedConfig& config);
    static void SaveInternal(SecretStore& secrets = s_secretStore);

    // Serialize under an exclusive Mutex, then write without it so readers are not blocked on disk I/O.
    static void SaveOutsideLock(SecretStore& secrets = s_secretStore) {
        std::optional<SerializedConfig> serialized;
        {
            std::unique_lock<std::shared_mutex> lock(Mutex);
            serialized = SerializeLocked(secrets);
        }
        if (serialized) WriteSerialized(*serialized);
    }

    static void MarkPendingSave() {
        {
            std::lock_guard<std::mutex> lock(s_saverMutex);
            s_pendingSave.store(true);
        }
        s_saverCv.notify_one();
    }

    void InitSaver() {
        if (s_saverRunning.exchange(true)) return;
        s_saveThread = std::jthread([](std::stop_token stopToken) {
            std::unique_lock<std::mutex> lock(s_saverMutex);
            while (!stopToken.stop_requested() && s_saverRunning.load()) {
                s_saverCv.wait(lock, [&]() {
                    return !s_saverRunning.load() || stopToken.stop_requested() || s_pendingSave.load();
                });
                if (!s_saverRunning.load() || stopToken.stop_requested()) break;

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                while (!stopToken.stop_requested() && s_saverRunning.load()) {
                    if (s_saverCv.wait_until(lock, deadline, [&]() {
                            return !s_saverRunning.load() || stopToken.stop_requested();
                        })) {
                        break;
                    }
                    if (std::chrono::steady_clock::now() >= deadline) {
                        break;
                    }
                }

                if (s_pendingSave.exchange(false)) {
                    lock.unlock();
                    SaveOutsideLock();
                    lock.lock();
                }
            }
        });
    }

    void ShutdownSaver() {
        {
            std::lock_guard<std::mutex> lock(s_saverMutex);
            if (!s_saverRunning.exchange(false)) return;
        }
        s_saverCv.notify_all();
        if (s_saveThread.joinable()) {
            s_saveThread.request_stop();
            s_saveThread.join();
        }
        if (s_pendingSave.exchange(false)) {
            SaveOutsideLock();
        }
    }

    void Update(std::function<void(ConfigData&)> fn, bool saveToDisk) {
        std::unique_lock<std::shared_mutex> lock(Mutex);
        if (fn) fn(Current);

        ConfigSchema::NormalizeLobbyRanks(Current);

        if (!Current.last_primary_id.empty()) {
            if (std::find(Current.known_primary_ids.begin(), Current.known_primary_ids.end(), Current.last_primary_id) == Current.known_primary_ids.end()) {
                Current.known_primary_ids.push_back(Current.last_primary_id);
            }
        }
        Current.match_summary_seconds = std::clamp(Current.match_summary_seconds, 5, 60);
        for (auto& container : Current.overlay_layout.containers) {
            OverlayLayout::Sanitize(container.visibility);
        }
        s_revision.fetch_add(1, std::memory_order_release);
        if (saveToDisk) {
            MarkPendingSave();
        }
    }

    std::string GetConfigPath() {
        return Storage::GetDataDirectory() + "config.json";
    }

    static std::optional<std::string> ReadConfigText(const std::string& path) {
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::ifstream file(fs::path(path), std::ios::binary);
            if (!file.is_open()) continue;
            std::ostringstream text;
            text << file.rdbuf();
            if (!file.bad()) return text.str();
        }
        return std::nullopt;
    }

    void Load() {
        Load(s_secretStore);
    }

    void Load(SecretStore& secrets) {
        std::unique_lock<std::shared_mutex> lock(Mutex);
        std::string configFile = GetConfigPath();
        std::error_code existsEc;
        const bool exists = fs::exists(configFile, existsEc);
        if (!exists && !existsEc) {
            s_writeBlocked = false;
            ConfigSchema::ResetPreservedThemes();
            std::cout << "[Config] No config found. Creating default config.\n";
            Current = ConfigData{};
            secrets.Reset();
            SaveInternal(secrets);
            s_revision.fetch_add(1, std::memory_order_release);
            return;
        }

        const std::optional<std::string> text = exists ? ReadConfigText(configFile) : std::nullopt;
        if (!text) {
            std::cout << "[Config] config.json could not be read. Using defaults and leaving the file untouched.\n";
            s_writeBlocked = true;
            ConfigSchema::ResetPreservedThemes();
            Current = ConfigData{};
            secrets.Reset();
            s_revision.fetch_add(1, std::memory_order_release);
            return;
        }
        s_writeBlocked = false;

        try {
            nlohmann::json j = nlohmann::json::parse(*text);
            if (!j.is_object()) throw std::invalid_argument("Configuration must be a JSON object");

            Current = ConfigData{};
            secrets.Reset();
            bool needsSave = false;
            bool needsLegacyMigration = false;

            ConfigSchema::Decode(j, Current, secrets, needsSave, needsLegacyMigration);

            if (j.contains("gamepad_overlay")) {
                bool migrated = false;
                if (j.contains("controller_default_back_migrated") && j["controller_default_back_migrated"].is_boolean()) {
                    migrated = j["controller_default_back_migrated"].get<bool>();
                }
                if (!migrated && Current.gamepad_overlay == 6) {
                    std::cout << "[Config] Migrating controller overlay bind from Start(6) to Back(4).\n";
                    Current.gamepad_overlay = 4;
                    needsSave = true;
                }
            } else {
                Current.gamepad_overlay = 4;
                needsSave = true;
            }

            if ((!j.contains("custom_api_key") || !j["custom_api_key"].is_string()) &&
                j.contains("pro_api_key") && j["pro_api_key"].is_string()) {
                secrets.LoadSecretField("custom_api_key", j["pro_api_key"].get<std::string>(), Current.custom_api_key, needsLegacyMigration, Current.credentials_decryption_failed);
                if (!Current.custom_api_key.empty()) needsLegacyMigration = true;
            }
            if ((!j.contains("custom_api_enabled") || !j["custom_api_enabled"].is_boolean()) &&
                j.contains("pro_account_enabled") && j["pro_account_enabled"].is_boolean()) {
                Current.custom_api_enabled = j["pro_account_enabled"].get<bool>();
            }

            if (!j.contains("onboarding_completed") || !j["onboarding_completed"].is_boolean()) {
                Current.onboarding_completed = !Current.privacy_policy_accepted_version.empty();
            }

            const auto nonEmptyJsonString = [&](const char* key) {
                return j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty();
            };
            const bool existingInstall =
                (j.contains("onboarding_completed") && j["onboarding_completed"].is_boolean() && j["onboarding_completed"].get<bool>()) ||
                nonEmptyJsonString("privacy_policy_accepted_version") ||
                nonEmptyJsonString("client_uuid") ||
                nonEmptyJsonString("last_primary_id") ||
                j.contains("overlay_layout") ||
                j.contains("dashboard_layout");

            if (!j.contains("match_summary_seconds") || !j["match_summary_seconds"].is_number_integer() ||
                j["match_summary_seconds"].get<long double>() < (std::numeric_limits<int>::lowest)() ||
                j["match_summary_seconds"].get<long double>() > (std::numeric_limits<int>::max)()) {
                Current.match_summary_seconds = existingInstall ? 30 : 20;
            }

            if (!j.contains("graph_mmr_category") || !j["graph_mmr_category"].is_string() ||
                !ConfigSchema::IsGraphMmrCategory(j["graph_mmr_category"].get<std::string>())) {
                Current.graph_mmr_category = ConfigSchema::IsGraphMmrCategory(Current.mmr_category) ? Current.mmr_category : "2v2";
                needsSave = true;
            }
            if (!j.contains("graph_follow_current_playlist") || !j["graph_follow_current_playlist"].is_boolean()) {
                Current.graph_follow_current_playlist = Current.auto_switch_mmr_category;
                needsSave = true;
            }
            if (!j.contains("rank_progress_mmr_category") || !j["rank_progress_mmr_category"].is_string() ||
                !ConfigSchema::IsKnownMmrCategory(j["rank_progress_mmr_category"].get<std::string>())) {
                Current.rank_progress_mmr_category = j.contains("rank_progress_mmr_category") ? "2v2" : (ConfigSchema::IsKnownMmrCategory(Current.mmr_category) ? Current.mmr_category : "2v2");
                needsSave = true;
            }
            if (!j.contains("rank_progress_follow_current_playlist") || !j["rank_progress_follow_current_playlist"].is_boolean()) {
                Current.rank_progress_follow_current_playlist = Current.auto_switch_mmr_category;
                needsSave = true;
            }
            if (!Current.show_extra_playlists) {
                if (ConfigSchema::IsExtraMmrCategory(Current.graph_mmr_category)) {
                    Current.graph_mmr_category = "2v2";
                    needsSave = true;
                }
                if (ConfigSchema::IsExtraMmrCategory(Current.rank_progress_mmr_category)) {
                    Current.rank_progress_mmr_category = "2v2";
                    needsSave = true;
                }
            }

            if (const int minutes = Current.tournament_reminder_minutes; minutes % 5 != 0) {
                Current.tournament_reminder_minutes = 10;
            }

            if (j.contains("check_for_updates")) {
                needsSave = true;
            }

            if (!j.contains("overlay_layout")) {
                Current.overlay_layout = OverlayLayout::DefaultOverlayLayout();
                if (existingInstall) {
                    for (auto& c : Current.overlay_layout.containers) {
                        c.visibility = OverlayLayout::ContainerConfig::Visibility{};
                    }
                }
            }

            ConfigSchema::NormalizeLobbyRanks(Current);

            if (Current.themeBg.a < 0.01f) {
                Current.themeBg.a = 0.85f;
                SaveInternal(secrets);
            }

            if (needsLegacyMigration) {
                const std::vector<std::pair<std::string, std::string>> secretsToMigrate = {
                    {"custom_api_key", Current.custom_api_key},
                    {"account_refresh_token", Current.account_refresh_token},
                    {"account_device_key", Current.account_device_key},
                    {"ballchasing_token", Current.ballchasing_token}};
                if (secrets.MigrateLegacySecrets(secretsToMigrate)) {
                    SaveInternal(secrets);
                } else {
                    std::cerr << "[Config] Legacy credential migration encryption failed. Plaintext will not be rewritten.\n";
                }
            }

            if (needsSave) {
                MarkPendingSave();
            }
            s_revision.fetch_add(1, std::memory_order_release);
            std::cout << "[Config] Loaded config.json successfully.\n";

        } catch (const std::exception& e) {
            std::cout << "[Config] Failed to parse config.json: " << e.what() << "\n";
            std::error_code ec;
            fs::rename(configFile, configFile + ".corrupt", ec);
            Current = ConfigData{};
            secrets.Reset();
            SaveInternal(secrets);
            s_revision.fetch_add(1, std::memory_order_release);
        }
    }

    static uint64_t s_serializedSequence = 0;
    static std::mutex s_fileMutex;
    static uint64_t s_writtenSequence = 0;

    static std::optional<SerializedConfig> SerializeLocked(SecretStore& secrets) {
        if (s_writeBlocked) {
            std::cerr << "[Config] Not saving: config.json could not be read at startup.\n";
            return std::nullopt;
        }
        SerializedConfig serialized;
        try {
            serialized.text = ConfigSchema::Encode(Current, secrets).dump(4);
        } catch (const std::exception& e) {
            std::cerr << "[Config] Failed to serialize config: " << e.what() << "\n";
            return std::nullopt;
        }
        serialized.sequence = ++s_serializedSequence;
        serialized.path = GetConfigPath();
        return serialized;
    }

    static void WriteSerialized(const SerializedConfig& config) {
        std::lock_guard<std::mutex> lock(s_fileMutex);
        // A save serialized earlier may reach the file lock after a newer one.
        if (config.sequence <= s_writtenSequence) return;
        s_writtenSequence = config.sequence;
        if (!Storage::WriteFileAtomically(fs::path(config.path), config.text)) {
            std::cout << "[Config] Failed to write config.json. Error=" << GetLastError() << "\n";
        }
    }

    static void SaveInternal(SecretStore& secrets) {
        if (const auto serialized = SerializeLocked(secrets)) WriteSerialized(*serialized);
    }

    void Save() {
        SaveOutsideLock();
    }

    void Save(SecretStore& secrets) {
        SaveOutsideLock(secrets);
    }

    void RequestSave() {
        MarkPendingSave();
    }

    void SetWindowsAutoStart(bool enable) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
        if (enable) {
            std::wstring exePath(MAX_PATH, L'\0');
            DWORD length = 0;
            while ((length = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()))) == exePath.size()) {
                exePath.resize(exePath.size() * 2);
            }
            if (length > 0) {
                exePath.resize(length);
                const std::wstring command = L"\"" + exePath + L"\"";
                RegSetValueExW(hKey, L"OmniStats", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                               static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
            }
        } else {
            RegDeleteValueW(hKey, L"OmniStats");
        }
        RegCloseKey(hKey);
    }

} // namespace Config
