#include <gtest/gtest.h>
#include "core/StatsApiConfig.hpp"
#include <fstream>
#include <filesystem>

class StatsApiConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir = std::filesystem::temp_directory_path() / "OmniStatsTest";
        std::filesystem::create_directories(tempDir);
        testFilePath = tempDir / "DefaultStatsAPI.ini";
    }

    void TearDown() override {
        std::filesystem::remove_all(tempDir);
    }

    void WriteFile(const std::string& content) {
        std::ofstream file(testFilePath, std::ios::binary);
        file.write(content.data(), content.size());
    }

    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream file(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    std::filesystem::path tempDir;
    std::filesystem::path testFilePath;
};

TEST_F(StatsApiConfigTest, VerifyValidConfig) {
    std::string content = 
        "[StatsAPI]\r\n"
        "PacketSendRate=30\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::Valid);
    EXPECT_FLOAT_EQ(res.packetSendRate, 30.0f);
    EXPECT_EQ(res.actualPort, 49123);
}

TEST_F(StatsApiConfigTest, VerifyDisabledPacketSendRate) {
    std::string content = 
        "[StatsAPI]\r\n"
        "PacketSendRate=0\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::DisabledPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyWrongPacketSendRate) {
    std::string content = 
        "[StatsAPI]\r\n"
        "PacketSendRate=10\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::WrongPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyMissingPacketSendRate) {
    std::string content = 
        "[StatsAPI]\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::MissingPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyMissingPort) {
    std::string content = 
        "[StatsAPI]\r\n"
        "PacketSendRate=30\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::MissingPort);
}

TEST_F(StatsApiConfigTest, VerifyWrongPort) {
    std::string content = 
        "[StatsAPI]\r\n"
        "PacketSendRate=30\r\n"
        "Port=55555\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::WrongPort);
    EXPECT_EQ(res.actualPort, 55555);
}

TEST_F(StatsApiConfigTest, VerifyWhitespaceAndCaseInsensitive) {
    std::string content = 
        " [statsapi] \r\n"
        " packetsendrate = 30 \r\n"
        " port = 49123 \r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::Valid);
    EXPECT_FLOAT_EQ(res.packetSendRate, 30.0f);
    EXPECT_EQ(res.actualPort, 49123);
}

TEST_F(StatsApiConfigTest, FixConfigPreservesCommentsAndFormat) {
    std::string content = 
        "; Some initial comment\r\n"
        "[StatsAPI]\r\n"
        "PacketSendRate=0\r\n"
        "; Another comment\r\n"
        "Port=12345\r\n"
        "UnrelatedSetting=True\r\n";
    WriteFile(content);

    // Call repair
    int exitCode = StatsApiConfig::FixConfigStrictHeadless(testFilePath.string(), 49123);
    EXPECT_EQ(exitCode, 0);

    // Verify file content
    std::string newContent = ReadFile(testFilePath);
    EXPECT_NE(newContent.find("; Some initial comment\r\n"), std::string::npos);
    EXPECT_NE(newContent.find("; Another comment\r\n"), std::string::npos);
    EXPECT_NE(newContent.find("UnrelatedSetting=True\r\n"), std::string::npos);
    EXPECT_NE(newContent.find("PacketSendRate=30\r\n"), std::string::npos);
    EXPECT_NE(newContent.find("Port=49123\r\n"), std::string::npos);

    // Check backup was created
    std::filesystem::path backupPath = testFilePath.string() + ".omnistats.bak";
    EXPECT_TRUE(std::filesystem::exists(backupPath));
    EXPECT_EQ(ReadFile(backupPath), content);
}

TEST_F(StatsApiConfigTest, FixConfigDoesNotOverwriteExistingBackup) {
    std::string originalContent = "Original\r\n";
    WriteFile(originalContent);

    // Create manual backup
    std::filesystem::path backupPath = testFilePath.string() + ".omnistats.bak";
    std::string existingBackupContent = "ExistingBackup\r\n";
    {
        std::ofstream bakFile(backupPath, std::ios::binary);
        bakFile.write(existingBackupContent.data(), existingBackupContent.size());
    }

    std::string modifiedContent = 
        "[StatsAPI]\r\n"
        "PacketSendRate=0\r\n"
        "Port=12345\r\n";
    WriteFile(modifiedContent);

    int exitCode = StatsApiConfig::FixConfigStrictHeadless(testFilePath.string(), 49123);
    EXPECT_EQ(exitCode, 0);

    // Verify backup content was not overwritten
    EXPECT_EQ(ReadFile(backupPath), existingBackupContent);
}

TEST_F(StatsApiConfigTest, FixConfigStrictValidationErrors) {
    // 1. Filename must be exactly DefaultStatsAPI.ini
    std::filesystem::path wrongNamePath = tempDir / "WrongName.ini";
    {
        std::ofstream f(wrongNamePath);
        f << "Port=123";
    }
    int exitCode = StatsApiConfig::FixConfigStrictHeadless(wrongNamePath.string(), 49123);
    EXPECT_EQ(exitCode, 2);

    // 2. File must exist
    std::filesystem::path nonExistentPath = tempDir / "DefaultStatsAPI.ini.missing";
    // We pass filename DefaultStatsAPI.ini but inside a missing folder
    std::filesystem::path missingSubdir = tempDir / "missing_subdir" / "DefaultStatsAPI.ini";
    exitCode = StatsApiConfig::FixConfigStrictHeadless(missingSubdir.string(), 49123);
    EXPECT_EQ(exitCode, 2);
}
