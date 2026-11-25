#include "core/config/ConfigSchema.hpp"
#include "core/game/MmrCategories.hpp"

#include "core/config/Config.hpp"
#include "core/config/SecretStore.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace ConfigSchema {
    bool IsKnownMmrCategory(const std::string& category) {
        return PlaylistMetadata::FindRatingMode(category) != nullptr;
    }

    bool IsGraphMmrCategory(const std::string& category) {
        return category != "best" && IsKnownMmrCategory(category);
    }

    bool IsExtraMmrCategory(const std::string& category) {
        return ::IsExtraMmrCategory(StringToMmrCategory(category));
    }

    void NormalizeLobbyRanks(ConfigData& config) {
        if (std::none_of(config.lobby_rank_columns.begin(), config.lobby_rank_columns.end(), [](bool enabled) { return enabled; })) {
            config.LobbyRankColumn(MmrCategory::TwoVTwo) = true;
        }
    }
}

namespace {
    using namespace ConfigSchema;

    static bool IsValidBallchasingVisibility(const std::string& visibility) {
        return visibility == "private" || visibility == "unlisted" || visibility == "public";
    }

    static bool IsValidTournamentRegion(const std::string& region) {
        return region == "auto" || region == "US" || region == "EU" || region == "OCE" ||
               region == "SAM" || region == "ASIA" || region == "ME" || region == "SAF";
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

    enum FieldUse { Stored = 0,
                    Control = 1,
                    Structure = 2,
                    ControlAndStructure = Control | Structure };

    struct NumberRange {
        double minimum = -(std::numeric_limits<double>::infinity)();
        double maximum = (std::numeric_limits<double>::infinity)();
        bool clampOnLoad = false;
    };

    struct FieldDescriptor {
        std::string_view key;
        std::function<void(const nlohmann::json&, ConfigData&, SecretStore&, bool&, bool&)> load;
        std::function<void(nlohmann::json&, const ConfigData&, SecretStore&)> save;
        std::function<void(ConfigData&, const std::string&, bool)> applyControl{};
        std::function<size_t(const ConfigData&)> renderHash{};
    };

    static const ConfigData& Defaults() {
        static const ConfigData config{};
        return config;
    }

    template <auto Member>
    static FieldDescriptor MakeField(const char* key, FieldUse use = Stored, NumberRange range = {},
                                     bool (*validate)(const std::string&) = nullptr) {
        using T = std::remove_cvref_t<decltype(Defaults().*Member)>;
        FieldDescriptor field{
            key,
            [key, range, validate](const nlohmann::json& j, ConfigData& config, SecretStore&, [[maybe_unused]] bool& needsSave, bool&) {
                const auto entry = j.find(key);
                if (entry != j.end()) {
                    if constexpr (std::is_same_v<T, bool>) {
                        if (entry->is_boolean()) {
                            config.*Member = entry->get<bool>();
                            return;
                        }
                    } else if constexpr (std::is_same_v<T, std::string>) {
                        if (entry->is_string()) {
                            const auto& value = entry->get_ref<const std::string&>();
                            if (!validate || validate(value)) {
                                config.*Member = value;
                                return;
                            }
                            needsSave = true;
                        }
                    } else {
                        const bool number = std::is_integral_v<T> ? entry->is_number_integer() : entry->is_number();
                        if (number) {
                            const long double raw = entry->get<long double>();
                            if (std::isfinite(raw) && raw >= (std::numeric_limits<T>::lowest)() && raw <= (std::numeric_limits<T>::max)()) {
                                const T value = static_cast<T>(raw);
                                const double bounded = std::clamp(static_cast<double>(value), range.minimum, range.maximum);
                                if (range.clampOnLoad || bounded == value) {
                                    needsSave |= bounded != value;
                                    config.*Member = static_cast<T>(bounded);
                                    return;
                                }
                            }
                        }
                    }
                }
                config.*Member = Defaults().*Member;
            },
            [key](nlohmann::json& j, const ConfigData& config, SecretStore&) { j[key] = config.*Member; }};
        if (use & Control) {
            field.applyControl = [range]([[maybe_unused]] ConfigData& config, [[maybe_unused]] const std::string& value, [[maybe_unused]] bool checked) {
                if constexpr (std::is_same_v<T, bool>) {
                    config.*Member = checked;
                } else if constexpr (std::is_same_v<T, std::string>) {
                    config.*Member = value;
                } else if constexpr (std::is_integral_v<T>) {
                    const double raw = static_cast<double>(std::strtol(value.c_str(), nullptr, 10));
                    config.*Member = static_cast<T>(std::clamp(raw, range.minimum, range.maximum));
                }
            };
        }
        if (use & Structure) {
            field.renderHash = [](const ConfigData& config) { return std::hash<T>{}(config.*Member); };
        }
        return field;
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

    static std::optional<nlohmann::json> invalidCustomThemes;

    static std::string ThemeDiskKey(std::string_view key) {
        if (key == "theme_panel") return "settings_panel";
        if (key == "theme_graph") return "graph_line";
        if (key == "theme_baseline") return "graph_baseline";
        return std::string(key.substr(6));
    }

    static bool ValidSavedThemes(const std::vector<Themes::SavedTheme>& themes) {
        if (themes.size() > 32) return false;
        for (size_t i = 0; i < themes.size(); ++i) {
            const auto& theme = themes[i];
            if (theme.name.empty() || theme.name.size() > 80 || Themes::Encode(theme.colors).empty()) return false;
            try {
                (void)nlohmann::json(theme.name).dump();
            } catch (const nlohmann::json::exception&) {
                return false;
            }
            for (size_t k = 0; k < i; ++k)
                if (themes[k].name == theme.name) return false;
        }
        return true;
    }

    static bool ParseSavedThemes(const nlohmann::json& value, std::vector<Themes::SavedTheme>& result) {
        if (!value.is_array() || value.size() > 32) return false;
        std::vector<Themes::SavedTheme> parsed;
        for (const auto& item : value) {
            if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
                !item.contains("colors") || !item["colors"].is_array() ||
                (item["colors"].size() != Themes::kLegacyPaletteSize && item["colors"].size() != Themes::Palette{}.size())) return false;
            Themes::SavedTheme saved{};
            saved.name = item["name"].get<std::string>();
            for (size_t i = 0; i < item["colors"].size(); ++i) {
                const auto& color = item["colors"][i];
                if (!color.is_array() || color.size() != 4) return false;
                for (const auto& channel : color) {
                    if (!channel.is_number()) return false;
                    const double v = channel.get<double>();
                    if (!std::isfinite(v) || v < 0 || v > 1) return false;
                }
                saved.colors[i] = {color[0].get<float>(), color[1].get<float>(), color[2].get<float>(), color[3].get<float>()};
            }
            if (item["colors"].size() == Themes::kLegacyPaletteSize) {
                ConfigData config{};
                Themes::Apply(config, saved.colors);
                Themes::CompleteLegacy(config);
                saved.colors = Themes::Read(config);
            }
            parsed.push_back(std::move(saved));
        }
        if (!ValidSavedThemes(parsed)) return false;
        result = std::move(parsed);
        return true;
    }

    static const std::vector<FieldDescriptor>& GetFieldDescriptors() {
        static const std::vector<FieldDescriptor> descriptors = [] {
            std::vector<FieldDescriptor> fields = {
                MakeField<&ConfigData::host>("host"),
                MakeField<&ConfigData::port>("port"),
                MakeField<&ConfigData::key_overlay>("key_overlay"),
                MakeField<&ConfigData::key_cycle>("key_cycle", Structure),
                MakeField<&ConfigData::key_expand>("key_expand", Structure),
                MakeField<&ConfigData::key_session>("key_session", Structure),
                MakeField<&ConfigData::key_menu>("key_menu"),
                MakeField<&ConfigData::key_graph_pan_left>("key_graph_pan_left"),
                MakeField<&ConfigData::key_graph_pan_right>("key_graph_pan_right"),
                MakeField<&ConfigData::key_lifetime_graph>("key_lifetime_graph"),

                MakeField<&ConfigData::gamepad_overlay>("gamepad_overlay"),
                MakeField<&ConfigData::gamepad_overlay_raw>("gamepad_overlay_raw"),
                MakeField<&ConfigData::gamepad_overlay_raw_button>("gamepad_overlay_raw_button"),
                MakeField<&ConfigData::gamepad_cycle>("gamepad_cycle"),
                MakeField<&ConfigData::gamepad_cycle_raw>("gamepad_cycle_raw"),
                MakeField<&ConfigData::gamepad_cycle_raw_button>("gamepad_cycle_raw_button"),
                MakeField<&ConfigData::gamepad_expand>("gamepad_expand"),
                MakeField<&ConfigData::gamepad_expand_raw>("gamepad_expand_raw"),
                MakeField<&ConfigData::gamepad_expand_raw_button>("gamepad_expand_raw_button"),
                MakeField<&ConfigData::gamepad_session>("gamepad_session"),
                MakeField<&ConfigData::gamepad_session_raw>("gamepad_session_raw"),
                MakeField<&ConfigData::gamepad_session_raw_button>("gamepad_session_raw_button"),
                MakeField<&ConfigData::gamepad_menu>("gamepad_menu"),
                MakeField<&ConfigData::gamepad_menu_raw>("gamepad_menu_raw"),
                MakeField<&ConfigData::gamepad_menu_raw_button>("gamepad_menu_raw_button"),
                MakeField<&ConfigData::gamepad_graph_pan_left>("gamepad_graph_pan_left"),
                MakeField<&ConfigData::gamepad_graph_pan_left_raw>("gamepad_graph_pan_left_raw"),
                MakeField<&ConfigData::gamepad_graph_pan_left_raw_button>("gamepad_graph_pan_left_raw_button"),
                MakeField<&ConfigData::gamepad_graph_pan_right>("gamepad_graph_pan_right"),
                MakeField<&ConfigData::gamepad_graph_pan_right_raw>("gamepad_graph_pan_right_raw"),
                MakeField<&ConfigData::gamepad_graph_pan_right_raw_button>("gamepad_graph_pan_right_raw_button"),

                MakeField<&ConfigData::mmr_category>("mmr_category", ControlAndStructure, {}, IsKnownMmrCategory),
                MakeField<&ConfigData::graph_mmr_category>("graph_mmr_category", ControlAndStructure, {}, IsGraphMmrCategory),
                MakeField<&ConfigData::graph_follow_current_playlist>("graph_follow_current_playlist", ControlAndStructure),
                MakeField<&ConfigData::graph_default_lifetime>("graph_default_lifetime", ControlAndStructure),
                MakeField<&ConfigData::rank_progress_mmr_category>("rank_progress_mmr_category", ControlAndStructure, {}, IsKnownMmrCategory),
                MakeField<&ConfigData::rank_progress_follow_current_playlist>("rank_progress_follow_current_playlist", ControlAndStructure),
                MakeField<&ConfigData::last_primary_id>("last_primary_id", Structure),

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

                MakeField<&ConfigData::require_rl_focus>("require_rl_focus", ControlAndStructure),
                MakeField<&ConfigData::show_match_summary>("show_match_summary", ControlAndStructure),
                MakeField<&ConfigData::match_summary_seconds>("match_summary_seconds", Structure, {5, 60, true}),
                MakeField<&ConfigData::discord_rpc_enabled>("discord_rpc_enabled", Control),
                MakeField<&ConfigData::enable_mmr_tracking>("enable_mmr_tracking", Control),
                MakeField<&ConfigData::auto_switch_mmr_category>("auto_switch_mmr_category", ControlAndStructure),
                MakeField<&ConfigData::enable_auto_updates>("enable_auto_updates", ControlAndStructure),
                MakeField<&ConfigData::crash_reports_enabled>("crash_reports_enabled", Control),
                MakeField<&ConfigData::debug_logging>("debug_logging", Control),
                MakeField<&ConfigData::imperial_units>("imperial_units", Structure),
                MakeField<&ConfigData::run_on_startup>("run_on_startup", Control),
                MakeField<&ConfigData::reset_session_on_close>("reset_session_on_close", Control),
                MakeField<&ConfigData::show_session_recap_on_close>("show_session_recap_on_close", Control),
                MakeField<&ConfigData::client_uuid>("client_uuid"),
                MakeField<&ConfigData::privacy_policy_accepted_version>("privacy_policy_accepted_version"),
                MakeField<&ConfigData::terms_accepted_version>("terms_accepted_version"),
                MakeField<&ConfigData::privacy_accepted_at>("privacy_accepted_at"),
                MakeField<&ConfigData::onboarding_completed>("onboarding_completed"),
                MakeField<&ConfigData::account_link_prompt_shown>("account_link_prompt_shown"),

                MakeField<&ConfigData::position>("position"),
                MakeField<&ConfigData::show_running_indicator>("show_running_indicator", ControlAndStructure),
                MakeField<&ConfigData::use_roman_numerals>("use_roman_numerals", ControlAndStructure),
                MakeField<&ConfigData::use_rank_icons>("use_rank_icons", ControlAndStructure),
                MakeField<&ConfigData::show_extra_playlists>("show_extra_playlists", Structure),

                MakeField<&ConfigData::custom_api_enabled>("custom_api_enabled", Control),
                MakeSecretField("custom_api_key", &ConfigData::custom_api_key),

                MakeField<&ConfigData::account_signed_in_name>("account_signed_in_name"),
                MakeField<&ConfigData::account_device_public_id>("account_device_public_id"),
                MakeSecretField("account_refresh_token", &ConfigData::account_refresh_token),
                MakeSecretField("account_device_key", &ConfigData::account_device_key),
                MakeField<&ConfigData::rocket_league_stats_api_config_path>("rocket_league_stats_api_config_path"),
                MakeField<&ConfigData::check_stats_api_config_on_startup>("check_stats_api_config_on_startup", Control),

                MakeSecretField("ballchasing_token", &ConfigData::ballchasing_token),
                MakeField<&ConfigData::auto_upload_replays>("auto_upload_replays", Control),
                MakeField<&ConfigData::ballchasing_upload_notice_accepted>("ballchasing_upload_notice_accepted"),
                MakeField<&ConfigData::ballchasing_visibility>("ballchasing_visibility", Control, {}, IsValidBallchasingVisibility),
                MakeField<&ConfigData::ballchasing_filter_ranked_only>("ballchasing_filter_ranked_only", Control),
                MakeField<&ConfigData::ballchasing_filter_wins_only>("ballchasing_filter_wins_only", Control),
                MakeField<&ConfigData::auto_save_replays>("auto_save_replays", Control),
                MakeField<&ConfigData::key_save_replay>("key_save_replay"),

                {"theme",
                 [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                     if (!j.contains("theme") || !j["theme"].is_object()) return;
                     const auto fields = Themes::Fields();
                     for (size_t i = 0; i < Themes::kLegacyPaletteSize; ++i) {
                         const auto& field = fields[i];
                         const auto key = ThemeDiskKey(field.key);
                         if (j["theme"].contains(key)) cfg.*field.member = JsonToColor(j["theme"][key], cfg.*field.member);
                     }
                     Themes::CompleteLegacy(cfg);
                     for (size_t i = Themes::kLegacyPaletteSize; i < fields.size(); ++i) {
                         const auto& field = fields[i];
                         const auto key = ThemeDiskKey(field.key);
                         if (j["theme"].contains(key)) cfg.*field.member = JsonToColor(j["theme"][key], cfg.*field.member);
                     }
                 },
                 [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                     for (const auto& field : Themes::Fields())
                         j["theme"][ThemeDiskKey(field.key)] = ColorToJson(cfg.*field.member);
                 }},
                {"custom_themes",
                 [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                     invalidCustomThemes.reset();
                     if (!j.contains("custom_themes")) return;
                     if (!ParseSavedThemes(j["custom_themes"], cfg.custom_themes)) {
                         invalidCustomThemes = j["custom_themes"];
                         cfg.custom_themes_unreadable = true;
                         std::cerr << "[Config] Invalid custom themes preserved without loading.\n";
                     }
                 },
                 [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                     if (invalidCustomThemes) {
                         j["custom_themes"] = *invalidCustomThemes;
                         return;
                     }
                     auto& output = j["custom_themes"];
                     output = nlohmann::json::array();
                     for (const auto& theme : cfg.custom_themes) {
                         nlohmann::json colors = nlohmann::json::array();
                         for (const auto& color : theme.colors)
                             colors.push_back(ColorToJson(color));
                         output.push_back({{"name", theme.name}, {"colors", std::move(colors)}});
                     }
                 }},

                MakeField<&ConfigData::show_session_record>("show_session_record", ControlAndStructure),
                MakeField<&ConfigData::show_session_goals>("show_session_goals", ControlAndStructure),
                MakeField<&ConfigData::show_session_saves>("show_session_saves", ControlAndStructure),
                MakeField<&ConfigData::show_session_demos>("show_session_demos", ControlAndStructure),
                MakeField<&ConfigData::show_session_assists>("show_session_assists", ControlAndStructure),
                MakeField<&ConfigData::show_session_goal_participation>("show_session_goal_participation", ControlAndStructure),
                MakeField<&ConfigData::show_session_mmr_change>("show_session_mmr_change", ControlAndStructure),
                MakeField<&ConfigData::show_session_boost>("show_session_boost", ControlAndStructure),
                MakeField<&ConfigData::show_lobby_ranks_overlay>("show_lobby_ranks_overlay", Structure),
                MakeField<&ConfigData::show_account_wins_overlay>("show_account_wins_overlay", ControlAndStructure),
                MakeField<&ConfigData::show_demo_tracker_overlay>("show_demo_tracker_overlay", Structure),
                MakeField<&ConfigData::show_rank_progress>("show_rank_progress", Structure),
                MakeField<&ConfigData::show_previous_games_summary>("show_previous_games_summary", Structure),
                MakeField<&ConfigData::previous_games_limit>("previous_games_limit", ControlAndStructure, {10, 50, true}),

                MakeField<&ConfigData::show_tournaments>("show_tournaments", Structure),
                MakeField<&ConfigData::tournament_region>("tournament_region", ControlAndStructure, {}, IsValidTournamentRegion),
                MakeField<&ConfigData::tournament_reminder_minutes>("tournament_reminder_minutes", ControlAndStructure, {0, 15, false}),
                MakeField<&ConfigData::tournament_reminder_standard_only>("tournament_reminder_standard_only", ControlAndStructure),
                MakeField<&ConfigData::tournament_sound_cue>("tournament_sound_cue", Control),

                MakeField<&ConfigData::show_season_countdown>("show_season_countdown", ControlAndStructure),
                MakeField<&ConfigData::season_reminder_days>("season_reminder_days", Control, {0, 30, false}),
                MakeField<&ConfigData::season_reward_reminders>("season_reward_reminders", Control),
                MakeField<&ConfigData::show_season_recap>("show_season_recap", Control),
                MakeField<&ConfigData::group_history_by_season>("group_history_by_season", Control),
                MakeField<&ConfigData::season_reminder_last_day>("season_reminder_last_day"),
                MakeField<&ConfigData::season_recap_last_seen>("season_recap_last_seen", Stored, {0, 1000, false}),

                MakeField<&ConfigData::show_streaks_stats>("show_streaks_stats", Structure),
                MakeField<&ConfigData::show_longest_loss_streak>("show_longest_loss_streak", ControlAndStructure),
                MakeField<&ConfigData::show_gamemode_breakdown>("show_gamemode_breakdown", Structure),
                MakeField<&ConfigData::show_gamemode_record_1v1>("show_gamemode_record_1v1", ControlAndStructure),
                MakeField<&ConfigData::show_gamemode_record_2v2>("show_gamemode_record_2v2", ControlAndStructure),
                MakeField<&ConfigData::show_gamemode_record_3v3>("show_gamemode_record_3v3", ControlAndStructure),
                MakeField<&ConfigData::gamemode_breakdown_scope>("gamemode_breakdown_scope", ControlAndStructure),
                MakeField<&ConfigData::crossbar_display_mode>("crossbar_display_mode", Structure),

                MakeField<&ConfigData::second_monitor_mode>("second_monitor_mode", Structure),
                MakeField<&ConfigData::second_monitor_show_roster>("second_monitor_show_roster", Structure),
                MakeField<&ConfigData::second_monitor_show_session>("second_monitor_show_session", Structure),
                MakeField<&ConfigData::second_monitor_show_settings>("second_monitor_show_settings"),
                MakeField<&ConfigData::second_monitor_has_bounds>("second_monitor_has_bounds"),
                MakeField<&ConfigData::second_monitor_x>("second_monitor_x"),
                MakeField<&ConfigData::second_monitor_y>("second_monitor_y"),
                MakeField<&ConfigData::second_monitor_w>("second_monitor_w"),
                MakeField<&ConfigData::second_monitor_h>("second_monitor_h"),

                MakeField<&ConfigData::ui_scale>("ui_scale", Structure),
                {"card_text_percent",
                 [](const nlohmann::json& j, ConfigData& cfg, SecretStore&, bool&, bool&) {
                     const auto field = j.find("card_text_percent");
                     if (field == j.end() || !field->is_object()) return;
                     for (size_t i = 0; i < cfg.card_text_percent.size(); ++i) {
                         const auto entry = field->find(DashboardLayout::ToConfigString(static_cast<DashboardLayout::WidgetId>(i)));
                         if (entry != field->end() && entry->is_number_integer() && *entry >= 75 && *entry <= 150)
                             cfg.card_text_percent[i] = ConfigData::NormalizeCardTextPercent(entry->get<int>());
                     }
                 },
                 [](nlohmann::json& j, const ConfigData& cfg, SecretStore&) {
                     auto sizes = nlohmann::json::object();
                     for (size_t i = 0; i < cfg.card_text_percent.size(); ++i) {
                         const auto widget = static_cast<DashboardLayout::WidgetId>(i);
                         if (const int percent = cfg.CardTextPercent(widget); percent != 100)
                             sizes[DashboardLayout::ToConfigString(widget)] = percent;
                     }
                     j["card_text_percent"] = std::move(sizes);
                 }},
                MakeField<&ConfigData::font_ui>("font_ui", Control),
                MakeField<&ConfigData::font_mono>("font_mono", Control),
                MakeField<&ConfigData::font_display>("font_display", Control),

                MakeField<&ConfigData::session_view_x>("session_view_x"),
                MakeField<&ConfigData::session_view_y>("session_view_y"),
                MakeField<&ConfigData::match_summary_x>("match_summary_x"),
                MakeField<&ConfigData::match_summary_y>("match_summary_y"),

                MakeField<&ConfigData::overlay_fps_cap>("overlay_fps_cap"),
                MakeField<&ConfigData::vsync>("vsync"),

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
            for (const auto& mode : PlaylistMetadata::kRatingModes) {
                if (!*mode.lobbySetting) continue;
                fields.push_back({mode.lobbySetting,
                                  [key = mode.lobbySetting, category = mode.category](const nlohmann::json& json, ConfigData& config, SecretStore&, bool&, bool&) {
                                      const auto entry = json.find(key);
                                      config.LobbyRankColumn(category) = entry != json.end() && entry->is_boolean()
                                                                             ? entry->get<bool>()
                                                                             : Defaults().LobbyRankColumn(category);
                                  },
                                  [key = mode.lobbySetting, category = mode.category](nlohmann::json& json, const ConfigData& config, SecretStore&) {
                                      json[key] = config.LobbyRankColumn(category);
                                  },
                                  [category = mode.category](ConfigData& config, const std::string&, bool checked) {
                                      config.LobbyRankColumn(category) = checked;
                                  },
                                  [category = mode.category](const ConfigData& config) { return config.LobbyRankColumn(category); }});
            }
            return fields;
        }();
        return descriptors;
    }

}

namespace ConfigSchema {
    void Decode(const nlohmann::json& json, ConfigData& config, SecretStore& secrets, bool& needsSave, bool& needsLegacyMigration) {
        for (const auto& field : GetFieldDescriptors())
            field.load(json, config, secrets, needsSave, needsLegacyMigration);
    }

    nlohmann::json Encode(const ConfigData& config, SecretStore& secrets) {
        if (!ValidSavedThemes(config.custom_themes) || (invalidCustomThemes && !config.custom_themes.empty())) {
            throw std::invalid_argument("Cannot save invalid custom themes or replace unrecognized saved themes.");
        }
        nlohmann::json json;
        for (const auto& field : GetFieldDescriptors())
            field.save(json, config, secrets);
        json["controller_default_back_migrated"] = true;
        return json;
    }

    void ResetPreservedThemes() {
        invalidCustomThemes.reset();
    }
}

bool Config::ApplyControl(ConfigData& config, std::string_view key, const std::string& value, bool checked) {
    for (const auto& field : GetFieldDescriptors()) {
        if (field.key != key || !field.applyControl) continue;
        field.applyControl(config, value, checked);
        return true;
    }
    return false;
}

uint64_t Config::RenderScalarHash(const ConfigData& config) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto& field : GetFieldDescriptors()) {
        if (field.renderHash) hash = (hash ^ field.renderHash(config)) * 0x100000001b3ull;
    }
    return hash;
}
