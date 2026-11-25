#include "core/game/RankTiers.hpp"

#include "core/game/MmrCategories.hpp"
#include "core/game/PlaylistRankThresholds.inl"

namespace {
    std::string LookupTierFromThresholds(std::span<const RankThreshold> table, int mmr) {
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
} // namespace

std::string RankTiers::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";
    return LookupTierFromThresholds(TournamentRankThresholds, mmr);
}

std::span<const RankProgress::Threshold> RankTiers::RankThresholdsForPlaylist(std::string_view playlist) {
    switch (StringToMmrCategory(playlist)) {
    case MmrCategory::OneVOne:
        return SoloDuelRankThresholds;
    case MmrCategory::TwoVTwo:
        return DoublesRankThresholds;
    case MmrCategory::ThreeVThree:
        return StandardRankThresholds;
    case MmrCategory::Hoops:
        return HoopsRankThresholds;
    case MmrCategory::Rumble:
        return RumbleRankThresholds;
    case MmrCategory::Dropshot:
        return DropshotRankThresholds;
    case MmrCategory::SnowDay:
        return SnowdayRankThresholds;
    case MmrCategory::Heatseeker:
        return HeatseekerRankThresholds;
    case MmrCategory::Tourny:
        return TournamentRankThresholds;
    default:
        return {};
    }
}

std::string RankTiers::GetRankTierForPlaylistMmr(std::string_view playlist, int mmr) {
    if (mmr <= 0 || playlist == "casual") return "Unranked";
    const auto table = RankThresholdsForPlaylist(playlist);
    return table.empty() ? GetTournamentTierForMmr(mmr) : LookupTierFromThresholds(table, mmr);
}

std::string RankTiers::RankTierName(int tier, int division) {
    static constexpr std::array<const char*, 23> kTiers = {
        "Unranked", "Bronze I", "Bronze II", "Bronze III",
        "Silver I", "Silver II", "Silver III",
        "Gold I", "Gold II", "Gold III",
        "Platinum I", "Platinum II", "Platinum III",
        "Diamond I", "Diamond II", "Diamond III",
        "Champion I", "Champion II", "Champion III",
        "Grand Champion I", "Grand Champion II", "Grand Champion III",
        "Supersonic Legend"};
    if (tier < 0 || static_cast<size_t>(tier) >= kTiers.size()) return "Unranked";
    std::string result = kTiers[static_cast<size_t>(tier)];
    if (tier > 0 && tier < 22 && division >= 0 && division < 4) {
        static constexpr std::array<const char*, 4> kDivisions = {"I", "II", "III", "IV"};
        result += " Div ";
        result += kDivisions[static_cast<size_t>(division)];
    }
    return result;
}
