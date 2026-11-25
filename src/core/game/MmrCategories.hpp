#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

enum class MmrCategory : int {
    Best,
    OneVOne,
    TwoVTwo,
    ThreeVThree,
    Casual,
    Tourny,
    Hoops,
    Rumble,
    Dropshot,
    SnowDay,
    Heatseeker
};

namespace PlaylistMetadata {
    struct RatingMode {
        MmrCategory category;
        const char* key;
        const char* label;
        const char* historyLabel;
        const char* lobbyLabel;
        const char* lobbySetting;
        int apiPlaylistId;
        int teamSize;
        bool extra;
    };

    inline constexpr std::array kRatingModes{
        RatingMode{MmrCategory::Best, "best", "Best Rank", "", "", "", 0, 0, false},
        RatingMode{MmrCategory::OneVOne, "1v1", "1v1", "Duel", "1v1", "show_lobby_rank_1v1", 10, 1, false},
        RatingMode{MmrCategory::TwoVTwo, "2v2", "2v2", "Doubles", "2v2", "show_lobby_rank_2v2", 11, 2, false},
        RatingMode{MmrCategory::ThreeVThree, "3v3", "3v3", "Standard", "3v3", "show_lobby_rank_3v3", 13, 3, false},
        RatingMode{MmrCategory::Casual, "casual", "Casual", "Casual", "Casual", "show_lobby_rank_casual", 0, 0, false},
        RatingMode{MmrCategory::Tourny, "t", "Tournament", "Tournament", "Tourney", "show_lobby_rank_tourny", 34, 0, false},
        RatingMode{MmrCategory::Hoops, "hoops", "Hoops", "Hoops", "Hoops", "show_lobby_rank_hoops", 27, 2, true},
        RatingMode{MmrCategory::Rumble, "rumble", "Rumble", "Rumble", "Rumble", "show_lobby_rank_rumble", 28, 3, true},
        RatingMode{MmrCategory::Dropshot, "dropshot", "Dropshot", "Dropshot", "Dropshot", "show_lobby_rank_dropshot", 29, 3, true},
        RatingMode{MmrCategory::SnowDay, "snowday", "Snow Day", "Snow Day", "Snow", "show_lobby_rank_snowday", 30, 3, true},
        RatingMode{MmrCategory::Heatseeker, "heatseeker", "Heatseeker", "Heatseeker", "Heat", "show_lobby_rank_heatseeker", 63, 2, true},
    };

    constexpr const RatingMode* FindRatingMode(std::string_view key) {
        for (const auto& mode : kRatingModes) {
            if (mode.key == key) return &mode;
        }
        return nullptr;
    }

    constexpr const RatingMode& RatingModeFor(MmrCategory category) {
        const auto index = static_cast<size_t>(category);
        return index < kRatingModes.size() ? kRatingModes[index] : kRatingModes.front();
    }

    constexpr bool IsRankedMode(std::string_view key) {
        const auto* mode = FindRatingMode(key);
        return mode && mode->teamSize > 0;
    }

    constexpr int ApiPlaylistId(std::string_view key) {
        const auto* mode = FindRatingMode(key);
        return mode ? mode->apiPlaylistId : 0;
    }
}

inline std::string MmrCategoryToString(MmrCategory category) {
    return PlaylistMetadata::RatingModeFor(category).key;
}

inline MmrCategory StringToMmrCategory(std::string_view key) {
    const auto* mode = PlaylistMetadata::FindRatingMode(key);
    return mode ? mode->category : MmrCategory::Best;
}

constexpr bool IsExtraMmrCategory(MmrCategory category) {
    return PlaylistMetadata::RatingModeFor(category).extra;
}
