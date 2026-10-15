#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace TrackedAccounts {

    enum class Platform {
        Epic,
        Steam,
        PlayStation,
        Xbox,
        Switch
    };

    struct Identity {
        Platform platform = Platform::Epic;
        std::string accountId;
        std::string primaryId;

        bool operator==(const Identity&) const = default;
    };

    const char* PlatformToken(Platform platform);
    const char* PlatformLabel(Platform platform);
    const char* PlatformKey(Platform platform);
    std::optional<Platform> ParsePlatform(std::string_view text);
    const std::vector<Platform>& AllPlatforms();

    bool IsValidAccountId(Platform platform, std::string_view accountId);
    std::optional<Identity> FromPrimaryId(std::string_view primaryId);
    std::string CanonicalPrimaryId(std::string_view primaryId);

    enum class InputError {
        None,
        Empty,
        TooLong,
        UnsupportedPlatform,
        PlatformMismatch,
        DisplayName,
        UnresolvableUrl,
        InvalidAccountId
    };

    struct ParseResult {
        std::optional<Identity> identity;
        InputError error = InputError::None;
        std::string message;
    };

    inline constexpr size_t kMaxInputLength = 256;
    inline constexpr size_t kMaxNicknameLength = 40;
    inline constexpr size_t kMaxDisplayNameLength = 64;

    ParseResult ParseManualInput(Platform platform, std::string_view input);
    std::string NormalizeLabel(std::string_view text, size_t maxCodePoints);

    enum class DiscoverySource {
        Manual,
        Auto,
        Import
    };
    const char* DiscoverySourceKey(DiscoverySource source);
    DiscoverySource ParseDiscoverySource(std::string_view key);

    struct PlaylistRank {
        std::string playlistKey;
        std::optional<int> mmr;
        std::string rankTier;
        std::optional<int> matchesPlayed;
        std::string source;
        int64_t fetchedAtUnix = 0;

        bool operator==(const PlaylistRank&) const = default;
    };

    // A provider-confirmed observation, not a generated intermediate MMR value.
    struct RankObservation {
        std::string primaryId;
        std::string playlistKey;
        int mmr = 0;
        std::string rankTier;
        std::optional<int> matchesPlayed;
        std::string source;
        int64_t observedAtUnix = 0;
    };

    struct RankHistorySeries {
        std::string primaryId;
        std::vector<RankObservation> observations;
        std::optional<int> highestObservedMmr;
        int64_t totalObservations = 0;
    };

    struct AccountRecord {
        std::string primaryId;
        Platform platform = Platform::Epic;
        std::string platformAccountId;
        std::string nickname;
        std::string lastKnownName;
        int64_t createdAtUnix = 0;
        int64_t lastSeenAtUnix = 0;
        int64_t lastRefreshAttemptAtUnix = 0;
        int64_t lastSuccessfulRefreshAtUnix = 0;
        int64_t sortOrder = 0;
        DiscoverySource discoverySource = DiscoverySource::Manual;
        std::map<std::string, PlaylistRank> ranks;
    };

    AccountRecord MakeRecord(const Identity& identity,
                             std::string_view nickname,
                             std::string_view lastKnownName,
                             DiscoverySource source);
    std::string DisplayLabel(const AccountRecord& record);
    std::string MaskedAccountId(std::string_view accountId);

    struct PlaylistColumn {
        std::string key;
        std::string label;
        std::string shortLabel;
        bool standard = false;
    };
    const std::vector<PlaylistColumn>& SupportedPlaylists();
    bool IsSupportedPlaylistKey(std::string_view key);

    std::string RankSourceLabel(std::string_view verificationSource);

} // namespace TrackedAccounts
