#include "core/config/Config.hpp"
#include "core/config/SecretStore.hpp"
#include "utils/Storage.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

namespace {

    static bool IsKnownMmrCategory(const std::string& category) {
        return category == "best" || category == "1v1" ||
               category == "2v2" || category == "3v3" ||
               category == "casual" || category == "t" ||
               category == "hoops" || category == "rumble" ||
               category == "dropshot" || category == "snowday" ||
               category == "heatseeker";
    }

    static bool IsGraphMmrCategory(const std::string& category) {
        return category != "best" && IsKnownMmrCategory(category);
    }

    static bool IsExtraMmrCategory(const std::string& category) {
        return category == "hoops" || category == "rumble" ||
               category == "dropshot" || category == "snowday" ||
               category == "heatseeker";
    }

    static bool IsValidBallchasingVisibility(const std::string& visibility) {
        return visibility == "private" || visibility == "unlisted" || visibility == "public";
    }

    static bool IsValidTournamentRegion(const std::string& region) {
        return region == "auto" || region == "US" || region == "EU" || region == "OCE" ||
               region == "SAM" || region == "ASIA" || region == "ME" || region == "SAF";
    }

    static bool IsValidTournamentReminderMinutes(int minutes) {
        return minutes == 0 || minutes == 5 || minutes == 10 || minutes == 15;
    }

    static nlohmann::json ColorToJson(const ColorRGBA& c) {
        return nlohmann::json::array({c.r, c.g, c.b, c.a});
    }

    static ColorRGBA JsonToColor(const nlohmann::json& j, const ColorRGBA& fallback) {
        if (!j.is_array() || j.size() < 4) return fallback;
        for (size_t i = 0; i < 4; ++i) {
            if (!j[i].is_number()) return fallback;
            const double component = j[i].get<double>();
            if (!std::isfinite(component) || component < 0.0 || component > 1.0) return fallback;
        }
        return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>()};
    }

    static void ReadContainerVisibility(const nlohmann::json& jv, OverlayLayout::ContainerConfig::Visibility& vis) {
        if (jv.contains("mode")) {
            if (jv["mode"].is_string()) {
                vis.mode = OverlayLayout::VisibilityModeFromConfigString(jv["mode"].get<std::string>());
            } else if (jv["mode"].is_number_integer()) {
                vis.mode = static_cast<OverlayLayout::VisibilityMode>(jv["mode"].get<int>());
            } else {
                vis.mode = OverlayLayout::Visibility::KeyHeld;
            }
        }
        if (jv.contains("event")) {
            if (jv["event"].is_string()) {
                vis.event = OverlayLayout::VisibilityEventFromConfigString(jv["event"].get<std::string>());
            } else if (jv["event"].is_number_integer()) {
                vis.event = static_cast<OverlayLayout::VisibilityEvent>(jv["event"].get<int>());
            } else {
                vis.event = OverlayLayout::Visibility::FirstCountdown;
            }
        }
        if (jv.contains("seconds") && jv["seconds"].is_number_integer()) {
            vis.seconds = jv["seconds"].get<int>();
        }
        if (jv.contains("also_while_key_held") && jv["also_while_key_held"].is_boolean()) {
            vis.alsoWhileKeyHeld = jv["also_while_key_held"].get<bool>();
        } else if (jv.contains("alsoWhileKeyHeld") && jv["alsoWhileKeyHeld"].is_boolean()) {
            vis.alsoWhileKeyHeld = jv["alsoWhileKeyHeld"].get<bool>();
        }
        if (jv.contains("hide_during_replay") && jv["hide_during_replay"].is_boolean()) {
            vis.hideDuringReplay = jv["hide_during_replay"].get<bool>();
        } else if (jv.contains("hideDuringReplay") && jv["hideDuringReplay"].is_boolean()) {
            vis.hideDuringReplay = jv["hideDuringReplay"].get<bool>();
        }
        OverlayLayout::Sanitize(vis);
    }

    static nlohmann::json ContainerVisibilityToJson(const OverlayLayout::ContainerConfig::Visibility& vis) {
        auto sanitized = vis;
        OverlayLayout::Sanitize(sanitized);
        return {
            {"mode", OverlayLayout::ToConfigString(sanitized.mode)},
            {"event", OverlayLayout::ToConfigString(sanitized.event)},
            {"seconds", sanitized.seconds},
            {"also_while_key_held", sanitized.alsoWhileKeyHeld},
            {"hide_during_replay", sanitized.hideDuringReplay}};
    }

    struct FieldDescriptor {
        std::string key;
        std::function<void(const nlohmann::json&, ConfigData&, SecretStore&, bool& needsSave, bool& needsLegacyMigration)> load;
        std::function<void(nlohmann::json&, const ConfigData&, SecretStore&)> save;
    };

    static FieldDescriptor MakeBoolField(const char* key, bool ConfigData::* member, bool defaultVal) {
        return {
            key,
            [key, member, defaultVal](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                if (j.contains(key) && j[key].is_boolean()) {
                    cfg.*member = j[key].get<bool>();
                } else {
                    cfg.*member = defaultVal;
                }
            },
            [key, member](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                j[key] = cfg.*member;
            }};
    }

    static FieldDescriptor MakeIntField(const char* key, int ConfigData::* member, int defaultVal,
                                        std::optional<int> minVal = std::nullopt,
                                        std::optional<int> maxVal = std::nullopt,
                                        bool clamp = false) {
        return {
            key,
            [key, member, defaultVal, minVal, maxVal, clamp](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool& needsSave, bool&) {
                if (j.contains(key) && j[key].is_number_integer()) {
                    const long double num = j[key].get<long double>();
                    if (std::isfinite(num) && num >= (std::numeric_limits<int>::lowest)() && num <= (std::numeric_limits<int>::max)()) {
                        const int val = static_cast<int>(num);
                        if (clamp && minVal && maxVal) {
                            const int clamped = std::clamp(val, *minVal, *maxVal);
                            if (clamped != val) needsSave = true;
                            cfg.*member = clamped;
                            return;
                        }
                        if (minVal && val < *minVal) {
                            cfg.*member = defaultVal;
                            return;
                        }
                        if (maxVal && val > *maxVal) {
                            cfg.*member = defaultVal;
                            return;
                        }
                        cfg.*member = val;
                        return;
                    }
                }
                cfg.*member = defaultVal;
            },
            [key, member](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                j[key] = cfg.*member;
            }};
    }

    static FieldDescriptor MakeFloatField(const char* key, float ConfigData::* member, float defaultVal,
                                          std::optional<float> minVal = std::nullopt,
                                          std::optional<float> maxVal = std::nullopt,
                                          bool clamp = false) {
        return {
            key,
            [key, member, defaultVal, minVal, maxVal, clamp](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool& needsSave, bool&) {
                if (j.contains(key) && j[key].is_number()) {
                    const long double num = j[key].get<long double>();
                    if (std::isfinite(num) && num >= (std::numeric_limits<float>::lowest)() && num <= (std::numeric_limits<float>::max)()) {
                        const float val = static_cast<float>(num);
                        if (clamp && minVal && maxVal) {
                            const float clamped = std::clamp(val, *minVal, *maxVal);
                            if (clamped != val) needsSave = true;
                            cfg.*member = clamped;
                            return;
                        }
                        if (minVal && val < *minVal) {
                            cfg.*member = defaultVal;
                            return;
                        }
                        if (maxVal && val > *maxVal) {
                            cfg.*member = defaultVal;
                            return;
                        }
                        cfg.*member = val;
                        return;
                    }
                }
                cfg.*member = defaultVal;
            },
            [key, member](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                j[key] = cfg.*member;
            }};
    }

    static FieldDescriptor MakeStringField(const char* key, std::string ConfigData::* member, std::string defaultVal,
                                           std::function<bool(const std::string&)> validator = nullptr) {
        return {
            key,
            [key, member, defaultVal, validator](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool& needsSave, bool&) {
                if (j.contains(key) && j[key].is_string()) {
                    std::string val = j[key].get<std::string>();
                    if (!validator || validator(val)) {
                        cfg.*member = std::move(val);
                        return;
                    }
                    needsSave = true;
                }
                cfg.*member = defaultVal;
            },
            [key, member](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                j[key] = cfg.*member;
            }};
    }

    static FieldDescriptor MakeSecretField(const char* key, std::string ConfigData::* member) {
        return {
            key,
            [key, member](const nlohmann::json& j, ConfigData& cfg, SecretStore& secrets, bool&, bool& needsLegacyMigration) {
                if (j.contains(key) && j[key].is_string()) {
                    secrets.LoadSecretField(key, j[key].get<std::string>(), cfg.*member, needsLegacyMigration, cfg.credentials_decryption_failed);
                } else {
                    std::string empty;
                    secrets.LoadSecretField(key, "", empty, needsLegacyMigration, cfg.credentials_decryption_failed);
                    (cfg.*member).clear();
                }
            },
            [key, member](nlohmann::json& j, const ConfigData& cfg, SecretStore& secrets) {
                std::string diskVal;
                secrets.SaveSecretField(key, cfg.*member, diskVal);
                j[key] = diskVal;
            }};
    }

    static FieldDescriptor MakeThemeColorField(const char* themeKey, ColorRGBA ConfigData::* member, ColorRGBA defaultVal) {
        return {
            std::string("theme.") + themeKey,
            [themeKey, member, defaultVal](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                if (j.contains("theme") && j["theme"].is_object() && j["theme"].contains(themeKey)) {
                    cfg.*member = JsonToColor(j["theme"][themeKey], defaultVal);
                } else {
                    cfg.*member = defaultVal;
                }
            },
            [themeKey, member](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                j["theme"][themeKey] = ColorToJson(cfg.*member);
            }};
    }

    static const std::vector<FieldDescriptor>& GetFieldDescriptors() {
        static const ConfigData def{};
        static const std::vector<FieldDescriptor> descriptors = {
            MakeStringField("host", &ConfigData::host, def.host),
            MakeIntField("port", &ConfigData::port, def.port),
            MakeIntField("key_overlay", &ConfigData::key_overlay, def.key_overlay),
            MakeIntField("key_cycle", &ConfigData::key_cycle, def.key_cycle),
            MakeIntField("key_expand", &ConfigData::key_expand, def.key_expand),
            MakeIntField("key_session", &ConfigData::key_session, def.key_session),
            MakeIntField("key_menu", &ConfigData::key_menu, def.key_menu),
            MakeIntField("key_graph_pan_left", &ConfigData::key_graph_pan_left, def.key_graph_pan_left),
            MakeIntField("key_graph_pan_right", &ConfigData::key_graph_pan_right, def.key_graph_pan_right),
            MakeIntField("key_lifetime_graph", &ConfigData::key_lifetime_graph, def.key_lifetime_graph),

            MakeIntField("gamepad_overlay", &ConfigData::gamepad_overlay, def.gamepad_overlay),
            MakeBoolField("gamepad_overlay_raw", &ConfigData::gamepad_overlay_raw, def.gamepad_overlay_raw),
            MakeIntField("gamepad_overlay_raw_button", &ConfigData::gamepad_overlay_raw_button, def.gamepad_overlay_raw_button),
            MakeIntField("gamepad_cycle", &ConfigData::gamepad_cycle, def.gamepad_cycle),
            MakeBoolField("gamepad_cycle_raw", &ConfigData::gamepad_cycle_raw, def.gamepad_cycle_raw),
            MakeIntField("gamepad_cycle_raw_button", &ConfigData::gamepad_cycle_raw_button, def.gamepad_cycle_raw_button),
            MakeIntField("gamepad_expand", &ConfigData::gamepad_expand, def.gamepad_expand),
            MakeBoolField("gamepad_expand_raw", &ConfigData::gamepad_expand_raw, def.gamepad_expand_raw),
            MakeIntField("gamepad_expand_raw_button", &ConfigData::gamepad_expand_raw_button, def.gamepad_expand_raw_button),
            MakeIntField("gamepad_session", &ConfigData::gamepad_session, def.gamepad_session),
            MakeBoolField("gamepad_session_raw", &ConfigData::gamepad_session_raw, def.gamepad_session_raw),
            MakeIntField("gamepad_session_raw_button", &ConfigData::gamepad_session_raw_button, def.gamepad_session_raw_button),
            MakeIntField("gamepad_menu", &ConfigData::gamepad_menu, def.gamepad_menu),
            MakeBoolField("gamepad_menu_raw", &ConfigData::gamepad_menu_raw, def.gamepad_menu_raw),
            MakeIntField("gamepad_menu_raw_button", &ConfigData::gamepad_menu_raw_button, def.gamepad_menu_raw_button),
            MakeIntField("gamepad_graph_pan_left", &ConfigData::gamepad_graph_pan_left, def.gamepad_graph_pan_left),
            MakeBoolField("gamepad_graph_pan_left_raw", &ConfigData::gamepad_graph_pan_left_raw, def.gamepad_graph_pan_left_raw),
            MakeIntField("gamepad_graph_pan_left_raw_button", &ConfigData::gamepad_graph_pan_left_raw_button, def.gamepad_graph_pan_left_raw_button),
            MakeIntField("gamepad_graph_pan_right", &ConfigData::gamepad_graph_pan_right, def.gamepad_graph_pan_right),
            MakeBoolField("gamepad_graph_pan_right_raw", &ConfigData::gamepad_graph_pan_right_raw, def.gamepad_graph_pan_right_raw),
            MakeIntField("gamepad_graph_pan_right_raw_button", &ConfigData::gamepad_graph_pan_right_raw_button, def.gamepad_graph_pan_right_raw_button),

            MakeStringField("mmr_category", &ConfigData::mmr_category, def.mmr_category, IsKnownMmrCategory),
            MakeStringField("graph_mmr_category", &ConfigData::graph_mmr_category, def.graph_mmr_category, IsGraphMmrCategory),
            MakeBoolField("graph_follow_current_playlist", &ConfigData::graph_follow_current_playlist, def.graph_follow_current_playlist),
            MakeStringField("rank_progress_mmr_category", &ConfigData::rank_progress_mmr_category, def.rank_progress_mmr_category, IsKnownMmrCategory),
            MakeBoolField("rank_progress_follow_current_playlist", &ConfigData::rank_progress_follow_current_playlist, def.rank_progress_follow_current_playlist),
            MakeStringField("last_primary_id", &ConfigData::last_primary_id, def.last_primary_id),

            {"known_primary_ids",
             [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                 cfg.known_primary_ids.clear();
                 if (j.contains("known_primary_ids") && j["known_primary_ids"].is_array()) {
                     for (const auto& item : j["known_primary_ids"]) {
                         if (item.is_string()) cfg.known_primary_ids.push_back(item.get<std::string>());
                     }
                 }
             },
             [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                 j["known_primary_ids"] = cfg.known_primary_ids;
             }},

            MakeBoolField("require_rl_focus", &ConfigData::require_rl_focus, def.require_rl_focus),
            MakeBoolField("show_match_summary", &ConfigData::show_match_summary, def.show_match_summary),
            MakeIntField("match_summary_seconds", &ConfigData::match_summary_seconds, def.match_summary_seconds, 5, 60, true),
            MakeBoolField("discord_rpc_enabled", &ConfigData::discord_rpc_enabled, def.discord_rpc_enabled),
            MakeBoolField("enable_mmr_tracking", &ConfigData::enable_mmr_tracking, def.enable_mmr_tracking),
            MakeBoolField("auto_switch_mmr_category", &ConfigData::auto_switch_mmr_category, def.auto_switch_mmr_category),
            MakeBoolField("enable_auto_updates", &ConfigData::enable_auto_updates, def.enable_auto_updates),
            MakeBoolField("crash_reports_enabled", &ConfigData::crash_reports_enabled, def.crash_reports_enabled),
            MakeBoolField("debug_logging", &ConfigData::debug_logging, def.debug_logging),
            MakeBoolField("imperial_units", &ConfigData::imperial_units, def.imperial_units),
            MakeBoolField("run_on_startup", &ConfigData::run_on_startup, def.run_on_startup),
            MakeBoolField("reset_session_on_close", &ConfigData::reset_session_on_close, def.reset_session_on_close),
            MakeBoolField("show_session_recap_on_close", &ConfigData::show_session_recap_on_close, def.show_session_recap_on_close),
            MakeStringField("client_uuid", &ConfigData::client_uuid, def.client_uuid),
            MakeStringField("privacy_policy_accepted_version", &ConfigData::privacy_policy_accepted_version, def.privacy_policy_accepted_version),
            MakeStringField("terms_accepted_version", &ConfigData::terms_accepted_version, def.terms_accepted_version),
            MakeStringField("privacy_accepted_at", &ConfigData::privacy_accepted_at, def.privacy_accepted_at),
            MakeBoolField("onboarding_completed", &ConfigData::onboarding_completed, def.onboarding_completed),
            MakeBoolField("account_link_prompt_shown", &ConfigData::account_link_prompt_shown, def.account_link_prompt_shown),

            MakeStringField("position", &ConfigData::position, def.position),
            MakeBoolField("show_running_indicator", &ConfigData::show_running_indicator, def.show_running_indicator),
            MakeBoolField("use_roman_numerals", &ConfigData::use_roman_numerals, def.use_roman_numerals),
            MakeBoolField("use_rank_icons", &ConfigData::use_rank_icons, def.use_rank_icons),
            MakeBoolField("show_extra_playlists", &ConfigData::show_extra_playlists, def.show_extra_playlists),

            MakeBoolField("custom_api_enabled", &ConfigData::custom_api_enabled, def.custom_api_enabled),
            MakeSecretField("custom_api_key", &ConfigData::custom_api_key),

            MakeStringField("account_signed_in_name", &ConfigData::account_signed_in_name, def.account_signed_in_name),
            MakeStringField("account_device_public_id", &ConfigData::account_device_public_id, def.account_device_public_id),
            MakeSecretField("account_refresh_token", &ConfigData::account_refresh_token),
            MakeSecretField("account_device_key", &ConfigData::account_device_key),
            MakeStringField("rocket_league_stats_api_config_path", &ConfigData::rocket_league_stats_api_config_path, def.rocket_league_stats_api_config_path),
            MakeBoolField("check_stats_api_config_on_startup", &ConfigData::check_stats_api_config_on_startup, def.check_stats_api_config_on_startup),

            MakeSecretField("ballchasing_token", &ConfigData::ballchasing_token),
            MakeBoolField("auto_upload_replays", &ConfigData::auto_upload_replays, def.auto_upload_replays),
            MakeBoolField("ballchasing_upload_notice_accepted", &ConfigData::ballchasing_upload_notice_accepted, def.ballchasing_upload_notice_accepted),
            MakeStringField("ballchasing_visibility", &ConfigData::ballchasing_visibility, def.ballchasing_visibility, IsValidBallchasingVisibility),
            MakeBoolField("ballchasing_filter_ranked_only", &ConfigData::ballchasing_filter_ranked_only, def.ballchasing_filter_ranked_only),
            MakeBoolField("ballchasing_filter_wins_only", &ConfigData::ballchasing_filter_wins_only, def.ballchasing_filter_wins_only),
            MakeBoolField("auto_save_replays", &ConfigData::auto_save_replays, def.auto_save_replays),
            MakeIntField("key_save_replay", &ConfigData::key_save_replay, def.key_save_replay),

            MakeThemeColorField("bg", &ConfigData::themeBg, def.themeBg),
            MakeThemeColorField("settings_panel", &ConfigData::themeSettingsPanel, def.themeSettingsPanel),
            MakeThemeColorField("topbar", &ConfigData::themeTopbar, def.themeTopbar),
            MakeThemeColorField("graph_panel", &ConfigData::themeGraphPanel, def.themeGraphPanel),
            MakeThemeColorField("text", &ConfigData::themeText, def.themeText),
            MakeThemeColorField("accent", &ConfigData::themeAccent, def.themeAccent),
            MakeThemeColorField("win", &ConfigData::themeWin, def.themeWin),
            MakeThemeColorField("loss", &ConfigData::themeLoss, def.themeLoss),
            MakeThemeColorField("dim", &ConfigData::themeDim, def.themeDim),
            MakeThemeColorField("muted", &ConfigData::themeMuted, def.themeMuted),
            MakeThemeColorField("graph_line", &ConfigData::themeGraphLine, def.themeGraphLine),
            MakeThemeColorField("graph_baseline", &ConfigData::themeGraphBaseline, def.themeGraphBaseline),
            MakeThemeColorField("roster_card", &ConfigData::themeRosterCard, def.themeRosterCard),
            MakeThemeColorField("roster_card_self", &ConfigData::themeRosterCardSelf, def.themeRosterCardSelf),
            MakeThemeColorField("scout_tag", &ConfigData::themeScoutTag, def.themeScoutTag),
            MakeThemeColorField("stat_box", &ConfigData::themeStatBox, def.themeStatBox),
            MakeThemeColorField("match_row", &ConfigData::themeMatchRow, def.themeMatchRow),
            MakeThemeColorField("match_row_alt", &ConfigData::themeMatchRowAlt, def.themeMatchRowAlt),

            MakeBoolField("show_session_record", &ConfigData::show_session_record, def.show_session_record),
            MakeBoolField("show_session_goals", &ConfigData::show_session_goals, def.show_session_goals),
            MakeBoolField("show_session_saves", &ConfigData::show_session_saves, def.show_session_saves),
            MakeBoolField("show_session_demos", &ConfigData::show_session_demos, def.show_session_demos),
            MakeBoolField("show_session_assists", &ConfigData::show_session_assists, def.show_session_assists),
            MakeBoolField("show_session_goal_participation", &ConfigData::show_session_goal_participation, def.show_session_goal_participation),
            MakeBoolField("show_session_mmr_change", &ConfigData::show_session_mmr_change, def.show_session_mmr_change),
            MakeBoolField("show_session_boost", &ConfigData::show_session_boost, def.show_session_boost),
            MakeBoolField("show_lobby_ranks_overlay", &ConfigData::show_lobby_ranks_overlay, def.show_lobby_ranks_overlay),
            MakeBoolField("show_lobby_rank_1v1", &ConfigData::show_lobby_rank_1v1, def.show_lobby_rank_1v1),
            MakeBoolField("show_lobby_rank_2v2", &ConfigData::show_lobby_rank_2v2, def.show_lobby_rank_2v2),
            MakeBoolField("show_lobby_rank_3v3", &ConfigData::show_lobby_rank_3v3, def.show_lobby_rank_3v3),
            MakeBoolField("show_lobby_rank_casual", &ConfigData::show_lobby_rank_casual, def.show_lobby_rank_casual),
            MakeBoolField("show_lobby_rank_tourny", &ConfigData::show_lobby_rank_tourny, def.show_lobby_rank_tourny),
            MakeBoolField("show_lobby_rank_hoops", &ConfigData::show_lobby_rank_hoops, def.show_lobby_rank_hoops),
            MakeBoolField("show_lobby_rank_rumble", &ConfigData::show_lobby_rank_rumble, def.show_lobby_rank_rumble),
            MakeBoolField("show_lobby_rank_dropshot", &ConfigData::show_lobby_rank_dropshot, def.show_lobby_rank_dropshot),
            MakeBoolField("show_lobby_rank_snowday", &ConfigData::show_lobby_rank_snowday, def.show_lobby_rank_snowday),
            MakeBoolField("show_lobby_rank_heatseeker", &ConfigData::show_lobby_rank_heatseeker, def.show_lobby_rank_heatseeker),
            MakeBoolField("show_account_wins_overlay", &ConfigData::show_account_wins_overlay, def.show_account_wins_overlay),
            MakeBoolField("show_demo_tracker_overlay", &ConfigData::show_demo_tracker_overlay, def.show_demo_tracker_overlay),
            MakeBoolField("show_rank_progress", &ConfigData::show_rank_progress, def.show_rank_progress),
            MakeBoolField("show_previous_games_summary", &ConfigData::show_previous_games_summary, def.show_previous_games_summary),
            MakeIntField("previous_games_limit", &ConfigData::previous_games_limit, def.previous_games_limit, 10, 50, true),

            MakeBoolField("show_tournaments", &ConfigData::show_tournaments, def.show_tournaments),
            MakeStringField("tournament_region", &ConfigData::tournament_region, def.tournament_region, IsValidTournamentRegion),
            MakeIntField("tournament_reminder_minutes", &ConfigData::tournament_reminder_minutes, def.tournament_reminder_minutes, 0, 15, false),
            MakeBoolField("tournament_reminder_standard_only", &ConfigData::tournament_reminder_standard_only, def.tournament_reminder_standard_only),
            MakeBoolField("tournament_sound_cue", &ConfigData::tournament_sound_cue, def.tournament_sound_cue),

            MakeBoolField("show_streaks_stats", &ConfigData::show_streaks_stats, def.show_streaks_stats),
            MakeBoolField("show_longest_loss_streak", &ConfigData::show_longest_loss_streak, def.show_longest_loss_streak),
            MakeBoolField("show_gamemode_breakdown", &ConfigData::show_gamemode_breakdown, def.show_gamemode_breakdown),
            MakeBoolField("show_gamemode_record_1v1", &ConfigData::show_gamemode_record_1v1, def.show_gamemode_record_1v1),
            MakeBoolField("show_gamemode_record_2v2", &ConfigData::show_gamemode_record_2v2, def.show_gamemode_record_2v2),
            MakeBoolField("show_gamemode_record_3v3", &ConfigData::show_gamemode_record_3v3, def.show_gamemode_record_3v3),
            MakeStringField("gamemode_breakdown_scope", &ConfigData::gamemode_breakdown_scope, def.gamemode_breakdown_scope),
            MakeStringField("crossbar_display_mode", &ConfigData::crossbar_display_mode, def.crossbar_display_mode),

            MakeBoolField("second_monitor_mode", &ConfigData::second_monitor_mode, def.second_monitor_mode),
            MakeBoolField("second_monitor_show_roster", &ConfigData::second_monitor_show_roster, def.second_monitor_show_roster),
            MakeBoolField("second_monitor_show_session", &ConfigData::second_monitor_show_session, def.second_monitor_show_session),
            MakeBoolField("second_monitor_show_settings", &ConfigData::second_monitor_show_settings, def.second_monitor_show_settings),
            MakeBoolField("second_monitor_has_bounds", &ConfigData::second_monitor_has_bounds, def.second_monitor_has_bounds),
            MakeIntField("second_monitor_x", &ConfigData::second_monitor_x, def.second_monitor_x),
            MakeIntField("second_monitor_y", &ConfigData::second_monitor_y, def.second_monitor_y),
            MakeIntField("second_monitor_w", &ConfigData::second_monitor_w, def.second_monitor_w),
            MakeIntField("second_monitor_h", &ConfigData::second_monitor_h, def.second_monitor_h),

            MakeFloatField("ui_scale", &ConfigData::ui_scale, def.ui_scale),
            MakeStringField("font_ui", &ConfigData::font_ui, def.font_ui),
            MakeStringField("font_mono", &ConfigData::font_mono, def.font_mono),
            MakeStringField("font_display", &ConfigData::font_display, def.font_display),

            MakeFloatField("session_view_x", &ConfigData::session_view_x, def.session_view_x),
            MakeFloatField("session_view_y", &ConfigData::session_view_y, def.session_view_y),
            MakeFloatField("match_summary_x", &ConfigData::match_summary_x, def.match_summary_x),
            MakeFloatField("match_summary_y", &ConfigData::match_summary_y, def.match_summary_y),

            MakeIntField("overlay_fps_cap", &ConfigData::overlay_fps_cap, def.overlay_fps_cap),
            MakeBoolField("vsync", &ConfigData::vsync, def.vsync),

            {"dashboard_layout",
             [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                 if (!j.contains("dashboard_layout") || !j["dashboard_layout"].is_object()) return;
                 const auto& jl = j["dashboard_layout"];
                 if (jl.contains("version") && jl["version"].is_number_integer()) cfg.dashboard_layout.version = jl["version"].get<int>();
                 if (jl.contains("left_column_weight") && jl["left_column_weight"].is_number()) cfg.dashboard_layout.leftColumnWeight = jl["left_column_weight"].get<float>();
                 if (jl.contains("widgets") && jl["widgets"].is_array()) {
                     cfg.dashboard_layout.widgets.clear();
                     for (const auto& jw : jl["widgets"]) {
                         if (!jw.is_object()) continue;
                         DashboardLayout::WidgetPlacement w;
                         if (jw.contains("id") && jw["id"].is_string()) w.id = DashboardLayout::WidgetIdFromConfigString(jw["id"].get<std::string>());
                         if (jw.contains("zone") && jw["zone"].is_string()) w.zone = DashboardLayout::ZoneFromConfigString(jw["zone"].get<std::string>());
                         if (jw.contains("order") && jw["order"].is_number_integer()) w.order = jw["order"].get<int>();
                         if (jw.contains("height") && jw["height"].is_number()) w.height = jw["height"].get<float>();
                         if (jw.contains("collapsed") && jw["collapsed"].is_boolean()) w.collapsed = jw["collapsed"].get<bool>();
                         if (jw.contains("enabled") && jw["enabled"].is_boolean()) w.enabled = jw["enabled"].get<bool>();
                         cfg.dashboard_layout.widgets.push_back(w);
                     }
                 }
                 DashboardLayout::Sanitize(cfg.dashboard_layout);
             },
             [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                 nlohmann::json jl;
                 jl["version"] = cfg.dashboard_layout.version;
                 jl["left_column_weight"] = cfg.dashboard_layout.leftColumnWeight;
                 nlohmann::json jw_arr = nlohmann::json::array();
                 for (const auto& w : cfg.dashboard_layout.widgets) {
                     jw_arr.push_back({{"id", DashboardLayout::ToConfigString(w.id)},
                                       {"zone", DashboardLayout::ToConfigString(w.zone)},
                                       {"order", w.order},
                                       {"height", w.height},
                                       {"collapsed", w.collapsed},
                                       {"enabled", w.enabled}});
                 }
                 jl["widgets"] = jw_arr;
                 j["dashboard_layout"] = jl;
             }},

            {"overlay_layout",
             [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                 if (!j.contains("overlay_layout") || !j["overlay_layout"].is_object()) return;
                 const auto& jl = j["overlay_layout"];
                 if (jl.contains("version") && jl["version"].is_number_integer()) cfg.overlay_layout.version = jl["version"].get<int>();
                 if (jl.contains("toolbox_open") && jl["toolbox_open"].is_boolean()) cfg.overlay_layout.toolboxOpen = jl["toolbox_open"].get<bool>();
                 cfg.overlay_layout.disabledWidgets.clear();
                 if (jl.contains("disabled_widgets") && jl["disabled_widgets"].is_array()) {
                     for (const auto& widget : jl["disabled_widgets"]) {
                         if (widget.is_string()) cfg.overlay_layout.disabledWidgets.push_back(DashboardLayout::WidgetIdFromConfigString(widget.get<std::string>()));
                     }
                 }
                 if (jl.contains("containers") && jl["containers"].is_array()) {
                     cfg.overlay_layout.containers.clear();
                     for (const auto& jc : jl["containers"]) {
                         if (!jc.is_object()) continue;
                         OverlayLayout::ContainerConfig c;
                         if (jc.contains("id") && jc["id"].is_string()) c.id = jc["id"].get<std::string>();
                         if (jc.contains("x") && jc["x"].is_number()) c.x = jc["x"].get<float>();
                         if (jc.contains("y") && jc["y"].is_number()) c.y = jc["y"].get<float>();
                         if (jc.contains("w") && jc["w"].is_number()) c.w = jc["w"].get<float>();
                         if (jc.contains("h") && jc["h"].is_number()) c.h = jc["h"].get<float>();
                         if (jc.contains("widgets") && jc["widgets"].is_array()) {
                             for (const auto& jw : jc["widgets"]) {
                                 if (jw.is_string()) c.widgets.push_back(DashboardLayout::WidgetIdFromConfigString(jw.get<std::string>()));
                             }
                         }
                         if (jc.contains("visibility") && jc["visibility"].is_object()) {
                             ReadContainerVisibility(jc["visibility"], c.visibility);
                         }
                         cfg.overlay_layout.containers.push_back(c);
                     }
                 }
                 cfg.overlay_layout.widgetVisibility.clear();
                 if (jl.contains("widget_visibility") && jl["widget_visibility"].is_object()) {
                     for (const auto& [key, jv] : jl["widget_visibility"].items()) {
                         if (!jv.is_object()) continue;
                         const auto wid = DashboardLayout::WidgetIdFromConfigString(key);
                         OverlayLayout::ContainerConfig::Visibility vis;
                         ReadContainerVisibility(jv, vis);
                         cfg.overlay_layout.widgetVisibility[wid] = vis;
                     }
                 }
                 OverlayLayout::Sanitize(cfg.overlay_layout);
             },
             [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                 nlohmann::json jol;
                 jol["version"] = cfg.overlay_layout.version;
                 jol["toolbox_open"] = cfg.overlay_layout.toolboxOpen;
                 jol["disabled_widgets"] = nlohmann::json::array();
                 for (auto widget : cfg.overlay_layout.disabledWidgets)
                     jol["disabled_widgets"].push_back(DashboardLayout::ToConfigString(widget));
                 nlohmann::json jwv = nlohmann::json::object();
                 for (const auto& [wid, vis] : cfg.overlay_layout.widgetVisibility) {
                     jwv[DashboardLayout::ToConfigString(wid)] = ContainerVisibilityToJson(vis);
                 }
                 jol["widget_visibility"] = jwv;
                 nlohmann::json jc_arr = nlohmann::json::array();
                 for (const auto& c : cfg.overlay_layout.containers) {
                     nlohmann::json jc;
                     jc["id"] = c.id;
                     jc["x"] = c.x;
                     jc["y"] = c.y;
                     jc["w"] = c.w;
                     jc["h"] = c.h;
                     nlohmann::json jw_arr2 = nlohmann::json::array();
                     for (auto w : c.widgets)
                         jw_arr2.push_back(DashboardLayout::ToConfigString(w));
                     jc["widgets"] = jw_arr2;
                     jc["visibility"] = ContainerVisibilityToJson(c.visibility);
                     jc_arr.push_back(jc);
                 }
                 jol["containers"] = jc_arr;
                 j["overlay_layout"] = jol;
             }}};
        return descriptors;
    }

} // namespace

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

    ConfigData Read() {
        std::shared_lock<std::shared_mutex> lock(Mutex);
        return Current;
    }

    uint64_t Revision() {
        return s_revision.load(std::memory_order_acquire);
    }

    static void SaveInternal();

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
                    {
                        std::unique_lock<std::shared_mutex> configLock(Mutex);
                        SaveInternal();
                    }
                    lock.lock();
                }
            }
        });
    }

    void ShutdownSaver() {
        if (!s_saverRunning.exchange(false)) return;
        s_saverCv.notify_all();
        if (s_saveThread.joinable()) {
            s_saveThread.request_stop();
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

        if (!Current.show_lobby_rank_1v1 && !Current.show_lobby_rank_2v2 &&
            !Current.show_lobby_rank_3v3 && !Current.show_lobby_rank_casual && !Current.show_lobby_rank_tourny &&
            !Current.show_lobby_rank_hoops && !Current.show_lobby_rank_rumble && !Current.show_lobby_rank_dropshot &&
            !Current.show_lobby_rank_snowday && !Current.show_lobby_rank_heatseeker) {
            Current.show_lobby_rank_2v2 = true;
        }

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
            s_pendingSave.store(true);
            s_saverCv.notify_one();
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
            Current = ConfigData{};
            s_secretStore.Reset();
            SaveInternal();
            s_revision.fetch_add(1, std::memory_order_release);
            return;
        }

        try {
            nlohmann::json j;
            {
                std::ifstream file(configFile);
                file >> j;
            }
            if (!j.is_object()) throw std::invalid_argument("Configuration must be a JSON object");

            Current = ConfigData{};
            s_secretStore.Reset();
            bool needsSave = false;
            bool needsLegacyMigration = false;

            const auto& descriptors = GetFieldDescriptors();
            for (const auto& desc : descriptors) {
                desc.load(j, Current, s_secretStore, needsSave, needsLegacyMigration);
            }

            // Gamepad overlay default back button migration
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

            // Pro account legacy key migrations
            if (!j.contains("custom_api_key") && j.contains("pro_api_key") && j["pro_api_key"].is_string()) {
                s_secretStore.LoadSecretField("custom_api_key", j["pro_api_key"].get<std::string>(), Current.custom_api_key, needsLegacyMigration, Current.credentials_decryption_failed);
                if (!Current.custom_api_key.empty()) needsLegacyMigration = true;
            }
            if (!j.contains("custom_api_enabled") && j.contains("pro_account_enabled") && j["pro_account_enabled"].is_boolean()) {
                Current.custom_api_enabled = j["pro_account_enabled"].get<bool>();
            }

            // Onboarding migration
            if (!j.contains("onboarding_completed")) {
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

            if (!j.contains("match_summary_seconds")) {
                Current.match_summary_seconds = existingInstall ? 30 : 20;
            } else {
                Current.match_summary_seconds = std::clamp(Current.match_summary_seconds, 5, 60);
            }

            // MMR category fallbacks
            if (!j.contains("graph_mmr_category") || !j["graph_mmr_category"].is_string() ||
                !IsGraphMmrCategory(j["graph_mmr_category"].get<std::string>())) {
                Current.graph_mmr_category = IsGraphMmrCategory(Current.mmr_category) ? Current.mmr_category : "2v2";
                needsSave = true;
            }
            if (!j.contains("graph_follow_current_playlist") || !j["graph_follow_current_playlist"].is_boolean()) {
                Current.graph_follow_current_playlist = Current.auto_switch_mmr_category;
                needsSave = true;
            }
            if (!j.contains("rank_progress_mmr_category") || !j["rank_progress_mmr_category"].is_string() ||
                !IsKnownMmrCategory(j["rank_progress_mmr_category"].get<std::string>())) {
                Current.rank_progress_mmr_category = j.contains("rank_progress_mmr_category") ? "2v2" : (IsKnownMmrCategory(Current.mmr_category) ? Current.mmr_category : "2v2");
                needsSave = true;
            }
            if (!j.contains("rank_progress_follow_current_playlist") || !j["rank_progress_follow_current_playlist"].is_boolean()) {
                Current.rank_progress_follow_current_playlist = Current.auto_switch_mmr_category;
                needsSave = true;
            }
            if (!Current.show_extra_playlists) {
                if (IsExtraMmrCategory(Current.graph_mmr_category)) {
                    Current.graph_mmr_category = "2v2";
                    needsSave = true;
                }
                if (IsExtraMmrCategory(Current.rank_progress_mmr_category)) {
                    Current.rank_progress_mmr_category = "2v2";
                    needsSave = true;
                }
            }

            // Tournament reminder fallbacks
            if (!IsValidTournamentReminderMinutes(Current.tournament_reminder_minutes)) {
                Current.tournament_reminder_minutes = 10;
            }

            // Check for updates migration
            if (!j.contains("check_for_updates") || !j["check_for_updates"].is_boolean() || !j["check_for_updates"].get<bool>()) {
                needsSave = true;
            }

            // Layout fallback for old install missing overlay_layout
            if (!j.contains("overlay_layout")) {
                Current.overlay_layout = OverlayLayout::DefaultOverlayLayout();
                if (existingInstall) {
                    for (auto& c : Current.overlay_layout.containers) {
                        c.visibility = OverlayLayout::ContainerConfig::Visibility{};
                    }
                }
            }

            // Lobby rank fallback
            if (!Current.show_lobby_rank_1v1 && !Current.show_lobby_rank_2v2 &&
                !Current.show_lobby_rank_3v3 && !Current.show_lobby_rank_casual && !Current.show_lobby_rank_tourny &&
                !Current.show_lobby_rank_hoops && !Current.show_lobby_rank_rumble && !Current.show_lobby_rank_dropshot &&
                !Current.show_lobby_rank_snowday && !Current.show_lobby_rank_heatseeker) {
                Current.show_lobby_rank_2v2 = true;
            }

            // Theme background minimum opacity safety
            if (Current.themeBg.a < 0.01f) {
                Current.themeBg.a = 0.85f;
                SaveInternal();
            }

            if (needsLegacyMigration) {
                const std::vector<std::pair<std::string, std::string>> secretsToMigrate = {
                    {"custom_api_key", Current.custom_api_key},
                    {"account_refresh_token", Current.account_refresh_token},
                    {"account_device_key", Current.account_device_key},
                    {"ballchasing_token", Current.ballchasing_token}};
                if (s_secretStore.MigrateLegacySecrets(secretsToMigrate)) {
                    SaveInternal();
                } else {
                    std::cerr << "[Config] Legacy credential migration encryption failed. Plaintext will not be rewritten.\n";
                }
            }

            if (needsSave) {
                s_pendingSave.store(true);
            }
            s_revision.fetch_add(1, std::memory_order_release);
            std::cout << "[Config] Loaded config.json successfully.\n";

        } catch (const std::exception& e) {
            std::cout << "[Config] Failed to parse config.json: " << e.what() << "\n";
            std::error_code ec;
            fs::rename(configFile, configFile + ".corrupt", ec);
            Current = ConfigData{};
            s_secretStore.Reset();
            SaveInternal();
            s_revision.fetch_add(1, std::memory_order_release);
        }
    }

    static void SaveInternal() {
        nlohmann::json j;
        const auto& descriptors = GetFieldDescriptors();
        for (const auto& desc : descriptors) {
            desc.save(j, Current, s_secretStore);
        }

        j["check_for_updates"] = true;
        j["controller_default_back_migrated"] = true;

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
        s_saverCv.notify_one();
    }

    void SetWindowsAutoStart(bool enable) {
        HKEY hKey;
        const char* keyPath = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (RegOpenKeyExA(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            if (enable) {
                char exePath[MAX_PATH];
                if (GetModuleFileNameA(NULL, exePath, MAX_PATH)) {
                    std::string pathStr = "\"" + std::string(exePath) + "\"";
                    RegSetValueExA(hKey, "OmniStats", 0, REG_SZ, reinterpret_cast<const BYTE*>(pathStr.c_str()), static_cast<DWORD>(pathStr.length() + 1));
                }
            } else {
                RegDeleteValueA(hKey, "OmniStats");
            }
            RegCloseKey(hKey);
        }
    }

#ifdef OMNISTATS_TEST_ENVIRONMENT
    void SetCryptProtectHookForTests(CryptProtectHook hook) {
        SecretStore::SetCryptProtectHookForTests(std::move(hook));
    }
    void SetCryptUnprotectHookForTests(CryptUnprotectHook hook) {
        SecretStore::SetCryptUnprotectHookForTests(std::move(hook));
    }
    void ResetCryptHooksForTests() {
        SecretStore::ResetCryptHooksForTests();
    }
#endif
} // namespace Config
