#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "core/config/AppVersion.hpp"

// Helper to extract the version string from vcpkg.json
std::string ReadVersionFromVcpkgJson(const std::filesystem::path& vcpkgPath) {
    std::ifstream file(vcpkgPath);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"version\"") != std::string::npos) {
            size_t firstQuote = line.find('"', line.find("\"version\"") + 9);
            if (firstQuote != std::string::npos) {
                size_t secondQuote = line.find('"', firstQuote + 1);
                if (secondQuote != std::string::npos) {
                    return line.substr(firstQuote + 1, secondQuote - firstQuote - 1);
                }
            }
        }
    }
    return "";
}

TEST(VersionConsistencyTest, VerifyDynamicVersionAlignment) {
    std::filesystem::path sourceDir(OMNISTATS_SOURCE_DIR);
    std::filesystem::path binaryDir(OMNISTATS_BINARY_DIR);

    // Read the single source of truth version from vcpkg.json
    std::filesystem::path vcpkgPath = sourceDir / "vcpkg.json";
    std::string expectedVersion = ReadVersionFromVcpkgJson(vcpkgPath);
    ASSERT_FALSE(expectedVersion.empty()) << "Failed to parse version from vcpkg.json";

    // Build the expected RC formats (e.g. "1.4.3" -> "1,4,3,0" and "1.4.3.0")
    std::string expectedRcVersionCommas = expectedVersion;
    std::replace(expectedRcVersionCommas.begin(), expectedRcVersionCommas.end(), '.', ',');
    expectedRcVersionCommas += ",0";
    std::string expectedRcVersionDots = expectedVersion + ".0";

    {
        EXPECT_STREQ(AppVersion::Current, expectedVersion.c_str())
            << "AppVersion::Current compiled value mismatch with vcpkg.json version";
    }

    {
        std::filesystem::path rcPath = binaryDir / "generated-resources" / "OmniStats.rc";
        std::ifstream file(rcPath);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << rcPath.string();
        std::string line;
        bool foundFileVersion = false;
        bool foundProductVersion = false;
        bool foundStringFileVersion = false;
        bool foundStringProductVersion = false;

        while (std::getline(file, line)) {
            if (line.find("FILEVERSION") != std::string::npos && line.find("FILEFLAG") == std::string::npos) {
                EXPECT_NE(line.find(expectedRcVersionCommas), std::string::npos)
                    << "Generated resources.rc FILEVERSION mismatch: " << line;
                foundFileVersion = true;
            }
            if (line.find("PRODUCTVERSION") != std::string::npos) {
                EXPECT_NE(line.find(expectedRcVersionCommas), std::string::npos)
                    << "Generated resources.rc PRODUCTVERSION mismatch: " << line;
                foundProductVersion = true;
            }
            if (line.find("\"FileVersion\"") != std::string::npos) {
                EXPECT_NE(line.find(expectedRcVersionDots), std::string::npos)
                    << "Generated resources.rc FileVersion string mismatch: " << line;
                foundStringFileVersion = true;
            }
            if (line.find("\"ProductVersion\"") != std::string::npos) {
                EXPECT_NE(line.find(expectedRcVersionDots), std::string::npos)
                    << "Generated resources.rc ProductVersion string mismatch: " << line;
                foundStringProductVersion = true;
            }
        }
        EXPECT_TRUE(foundFileVersion) << "Could not find FILEVERSION in generated resources.rc";
        EXPECT_TRUE(foundProductVersion) << "Could not find PRODUCTVERSION in generated resources.rc";
        EXPECT_TRUE(foundStringFileVersion) << "Could not find FileVersion string in generated resources.rc";
        EXPECT_TRUE(foundStringProductVersion) << "Could not find ProductVersion string in generated resources.rc";
    }
}
