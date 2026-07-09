#pragma once
#include <string>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// rpcndr.h (included transitively via windows.h) defines 'small' as a macro that
// collides with field names. Undefine it after windows.h is fully processed.
#ifdef small
#undef small
#endif
#include <shared_mutex>
#include <vector>
#include "core/DashboardLayoutConfig.hpp"
#include "core/OverlayLayoutConfig.hpp"

// Lightweight color struct matching ImVec4 layout — avoids pulling imgui.h into Config
struct ColorRGBA {
    float r, g, b, a;
};

// Holds all user preferences
struct ConfigData {
    std::string host = "127.0.0.1";
    int port = 49123;
    
    int key_overlay = VK_TAB;
    int key_cycle = VK_F6;
    int key_expand = VK_F7;
    int key_session = VK_F8;
    int key_menu = VK_F5;
    
    int gamepad_overlay = 4; // SDL_CONTROLLER_BUTTON_BACK
    bool gamepad_overlay_raw = false;
    int gamepad_overlay_raw_button = 4;
    
    std::string mmr_category = "best";
    std::string last_primary_id = "";
    std::vector<std::string> known_primary_ids; // previously used/seen accounts for faster identity detection
    
    bool require_rl_focus = true;
    bool show_match_summary = true;
    bool discord_rpc_enabled = false;
    bool enable_mmr_tracking = false;
    bool auto_switch_mmr_category = true;
    bool check_for_updates = false;
    bool enable_auto_updates = false;
    bool crash_reports_enabled = false;
    bool debug_logging = false;
    bool imperial_units = false;
    bool run_on_startup = false;
    bool reset_session_on_close = true;
    std::string client_uuid = "";
    std::string privacy_policy_accepted_version = "";
    std::string terms_accepted_version = "";
    std::string privacy_accepted_at = "";
    
    std::string position = "top-right";
    bool show_running_indicator = true;
    bool use_roman_numerals = true;
    bool use_rank_icons = false;
    bool show_extra_playlists = true;

    std::string rocket_league_stats_api_config_path = "";
    bool check_stats_api_config_on_startup = true;

    // Ballchasing uploader settings
    std::string ballchasing_token = ""; // API token for ballchasing.com
    bool auto_upload_replays = false;     // Toggle background auto-uploading of replays
    bool ballchasing_upload_notice_accepted = false;
    std::string ballchasing_visibility = "unlisted";
    
    // Replay auto-save settings
    bool auto_save_replays = false;       // Automatically trigger the "Save Replay" keybind mid-game
    int key_save_replay = VK_BACK;        // Key code for saving replays in Rocket League
    
    // Theme colors
    ColorRGBA themeBg      = { 0.05f, 0.06f, 0.08f, 0.90f };
    ColorRGBA themeText    = { 0.92f, 0.94f, 0.96f, 1.00f };
    ColorRGBA themeAccent  = { 1.00f, 0.00f, 0.13f, 1.00f };
    ColorRGBA themeWin     = { 0.2629031836986542f, 0.85f, 0.15f, 1.00f };
    ColorRGBA themeLoss    = { 1.00f, 0.012096762657165527f, 0.012096762657165527f, 1.00f };
    ColorRGBA themeDim     = { 0.45f, 0.50f, 0.55f, 0.60f };
    ColorRGBA themeMuted   = { 0.65f, 0.70f, 0.75f, 1.00f };
    ColorRGBA themeGraphLine = { 0.20201940834522247f, 0.36447927355766296f, 0.7056452035903931f, 1.00f };
    ColorRGBA themeGraphBaseline = { 0.913241446018219f, 0.9516128897666931f, 0.0f, 1.00f };

    // Session Card Visibility Toggles
    bool show_session_card_in_game = true;
    bool show_session_record = true;
    bool show_session_goals = true;
    bool show_session_saves = true;
    bool show_session_demos = true;
    bool show_session_assists = true;
    bool show_session_goal_participation = true;
    bool show_session_mmr_change = true;
    bool show_session_boost = true;
    bool show_lobby_ranks_overlay = false;
    bool show_lobby_rank_1v1 = true;
    bool show_lobby_rank_2v2 = true;
    bool show_lobby_rank_3v3 = true;
    bool show_lobby_rank_casual = true;
    bool show_lobby_rank_tourny = true;
    bool show_lobby_rank_hoops = false;
    bool show_lobby_rank_rumble = false;
    bool show_lobby_rank_dropshot = false;
    bool show_lobby_rank_snowday = false;
    bool show_lobby_rank_heatseeker = false;
    bool show_account_wins_overlay = true;
    bool show_demo_tracker_overlay = false;
    bool show_previous_games_summary = true;
    int previous_games_limit = 20;

    // Overlay Component Visibility Toggles
    bool show_streaks_stats = true;
    bool show_longest_loss_streak = false;
    bool show_gamemode_breakdown = true;
    bool show_gamemode_record_1v1 = true;
    bool show_gamemode_record_2v2 = true;
    bool show_gamemode_record_3v3 = true;
    std::string gamemode_breakdown_scope = "current_session";

    std::string crossbar_display_mode = "raw"; // "raw" or "speed"

    bool second_monitor_mode = false;
    bool second_monitor_show_roster = true;
    bool second_monitor_show_session = true;
    bool second_monitor_show_settings = true;
    bool second_monitor_has_bounds = false;
    int second_monitor_x = 0;
    int second_monitor_y = 0;
    int second_monitor_w = 1024;
    int second_monitor_h = 768;

    float ui_scale = 1.0f;

    int overlay_fps_cap = 60;
    bool vsync = false;
    
    DashboardLayout::LayoutConfig dashboard_layout = DashboardLayout::DefaultLayout();
    OverlayLayout::LayoutConfig overlay_layout = OverlayLayout::DefaultOverlayLayout();
};

#include <functional>
namespace Config {
    inline constexpr const char* CurrentPrivacyPolicyVersion = "2026-05-31";
    inline constexpr const char* CurrentTermsVersion = "2026-05-31";

    // Safely reads a point-in-time snapshot of the configuration under a shared lock
    ConfigData Read();

    // Safely updates the configuration under an exclusive lock and saves to disk
    void Update(std::function<void(ConfigData&)> fn, bool saveToDisk = true);

    // Loads data/config.json from disk (or creates it with defaults if missing)
    void Load();

    // Initializes the background thread for saving config to disk asynchronously
    void InitSaver();

    // Shuts down the background save thread safely
    void ShutdownSaver();

    // Saves the current settings back to data/config.json
    void Save();

    // Requests an asynchronous save without blocking the caller
    void RequestSave();

    // Sets or removes the Windows registry key for running on startup
    void SetWindowsAutoStart(bool enable);
}
