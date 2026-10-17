#include "core/reducer/TelemetryReducerDetail.hpp"
#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "utils/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>

namespace TelemetryReducerDetail {
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
