#pragma once

// Personal notes about other players. Notes are keyed by a platform-qualified
// account so two players who share a display name never share a note, and are
// stored only in the local database.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace PlayerNotes {

    inline constexpr size_t kMaxNoteCodePoints = 2000;

    struct Identity {
        std::string platform;  // TrackedAccounts::PlatformToken, e.g. "Epic"
        std::string accountId; // normalized platform account ID
        // Splitscreen guests share the host's account ID; the slot keeps them apart.
        int splitscreen = 0;

        std::string Key() const;
        bool operator==(const Identity&) const = default;
    };

    // nullopt for bots, unrecognized platforms, and malformed account IDs:
    // those players have no identity a note could be attached to safely.
    std::optional<Identity> IdentityFromPrimaryId(std::string_view primaryId);

    // Normalizes line endings to LF, drops control characters other than LF and
    // TAB, replaces invalid UTF-8, trims surrounding whitespace, and truncates to
    // kMaxNoteCodePoints. An empty result means "no note".
    std::string NormalizeNote(std::string_view text);
    size_t CodePointCount(std::string_view text);
    // Single-line excerpt, ellipsized after `maxCodePoints`.
    std::string Preview(std::string_view note, size_t maxCodePoints);

    struct Note {
        Identity identity;
        std::string text;
        std::string lastKnownName;
        int64_t createdAtUnix = 0;
        int64_t updatedAtUnix = 0;
    };

} // namespace PlayerNotes
