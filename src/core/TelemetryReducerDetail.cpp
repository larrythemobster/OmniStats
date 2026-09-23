#include "core/TelemetryReducerDetail.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>

namespace TelemetryReducerDetail {
    std::string FormatArenaName(const std::string& asset) {
        if (asset.empty()) return "";
        std::string base = asset;
        std::transform(base.begin(), base.end(), base.begin(), ::tolower);
        if (base.length() > 2 && base.substr(base.length() - 2) == "_p") {
            base = base.substr(0, base.length() - 2);
        }
        static const std::unordered_map<std::string, std::string> ARENA_BASE = {
            {"stadium", "DFH Stadium"}, {"park", "Beckwith Park"}, {"mannfield", "Mannfield"}, {"trainstation", "Urban Central"}, {"haunted_trainstation", "Urban Central (Haunted)"}, {"underwater", "AquaDome"}, {"wasteland", "Wasteland"}, {"neotokyo", "Neo Tokyo"}, {"neotokyo_standard", "Neo Tokyo"}, {"eurostadium", "Champions Field"}, {"beach", "Salty Shores"}, {"beachvolley", "Salty Shores"}, {"chinastadium", "Forbidden Temple"}, {"temple", "Forbidden Temple"}, {"cosmic", "Starbase ARC"}, {"arc_standard", "Starbase ARC"}, {"throwback_stadium", "Throwback Stadium"}, {"hoops_dunkhouse", "DunkHouse"}, {"music", "Estadio Vida"}, {"estadio_vida", "Estadio Vida"}, {"farm", "Farmstead"}, {"outlaw_oasis", "Deadeye Canyon"}, {"canyon", "Deadeye Canyon"}, {"shattershot", "Core 707"}, {"labs_octagon", "Octagon"}, {"labs_pillars", "Pillars"}, {"labs_cosmic", "Cosmic"}, {"labs_double_goal", "Double Goal"}, {"labs_underpass", "Underpass"}, {"labs_utopia", "Utopia Retro"}, {"neoasphalt", "Neon Fields"}, {"neon", "Neon Fields"}, {"utopia", "Utopia Coliseum"}, {"sovereign", "Sovereign Heights"}};
        static const std::unordered_map<std::string, std::string> ARENA_VARIANT = {
            {"night", "Night"}, {"day", "Day"}, {"rainy", "Stormy"}, {"stormy", "Stormy"}, {"race_day", "Stormy"}, {"snowy", "Snowy"}, {"snowfall", "Snowy"}, {"dawn", "Dawn"}, {"spring", "Spring"}, {"spooky", "Spooky"}, {"circuit", "Circuit"}};
        auto baseIt = ARENA_BASE.find(base);
        if (baseIt != ARENA_BASE.end()) return baseIt->second;
        size_t lastUnderscore = base.find_last_of('_');
        if (lastUnderscore != std::string::npos) {
            std::string candidate = base.substr(0, lastUnderscore);
            std::string variant = base.substr(lastUnderscore + 1);
            auto candIt = ARENA_BASE.find(candidate);
            if (candIt != ARENA_BASE.end()) {
                auto varIt = ARENA_VARIANT.find(variant);
                if (varIt != ARENA_VARIANT.end()) return candIt->second + " (" + varIt->second + ")";
                if (!variant.empty()) {
                    variant[0] = toupper(variant[0]);
                    return candIt->second + " (" + variant + ")";
                }
                return candIt->second;
            }
        }
        return asset;
    }

    // Game.PlaylistId is authoritative when Rocket League provides it. Playlist
    // metadata is centralized in PlaylistMetadata so gameplay, persistence, and
    // Tracker parsing cannot silently drift apart.

    MmrCategory CategoryFromTeamCounts(const std::array<int, 2>& teamCounts, bool roundStarted) {
        int teamSize = std::min(teamCounts[0], teamCounts[1]);
        if (teamSize >= 3) return MmrCategory::ThreeVThree;
        if (teamSize >= 2) return MmrCategory::TwoVTwo;
        if (roundStarted && teamCounts[0] == 1 && teamCounts[1] == 1) return MmrCategory::OneVOne;
        return MmrCategory::Best;
    }

    MmrCategory CategoryFromMode(const std::string& mode) {
        if (mode == "1v1") return MmrCategory::OneVOne;
        if (mode == "2v2") return MmrCategory::TwoVTwo;
        if (mode == "3v3") return MmrCategory::ThreeVThree;
        if (mode == "hoops") return MmrCategory::Hoops;
        if (mode == "rumble") return MmrCategory::Rumble;
        if (mode == "dropshot") return MmrCategory::Dropshot;
        if (mode == "snowday") return MmrCategory::SnowDay;
        if (mode == "heatseeker") return MmrCategory::Heatseeker;
        return MmrCategory::Best;
    }

    std::string InferModeFromMatchState(
        const GameState& game,
        MmrCategory rosterCategory) {
        if (PlaylistMetadata::HasAuthoritativeId(game.playlistId)) {
            const std::string playlistMode =
                PlaylistMetadata::CanonicalMode(game.playlistId);
            return playlistMode.empty() ? "unknown" : playlistMode;
        }

        // Legacy fallback for telemetry that genuinely did not provide PlaylistId.
        // Explicit telemetry context is stronger than UI/arena heuristics.
        if (game.fallbackCasualContext) return "casual";
        if (game.fallbackNonRecordableContext) return "unknown";

        // UI selection, arena, and observed player counts are never allowed to
        // override an authoritative (including unknown-to-us) playlist ID.
        std::string arenaKey = !game.arenaAsset.empty()
                                   ? game.arenaAsset
                                   : game.arenaName;
        return GamemodeUtils::InferFromSnapshot(
            game.legacyMaxPlayersSeen,
            static_cast<int>(game.roster.size()),
            rosterCategory,
            arenaKey);
    }

    MmrCategory CategoryFromMatchContext(const GameState& game) {
        if (PlaylistMetadata::HasAuthoritativeId(game.playlistId)) {
            return PlaylistMetadata::Category(game.playlistId);
        }

        // Legacy-only inference when Game.PlaylistId was absent.
        if (game.fallbackNonRecordableContext) return MmrCategory::Best;
        if (game.fallbackCasualContext) return MmrCategory::Casual;
        std::string arenaKey = !game.arenaAsset.empty() ? game.arenaAsset : game.arenaName;
        MmrCategory arenaCategory = CategoryFromMode(GamemodeUtils::InferFromArenaName(arenaKey));
        if (arenaCategory != MmrCategory::Best) return arenaCategory;
        return CategoryFromTeamCounts(game.legacyMaxTeamPlayersSeen, game.roundEverStarted);
    }

    bool IsSupportedMmrCategory(MmrCategory category) {
        return category == MmrCategory::OneVOne ||
               category == MmrCategory::TwoVTwo ||
               category == MmrCategory::ThreeVThree ||
               category == MmrCategory::Casual ||
               category == MmrCategory::Tourny ||
               IsExtraMmrCategory(category);
    }

    bool IsTrackedRankedEarlyExitMode(const std::string& mode) {
        return mode == "1v1" || mode == "2v2" || mode == "3v3" ||
               mode == "hoops" || mode == "rumble" || mode == "dropshot" ||
               mode == "snowday" || mode == "heatseeker";
    }

    std::string FormatPendingMatchHistoryMode(const std::string& mode) {
        if (mode == "1v1") return "Duel";
        if (mode == "2v2") return "Doubles";
        if (mode == "3v3") return "Standard";
        if (mode == "hoops") return "Hoops";
        if (mode == "rumble") return "Rumble";
        if (mode == "dropshot") return "Dropshot";
        if (mode == "snowday") return "Snow Day";
        if (mode == "heatseeker") return "Heatseeker";
        return mode.empty() ? "Unknown" : mode;
    }

    std::string Lowercase(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool ReadTrueBoolean(const nlohmann::json& object,
                         std::initializer_list<const char*> keys) {
        if (!object.is_object()) return false;
        for (const char* key : keys) {
            if (object.contains(key) && object[key].is_boolean() &&
                object[key].get<bool>()) {
                return true;
            }
        }
        return false;
    }

    std::optional<int> ReadInteger(const nlohmann::json& object,
                                   std::initializer_list<const char*> keys) {
        if (!object.is_object()) return std::nullopt;
        for (const char* key : keys) {
            if (object.contains(key) && object[key].is_number_integer()) {
                return object[key].get<int>();
            }
        }
        return std::nullopt;
    }

    bool MatchTypeContains(const nlohmann::json& game,
                           std::initializer_list<const char*> keywords) {
        if (!game.is_object()) return false;
        for (const char* key : {"Playlist", "PlaylistName", "MatchType", "GameMode"}) {
            if (!game.contains(key) || !game[key].is_string()) continue;
            const std::string value = Lowercase(game[key].get<std::string>());
            for (const char* keyword : keywords) {
                if (value.find(keyword) != std::string::npos) return true;
            }
        }
        return false;
    }
}
