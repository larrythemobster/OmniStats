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
#include "updater/UpdaterPaths.hpp"
#include "utils/FileHash.hpp"
#include "updater/ReleaseManifest.hpp"
#include "network/http/SigningConfig.hpp"
#include <sodium.h>
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
    EXPECT_EQ(appData.substr(appData.length() - 16), "\\omnistats_test\\");
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
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(tempFile.string(), wrongHash, "fake_cert_hash"),
              UpdaterCommon::MsiVerifyResult::ChecksumMismatch);
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
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(tempFile.string(), realHash, SigningConfig::CURRENT_CERT_SHA256),
              UpdaterCommon::MsiVerifyResult::SignatureInvalid);
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

    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(tempFile.string(), realHash,
                                              "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"),
              UpdaterCommon::MsiVerifyResult::SignatureInvalid);
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
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(msiPath.string(), realHash), UpdaterCommon::MsiVerifyResult::Ok);

    // 2. Rotation support: accepts if nextPinnedCertSha matches even if current differs
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(
                  msiPath.string(), realHash,
                  "0000000000000000000000000000000000000000000000000000000000000000",
                  SigningConfig::CURRENT_CERT_SHA256),
              UpdaterCommon::MsiVerifyResult::Ok);

    // 3. Rejects if neither current nor next matches
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(
                  msiPath.string(), realHash,
                  "0000000000000000000000000000000000000000000000000000000000000000",
                  "1111111111111111111111111111111111111111111111111111111111111111"),
              UpdaterCommon::MsiVerifyResult::SignatureInvalid);

    // 4. Rejects if file SHA-256 does not match even if signature is valid
    EXPECT_EQ(UpdaterCommon::VerifyMsiPackage(
                  msiPath.string(),
                  "0000000000000000000000000000000000000000000000000000000000000000"),
              UpdaterCommon::MsiVerifyResult::ChecksumMismatch);
}

namespace {

    struct TestKeypair {
        std::string publicKeyHex;
        std::vector<unsigned char> secretKey;

        static TestKeypair Generate() {
            EXPECT_GE(sodium_init(), 0);
            TestKeypair kp;
            kp.secretKey.resize(crypto_sign_SECRETKEYBYTES);
            std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
            crypto_sign_keypair(pk.data(), kp.secretKey.data());

            std::vector<char> hex(crypto_sign_PUBLICKEYBYTES * 2 + 1);
            sodium_bin2hex(hex.data(), hex.size(), pk.data(), pk.size());
            kp.publicKeyHex = hex.data();
            return kp;
        }

        std::string Sign(const std::string& message) const {
            std::vector<unsigned char> sig(crypto_sign_BYTES);
            unsigned long long sigLen = 0;
            EXPECT_EQ(crypto_sign_detached(sig.data(), &sigLen,
                                           reinterpret_cast<const unsigned char*>(message.data()),
                                           message.size(), secretKey.data()),
                      0);
            return std::string(reinterpret_cast<char*>(sig.data()), sig.size());
        }
    };

} // namespace

TEST(ReleaseManifestTest, ParseValidManifest) {
    const std::string json = R"({
        "version": "2.5.0",
        "msi_sha256": "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea",
        "msi_url": "OmniStats.msi",
        "min_version": "2.0.0",
        "published_at": "2026-10-25T12:00:00Z"
    })";

    ReleaseManifest manifest;
    EXPECT_TRUE(ReleaseManifestVerifier::ParseManifest(json, manifest));
    EXPECT_EQ(manifest.version, "2.5.0");
    EXPECT_EQ(manifest.msi_sha256, "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea");
    EXPECT_EQ(manifest.msi_url, "OmniStats.msi");
    EXPECT_EQ(manifest.min_version, "2.0.0");
    EXPECT_EQ(manifest.published_at, "2026-10-25T12:00:00Z");
}

TEST(ReleaseManifestTest, ParseInvalidJson) {
    ReleaseManifest manifest;
    EXPECT_FALSE(ReleaseManifestVerifier::ParseManifest("not a json string", manifest));
    EXPECT_FALSE(ReleaseManifestVerifier::ParseManifest("", manifest));
}

TEST(ReleaseManifestTest, ParseMissingRequiredFields) {
    ReleaseManifest manifest;
    // Missing version
    EXPECT_FALSE(ReleaseManifestVerifier::ParseManifest(R"({
        "msi_sha256": "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea",
        "msi_url": "OmniStats.msi"
    })",
                                                        manifest));

    // Missing msi_sha256
    EXPECT_FALSE(ReleaseManifestVerifier::ParseManifest(R"({
        "version": "2.5.0",
        "msi_url": "OmniStats.msi"
    })",
                                                        manifest));

    // Invalid hash length
    EXPECT_FALSE(ReleaseManifestVerifier::ParseManifest(R"({
        "version": "2.5.0",
        "msi_sha256": "short_hash",
        "msi_url": "OmniStats.msi"
    })",
                                                        manifest));
}

TEST(ReleaseManifestTest, ValidSignatureAccepted) {
    const auto kp = TestKeypair::Generate();
    ReleaseManifest manifest;
    manifest.version = "2.5.0";
    manifest.msi_sha256 = "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea";
    manifest.msi_url = "OmniStats.msi";
    manifest.min_version = "2.0.0";
    manifest.published_at = "2026-10-25T12:00:00Z";

    const std::string manifestJson = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sig = kp.Sign(manifestJson);

    EXPECT_TRUE(ReleaseManifestVerifier::VerifySignature(manifestJson, sig, kp.publicKeyHex));

    ReleaseManifest parsed;
    const auto result = ReleaseManifestVerifier::VerifyManifest(manifestJson, sig, "2.4.0", kp.publicKeyHex, "", &parsed);
    EXPECT_EQ(result, ManifestVerifyResult::Ok);
    EXPECT_EQ(parsed.version, "2.5.0");
    EXPECT_EQ(parsed.msi_sha256, manifest.msi_sha256);
}

TEST(ReleaseManifestTest, TamperedManifestRejected) {
    const auto kp = TestKeypair::Generate();
    ReleaseManifest manifest;
    manifest.version = "2.5.0";
    manifest.msi_sha256 = "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea";
    manifest.msi_url = "OmniStats.msi";
    manifest.min_version = "2.0.0";
    manifest.published_at = "2026-10-25T12:00:00Z";

    const std::string manifestJson = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sig = kp.Sign(manifestJson);

    // Tamper with the manifest payload
    std::string tampered = manifestJson;
    tampered.replace(tampered.find("2.5.0"), 5, "2.9.9");

    EXPECT_FALSE(ReleaseManifestVerifier::VerifySignature(tampered, sig, kp.publicKeyHex));
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(tampered, sig, "2.4.0", kp.publicKeyHex),
              ManifestVerifyResult::SignatureInvalid);
}

TEST(ReleaseManifestTest, WrongKeyRejected) {
    const auto kp1 = TestKeypair::Generate();
    const auto kp2 = TestKeypair::Generate();

    ReleaseManifest manifest;
    manifest.version = "2.5.0";
    manifest.msi_sha256 = "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea";
    manifest.msi_url = "OmniStats.msi";

    const std::string manifestJson = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sig = kp1.Sign(manifestJson);

    // Verify against kp2 (wrong key)
    EXPECT_FALSE(ReleaseManifestVerifier::VerifySignature(manifestJson, sig, kp2.publicKeyHex));
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(manifestJson, sig, "2.4.0", kp2.publicKeyHex),
              ManifestVerifyResult::SignatureInvalid);
}

TEST(ReleaseManifestTest, DowngradeRefused) {
    const auto kp = TestKeypair::Generate();
    ReleaseManifest manifest;
    manifest.version = "2.3.0";
    manifest.msi_sha256 = "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea";
    manifest.msi_url = "OmniStats.msi";

    const std::string manifestJson = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sig = kp.Sign(manifestJson);

    // Installed version is 2.4.0, manifest is 2.3.0 -> Refuse downgrade
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(manifestJson, sig, "2.4.0", kp.publicKeyHex),
              ManifestVerifyResult::DowngradeRefused);

    // Equal version is valid and retains the verified manifest.
    manifest.version = "2.4.0";
    const std::string manifestJsonEqual = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sigEqual = kp.Sign(manifestJsonEqual);
    ReleaseManifest parsedEqual;
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(manifestJsonEqual, sigEqual, "2.4.0", kp.publicKeyHex, "", &parsedEqual),
              ManifestVerifyResult::Ok);
    EXPECT_EQ(parsedEqual.version, "2.4.0");
    EXPECT_FALSE(UpdaterCommon::IsNewerVersion("2.4.0", parsedEqual.version));

    // Newer version 2.4.1 -> Accept
    manifest.version = "2.4.1";
    const std::string manifestJsonNewer = ReleaseManifestVerifier::SerializeManifest(manifest);
    const std::string sigNewer = kp.Sign(manifestJsonNewer);
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(manifestJsonNewer, sigNewer, "2.4.0", kp.publicKeyHex),
              ManifestVerifyResult::Ok);
}

TEST(ReleaseManifestTest, MissingManifestFailsClosed) {
    const auto kp = TestKeypair::Generate();
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest("", "", "2.4.0", kp.publicKeyHex),
              ManifestVerifyResult::ManifestMissing);
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest("{}", "", "2.4.0", kp.publicKeyHex),
              ManifestVerifyResult::SignatureInvalid);
}

TEST(ReleaseManifestTest, NextKeyRotationAccepted) {
    const auto kpCurrent = TestKeypair::Generate();
    const auto kpNext = TestKeypair::Generate();
    const auto kpOther = TestKeypair::Generate();

    ReleaseManifest manifest;
    manifest.version = "2.5.0";
    manifest.msi_sha256 = "4a7b48325a7d7efb991b5c469b8fe1ad15694c9f13e711be8bb698d28cf8efea";
    manifest.msi_url = "OmniStats.msi";

    const std::string manifestJson = ReleaseManifestVerifier::SerializeManifest(manifest);

    // 1. Signed by current key -> accepted with rotation configured
    const std::string sigCurrent = kpCurrent.Sign(manifestJson);
    EXPECT_TRUE(ReleaseManifestVerifier::VerifySignatureWithRotation(
        manifestJson, sigCurrent, kpCurrent.publicKeyHex, kpNext.publicKeyHex));
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(
                  manifestJson, sigCurrent, "2.4.0", kpCurrent.publicKeyHex, kpNext.publicKeyHex),
              ManifestVerifyResult::Ok);

    // 2. Signed by next key -> accepted with rotation configured
    const std::string sigNext = kpNext.Sign(manifestJson);
    EXPECT_TRUE(ReleaseManifestVerifier::VerifySignatureWithRotation(
        manifestJson, sigNext, kpCurrent.publicKeyHex, kpNext.publicKeyHex));
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(
                  manifestJson, sigNext, "2.4.0", kpCurrent.publicKeyHex, kpNext.publicKeyHex),
              ManifestVerifyResult::Ok);

    // 3. Signed by third untrusted key -> rejected
    const std::string sigOther = kpOther.Sign(manifestJson);
    EXPECT_FALSE(ReleaseManifestVerifier::VerifySignatureWithRotation(
        manifestJson, sigOther, kpCurrent.publicKeyHex, kpNext.publicKeyHex));
    EXPECT_EQ(ReleaseManifestVerifier::VerifyManifest(
                  manifestJson, sigOther, "2.4.0", kpCurrent.publicKeyHex, kpNext.publicKeyHex),
              ManifestVerifyResult::SignatureInvalid);
}

TEST(ReleaseManifestTest, MinVersionForcingUpdate) {
    // Installed version is below min_version -> forced update
    EXPECT_TRUE(ReleaseManifestVerifier::IsUpdateForced("1.9.0", "2.0.0"));
    EXPECT_TRUE(ReleaseManifestVerifier::IsUpdateForced("1.9.9", "2.0.0"));

    // Installed version is equal or above min_version -> not forced
    EXPECT_FALSE(ReleaseManifestVerifier::IsUpdateForced("2.0.0", "2.0.0"));
    EXPECT_FALSE(ReleaseManifestVerifier::IsUpdateForced("2.1.0", "2.0.0"));

    // Empty fields
    EXPECT_FALSE(ReleaseManifestVerifier::IsUpdateForced("", "2.0.0"));
    EXPECT_FALSE(ReleaseManifestVerifier::IsUpdateForced("2.0.0", ""));
}
