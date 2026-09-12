#pragma once

#include "core/SessionState.hpp"
#include <string>

namespace PlaylistMetadata {

    enum class PlaylistClass {
        Casual,
        Ranked,
        Tournament,
        NonRecordable,
        Other
    };

    struct Info {
        int id = -1;
        const char* canonicalMode = "";
        const char* storageMode = "";
        const char* mmrKey = "";
        const char* displayName = "Unknown";
        PlaylistClass playlistClass = PlaylistClass::Other;
        MmrCategory mmrCategory = MmrCategory::Best;
        const char* trackerKey = "";
        const char* nonRecordableReason = nullptr;
    };

    // -1 means Rocket League did not supply Game.PlaylistId. Every other value is
    // authoritative telemetry, even when OmniStats does not recognize the ID yet.
    bool HasAuthoritativeId(int playlistId);
    const Info* Find(int playlistId);
    bool IsKnown(int playlistId);
    bool IsCasual(int playlistId);
    bool IsNonRecordable(int playlistId);
    bool IsRanked(int playlistId);
    const char* NonRecordableReason(int playlistId);
    std::string CanonicalMode(int playlistId);
    std::string StorageMode(int playlistId);
    std::string MmrKey(int playlistId);
    std::string DisplayName(int playlistId);
    MmrCategory Category(int playlistId);
    std::string TrackerKey(int playlistId);

} // namespace PlaylistMetadata
