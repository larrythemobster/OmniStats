#include "core/PlaylistMetadata.hpp"
#include <array>

namespace PlaylistMetadata {
    namespace {

        // Gameplay classification intentionally reflects how OmniStats should treat
        // the current Game.PlaylistId. trackerKey is populated only for IDs Tracker's
        // profile segments expose directly; gameplay IDs can still share an MMR key
        // without being Tracker segment IDs themselves.
        constexpr std::array<Info, 70> kPlaylists{{
            {-2, "intermission", "intermission", "", "Intermission", PlaylistClass::NonRecordable, MmrCategory::Best, "", "intermission_playlist"},
            {0, "casual", "casual", "casual", "Casual", PlaylistClass::Casual, MmrCategory::Casual, "casual", nullptr},
            {1, "casual", "casual", "casual", "Duel", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {2, "casual", "casual", "casual", "Doubles", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {3, "casual", "casual", "casual", "Standard", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {4, "casual", "casual", "casual", "Chaos", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {6, "private", "private", "", "Private Match", PlaylistClass::NonRecordable, MmrCategory::Best, "", "private_match_playlist"},
            {7, "season", "season", "", "Season", PlaylistClass::NonRecordable, MmrCategory::Best, "", "season_playlist"},
            {8, "exhibition", "exhibition", "", "Exhibition", PlaylistClass::NonRecordable, MmrCategory::Best, "", "exhibition_playlist"},
            {9, "training", "training", "", "Training", PlaylistClass::NonRecordable, MmrCategory::Best, "", "training_playlist"},
            {10, "1v1", "1v1", "1v1", "Duel", PlaylistClass::Ranked, MmrCategory::OneVOne, "1v1", nullptr},
            {11, "2v2", "2v2", "2v2", "Doubles", PlaylistClass::Ranked, MmrCategory::TwoVTwo, "2v2", nullptr},
            {12, "3v3", "3v3", "3v3", "Standard", PlaylistClass::Ranked, MmrCategory::ThreeVThree, "", nullptr},
            {13, "3v3", "3v3", "3v3", "Standard", PlaylistClass::Ranked, MmrCategory::ThreeVThree, "3v3", nullptr},
            {15, "casual", "casual", "casual", "Snow Day", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {16, "casual", "casual", "casual", "Rocket Labs", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {17, "casual", "casual", "casual", "Hoops", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {18, "casual", "casual", "casual", "Rumble", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {19, "workshop", "workshop", "", "Workshop", PlaylistClass::NonRecordable, MmrCategory::Best, "", "workshop_playlist"},
            {20, "training", "training", "", "Custom Training Editor", PlaylistClass::NonRecordable, MmrCategory::Best, "", "custom_training_editor_playlist"},
            {21, "training", "training", "", "Custom Training", PlaylistClass::NonRecordable, MmrCategory::Best, "", "custom_training_playlist"},
            {22, "t", "t", "t", "Tournament Match (Custom)", PlaylistClass::Tournament, MmrCategory::Tourny, "", nullptr},
            {23, "casual", "casual", "casual", "Dropshot", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {24, "local", "local", "", "Local Match", PlaylistClass::NonRecordable, MmrCategory::Best, "", "local_match_playlist"},
            {26, "external", "external", "", "External Match", PlaylistClass::Other, MmrCategory::Best, "", nullptr},
            {27, "hoops", "hoops", "hoops", "Hoops", PlaylistClass::Ranked, MmrCategory::Hoops, "hoops", nullptr},
            {28, "rumble", "rumble", "rumble", "Rumble", PlaylistClass::Ranked, MmrCategory::Rumble, "rumble", nullptr},
            {29, "dropshot", "dropshot", "dropshot", "Dropshot", PlaylistClass::Ranked, MmrCategory::Dropshot, "dropshot", nullptr},
            {30, "snowday", "snowday", "snowday", "Snow Day", PlaylistClass::Ranked, MmrCategory::SnowDay, "snowday", nullptr},
            {31, "casual", "casual", "casual", "Ghost Hunt", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {32, "casual", "casual", "casual", "Beach Ball", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {33, "casual", "casual", "casual", "Spike Rush", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {34, "t", "t", "t", "Tournament Match", PlaylistClass::Tournament, MmrCategory::Tourny, "t", nullptr},
            {35, "casual", "casual", "casual", "Rocket Labs", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {37, "casual", "casual", "casual", "Dropshot Rumble", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {38, "casual", "casual", "casual", "Heatseeker", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {41, "casual", "casual", "casual", "Boomer Ball", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {43, "heatseeker", "heatseeker", "heatseeker", "Heatseeker", PlaylistClass::Ranked, MmrCategory::Heatseeker, "heatseeker", nullptr},
            {44, "casual", "casual", "casual", "Winter Breakaway", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {46, "casual", "casual", "casual", "Gridiron", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {47, "casual", "casual", "casual", "Super Cube", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {48, "casual", "casual", "casual", "Tactical Rumble", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {49, "casual", "casual", "casual", "Spring Loaded", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {50, "casual", "casual", "casual", "Speed Demon", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {52, "casual", "casual", "casual", "Gotham City Rumble", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {54, "casual", "casual", "casual", "Knockout", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {55, "casual", "casual", "casual", "Confidential Thirdwheel Test", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {61, "casual", "casual", "casual", "Quads", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {62, "casual", "casual", "casual", "Magnus Futball", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {64, "casual", "casual", "casual", "God Ball Spooky", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {65, "casual", "casual", "casual", "God Ball Haunted", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {66, "casual", "casual", "casual", "God Ball Ricochet", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {67, "casual", "casual", "casual", "Cubic Spooky", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {68, "casual", "casual", "casual", "G-Force Frenzy", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {70, "casual", "casual", "casual", "RumShot Doubles", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {72, "casual", "casual", "casual", "Territory", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {73, "training", "training", "", "Online Freeplay", PlaylistClass::NonRecordable, MmrCategory::Best, "", "online_freeplay_playlist"},
            {74, "casual", "casual", "casual", "Territory Doubles", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {75, "casual", "casual", "casual", "Godball Territory", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {76, "casual", "casual", "casual", "Godball Territory Doubles", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {77, "casual", "casual", "casual", "Non-Standard Soccar", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {79, "casual", "casual", "casual", "Snowday Territory", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {80, "casual", "casual", "casual", "Run It Back", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {81, "casual", "casual", "casual", "Car Wars", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {82, "casual", "casual", "casual", "Pizza Party", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {83, "casual", "casual", "casual", "Push The Puck", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {84, "casual", "casual", "casual", "Possession", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {86, "casual", "casual", "casual", "FC Showdown", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {87, "casual", "casual", "casual", "Sacrifice", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
            {88, "casual", "casual", "casual", "Jump Jam", PlaylistClass::Casual, MmrCategory::Casual, "", nullptr},
        }};

    } // namespace

    bool HasAuthoritativeId(int playlistId) {
        return playlistId != -1;
    }

    const Info* Find(int playlistId) {
        for (const auto& playlist : kPlaylists) {
            if (playlist.id == playlistId) return &playlist;
        }
        return nullptr;
    }

    bool IsKnown(int playlistId) {
        return Find(playlistId) != nullptr;
    }

    bool IsCasual(int playlistId) {
        const auto* info = Find(playlistId);
        return info && info->playlistClass == PlaylistClass::Casual;
    }

    bool IsNonRecordable(int playlistId) {
        const auto* info = Find(playlistId);
        return info && info->playlistClass == PlaylistClass::NonRecordable;
    }

    bool IsRanked(int playlistId) {
        const auto* info = Find(playlistId);
        return info && info->playlistClass == PlaylistClass::Ranked;
    }

    const char* NonRecordableReason(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->nonRecordableReason : nullptr;
    }

    std::string CanonicalMode(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->canonicalMode : "";
    }

    std::string StorageMode(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->storageMode : "";
    }

    std::string MmrKey(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->mmrKey : "";
    }

    std::string DisplayName(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->displayName : "Unknown";
    }

    MmrCategory Category(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->mmrCategory : MmrCategory::Best;
    }

    std::string TrackerKey(int playlistId) {
        const auto* info = Find(playlistId);
        return info ? info->trackerKey : "";
    }

} // namespace PlaylistMetadata
