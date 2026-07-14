#include "Config.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <atomic>
#include "Storage.hpp"

#include <windows.h>
#include <wincrypt.h>

namespace fs = std::filesystem;

static std::string StringToHex(const std::string& input) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c : input) {
        ss << std::setw(2) << (int)c;
    }
    return ss.str();
}

static bool IsHexString(const std::string& s) {
    if (s.empty() || s.length() % 2 != 0) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

static std::string HexToString(const std::string& input) {
    if (!IsHexString(input)) return "";
    std::string output;
    try {
        for (size_t i = 0; i < input.length(); i += 2) {
            std::string part = input.substr(i, 2);
            char chr = (char)std::stoul(part, nullptr, 16);
            output.push_back(chr);
        }
    } catch (...) {
        return "";
    }
    return output;
}

static std::string EncryptToken(const std::string& plainText) {
    if (plainText.empty()) return "";

    DATA_BLOB inputBlob;
    inputBlob.pbData = (BYTE*)plainText.data();
    inputBlob.cbData = (DWORD)plainText.size();

    DATA_BLOB outputBlob;
    if (CryptProtectData(&inputBlob, L"OmniStats Token", nullptr, nullptr, nullptr, 0, &outputBlob)) {
        std::string encryptedBytes((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return StringToHex(encryptedBytes);
    }
    std::cerr << "[Config] DPAPI encryption failed with error: " << GetLastError() << "\n";
    return plainText;
}

static std::string DecryptToken(const std::string& hexCipher) {
    if (hexCipher.empty()) return "";

    std::string cipherBytes = HexToString(hexCipher);
    if (cipherBytes.empty()) return hexCipher;

    DATA_BLOB inputBlob;
    inputBlob.pbData = (BYTE*)cipherBytes.data();
    inputBlob.cbData = (DWORD)cipherBytes.size();

    DATA_BLOB outputBlob;
    if (CryptUnprotectData(&inputBlob, nullptr, nullptr, nullptr, nullptr, 0, &outputBlob)) {
        std::string decryptedBytes((char*)outputBlob.pbData, outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return decryptedBytes;
    }
    return hexCipher; // Fallback to raw if decryption fails (plain text from older version)
}

// Helper: serialize ColorRGBA to JSON array [r, g, b, a]
static nlohmann::json ColorToJson(const ColorRGBA& c) {
    return nlohmann::json::array({c.r, c.g, c.b, c.a});
}

// Helper: deserialize JSON array [r, g, b, a] to ColorRGBA
static ColorRGBA JsonToColor(const nlohmann::json& j, const ColorRGBA& fallback) {
    if (!j.is_array() || j.size() < 4) return fallback;
    return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() };
}

namespace Config {
    static ConfigData Current;
    static std::shared_mutex Mutex;
    static std::atomic<bool> s_pendingSave{false};
    static std::atomic<bool> s_appRunning{true};
    static std::jthread s_saveThread;

    ConfigData Read() {
        std::shared_lock<std::shared_mutex> lock(Mutex);
        return Current;
    }

    // Forward declaration of internal non-locking save
    static void SaveInternal();

    void InitSaver() {
        s_appRunning.store(true);
        s_saveThread = std::jthread([]() {
            while (s_appRunning.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (s_pendingSave.exchange(false)) {
                    std::unique_lock<std::shared_mutex> lock(Mutex);
                    SaveInternal();
                }
            }
        });
    }

    void ShutdownSaver() {
        s_appRunning.store(false);
        if (s_saveThread.joinable()) {
            s_saveThread.join();
        }
        if (s_pendingSave.exchange(false)) {
            std::unique_lock<std::shared_mutex> lock(Mutex);
            SaveInternal();
        }
    }

    void Update(std::function<void(ConfigData&)> fn, bool saveToDisk) {
        std::unique_lock<std::shared_mutex> lock(Mutex);
        if (fn) fn(Current);
        // Fallback: Prevent the user from disabling every column.
        if (!Current.show_lobby_rank_1v1 && !Current.show_lobby_rank_2v2 &&
            !Current.show_lobby_rank_3v3 && !Current.show_lobby_rank_casual && !Current.show_lobby_rank_tourny &&
            !Current.show_lobby_rank_hoops && !Current.show_lobby_rank_rumble && !Current.show_lobby_rank_dropshot &&
            !Current.show_lobby_rank_snowday && !Current.show_lobby_rank_heatseeker) {
            Current.show_lobby_rank_2v2 = true;
        }
        // Keep a small local cache of known primary ids so heuristics can prefer
        // recently-used accounts when attempting to auto-detect identity.
        if (!Current.last_primary_id.empty()) {
            if (std::find(Current.known_primary_ids.begin(), Current.known_primary_ids.end(), Current.last_primary_id) == Current.known_primary_ids.end()) {
                Current.known_primary_ids.push_back(Current.last_primary_id);
            }
        }
        if (saveToDisk) {
            s_pendingSave.store(true);
        }
    }

    std::string GetConfigPath() {
        return Storage::GetDataDirectory() + "config.json";
    }

    void Load() {
        std::unique_lock<std::shared_mutex> lock(Mutex);
        std::string configFile = GetConfigPath();
        if (!fs::exists(configFile)) {
            std::cout << "[Config] No config found. Creating default config.\n";
            SaveInternal();
            return;
        }

        try {
            std::ifstream file(configFile);
            nlohmann::json j;
            file >> j;
            bool needsSave = false;

            // Safely load values, falling back to defaults if keys are missing
            if (j.contains("host")) Current.host = j["host"];
            if (j.contains("port")) Current.port = j["port"];
            if (j.contains("key_overlay")) Current.key_overlay = j["key_overlay"];
            if (j.contains("key_cycle")) Current.key_cycle = j["key_cycle"];
            if (j.contains("key_expand")) Current.key_expand = j["key_expand"];
            if (j.contains("key_session")) Current.key_session = j["key_session"];
            if (j.contains("key_menu")) Current.key_menu = j["key_menu"];

            // Load gamepad overlay with migration for the old bad default
            if (j.contains("gamepad_overlay")) {
                Current.gamepad_overlay = j["gamepad_overlay"];

                // Old bug: default value was 6 (START), but intended as 4 (BACK).
                // Migrate old configs one time so users get the intended Select/View/Back button.
                const bool migrated = j.value("controller_default_back_migrated", false);
                if (!migrated && Current.gamepad_overlay == 6) {
                    std::cout << "[Config] Migrating controller overlay bind from Start(6) to Back(4).\n";
                    Current.gamepad_overlay = 4; // SDL_CONTROLLER_BUTTON_BACK
                    needsSave = true;
                }
            } else {
                Current.gamepad_overlay = 4; // SDL_CONTROLLER_BUTTON_BACK
                needsSave = true;
            }
            if (j.contains("gamepad_overlay_raw")) Current.gamepad_overlay_raw = j["gamepad_overlay_raw"];
            if (j.contains("gamepad_overlay_raw_button")) Current.gamepad_overlay_raw_button = j["gamepad_overlay_raw_button"];
            if (j.contains("gamepad_cycle")) Current.gamepad_cycle = j["gamepad_cycle"];
            if (j.contains("gamepad_cycle_raw")) Current.gamepad_cycle_raw = j["gamepad_cycle_raw"];
            if (j.contains("gamepad_cycle_raw_button")) Current.gamepad_cycle_raw_button = j["gamepad_cycle_raw_button"];
            if (j.contains("gamepad_expand")) Current.gamepad_expand = j["gamepad_expand"];
            if (j.contains("gamepad_expand_raw")) Current.gamepad_expand_raw = j["gamepad_expand_raw"];
            if (j.contains("gamepad_expand_raw_button")) Current.gamepad_expand_raw_button = j["gamepad_expand_raw_button"];
            if (j.contains("gamepad_session")) Current.gamepad_session = j["gamepad_session"];
            if (j.contains("gamepad_session_raw")) Current.gamepad_session_raw = j["gamepad_session_raw"];
            if (j.contains("gamepad_session_raw_button")) Current.gamepad_session_raw_button = j["gamepad_session_raw_button"];
            if (j.contains("gamepad_menu")) Current.gamepad_menu = j["gamepad_menu"];
            if (j.contains("gamepad_menu_raw")) Current.gamepad_menu_raw = j["gamepad_menu_raw"];
            if (j.contains("gamepad_menu_raw_button")) Current.gamepad_menu_raw_button = j["gamepad_menu_raw_button"];
            if (j.contains("mmr_category")) Current.mmr_category = j["mmr_category"];
            if (j.contains("last_primary_id")) Current.last_primary_id = j["last_primary_id"];
            if (j.contains("require_rl_focus")) Current.require_rl_focus = j["require_rl_focus"];
            if (j.contains("show_match_summary")) Current.show_match_summary = j["show_match_summary"];
            if (j.contains("discord_rpc_enabled")) Current.discord_rpc_enabled = j["discord_rpc_enabled"];
            if (j.contains("enable_mmr_tracking")) Current.enable_mmr_tracking = j["enable_mmr_tracking"];
            if (j.contains("auto_switch_mmr_category")) Current.auto_switch_mmr_category = j["auto_switch_mmr_category"];
            if (j.contains("check_for_updates")) {
                Current.check_for_updates = j["check_for_updates"];
            } else if (j.contains("enable_auto_updates") && j["enable_auto_updates"].is_boolean()) {
                Current.check_for_updates = j["enable_auto_updates"];
            }
            if (j.contains("enable_auto_updates")) Current.enable_auto_updates = j["enable_auto_updates"];
            if (j.contains("crash_reports_enabled")) Current.crash_reports_enabled = j["crash_reports_enabled"];
            if (j.contains("debug_logging")) Current.debug_logging = j["debug_logging"];
            if (j.contains("run_on_startup")) Current.run_on_startup = j["run_on_startup"];
            if (j.contains("reset_session_on_close")) Current.reset_session_on_close = j["reset_session_on_close"];
            if (j.contains("imperial_units")) Current.imperial_units = j["imperial_units"];
            if (j.contains("client_uuid")) Current.client_uuid = j["client_uuid"];
            if (j.contains("privacy_policy_accepted_version")) Current.privacy_policy_accepted_version = j["privacy_policy_accepted_version"];
            if (j.contains("terms_accepted_version")) Current.terms_accepted_version = j["terms_accepted_version"];
            if (j.contains("privacy_accepted_at")) Current.privacy_accepted_at = j["privacy_accepted_at"];
            if (j.contains("position")) Current.position = j["position"];
            if (j.contains("show_running_indicator")) Current.show_running_indicator = j["show_running_indicator"];
            if (j.contains("use_roman_numerals")) Current.use_roman_numerals = j["use_roman_numerals"];
            if (j.contains("use_rank_icons")) Current.use_rank_icons = j["use_rank_icons"];
            if (j.contains("show_extra_playlists")) Current.show_extra_playlists = j["show_extra_playlists"];
            if (j.contains("rocket_league_stats_api_config_path")) Current.rocket_league_stats_api_config_path = j["rocket_league_stats_api_config_path"];
            if (j.contains("check_stats_api_config_on_startup")) Current.check_stats_api_config_on_startup = j["check_stats_api_config_on_startup"];
            if (j.contains("ballchasing_token") && j["ballchasing_token"].is_string()) {
                Current.ballchasing_token = DecryptToken(j["ballchasing_token"].get<std::string>());
            }
            if (j.contains("auto_upload_replays")) Current.auto_upload_replays = j["auto_upload_replays"];
            if (j.contains("ballchasing_upload_notice_accepted")) Current.ballchasing_upload_notice_accepted = j["ballchasing_upload_notice_accepted"];
            if (j.contains("ballchasing_visibility") && j["ballchasing_visibility"].is_string()) {
                std::string visibility = j["ballchasing_visibility"].get<std::string>();
                if (visibility == "private" || visibility == "unlisted" || visibility == "public") {
                    Current.ballchasing_visibility = visibility;
                }
            }
            if (j.contains("auto_save_replays")) Current.auto_save_replays = j["auto_save_replays"];
            if (j.contains("key_save_replay")) Current.key_save_replay = j["key_save_replay"];

            // Load session visibility flags
            if (j.contains("show_session_card_in_game")) Current.show_session_card_in_game = j["show_session_card_in_game"];
            if (j.contains("show_session_record")) Current.show_session_record = j["show_session_record"];
            if (j.contains("show_session_goals")) Current.show_session_goals = j["show_session_goals"];
            if (j.contains("show_session_saves")) Current.show_session_saves = j["show_session_saves"];
            if (j.contains("show_session_demos")) Current.show_session_demos = j["show_session_demos"];
            if (j.contains("show_session_assists")) Current.show_session_assists = j["show_session_assists"];
            if (j.contains("show_session_goal_participation")) Current.show_session_goal_participation = j["show_session_goal_participation"];
            if (j.contains("show_session_mmr_change")) Current.show_session_mmr_change = j["show_session_mmr_change"];
            if (j.contains("show_session_boost")) Current.show_session_boost = j["show_session_boost"];
            if (j.contains("show_lobby_ranks_overlay")) Current.show_lobby_ranks_overlay = j["show_lobby_ranks_overlay"];
            if (j.contains("show_lobby_rank_1v1")) Current.show_lobby_rank_1v1 = j["show_lobby_rank_1v1"];
            if (j.contains("show_lobby_rank_2v2")) Current.show_lobby_rank_2v2 = j["show_lobby_rank_2v2"];
            if (j.contains("show_lobby_rank_3v3")) Current.show_lobby_rank_3v3 = j["show_lobby_rank_3v3"];
            if (j.contains("show_lobby_rank_casual")) Current.show_lobby_rank_casual = j["show_lobby_rank_casual"];
            if (j.contains("show_lobby_rank_tourny")) Current.show_lobby_rank_tourny = j["show_lobby_rank_tourny"];
            if (j.contains("show_lobby_rank_hoops")) Current.show_lobby_rank_hoops = j["show_lobby_rank_hoops"];
            if (j.contains("show_lobby_rank_rumble")) Current.show_lobby_rank_rumble = j["show_lobby_rank_rumble"];
            if (j.contains("show_lobby_rank_dropshot")) Current.show_lobby_rank_dropshot = j["show_lobby_rank_dropshot"];
            if (j.contains("show_lobby_rank_snowday")) Current.show_lobby_rank_snowday = j["show_lobby_rank_snowday"];
            if (j.contains("show_lobby_rank_heatseeker")) Current.show_lobby_rank_heatseeker = j["show_lobby_rank_heatseeker"];
            
            // Fallback: Prevent the user from disabling every column.
            if (!Current.show_lobby_rank_1v1 && !Current.show_lobby_rank_2v2 &&
                !Current.show_lobby_rank_3v3 && !Current.show_lobby_rank_casual && !Current.show_lobby_rank_tourny &&
                !Current.show_lobby_rank_hoops && !Current.show_lobby_rank_rumble && !Current.show_lobby_rank_dropshot &&
                !Current.show_lobby_rank_snowday && !Current.show_lobby_rank_heatseeker) {
                Current.show_lobby_rank_2v2 = true;
            }
            if (j.contains("show_account_wins_overlay")) Current.show_account_wins_overlay = j["show_account_wins_overlay"];
            if (j.contains("show_demo_tracker_overlay")) Current.show_demo_tracker_overlay = j["show_demo_tracker_overlay"];
            if (j.contains("show_previous_games_summary")) Current.show_previous_games_summary = j["show_previous_games_summary"];
            if (j.contains("previous_games_limit")) Current.previous_games_limit = std::clamp(j["previous_games_limit"].get<int>(), 10, 50);
            if (j.contains("show_streaks_stats")) Current.show_streaks_stats = j["show_streaks_stats"];
            if (j.contains("show_longest_loss_streak")) Current.show_longest_loss_streak = j["show_longest_loss_streak"];
            if (j.contains("show_gamemode_breakdown")) Current.show_gamemode_breakdown = j["show_gamemode_breakdown"];
            if (j.contains("show_gamemode_record_1v1")) Current.show_gamemode_record_1v1 = j["show_gamemode_record_1v1"];
            if (j.contains("show_gamemode_record_2v2")) Current.show_gamemode_record_2v2 = j["show_gamemode_record_2v2"];
            if (j.contains("show_gamemode_record_3v3")) Current.show_gamemode_record_3v3 = j["show_gamemode_record_3v3"];
            if (j.contains("gamemode_breakdown_scope")) Current.gamemode_breakdown_scope = j["gamemode_breakdown_scope"];
            if (j.contains("crossbar_display_mode")) Current.crossbar_display_mode = j["crossbar_display_mode"];
            if (j.contains("second_monitor_mode")) Current.second_monitor_mode = j["second_monitor_mode"];
            if (j.contains("second_monitor_show_roster")) Current.second_monitor_show_roster = j["second_monitor_show_roster"];
            if (j.contains("second_monitor_show_session")) Current.second_monitor_show_session = j["second_monitor_show_session"];
            if (j.contains("second_monitor_show_settings")) Current.second_monitor_show_settings = j["second_monitor_show_settings"];
            if (j.contains("second_monitor_has_bounds")) Current.second_monitor_has_bounds = j["second_monitor_has_bounds"];
            if (j.contains("second_monitor_x")) Current.second_monitor_x = j["second_monitor_x"];
            if (j.contains("second_monitor_y")) Current.second_monitor_y = j["second_monitor_y"];
            if (j.contains("second_monitor_w")) Current.second_monitor_w = j["second_monitor_w"];
            if (j.contains("second_monitor_h")) Current.second_monitor_h = j["second_monitor_h"];
            if (j.contains("ui_scale")) Current.ui_scale = j["ui_scale"];
            if (j.contains("overlay_fps_cap")) Current.overlay_fps_cap = j["overlay_fps_cap"];
            if (j.contains("vsync")) Current.vsync = j["vsync"];

            // Theme colors
            if (j.contains("theme")) {
                auto& t = j["theme"];
                if (t.contains("bg"))      Current.themeBg     = JsonToColor(t["bg"],     Current.themeBg);
                if (t.contains("text"))    Current.themeText   = JsonToColor(t["text"],   Current.themeText);
                if (t.contains("accent"))  Current.themeAccent = JsonToColor(t["accent"], Current.themeAccent);
                if (t.contains("win"))     Current.themeWin    = JsonToColor(t["win"],    Current.themeWin);
                if (t.contains("loss"))    Current.themeLoss   = JsonToColor(t["loss"],   Current.themeLoss);
                if (t.contains("dim"))     Current.themeDim    = JsonToColor(t["dim"],    Current.themeDim);
                if (t.contains("muted"))   Current.themeMuted  = JsonToColor(t["muted"],  Current.themeMuted);
                if (t.contains("graph_line")) Current.themeGraphLine = JsonToColor(t["graph_line"], Current.themeGraphLine);
                if (t.contains("graph_baseline")) Current.themeGraphBaseline = JsonToColor(t["graph_baseline"], Current.themeGraphBaseline);
            }

            // Migration: if upgrading or first run, ensure theme background opacity is visible and safe.
            if (Current.themeBg.a < 0.01f) {
                Current.themeBg.a = 0.85f;
                SaveInternal();
            }

            if (j.contains("known_primary_ids") && j["known_primary_ids"].is_array()) {
                Current.known_primary_ids.clear();
                for (const auto& item : j["known_primary_ids"]) {
                    if (item.is_string()) Current.known_primary_ids.push_back(item.get<std::string>());
                }
            }

            if (j.contains("dashboard_layout")) {
                auto& jl = j["dashboard_layout"];
                if (jl.contains("version")) Current.dashboard_layout.version = jl["version"];
                if (jl.contains("left_column_weight")) Current.dashboard_layout.leftColumnWeight = jl["left_column_weight"];
                if (jl.contains("widgets") && jl["widgets"].is_array()) {
                    Current.dashboard_layout.widgets.clear();
                    for (const auto& jw : jl["widgets"]) {
                        DashboardLayout::WidgetPlacement w;
                        if (jw.contains("id")) w.id = DashboardLayout::WidgetIdFromConfigString(jw["id"]);
                        if (jw.contains("zone")) w.zone = DashboardLayout::ZoneFromConfigString(jw["zone"]);
                        if (jw.contains("order")) w.order = jw["order"];
                        if (jw.contains("height")) w.height = jw["height"];
                        if (jw.contains("collapsed")) w.collapsed = jw["collapsed"];
                        Current.dashboard_layout.widgets.push_back(w);
                    }
                }
                DashboardLayout::Sanitize(Current.dashboard_layout);
            }

            if (j.contains("overlay_layout")) {
                auto& jl = j["overlay_layout"];
                if (jl.contains("version")) Current.overlay_layout.version = jl["version"];
                if (jl.contains("toolbox_open")) Current.overlay_layout.toolboxOpen = jl["toolbox_open"];
                if (jl.contains("containers") && jl["containers"].is_array()) {
                    Current.overlay_layout.containers.clear();
                    for (const auto& jc : jl["containers"]) {
                        OverlayLayout::ContainerConfig c;
                        if (jc.contains("id")) c.id = jc["id"].get<std::string>();
                        if (jc.contains("x")) c.x = jc["x"].get<float>();
                        if (jc.contains("y")) c.y = jc["y"].get<float>();
                        if (jc.contains("w")) c.w = jc["w"].get<float>();
                        if (jc.contains("h")) c.h = jc["h"].get<float>();
                        if (jc.contains("widgets") && jc["widgets"].is_array()) {
                            for (const auto& jw : jc["widgets"]) {
                                c.widgets.push_back(DashboardLayout::WidgetIdFromConfigString(jw.get<std::string>()));
                            }
                        }
                        Current.overlay_layout.containers.push_back(c);
                    }
                }
                OverlayLayout::Sanitize(Current.overlay_layout);
            }

            std::cout << "[Config] Loaded config.json successfully.\n";

            if (needsSave) {
                s_pendingSave.store(true);
            }

        } catch (const std::exception& e) {
            std::cout << "[Config] Failed to parse config.json: " << e.what() << "\n";
            std::error_code ec;
            fs::rename(configFile, configFile + ".corrupt", ec);
            Current = ConfigData{};
            SaveInternal();
        }
    }

    static void SaveInternal() {
        nlohmann::json j;
        
        j["host"] = Current.host;
        j["port"] = Current.port;
        j["key_overlay"] = Current.key_overlay;
        j["key_cycle"] = Current.key_cycle;
        j["key_expand"] = Current.key_expand;
        j["key_session"] = Current.key_session;
        j["key_menu"] = Current.key_menu;
        j["gamepad_overlay"] = Current.gamepad_overlay;
        j["gamepad_overlay_raw"] = Current.gamepad_overlay_raw;
        j["gamepad_overlay_raw_button"] = Current.gamepad_overlay_raw_button;
        j["gamepad_cycle"] = Current.gamepad_cycle;
        j["gamepad_cycle_raw"] = Current.gamepad_cycle_raw;
        j["gamepad_cycle_raw_button"] = Current.gamepad_cycle_raw_button;
        j["gamepad_expand"] = Current.gamepad_expand;
        j["gamepad_expand_raw"] = Current.gamepad_expand_raw;
        j["gamepad_expand_raw_button"] = Current.gamepad_expand_raw_button;
        j["gamepad_session"] = Current.gamepad_session;
        j["gamepad_session_raw"] = Current.gamepad_session_raw;
        j["gamepad_session_raw_button"] = Current.gamepad_session_raw_button;
        j["gamepad_menu"] = Current.gamepad_menu;
        j["gamepad_menu_raw"] = Current.gamepad_menu_raw;
        j["gamepad_menu_raw_button"] = Current.gamepad_menu_raw_button;
        j["controller_default_back_migrated"] = true;
        j["mmr_category"] = Current.mmr_category;
        j["last_primary_id"] = Current.last_primary_id;
        j["known_primary_ids"] = Current.known_primary_ids;
        j["require_rl_focus"] = Current.require_rl_focus;
        j["show_match_summary"] = Current.show_match_summary;
        j["discord_rpc_enabled"] = Current.discord_rpc_enabled;
        j["enable_mmr_tracking"] = Current.enable_mmr_tracking;
        j["auto_switch_mmr_category"] = Current.auto_switch_mmr_category;
        j["check_for_updates"] = Current.check_for_updates;
        j["enable_auto_updates"] = Current.enable_auto_updates;
        j["crash_reports_enabled"] = Current.crash_reports_enabled;
        j["debug_logging"] = Current.debug_logging;
        j["run_on_startup"] = Current.run_on_startup;
        j["reset_session_on_close"] = Current.reset_session_on_close;
        j["imperial_units"] = Current.imperial_units;
        j["client_uuid"] = Current.client_uuid;
        j["privacy_policy_accepted_version"] = Current.privacy_policy_accepted_version;
        j["terms_accepted_version"] = Current.terms_accepted_version;
        j["privacy_accepted_at"] = Current.privacy_accepted_at;
        j["position"] = Current.position;
        j["show_running_indicator"] = Current.show_running_indicator;
        j["use_roman_numerals"] = Current.use_roman_numerals;
        j["use_rank_icons"] = Current.use_rank_icons;
        j["show_extra_playlists"] = Current.show_extra_playlists;
        j["rocket_league_stats_api_config_path"] = Current.rocket_league_stats_api_config_path;
        j["check_stats_api_config_on_startup"] = Current.check_stats_api_config_on_startup;
        j["ballchasing_token"] = EncryptToken(Current.ballchasing_token);
        j["auto_upload_replays"] = Current.auto_upload_replays;
        j["ballchasing_upload_notice_accepted"] = Current.ballchasing_upload_notice_accepted;
        j["ballchasing_visibility"] = Current.ballchasing_visibility;
        j["auto_save_replays"] = Current.auto_save_replays;
        j["key_save_replay"] = Current.key_save_replay;
        
        // Save session visibility flags
        j["show_session_card_in_game"] = Current.show_session_card_in_game;
        j["show_session_record"] = Current.show_session_record;
        j["show_session_goals"] = Current.show_session_goals;
        j["show_session_saves"] = Current.show_session_saves;
        j["show_session_demos"] = Current.show_session_demos;
        j["show_session_assists"] = Current.show_session_assists;
        j["show_session_goal_participation"] = Current.show_session_goal_participation;
        j["show_session_mmr_change"] = Current.show_session_mmr_change;
        j["show_session_boost"] = Current.show_session_boost;
        j["show_lobby_ranks_overlay"] = Current.show_lobby_ranks_overlay;
        j["show_lobby_rank_1v1"] = Current.show_lobby_rank_1v1;
        j["show_lobby_rank_2v2"] = Current.show_lobby_rank_2v2;
        j["show_lobby_rank_3v3"] = Current.show_lobby_rank_3v3;
        j["show_lobby_rank_casual"] = Current.show_lobby_rank_casual;
        j["show_lobby_rank_tourny"] = Current.show_lobby_rank_tourny;
        j["show_lobby_rank_hoops"] = Current.show_lobby_rank_hoops;
        j["show_lobby_rank_rumble"] = Current.show_lobby_rank_rumble;
        j["show_lobby_rank_dropshot"] = Current.show_lobby_rank_dropshot;
        j["show_lobby_rank_snowday"] = Current.show_lobby_rank_snowday;
        j["show_lobby_rank_heatseeker"] = Current.show_lobby_rank_heatseeker;
        j["show_account_wins_overlay"] = Current.show_account_wins_overlay;
        j["show_demo_tracker_overlay"] = Current.show_demo_tracker_overlay;
        j["show_previous_games_summary"] = Current.show_previous_games_summary;
        j["previous_games_limit"] = Current.previous_games_limit;
        j["show_streaks_stats"] = Current.show_streaks_stats;
        j["show_longest_loss_streak"] = Current.show_longest_loss_streak;
        j["show_gamemode_breakdown"] = Current.show_gamemode_breakdown;
        j["show_gamemode_record_1v1"] = Current.show_gamemode_record_1v1;
        j["show_gamemode_record_2v2"] = Current.show_gamemode_record_2v2;
        j["show_gamemode_record_3v3"] = Current.show_gamemode_record_3v3;
        j["gamemode_breakdown_scope"] = Current.gamemode_breakdown_scope;
        j["crossbar_display_mode"] = Current.crossbar_display_mode;
        j["second_monitor_mode"] = Current.second_monitor_mode;
        j["second_monitor_show_roster"] = Current.second_monitor_show_roster;
        j["second_monitor_show_session"] = Current.second_monitor_show_session;
        j["second_monitor_show_settings"] = Current.second_monitor_show_settings;
        j["second_monitor_has_bounds"] = Current.second_monitor_has_bounds;
        j["second_monitor_x"] = Current.second_monitor_x;
        j["second_monitor_y"] = Current.second_monitor_y;
        j["second_monitor_w"] = Current.second_monitor_w;
        j["second_monitor_h"] = Current.second_monitor_h;
        j["ui_scale"] = Current.ui_scale;
        j["overlay_fps_cap"] = Current.overlay_fps_cap;
        j["vsync"] = Current.vsync;
        nlohmann::json jl;
        jl["version"] = Current.dashboard_layout.version;
        jl["left_column_weight"] = Current.dashboard_layout.leftColumnWeight;
        nlohmann::json jw_arr = nlohmann::json::array();
        for (const auto& w : Current.dashboard_layout.widgets) {
            nlohmann::json jw;
            jw["id"] = DashboardLayout::ToConfigString(w.id);
            jw["zone"] = DashboardLayout::ToConfigString(w.zone);
            jw["order"] = w.order;
            jw["height"] = w.height;
            jw["collapsed"] = w.collapsed;
            jw_arr.push_back(jw);
        }
        jl["widgets"] = jw_arr;
        j["dashboard_layout"] = jl;

        nlohmann::json jol;
        jol["version"] = Current.overlay_layout.version;
        jol["toolbox_open"] = Current.overlay_layout.toolboxOpen;
        nlohmann::json jc_arr = nlohmann::json::array();
        for (const auto& c : Current.overlay_layout.containers) {
            nlohmann::json jc;
            jc["id"] = c.id;
            jc["x"] = c.x;
            jc["y"] = c.y;
            jc["w"] = c.w;
            jc["h"] = c.h;
            nlohmann::json jw_arr2 = nlohmann::json::array();
            for (auto w : c.widgets) {
                jw_arr2.push_back(DashboardLayout::ToConfigString(w));
            }
            jc["widgets"] = jw_arr2;
            jc_arr.push_back(jc);
        }
        jol["containers"] = jc_arr;
        j["overlay_layout"] = jol;
        
        // Theme colors
        j["theme"]["bg"]     = ColorToJson(Current.themeBg);
        j["theme"]["text"]   = ColorToJson(Current.themeText);
        j["theme"]["accent"] = ColorToJson(Current.themeAccent);
        j["theme"]["win"]    = ColorToJson(Current.themeWin);
        j["theme"]["loss"]   = ColorToJson(Current.themeLoss);
        j["theme"]["dim"]    = ColorToJson(Current.themeDim);
        j["theme"]["muted"]  = ColorToJson(Current.themeMuted);
        j["theme"]["graph_line"] = ColorToJson(Current.themeGraphLine);
        j["theme"]["graph_baseline"] = ColorToJson(Current.themeGraphBaseline);

        std::string configFile = GetConfigPath();
        std::string tempFile = configFile + ".tmp";
        std::ofstream file(tempFile, std::ios::trunc);
        if (file.is_open()) {
            file << j.dump(4);
            file.flush();
            file.close();
            if (!MoveFileExA(tempFile.c_str(), configFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                std::cout << "[Config] Failed to replace config.json. Error=" << GetLastError() << "\n";
            }
        } else {
            std::cout << "[Config] Failed to open temp config for writing.\n";
        }
    }

    void Save() {
        std::unique_lock<std::shared_mutex> lock(Mutex);
        SaveInternal();
    }

    void RequestSave() {
        s_pendingSave.store(true);
    }

    void SetWindowsAutoStart(bool enable) {
        HKEY hKey;
        const char* keyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            if (enable) {
                char exePath[MAX_PATH];
                if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
                    std::string pathStr = "\"" + std::string(exePath) + "\"";
                    RegSetValueExA(hKey, "OmniStats", 0, REG_SZ, (const BYTE*)pathStr.c_str(), (DWORD)(pathStr.length() + 1));
                }
            } else {
                RegDeleteValueA(hKey, "OmniStats");
            }
            RegCloseKey(hKey);
        }
    }
}
