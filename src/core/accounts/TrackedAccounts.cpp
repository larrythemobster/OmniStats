#include "core/accounts/TrackedAccounts.hpp"

#include "core/game/PlaylistMetadata.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace TrackedAccounts {
    namespace {
        std::string_view Trim(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);
            return text;
        }

        std::string Lower(std::string_view text) {
            std::string out(text);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        bool AllOf(std::string_view text, int (*predicate)(int)) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [predicate](char c) {
                return predicate(static_cast<unsigned char>(c)) != 0;
            });
        }

        bool IsHex(std::string_view text) {
            return AllOf(text, std::isxdigit);
        }

        bool IsDigits(std::string_view text) {
            return AllOf(text, std::isdigit);
        }

        std::string NormalizeAccountId(Platform platform, std::string_view accountId) {
            const std::string_view trimmed = Trim(accountId);
            if (platform == Platform::Epic || platform == Platform::Switch) return Lower(trimmed);
            return std::string(trimmed);
        }

        Identity MakeIdentity(Platform platform, std::string accountId) {
            Identity identity;
            identity.platform = platform;
            identity.primaryId = std::string(PlatformToken(platform)) + "|" + accountId;
            identity.accountId = std::move(accountId);
            return identity;
        }

        std::vector<std::string_view> SplitPath(std::string_view path) {
            std::vector<std::string_view> parts;
            while (!path.empty()) {
                const size_t slash = path.find('/');
                const std::string_view part = path.substr(0, slash);
                if (!part.empty()) parts.push_back(part);
                if (slash == std::string_view::npos) break;
                path.remove_prefix(slash + 1);
            }
            return parts;
        }

        ParseResult Fail(InputError error, std::string message) {
            ParseResult result;
            result.error = error;
            result.message = std::move(message);
            return result;
        }

        std::string InvalidIdMessage(Platform platform, std::string_view accountId) {
            switch (platform) {
            case Platform::Epic:
                if (!IsHex(accountId))
                    return "That looks like a display name. Epic accounts need the 32-character account ID shown in your Epic Games account settings.";
                return "Epic account IDs are exactly 32 hexadecimal characters.";
            case Platform::Steam:
                if (!IsDigits(accountId))
                    return "Steam names and custom profile URLs can't be resolved. Enter the 17-digit SteamID64.";
                return "Steam needs the 17-digit SteamID64, which starts with 7656119.";
            case Platform::PlayStation:
                if (!IsDigits(accountId))
                    return "PSN Online IDs aren't unique identifiers. Enter the numeric PlayStation account ID.";
                return "PlayStation account IDs are numeric (up to 20 digits).";
            case Platform::Xbox:
                if (!IsDigits(accountId))
                    return "Gamertags aren't unique identifiers. Enter the numeric Xbox user ID (XUID).";
                return "Xbox user IDs (XUIDs) are numeric (up to 20 digits).";
            case Platform::Switch:
                return "Switch account IDs are hexadecimal (up to 32 characters).";
            }
            return "That account ID is not valid for this platform.";
        }

        ParseResult ParseRawId(Platform platform, std::string_view rawId) {
            const std::string accountId = NormalizeAccountId(platform, rawId);
            if (accountId.empty()) return Fail(InputError::Empty, "Enter an account ID.");
            if (!IsValidAccountId(platform, accountId)) {
                const bool displayName =
                    (platform == Platform::Epic && !IsHex(accountId)) ||
                    ((platform == Platform::Steam || platform == Platform::PlayStation || platform == Platform::Xbox) && !IsDigits(accountId));
                return Fail(displayName ? InputError::DisplayName : InputError::InvalidAccountId, InvalidIdMessage(platform, accountId));
            }
            ParseResult result;
            result.identity = MakeIdentity(platform, accountId);
            return result;
        }

        ParseResult ParseUrl(Platform platform, std::string_view input) {
            std::string_view rest = input;
            if (const size_t scheme = rest.find("://"); scheme != std::string_view::npos) rest.remove_prefix(scheme + 3);
            const size_t cut = rest.find_first_of("?#");
            if (cut != std::string_view::npos) rest = rest.substr(0, cut);
            const size_t slash = rest.find('/');
            const std::string host = Lower(rest.substr(0, slash));
            const std::vector<std::string_view> path = SplitPath(slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1));

            const auto hostIs = [&](std::string_view domain) {
                return host == domain || (host.size() > domain.size() && host.ends_with(domain) && host[host.size() - domain.size() - 1] == '.');
            };
            const auto requireSteam = [&](std::string_view id) -> ParseResult {
                if (platform != Platform::Steam) return Fail(InputError::PlatformMismatch, "This is a Steam profile link. Select Steam first.");
                if (!IsValidAccountId(Platform::Steam, id))
                    return Fail(InputError::UnresolvableUrl, "This Steam link doesn't contain a SteamID64. Use a steamcommunity.com/profiles/<SteamID64> link or paste the 17-digit ID.");
                return ParseRawId(Platform::Steam, id);
            };

            if (hostIs("steamcommunity.com")) {
                if (path.size() >= 2 && Lower(path[0]) == "profiles") return requireSteam(path[1]);
                if (path.size() >= 2 && Lower(path[0]) == "id") {
                    if (platform != Platform::Steam) return Fail(InputError::PlatformMismatch, "This is a Steam profile link. Select Steam first.");
                    return Fail(InputError::UnresolvableUrl, "Steam custom profile URLs can't be resolved. Use the 17-digit SteamID64 instead.");
                }
                return Fail(InputError::UnresolvableUrl, "Unrecognized Steam link. Paste the 17-digit SteamID64 instead.");
            }
            if (hostIs("tracker.network") || hostIs("tracker.gg")) {
                auto profile = std::find_if(path.begin(), path.end(), [](std::string_view part) { return Lower(part) == "profile"; });
                if (profile != path.end() && std::distance(profile, path.end()) >= 3) {
                    const std::string trackerPlatform = Lower(*(profile + 1));
                    const std::string_view ident = *(profile + 2);
                    if (trackerPlatform == "steam") return requireSteam(ident);
                }
                return Fail(InputError::UnresolvableUrl, "Tracker links for this platform use display names, which can't identify an account. Enter the platform account ID instead.");
            }
            return Fail(InputError::UnresolvableUrl, "Unsupported link. Enter the platform account ID instead.");
        }

        bool LooksLikeUrl(std::string_view input) {
            const std::string lower = Lower(input.substr(0, std::min<size_t>(input.size(), 64)));
            return lower.find("://") != std::string::npos || lower.starts_with("www.") ||
                   lower.find("steamcommunity.com") != std::string::npos || lower.find("tracker.network") != std::string::npos ||
                   lower.find("tracker.gg") != std::string::npos;
        }
    }

    const char* PlatformToken(Platform platform) {
        switch (platform) {
        case Platform::Epic:
            return "Epic";
        case Platform::Steam:
            return "Steam";
        case Platform::PlayStation:
            return "PS4";
        case Platform::Xbox:
            return "XboxOne";
        case Platform::Switch:
            return "Switch";
        }
        return "Epic";
    }

    const char* PlatformLabel(Platform platform) {
        switch (platform) {
        case Platform::Epic:
            return "Epic";
        case Platform::Steam:
            return "Steam";
        case Platform::PlayStation:
            return "PlayStation";
        case Platform::Xbox:
            return "Xbox";
        case Platform::Switch:
            return "Switch";
        }
        return "Epic";
    }

    const char* PlatformKey(Platform platform) {
        switch (platform) {
        case Platform::Epic:
            return "epic";
        case Platform::Steam:
            return "steam";
        case Platform::PlayStation:
            return "playstation";
        case Platform::Xbox:
            return "xbox";
        case Platform::Switch:
            return "switch";
        }
        return "epic";
    }

    std::optional<Platform> ParsePlatform(std::string_view text) {
        const std::string key = Lower(Trim(text));
        if (key == "epic" || key == "epicgames") return Platform::Epic;
        if (key == "steam") return Platform::Steam;
        if (key == "ps4" || key == "ps5" || key == "psn" || key == "playstation") return Platform::PlayStation;
        if (key == "xbox" || key == "xboxone" || key == "xbl") return Platform::Xbox;
        if (key == "switch" || key == "nintendo") return Platform::Switch;
        return std::nullopt;
    }

    const std::vector<Platform>& AllPlatforms() {
        static const std::vector<Platform> platforms = {
            Platform::Epic, Platform::Steam, Platform::PlayStation, Platform::Xbox, Platform::Switch};
        return platforms;
    }

    bool IsValidAccountId(Platform platform, std::string_view accountId) {
        switch (platform) {
        case Platform::Epic:
            return accountId.size() == 32 && IsHex(accountId);
        case Platform::Steam:
            return accountId.size() == 17 && IsDigits(accountId) && accountId.starts_with("7656119");
        case Platform::PlayStation:
        case Platform::Xbox:
            return accountId.size() <= 20 && IsDigits(accountId) && accountId.find_first_not_of('0') != std::string_view::npos;
        case Platform::Switch:
            return accountId.size() <= 32 && IsHex(accountId);
        }
        return false;
    }

    std::optional<Identity> FromPrimaryId(std::string_view primaryId) {
        primaryId = Trim(primaryId);
        if (primaryId.empty() || primaryId.size() > kMaxInputLength) return std::nullopt;
        const size_t delim = primaryId.find('|');
        if (delim == std::string_view::npos) return std::nullopt;
        const auto platform = ParsePlatform(primaryId.substr(0, delim));
        if (!platform) return std::nullopt;
        std::string_view rest = primaryId.substr(delim + 1);
        rest = rest.substr(0, rest.find('|'));
        std::string accountId = NormalizeAccountId(*platform, rest);
        if (!IsValidAccountId(*platform, accountId)) return std::nullopt;
        return MakeIdentity(*platform, std::move(accountId));
    }

    std::string CanonicalPrimaryId(std::string_view primaryId) {
        const auto identity = FromPrimaryId(primaryId);
        return identity ? identity->primaryId : std::string{};
    }

    ParseResult ParseManualInput(Platform platform, std::string_view input) {
        if (input.size() > kMaxInputLength) return Fail(InputError::TooLong, "That input is too long to be an account ID.");
        input = Trim(input);
        if (input.empty()) return Fail(InputError::Empty, "Enter an account ID.");
        if (LooksLikeUrl(input)) return ParseUrl(platform, input);
        if (input.find('|') != std::string_view::npos) {
            const size_t delim = input.find('|');
            const auto embedded = ParsePlatform(input.substr(0, delim));
            if (!embedded) return Fail(InputError::UnsupportedPlatform, "Unsupported platform in that ID.");
            if (*embedded != platform)
                return Fail(InputError::PlatformMismatch, std::string("That ID belongs to ") + PlatformLabel(*embedded) + ". Select " + PlatformLabel(*embedded) + " first.");
            std::string_view rest = input.substr(delim + 1);
            return ParseRawId(platform, rest.substr(0, rest.find('|')));
        }
        return ParseRawId(platform, input);
    }

    std::string NormalizeLabel(std::string_view text, size_t maxCodePoints) {
        std::string out;
        out.reserve(std::min(text.size(), maxCodePoints * 4));
        size_t codePoints = 0;
        size_t i = 0;
        bool pendingSpace = false;
        while (i < text.size() && codePoints < maxCodePoints) {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            size_t length = 0;
            if (lead < 0x80)
                length = 1;
            else if ((lead & 0xE0) == 0xC0 && lead >= 0xC2)
                length = 2;
            else if ((lead & 0xF0) == 0xE0)
                length = 3;
            else if ((lead & 0xF8) == 0xF0 && lead <= 0xF4)
                length = 4;
            if (length == 0 || i + length > text.size()) {
                ++i;
                continue;
            }
            bool valid = true;
            for (size_t k = 1; k < length; ++k) {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) valid = false;
            }
            if (!valid) {
                ++i;
                continue;
            }
            if (length == 1) {
                if (lead < 0x20 || lead == 0x7F || lead == ' ') {
                    pendingSpace = !out.empty();
                    ++i;
                    continue;
                }
            } else if (length == 2 && lead == 0xC2 && static_cast<unsigned char>(text[i + 1]) < 0xA0) {
                i += length;
                continue;
            }
            if (pendingSpace) {
                if (codePoints + 1 >= maxCodePoints) break;
                out.push_back(' ');
                ++codePoints;
                pendingSpace = false;
            }
            out.append(text.data() + i, length);
            ++codePoints;
            i += length;
        }
        return out;
    }

    const char* DiscoverySourceKey(DiscoverySource source) {
        switch (source) {
        case DiscoverySource::Manual:
            return "manual";
        case DiscoverySource::Auto:
            return "auto";
        case DiscoverySource::Import:
            return "import";
        }
        return "manual";
    }

    DiscoverySource ParseDiscoverySource(std::string_view key) {
        if (key == "auto") return DiscoverySource::Auto;
        if (key == "import") return DiscoverySource::Import;
        return DiscoverySource::Manual;
    }

    AccountRecord MakeRecord(const Identity& identity,
                             std::string_view nickname,
                             std::string_view lastKnownName,
                             DiscoverySource source) {
        AccountRecord record;
        record.primaryId = identity.primaryId;
        record.platform = identity.platform;
        record.platformAccountId = identity.accountId;
        record.nickname = NormalizeLabel(nickname, kMaxNicknameLength);
        record.lastKnownName = NormalizeLabel(lastKnownName, kMaxDisplayNameLength);
        record.discoverySource = source;
        return record;
    }

    std::string DisplayLabel(const AccountRecord& record) {
        if (!record.nickname.empty()) return record.nickname;
        if (!record.lastKnownName.empty()) return record.lastKnownName;
        return std::string(PlatformLabel(record.platform)) + " " + MaskedAccountId(record.platformAccountId);
    }

    std::string MaskedAccountId(std::string_view accountId) {
        if (accountId.size() <= 12) return std::string(accountId);
        return std::string(accountId.substr(0, 6)) + "\xE2\x80\xA6" + std::string(accountId.substr(accountId.size() - 4));
    }

    const std::vector<PlaylistColumn>& SupportedPlaylists() {
        static const std::vector<PlaylistColumn> columns = [] {
            std::vector<PlaylistColumn> out;
            for (int id = 0; id < 256; ++id) {
                const auto* info = PlaylistMetadata::Find(id);
                if (!info || info->trackerKey[0] == '\0') continue;
                if (info->playlistClass != PlaylistMetadata::PlaylistClass::Ranked &&
                    info->playlistClass != PlaylistMetadata::PlaylistClass::Tournament) {
                    continue;
                }
                const std::string key = info->trackerKey;
                const bool standard = info->mmrCategory == MmrCategory::OneVOne ||
                                      info->mmrCategory == MmrCategory::TwoVTwo ||
                                      info->mmrCategory == MmrCategory::ThreeVThree;
                out.push_back(PlaylistColumn{key, info->displayName, standard ? key : std::string(info->displayName), standard});
            }
            return out;
        }();
        return columns;
    }

    bool IsSupportedPlaylistKey(std::string_view key) {
        const auto& columns = SupportedPlaylists();
        return std::any_of(columns.begin(), columns.end(), [key](const PlaylistColumn& column) { return column.key == key; });
    }

    std::string RankSourceLabel(std::string_view verificationSource) {
        if (verificationSource == "ServerA") return "OmniStats API";
        return std::string(verificationSource);
    }

} // namespace TrackedAccounts
