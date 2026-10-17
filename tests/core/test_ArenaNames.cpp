#include <gtest/gtest.h>
#include "core/game/ArenaNames.hpp"

TEST(ArenaNamesTest, PackageNamesResolveCaseInsensitivelyToInGameNames) {
    EXPECT_EQ(ArenaNames::DisplayName("EuroStadium_Rainy_P"), "Mannfield (Stormy)");
    EXPECT_EQ(ArenaNames::DisplayName("cs_p"), "Champions Field");
    EXPECT_EQ(ArenaNames::DisplayName("Stadium_Foggy_P"), "DFH Stadium (Stormy)");
    EXPECT_EQ(ArenaNames::DisplayName("Wasteland_P"), "Badlands");
    EXPECT_EQ(ArenaNames::DisplayName("wasteland_s_p"), "Wasteland");
    EXPECT_EQ(ArenaNames::DisplayName("NeoTokyo_P"), "Tokyo Underpass");
    EXPECT_EQ(ArenaNames::DisplayName("NeoTokyo_Standard_P"), "Neo Tokyo");
    EXPECT_EQ(ArenaNames::DisplayName("Paname_Dusk_P"), "Parc de Paris");
}

TEST(ArenaNamesTest, UnknownPackagesBecomeReadableInsteadOfRaw) {
    EXPECT_EQ(ArenaNames::DisplayName("Labs_NewThing_Night_P"), "Labs Newthing Night");
    EXPECT_EQ(ArenaNames::DisplayName(""), "");
}

TEST(ArenaNamesTest, RepairOnlyRewritesRawOrKnownWrongStoredNames) {
    EXPECT_EQ(ArenaNames::RepairStoredName("street_p"), "Sovereign Heights");
    EXPECT_EQ(ArenaNames::RepairStoredName("Champions Field"), "Mannfield");
    EXPECT_EQ(ArenaNames::RepairStoredName("DFH Stadium (10a)"), "DFH Stadium (10th Anniversary)");
    EXPECT_EQ(ArenaNames::RepairStoredName("Mannfield"), "Mannfield");
    EXPECT_EQ(ArenaNames::RepairStoredName("Neo Tokyo"), "Neo Tokyo");
    EXPECT_EQ(ArenaNames::RepairStoredName("Some Custom Map"), "Some Custom Map");
}
