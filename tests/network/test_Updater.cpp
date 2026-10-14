#include "network/ExternalUpdaterLauncher.hpp"
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stop_token>
#include <string>
#include <thread>
#include "core/config/AppVersion.hpp"
#include "updater/UpdaterCommon.hpp"
#include "utils/FileHash.hpp"
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

    bool ok = ExternalUpdaterLauncher::RepairStatsApiConfig(iniPath.string(), 49123, std::stop_token{});
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

namespace {
    PROCESS_INFORMATION StartChild(std::wstring commandLine) {
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return {};
        CloseHandle(pi.hThread);
        return pi;
    }
}

TEST(ExternalUpdaterLauncherTest, RepairWaitStopsOnRequestAndTimeoutWithoutBlocking) {
    PROCESS_INFORMATION hung = StartChild(L"cmd.exe /c ping -n 30 127.0.0.1 >nul");
    ASSERT_NE(hung.hProcess, nullptr);
    std::stop_source stopSource;
    unsigned long exitCode = 0;
    const auto start = std::chrono::steady_clock::now();
    std::jthread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stopSource.request_stop();
    });
    EXPECT_FALSE(ExternalUpdaterLauncher::WaitForRepairProcess(hung.hProcess, stopSource.get_token(), 60'000, exitCode));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));

    const auto timeoutStart = std::chrono::steady_clock::now();
    EXPECT_FALSE(ExternalUpdaterLauncher::WaitForRepairProcess(hung.hProcess, std::stop_token{}, 500, exitCode));
    EXPECT_LT(std::chrono::steady_clock::now() - timeoutStart, std::chrono::seconds(3));
    TerminateProcess(hung.hProcess, 1);
    CloseHandle(hung.hProcess);

    PROCESS_INFORMATION done = StartChild(L"cmd.exe /c exit 7");
    ASSERT_NE(done.hProcess, nullptr);
    EXPECT_TRUE(ExternalUpdaterLauncher::WaitForRepairProcess(done.hProcess, std::stop_token{}, 10'000, exitCode));
    EXPECT_EQ(exitCode, 7u);
    CloseHandle(done.hProcess);
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
