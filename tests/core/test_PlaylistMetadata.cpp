#include <gtest/gtest.h>
#include "core/PlaylistMetadata.hpp"

TEST(PlaylistMetadataTest, CollapsesEveryCasualPlaylistIntoSingleModeAndMmrBucket) {
    EXPECT_TRUE(PlaylistMetadata::IsCasual(1));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(1), "casual");
    EXPECT_EQ(PlaylistMetadata::StorageMode(1), "casual");
    EXPECT_EQ(PlaylistMetadata::MmrKey(1), "casual");

    EXPECT_TRUE(PlaylistMetadata::IsCasual(2));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(2), "casual");

    EXPECT_TRUE(PlaylistMetadata::IsCasual(3));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(3), "casual");

    EXPECT_TRUE(PlaylistMetadata::IsCasual(4));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(4), "casual");
}

TEST(PlaylistMetadataTest, KeepsCasualAndRankedExtraModesDistinct) {
    EXPECT_TRUE(PlaylistMetadata::IsCasual(17));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(17), "casual");
    EXPECT_EQ(PlaylistMetadata::MmrKey(17), "casual");
    EXPECT_EQ(PlaylistMetadata::Category(17), MmrCategory::Casual);

    EXPECT_TRUE(PlaylistMetadata::IsRanked(27));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(27), "hoops");
    EXPECT_EQ(PlaylistMetadata::MmrKey(27), "hoops");
    EXPECT_EQ(PlaylistMetadata::Category(27), MmrCategory::Hoops);

    // 38 is the 3v3 Heatseeker LTM. Ranked Heatseeker uses the 2v2 playlist.
    EXPECT_TRUE(PlaylistMetadata::IsCasual(38));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(38), "casual");
    EXPECT_EQ(PlaylistMetadata::MmrKey(38), "casual");
    EXPECT_EQ(PlaylistMetadata::Category(38), MmrCategory::Casual);

    EXPECT_TRUE(PlaylistMetadata::IsRanked(43));
    EXPECT_EQ(PlaylistMetadata::MmrKey(43), "heatseeker");
    EXPECT_EQ(PlaylistMetadata::Category(43), MmrCategory::Heatseeker);
}

TEST(PlaylistMetadataTest, TrackerExposureIsExplicitAndUnknownIdsStayUnknown) {
    EXPECT_EQ(PlaylistMetadata::TrackerKey(0), "casual");
    EXPECT_EQ(PlaylistMetadata::TrackerKey(10), "1v1");
    EXPECT_EQ(PlaylistMetadata::TrackerKey(43), "heatseeker");
    EXPECT_EQ(PlaylistMetadata::TrackerKey(2), "");

    EXPECT_TRUE(PlaylistMetadata::HasAuthoritativeId(999));
    EXPECT_FALSE(PlaylistMetadata::IsKnown(999));
    EXPECT_EQ(PlaylistMetadata::CanonicalMode(999), "");
    EXPECT_FALSE(PlaylistMetadata::HasAuthoritativeId(-1));
}
