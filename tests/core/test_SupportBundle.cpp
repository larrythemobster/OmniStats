#include <gtest/gtest.h>
#include "core/SupportBundle.hpp"
#include "core/ZipWriter.hpp"

#include <sqlite3.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

    class SupportBundleTest : public ::testing::Test {
      protected:
        void SetUp() override {
            testDir = std::filesystem::temp_directory_path() / ("omnistats_support_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::error_code ec;
            std::filesystem::create_directories(testDir, ec);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(testDir, ec);
        }

        std::filesystem::path testDir;
    };

    TEST_F(SupportBundleTest, ConfigRedactionRemovesEverySecretKeyAndKeepsNonSecretKeys) {
        nlohmann::json input = {
            {"port", 49123},
            {"host", "127.0.0.1"},
            {"debug_logging", false},
            {"require_rl_focus", true},
            {"position", "top-right"},
            {"themeAccent", {1.0, 0.0, 0.0, 1.0}},
            {"ballchasing_token", "bc_token_abcdef123456"},
            {"custom_api_key", "omni_key_9876543210"},
            {"pro_api_key", "legacy_pro_key_123"},
            {"account_device_key", "ed25519_private_key_hex"},
            {"account_device_public_id", "dev_pub_id_12345"},
            {"account_refresh_token", "refresh_secret_token_abc"},
            {"account_access_token", "access_jwt_token_xyz"},
            {"account_signed_in_name", "MyRLPlayerName"},
            {"client_uuid", "01234567-89ab-cdef-0123-456789abcdef"},
            {"last_primary_id", "steam:76561198000000000"},
            {"known_primary_ids", {"steam:76561198000000000", "epic:e123456789"}}};

        const nlohmann::json redacted = SupportBundle::RedactConfig(input);

        EXPECT_FALSE(redacted.contains("ballchasing_token"));
        EXPECT_FALSE(redacted.contains("custom_api_key"));
        EXPECT_FALSE(redacted.contains("pro_api_key"));
        EXPECT_FALSE(redacted.contains("account_device_key"));
        EXPECT_FALSE(redacted.contains("account_device_public_id"));
        EXPECT_FALSE(redacted.contains("account_refresh_token"));
        EXPECT_FALSE(redacted.contains("account_access_token"));
        EXPECT_FALSE(redacted.contains("account_signed_in_name"));
        EXPECT_FALSE(redacted.contains("client_uuid"));
        EXPECT_FALSE(redacted.contains("last_primary_id"));
        EXPECT_FALSE(redacted.contains("known_primary_ids"));

        EXPECT_TRUE(redacted.contains("port"));
        EXPECT_EQ(redacted["port"], 49123);
        EXPECT_TRUE(redacted.contains("host"));
        EXPECT_EQ(redacted["host"], "127.0.0.1");
        EXPECT_TRUE(redacted.contains("debug_logging"));
        EXPECT_EQ(redacted["debug_logging"], false);
        EXPECT_TRUE(redacted.contains("require_rl_focus"));
        EXPECT_EQ(redacted["require_rl_focus"], true);
        EXPECT_TRUE(redacted.contains("position"));
        EXPECT_EQ(redacted["position"], "top-right");
        EXPECT_TRUE(redacted.contains("themeAccent"));
    }

    TEST_F(SupportBundleTest, LogRedactionScrubsPlantedTokenClientUuidAndBearerHeader) {
        const std::string plantedToken = "plant_token_sec_999";
        const std::string plantedUuid = "44556677-8899-aabb-ccdd-eeff00112233";

        const std::string rawLog =
            "[10:00:00] [Network] Connecting with Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload.sig\n"
            "[10:00:01] [Network] Request headers include X-API-Key: super_secret_omnistats_key_123\n"
            "[10:00:02] [Telemetry] Initialized client_uuid: " +
            plantedUuid + "\n"
                          "[10:00:03] [Uploader] Ballchasing token: " +
            plantedToken + " verified successfully.\n";

        const std::vector<std::string> secrets = {plantedToken, plantedUuid};
        const std::string scrubbed = SupportBundle::RedactLog(rawLog, secrets);

        EXPECT_NE(scrubbed.find("Bearer [redacted]"), std::string::npos);
        EXPECT_NE(scrubbed.find("X-API-Key: [redacted]"), std::string::npos);
        EXPECT_NE(scrubbed.find("client_uuid: [redacted]"), std::string::npos);
        EXPECT_NE(scrubbed.find("[redacted] verified successfully"), std::string::npos);

        EXPECT_EQ(scrubbed.find("eyJhbGciOiJIUzI1NiJ9"), std::string::npos);
        EXPECT_EQ(scrubbed.find("super_secret_omnistats_key_123"), std::string::npos);
        EXPECT_EQ(scrubbed.find(plantedUuid), std::string::npos);
        EXPECT_EQ(scrubbed.find(plantedToken), std::string::npos);
    }

    TEST_F(SupportBundleTest, ZipWriterRoundTripReadable) {
        Core::ZipWriter zip;

        const std::string helloText = "Hello, OmniStats Support!";
        const std::string emptyText = "";
        std::string largeText;
        largeText.reserve(50000);
        for (int i = 0; i < 1000; ++i) {
            largeText += "Line " + std::to_string(i) + ": Repeating payload for Deflate compression test.\n";
        }

        EXPECT_TRUE(zip.AddFile("hello.txt", helloText));
        EXPECT_TRUE(zip.AddFile("empty.dat", emptyText));
        EXPECT_TRUE(zip.AddFile("large.txt", largeText));

        const std::vector<uint8_t> zipBytes = zip.Finalize();
        EXPECT_GT(zipBytes.size(), 100u);

        std::vector<Core::ZipEntryInfo> entries;
        const bool readOk = Core::ReadZipEntries(zipBytes, entries);
        EXPECT_TRUE(readOk);
        ASSERT_EQ(entries.size(), 3u);

        EXPECT_EQ(entries[0].filename, "hello.txt");
        EXPECT_EQ(entries[0].data, helloText);
        EXPECT_EQ(entries[0].uncompressedSize, helloText.size());

        EXPECT_EQ(entries[1].filename, "empty.dat");
        EXPECT_EQ(entries[1].data, emptyText);
        EXPECT_EQ(entries[1].uncompressedSize, 0u);
        EXPECT_EQ(entries[1].compressedSize, 0u);

        EXPECT_EQ(entries[2].filename, "large.txt");
        EXPECT_EQ(entries[2].data, largeText);
        EXPECT_EQ(entries[2].uncompressedSize, largeText.size());
        EXPECT_LT(entries[2].compressedSize, entries[2].uncompressedSize);
    }

    TEST_F(SupportBundleTest, ZipWriterWriteToFileAtomic) {
        Core::ZipWriter zip;
        zip.AddFile("summary.txt", "Support diagnostics summary text");

        const std::filesystem::path destZip = testDir / "bundle.zip";
        EXPECT_TRUE(zip.WriteToFile(destZip));
        EXPECT_TRUE(std::filesystem::exists(destZip));

        std::ifstream in(destZip, std::ios::binary);
        ASSERT_TRUE(in.is_open());
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        std::vector<Core::ZipEntryInfo> entries;
        EXPECT_TRUE(Core::ReadZipEntries(bytes, entries));
        ASSERT_EQ(entries.size(), 1u);
        EXPECT_EQ(entries[0].filename, "summary.txt");
        EXPECT_EQ(entries[0].data, "Support diagnostics summary text");
    }

    TEST_F(SupportBundleTest, DatabaseSummaryReadOnlyOnTestDb) {
        const std::filesystem::path dbPath = testDir / "test.db";

        sqlite3* db = nullptr;
        ASSERT_EQ(sqlite3_open_v2(dbPath.string().c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr), SQLITE_OK);

        char* errMsg = nullptr;
        const char* sql =
            "PRAGMA user_version = 42;"
            "CREATE TABLE TestMatches (id INTEGER PRIMARY KEY, note TEXT);"
            "INSERT INTO TestMatches VALUES (1, 'SecretPlayerA');"
            "INSERT INTO TestMatches VALUES (2, 'SecretPlayerB');"
            "INSERT INTO TestMatches VALUES (3, 'SecretPlayerC');";
        ASSERT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &errMsg), SQLITE_OK);
        sqlite3_close(db);

        const std::string summary = SupportBundle::GetDatabaseSummary(dbPath);

        EXPECT_NE(summary.find("Schema Version (user_version): 42"), std::string::npos);
        EXPECT_NE(summary.find("TestMatches: 3 rows"), std::string::npos);
        EXPECT_EQ(summary.find("SecretPlayerA"), std::string::npos);
        EXPECT_EQ(summary.find("SecretPlayerB"), std::string::npos);
    }

} // namespace
