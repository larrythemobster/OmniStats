#include "network/ExternalUpdaterLauncher.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include "core/AppVersion.hpp"
#include "network/UpdaterCommon.hpp"
#include "core/FileHash.hpp"
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

TEST(ExternalUpdaterLauncherTest, RepairStatsApiLaunchesUpdaterProcess) {
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "OmniStatsLauncherTest";
    std::filesystem::create_directories(tempDir);
    const std::filesystem::path iniPath = tempDir / "DefaultStatsAPI.ini";

    {
        std::ofstream file(iniPath, std::ios::binary);
        file << "[StatsAPI]\r\nPacketSendRate=0\r\nPort=12345\r\n";
    }

    bool ok = ExternalUpdaterLauncher::RepairStatsApiConfig(iniPath.string(), 49123);
    EXPECT_TRUE(ok);

    {
        std::ifstream result(iniPath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
        EXPECT_NE(content.find("PacketSendRate=30"), std::string::npos);
        EXPECT_NE(content.find("Port=49123"), std::string::npos);
    }

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
}

TEST(UpdaterCommonTest, VerifyAuthenticodeSignatureRejectsNonExistentFile) {
    auto result = UpdaterCommon::VerifyAuthenticodeSignature("C:\\NonExistentPath\\FakeFile.msi");
    EXPECT_FALSE(result.digestValid);
}

TEST(UpdaterCommonTest, VerifyAuthenticodeSignatureRejectsUnsignedFile) {
    const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "omnistats_unsigned_test.tmp";
    {
        std::ofstream out(tempFile, std::ios::binary);
        out << "Not a signed file content\n";
    }
    auto result = UpdaterCommon::VerifyAuthenticodeSignature(tempFile.string());
    EXPECT_FALSE(result.digestValid);
    std::error_code ec;
    std::filesystem::remove(tempFile, ec);
}

TEST(UpdaterCommonTest, VerifyMsiPackageRejectsFileHashMismatch) {
    const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "omnistats_hash_mismatch.msi";
    {
        std::ofstream out(tempFile, std::ios::binary);
        out << "Fake MSI payload for hash mismatch test\n";
    }
    const std::string wrongHash = "0000000000000000000000000000000000000000000000000000000000000000";
    bool ok = UpdaterCommon::VerifyMsiPackage(tempFile.string(), wrongHash, "fake_cert_hash");
    EXPECT_FALSE(ok);
    std::error_code ec;
    std::filesystem::remove(tempFile, ec);
}

TEST(UpdaterCommonTest, VerifyMsiPackageRejectsUnsignedFileEvenIfFileHashMatches) {
    const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "omnistats_unsigned_hash_ok.msi";
    {
        std::ofstream out(tempFile, std::ios::binary);
        out << "Dummy payload for unsigned check\n";
    }
    std::string realHash = CalculateSHA256(tempFile.string());
    ASSERT_FALSE(realHash.empty());

    // Pass matching file hash, but unsigned file must still be rejected
    bool ok = UpdaterCommon::VerifyMsiPackage(tempFile.string(), realHash,
                                              SigningConfig::CURRENT_CERT_SHA256);
    EXPECT_FALSE(ok);
    std::error_code ec;
    std::filesystem::remove(tempFile, ec);
}

TEST(UpdaterCommonTest, VerifyMsiPackageRejectsWrongCertHash) {
    const std::filesystem::path tempFile = std::filesystem::temp_directory_path() / "omnistats_wrong_cert.msi";
    {
        std::ofstream out(tempFile, std::ios::binary);
        out << "Another dummy payload\n";
    }
    std::string realHash = CalculateSHA256(tempFile.string());

    bool ok = UpdaterCommon::VerifyMsiPackage(tempFile.string(), realHash,
                                              "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
    EXPECT_FALSE(ok);
    std::error_code ec;
    std::filesystem::remove(tempFile, ec);
}

TEST(UpdaterCommonTest, VerifyMsiPackageAcceptsSignedArtifactWhenHashesMatch) {
    const std::filesystem::path msiPath =
        std::filesystem::path(OMNISTATS_BINARY_DIR) / "Release" / "OmniStats.msi";
    if (!std::filesystem::exists(msiPath)) {
        GTEST_SKIP() << "OmniStats.msi not present in build directory, skipping signed artifact check.";
    }

    std::string realHash = CalculateSHA256(msiPath.string());
    ASSERT_FALSE(realHash.empty());

    // 1. Accepts with default current pinned cert
    bool ok = UpdaterCommon::VerifyMsiPackage(msiPath.string(), realHash);
    EXPECT_TRUE(ok);

    // 2. Rotation support: accepts if nextPinnedCertSha matches even if current differs
    bool rotationOk = UpdaterCommon::VerifyMsiPackage(
        msiPath.string(), realHash,
        "0000000000000000000000000000000000000000000000000000000000000000",
        SigningConfig::CURRENT_CERT_SHA256);
    EXPECT_TRUE(rotationOk);

    // 3. Rejects if neither current nor next matches
    bool neitherMatches = UpdaterCommon::VerifyMsiPackage(
        msiPath.string(), realHash,
        "0000000000000000000000000000000000000000000000000000000000000000",
        "1111111111111111111111111111111111111111111111111111111111111111");
    EXPECT_FALSE(neitherMatches);

    // 4. Rejects if file SHA-256 does not match even if signature is valid
    bool wrongFileHash = UpdaterCommon::VerifyMsiPackage(
        msiPath.string(),
        "0000000000000000000000000000000000000000000000000000000000000000");
    EXPECT_FALSE(wrongFileHash);
}
