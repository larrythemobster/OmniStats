#include <gtest/gtest.h>
#include "core/AppVersion.hpp"
#include "network/UpdaterCommon.hpp"

TEST(UpdaterTest, VersionStringIsPopulated) {
    EXPECT_NE(AppVersion::Current[0], '\0');
}

TEST(UpdaterCommonTest, VersionComparison) {
    // Newer major
    EXPECT_TRUE(UpdaterCommon::IsNewerVersion("1.0.0", "2.0.0"));
    // Newer minor
    EXPECT_TRUE(UpdaterCommon::IsNewerVersion("1.0.0", "1.1.0"));
    // Newer patch
    EXPECT_TRUE(UpdaterCommon::IsNewerVersion("1.0.0", "1.0.1"));
    
    // Equal versions
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("1.0.0", "1.0.0"));
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("1.2.3", "1.2.3"));
    
    // Older versions
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("2.0.0", "1.0.0"));
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("1.1.0", "1.0.0"));
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("1.0.1", "1.0.0"));

    // Multi-digit components
    EXPECT_TRUE(UpdaterCommon::IsNewerVersion("1.9.12", "1.10.2"));
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("1.10.2", "1.9.12"));
}

TEST(UpdaterCommonTest, TrimWhitespace) {
    EXPECT_EQ(UpdaterCommon::Trim("  test  \r\n"), "test");
    EXPECT_EQ(UpdaterCommon::Trim("test"), "test");
    EXPECT_EQ(UpdaterCommon::Trim(""), "");
    EXPECT_EQ(UpdaterCommon::Trim(" \t \r \n "), "");
}

TEST(UpdaterCommonTest, GetAppDataDir) {
    std::string appData = UpdaterCommon::GetAppDataDir();
    EXPECT_FALSE(appData.empty());
    // Since unit tests run in a test environment, OMNISTATS_TEST_ENVIRONMENT=1 might be defined.
    // Let's check both possibilities.
#ifdef OMNISTATS_TEST_ENVIRONMENT
    EXPECT_EQ(appData.substr(appData.length() - 16), "\\omnistats_test\\");
#else
    EXPECT_EQ(appData.substr(appData.length() - 11), "\\omnistats\\");
#endif
}

TEST(UpdaterCommonTest, GetLocalAppDataDir) {
    std::string localAppData = UpdaterCommon::GetLocalAppDataDir();
    EXPECT_FALSE(localAppData.empty());
    EXPECT_EQ(localAppData.substr(localAppData.length() - 11), "\\OmniStats\\");
}
