#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <cmath>
#include <cctype>
#include <string_view>
#include "network/MMRFetcherDetail.hpp"

using namespace MMRFetcherDetail;
#include "PlaylistRankThresholds.inl"

namespace {
    template <size_t N>
    std::string LookupTierFromThresholds(const std::array<RankThreshold, N>& table, int mmr) {
        if (mmr <= 0) return "Unranked";
        const RankThreshold* threshold = nullptr;
        for (const auto& entry : table) {
            if (mmr < entry.minMmr) break;
            threshold = &entry;
        }
        if (!threshold) return "Unranked";
        if (threshold->division[0] == '\0') return threshold->tier;
        return std::string(threshold->tier) + " " + threshold->division;
    }
}

std::string MMRFetcher::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";
    return LookupTierFromThresholds(TournamentRankThresholds, mmr);
}

std::string MMRFetcher::GetRankTierForPlaylistMmr(const std::string& playlist, int mmr) {
    if (mmr <= 0 || playlist == "casual") return "Unranked";
    if (playlist == "1v1") {
        return LookupTierFromThresholds(SoloDuelRankThresholds, mmr);
    }
    if (playlist == "2v2") {
        return LookupTierFromThresholds(DoublesRankThresholds, mmr);
    }
    if (playlist == "3v3") {
        return LookupTierFromThresholds(StandardRankThresholds, mmr);
    }
    if (playlist == "hoops") {
        return LookupTierFromThresholds(HoopsRankThresholds, mmr);
    }
    if (playlist == "rumble") {
        return LookupTierFromThresholds(RumbleRankThresholds, mmr);
    }
    if (playlist == "dropshot") {
        return LookupTierFromThresholds(DropshotRankThresholds, mmr);
    }
    if (playlist == "snowday") {
        return LookupTierFromThresholds(SnowdayRankThresholds, mmr);
    }
    if (playlist == "heatseeker") {
        return LookupTierFromThresholds(HeatseekerRankThresholds, mmr);
    }
    if (playlist == "t") {
        return LookupTierFromThresholds(TournamentRankThresholds, mmr);
    }
    return GetTournamentTierForMmr(mmr);
}

std::string MMRFetcher::PlaylistNameForTrackerId(int playlistId) {
    // Tracker profile segments use a subset of Rocket League playlist IDs.
    // Keep this mapping in the same metadata table as gameplay classification
    // so the two paths cannot silently drift apart.
    return PlaylistMetadata::TrackerKey(playlistId);
}

std::string MMRFetcher::RankTierName(int tier, int division) {
    static constexpr std::array<const char*, 23> kTiers = {
        "Unranked",
        "Bronze I",
        "Bronze II",
        "Bronze III",
        "Silver I",
        "Silver II",
        "Silver III",
        "Gold I",
        "Gold II",
        "Gold III",
        "Platinum I",
        "Platinum II",
        "Platinum III",
        "Diamond I",
        "Diamond II",
        "Diamond III",
        "Champion I",
        "Champion II",
        "Champion III",
        "Grand Champion I",
        "Grand Champion II",
        "Grand Champion III",
        "Supersonic Legend",
    };
    if (tier < 0 || static_cast<size_t>(tier) >= kTiers.size()) return "Unranked";
    std::string result = kTiers[static_cast<size_t>(tier)];
    if (tier > 0 && tier < 22 && division >= 0 && division < 4) {
        static constexpr std::array<const char*, 4> kDivisions = {
            "I", "II", "III", "IV"};
        result += " Div ";
        result += kDivisions[static_cast<size_t>(division)];
    }
    return result;
}

MMRProfileTotals MMRFetcher::ExtractProfileTotals(const nlohmann::json& jsonResp) {
    MMRProfileTotals totals;
    if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
        !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
        return totals;
    }

    for (const auto& seg : jsonResp["data"]["segments"]) {
        if (!seg.is_object()) continue;
        if (seg.contains("type") && seg["type"].is_string() && seg["type"] == "playlist") continue;
        if (!seg.contains("stats") || !seg["stats"].is_object()) continue;

        const auto& stats = seg["stats"];
        if (totals.totalWins < 0) {
            (void)TryReadStatValue(stats, {"wins", "Wins"}, totals.totalWins);
        }
    }

    return totals;
}

std::string MMRFetcher::GetTRNPlatform(const std::string& primaryId) {
    size_t delim = primaryId.find('|');
    if (delim == std::string::npos) return "";

    std::string plat = primaryId.substr(0, delim);
    // Trim potential spaces
    plat.erase(0, plat.find_first_not_of(" \t\r\n"));
    plat.erase(plat.find_last_not_of(" \t\r\n") + 1);

    // Convert to lowercase for case-insensitive matching
    std::transform(plat.begin(), plat.end(), plat.begin(), ::tolower);

    if (plat == "epic" || plat == "epicgames") return "epic";
    if (plat == "steam") return "steam";
    if (plat == "ps4" || plat == "psn" || plat == "playstation") return "psn";
    if (plat == "xboxone" || plat == "xbox" || plat == "xbl") return "xbl";
    if (plat == "switch" || plat == "nintendo") return "switch";
    return "";
}
