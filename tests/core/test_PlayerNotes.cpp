#include <gtest/gtest.h>

#include <string>

#include "core/accounts/PlayerNotes.hpp"

TEST(PlayerNotesTest, IdentityIsPlatformQualifiedAndNormalized) {
    const auto steam = PlayerNotes::IdentityFromPrimaryId("Steam|76561198000000001|0");
    ASSERT_TRUE(steam);
    EXPECT_EQ(steam->platform, "Steam");
    EXPECT_EQ(steam->accountId, "76561198000000001");
    EXPECT_EQ(steam->Key(), "Steam|76561198000000001");

    const auto epic = PlayerNotes::IdentityFromPrimaryId("epic|0123456789ABCDEF0123456789ABCDEF|0");
    ASSERT_TRUE(epic);
    EXPECT_EQ(epic->platform, "Epic");
    EXPECT_EQ(epic->accountId, "0123456789abcdef0123456789abcdef");

    const auto psn = PlayerNotes::IdentityFromPrimaryId("PS4|1234567890123456789|0");
    const auto xbox = PlayerNotes::IdentityFromPrimaryId("XboxOne|2535400000000000|0");
    const auto nintendo = PlayerNotes::IdentityFromPrimaryId("Switch|abcdef0123|0");
    ASSERT_TRUE(psn && xbox && nintendo);
    EXPECT_EQ(psn->Key(), "PS4|1234567890123456789");
    EXPECT_EQ(xbox->Key(), "XboxOne|2535400000000000");
    EXPECT_EQ(nintendo->Key(), "Switch|abcdef0123");
}

TEST(PlayerNotesTest, SameAccountNumberOnDifferentPlatformsOrSplitscreenSlotsStaysDistinct) {
    const auto psn = PlayerNotes::IdentityFromPrimaryId("PS4|2535400000000000|0");
    const auto xbox = PlayerNotes::IdentityFromPrimaryId("XboxOne|2535400000000000|0");
    ASSERT_TRUE(psn && xbox);
    EXPECT_NE(psn->Key(), xbox->Key());

    const auto host = PlayerNotes::IdentityFromPrimaryId("Epic|0123456789abcdef0123456789abcdef|0");
    const auto guest = PlayerNotes::IdentityFromPrimaryId("Epic|0123456789abcdef0123456789abcdef|1");
    ASSERT_TRUE(host && guest);
    EXPECT_EQ(guest->splitscreen, 1);
    EXPECT_NE(host->Key(), guest->Key());
}

TEST(PlayerNotesTest, PlayersWithoutAStableAccountHaveNoIdentity) {
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId("Unknown|Merlin"));
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId("Unknown|0|0"));
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId("Epic|DisplayName|0"));
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId("Steam|123|0"));
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId("Stadia|whatever|0"));
    EXPECT_FALSE(PlayerNotes::IdentityFromPrimaryId(""));
}

TEST(PlayerNotesTest, NormalizationKeepsTextButStripsControlsAndSurroundingWhitespace) {
    EXPECT_EQ(PlayerNotes::NormalizeNote("  \r\nGood kickoffs\r\nRotates well\t\n\n "), "Good kickoffs\nRotates well");
    EXPECT_EQ(PlayerNotes::NormalizeNote(std::string("a\0b\x07"
                                                     "c",
                                                     5)),
              "abc");
    EXPECT_EQ(PlayerNotes::NormalizeNote("old\rmac"), "old\nmac");
    EXPECT_EQ(PlayerNotes::NormalizeNote(" \n\t "), "");

    const std::string markup = "<div class='x'>{{note}}</div> & \"quotes\" \xC3\xA9\xF0\x9F\x8E\xAE";
    EXPECT_EQ(PlayerNotes::NormalizeNote(markup), markup);
    EXPECT_EQ(PlayerNotes::NormalizeNote("bad\xFF"
                                         "byte"),
              "bad\xEF\xBF\xBD"
              "byte");
}

TEST(PlayerNotesTest, NormalizationTruncatesOnCodePointBoundaries) {
    std::string longNote;
    for (size_t i = 0; i < PlayerNotes::kMaxNoteCodePoints + 50; ++i)
        longNote += "\xC3\xA9";
    const std::string normalized = PlayerNotes::NormalizeNote(longNote);
    EXPECT_EQ(PlayerNotes::CodePointCount(normalized), PlayerNotes::kMaxNoteCodePoints);
    EXPECT_EQ(normalized.size(), PlayerNotes::kMaxNoteCodePoints * 2);

    const std::string exact(PlayerNotes::kMaxNoteCodePoints, 'x');
    EXPECT_EQ(PlayerNotes::NormalizeNote(exact), exact);
}

TEST(PlayerNotesTest, PreviewIsSingleLineAndEllipsized) {
    EXPECT_EQ(PlayerNotes::Preview("Good mechanics\n\npoor game sense", 100), "Good mechanics poor game sense");
    EXPECT_EQ(PlayerNotes::Preview("abcdefghij", 4), "abcd\xE2\x80\xA6");
}
