#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "core/AppVersion.hpp"

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

    // 1. Check CMakeLists.txt uses ${OMNISTATS_VERSION}
    {
        std::filesystem::path cmakePath = sourceDir / "CMakeLists.txt";
        std::ifstream file(cmakePath);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << cmakePath.string();
        std::string line;
        bool found = false;
        while (std::getline(file, line)) {
            if (line.find("project(OmniStats VERSION") != std::string::npos) {
                EXPECT_NE(line.find("${OMNISTATS_VERSION}"), std::string::npos)
                    << "CMakeLists.txt project() does not reference dynamic ${OMNISTATS_VERSION} variable: " << line;
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Could not find project VERSION line in CMakeLists.txt";
    }

    // 2. Check AppVersion.hpp uses OMNISTATS_VERSION macro
    {
        std::filesystem::path versionPath = sourceDir / "src" / "core" / "AppVersion.hpp";
        std::ifstream file(versionPath);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << versionPath.string();
        std::string line;
        bool found = false;
        while (std::getline(file, line)) {
            if (line.find("Current") != std::string::npos) {
                EXPECT_NE(line.find("OMNISTATS_VERSION"), std::string::npos)
                    << "AppVersion.hpp Current does not use OMNISTATS_VERSION macro: " << line;
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found) << "Could not find Current definition in AppVersion.hpp";
    }

    // 3. Check compiled binary version matches vcpkg.json
    {
        EXPECT_STREQ(AppVersion::Current, expectedVersion.c_str())
            << "AppVersion::Current compiled value mismatch with vcpkg.json version";
    }

    // 4. Check resources.rc.in template placeholders
    {
        std::filesystem::path rcInPath = sourceDir / "resources" / "omnistats.rc.in";
        std::ifstream file(rcInPath);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << rcInPath.string();
        std::string line;
        bool foundFileVersion = false;
        bool foundProductVersion = false;
        bool foundStringFileVersion = false;
        bool foundStringProductVersion = false;

        while (std::getline(file, line)) {
            if (line.find("FILEVERSION") != std::string::npos && line.find("FILEFLAG") == std::string::npos) {
                EXPECT_NE(line.find("@OMNISTATS_VERSION_COMMA@"), std::string::npos)
                    << "resources.rc.in FILEVERSION placeholder mismatch: " << line;
                foundFileVersion = true;
            }
            if (line.find("PRODUCTVERSION") != std::string::npos) {
                EXPECT_NE(line.find("@OMNISTATS_VERSION_COMMA@"), std::string::npos)
                    << "resources.rc.in PRODUCTVERSION placeholder mismatch: " << line;
                foundProductVersion = true;
            }
            if (line.find("\"FileVersion\"") != std::string::npos) {
                EXPECT_NE(line.find("@PROJECT_VERSION@.0"), std::string::npos)
                    << "resources.rc.in FileVersion placeholder mismatch: " << line;
                foundStringFileVersion = true;
            }
            if (line.find("\"ProductVersion\"") != std::string::npos) {
                EXPECT_NE(line.find("@PROJECT_VERSION@.0"), std::string::npos)
                    << "resources.rc.in ProductVersion placeholder mismatch: " << line;
                foundStringProductVersion = true;
            }
        }
        EXPECT_TRUE(foundFileVersion) << "Could not find FILEVERSION placeholder in resources.rc.in";
        EXPECT_TRUE(foundProductVersion) << "Could not find PRODUCTVERSION placeholder in resources.rc.in";
        EXPECT_TRUE(foundStringFileVersion) << "Could not find FileVersion placeholder in resources.rc.in";
        EXPECT_TRUE(foundStringProductVersion) << "Could not find ProductVersion placeholder in resources.rc.in";
    }

    // 5. Check generated resources.rc values
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

TEST(PublicBuildSeparationTest, MainAppSourcesDoNotDependOnUpdaterImplementation) {
    std::filesystem::path sourceDir(OMNISTATS_SOURCE_DIR);
    const std::vector<std::filesystem::path> mainAppFiles = {
        sourceDir / "src" / "main.cpp",
        sourceDir / "src" / "ui" / "Overlay.cpp",
        sourceDir / "src" / "ui" / "panels" / "DashboardPanel.cpp",
        sourceDir / "src" / "ui" / "panels" / "SettingsPanel.cpp",
        sourceDir / "src" / "network" / "TelemetryManager.cpp",
    };

    for (const auto& path : mainAppFiles) {
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open()) << "Failed to open " << path.string();
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        EXPECT_EQ(content.find("network/Updater.hpp"), std::string::npos) << path.string();
        EXPECT_EQ(content.find("Updater::"), std::string::npos) << path.string();
    }
}

TEST(PublicBuildSeparationTest, InputManagerKeepsLegacyHookCompileGated) {
    std::filesystem::path sourceDir(OMNISTATS_SOURCE_DIR);
    std::ifstream file(sourceDir / "src" / "core" / "InputManager.cpp");
    ASSERT_TRUE(file.is_open());
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("RegisterHotKey"), std::string::npos);
    EXPECT_NE(content.find("OMNISTATS_ENABLE_LOW_LEVEL_HOOK"), std::string::npos);
}
