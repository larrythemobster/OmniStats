#pragma once

#include "core/game/RankProgress.hpp"

namespace RankTiers {
    std::span<const RankProgress::Threshold> RankThresholdsForPlaylist(std::string_view playlist);
    std::string GetRankTierForPlaylistMmr(std::string_view playlist, int mmr);
    std::string GetTournamentTierForMmr(int mmr);
    std::string RankTierName(int tier, int division);
}
