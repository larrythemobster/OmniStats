#include <gtest/gtest.h>
#include "core/config/StatsApiConfig.hpp"
#include "updater/StatsApiRepair.hpp"
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
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=30\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::Valid);
    EXPECT_FLOAT_EQ(res.packetSendRate, 30.0f);
    EXPECT_EQ(res.actualPort, 49123);
}

TEST_F(StatsApiConfigTest, LegendaryInstalledResolvesRocketLeagueOnly) {
    const auto rlRoot = tempDir / "Heroic" / "rocketleague";
    const auto otherRoot = tempDir / "Heroic" / "other";
    std::filesystem::create_directories(rlRoot / "TAGame" / "Config");
    std::filesystem::create_directories(otherRoot / "TAGame" / "Config");
    std::ofstream(rlRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini") << "[TAGame.MatchStatsExporter_TA]\n";
    std::ofstream(otherRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini") << "[TAGame.MatchStatsExporter_TA]\n";

    const auto manifest = tempDir / "installed.json";
    std::ofstream(manifest) << R"({"Fortnite":{"app_name":"Fortnite","install_path":")" << otherRoot.generic_string()
                            << R"("},"Sugar":{"app_name":"Sugar","install_path":")" << rlRoot.generic_string() << R"("}})";

    const std::filesystem::path found = StatsApiConfig::FindConfigPathInLegendaryInstalled(manifest);
    EXPECT_TRUE(std::filesystem::equivalent(found, rlRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini"));

    std::filesystem::remove(rlRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini");
    EXPECT_EQ(StatsApiConfig::FindConfigPathInLegendaryInstalled(manifest), "");
    EXPECT_EQ(StatsApiConfig::FindConfigPathInLegendaryInstalled(tempDir / "missing.json"), "");
}

TEST_F(StatsApiConfigTest, VerifyDisabledPacketSendRate) {
    std::string content =
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=0\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::DisabledPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyWrongPacketSendRate) {
    std::string content =
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=10\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::WrongPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyMissingPacketSendRate) {
    std::string content =
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "Port=49123\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::MissingPacketSendRate);
}

TEST_F(StatsApiConfigTest, VerifyMissingPort) {
    std::string content =
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=30\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::MissingPort);
}

TEST_F(StatsApiConfigTest, VerifyWrongPort) {
    std::string content =
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=30\r\n"
        "Port=55555\r\n";
    WriteFile(content);

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::WrongPort);
    EXPECT_EQ(res.actualPort, 55555);
}

TEST_F(StatsApiConfigTest, VerifyWhitespaceAndCaseInsensitive) {
    std::string content =
        " [tagame.matchstatsexporter_ta] \r\n"
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
        "[TAGame.MatchStatsExporter_TA]\r\n"
        "PacketSendRate=0\r\n"
        "; Another comment\r\n"
        "Port=12345\r\n"
        "UnrelatedSetting=True\r\n";
    WriteFile(content);

    // Call repair
    int exitCode = UpdaterStatsApiRepair::FixConfigStrictHeadless(testFilePath.string(), 49123);
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

    int exitCode = UpdaterStatsApiRepair::FixConfigStrictHeadless(testFilePath.string(), 49123);
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
    int exitCode = UpdaterStatsApiRepair::FixConfigStrictHeadless(wrongNamePath.string(), 49123);
    EXPECT_EQ(exitCode, 2);

    // 2. File must exist
    std::filesystem::path nonExistentPath = tempDir / "DefaultStatsAPI.ini.missing";
    // We pass filename DefaultStatsAPI.ini but inside a missing folder
    std::filesystem::path missingSubdir = tempDir / "missing_subdir" / "DefaultStatsAPI.ini";
    exitCode = UpdaterStatsApiRepair::FixConfigStrictHeadless(missingSubdir.string(), 49123);
    EXPECT_EQ(exitCode, 2);
}

TEST_F(StatsApiConfigTest, VerifyIgnoresKeysOutsideExporterSection) {
    WriteFile("[StatsAPI]\r\nPacketSendRate=30\r\nPort=49123\r\n");
    const auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::MissingPacketSendRate);
}

TEST_F(StatsApiConfigTest, TAStatsApiOverridesDefaultForVerifyAndRepair) {
    const std::string defaultContent = "[TAGame.MatchStatsExporter_TA]\r\nPacketSendRate=30\r\nPort=49123\r\n";
    WriteFile(defaultContent);
    const std::filesystem::path overridePath = tempDir / "TAStatsAPI.ini";
    {
        std::ofstream file(overridePath, std::ios::binary);
        file << "[TAGame.MatchStatsExporter_TA]\r\nPacketSendRate=0\r\nPort=49123\r\n";
    }

    auto res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(std::filesystem::path(res.path).filename(), "TAStatsAPI.ini");
    EXPECT_EQ(res.status, StatsApiConfig::Status::DisabledPacketSendRate);

    EXPECT_EQ(UpdaterStatsApiRepair::FixConfigStrictHeadless(testFilePath.string(), 49123), 0);
    EXPECT_EQ(ReadFile(testFilePath), defaultContent);
    res = StatsApiConfig::VerifyConfig(testFilePath.string(), 49123);
    EXPECT_EQ(res.status, StatsApiConfig::Status::Valid);
}

TEST_F(StatsApiConfigTest, RepairAddsExporterKeysWithoutTouchingOtherSections) {
    WriteFile("[Other]\r\nPort=1\r\n\r\n[TAGame.MatchStatsExporter_TA]\r\nPacketSendRate=0\r\nWebPort=49124\r\n\r\n[Tail]\r\nKey=Value\r\n");
    EXPECT_EQ(UpdaterStatsApiRepair::FixConfigStrictHeadless(testFilePath.string(), 49123), 0);
    EXPECT_EQ(ReadFile(testFilePath),
              "[Other]\r\nPort=1\r\n\r\n[TAGame.MatchStatsExporter_TA]\r\nPacketSendRate=30\r\nWebPort=49124\r\nPort=49123\r\n\r\n[Tail]\r\nKey=Value\r\n");

    WriteFile("[StatsAPI]\r\nPacketSendRate=30\r\nPort=49123\r\n");
    std::filesystem::remove(testFilePath.string() + ".omnistats.bak");
    EXPECT_EQ(UpdaterStatsApiRepair::FixConfigStrictHeadless(testFilePath.string(), 49123), 0);
    EXPECT_EQ(StatsApiConfig::VerifyConfig(testFilePath.string(), 49123).status, StatsApiConfig::Status::Valid);
}

TEST_F(StatsApiConfigTest, FindConfigPathFromExecutable_Standard64Bit) {
    auto gameRoot = tempDir / "RocketLeague";
    auto exePath = gameRoot / "Binaries" / "Win64" / "RocketLeague.exe";
    auto configPath = gameRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini";

    std::filesystem::create_directories(exePath.parent_path());
    std::filesystem::create_directories(configPath.parent_path());
    {
        std::ofstream exeFile(exePath);
        exeFile << "dummy_exe";
        std::ofstream configFile(configPath);
        configFile << "[StatsAPI]\nPacketSendRate=30\nPort=49123\n";
    }

    std::string detected = StatsApiConfig::FindConfigPathFromExecutable(exePath);
    EXPECT_EQ(detected, configPath.string());
}

TEST_F(StatsApiConfigTest, FindConfigPathFromExecutable_Legacy32Bit) {
    auto gameRoot = tempDir / "RocketLeague32";
    auto exePath = gameRoot / "Binaries" / "Win32" / "RocketLeague.exe";
    auto configPath = gameRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini";

    std::filesystem::create_directories(exePath.parent_path());
    std::filesystem::create_directories(configPath.parent_path());
    {
        std::ofstream exeFile(exePath);
        exeFile << "dummy_exe";
        std::ofstream configFile(configPath);
        configFile << "[StatsAPI]\nPacketSendRate=30\nPort=49123\n";
    }

    std::string detected = StatsApiConfig::FindConfigPathFromExecutable(exePath);
    EXPECT_EQ(detected, configPath.string());
}

TEST_F(StatsApiConfigTest, FindConfigPathFromExecutable_MultipleInstallsDisambiguation) {
    auto steamRoot = tempDir / "SteamInstall" / "steamapps" / "common" / "rocketleague";
    auto steamExe = steamRoot / "Binaries" / "Win64" / "RocketLeague.exe";
    auto steamConfig = steamRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini";

    auto epicRoot = tempDir / "EpicInstall" / "rocketleague";
    auto epicExe = epicRoot / "Binaries" / "Win64" / "RocketLeague.exe";
    auto epicConfig = epicRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini";

    std::filesystem::create_directories(steamExe.parent_path());
    std::filesystem::create_directories(steamConfig.parent_path());
    std::filesystem::create_directories(epicExe.parent_path());
    std::filesystem::create_directories(epicConfig.parent_path());

    {
        std::ofstream(steamExe) << "steam_exe";
        std::ofstream(steamConfig) << "[StatsAPI]\nPacketSendRate=30\nPort=49123\n";
        std::ofstream(epicExe) << "epic_exe";
        std::ofstream(epicConfig) << "[StatsAPI]\nPacketSendRate=30\nPort=49123\n";
    }

    // When Steam instance runs, it must return the Steam config path
    EXPECT_EQ(StatsApiConfig::FindConfigPathFromExecutable(steamExe), steamConfig.string());
    // When Epic instance runs, it must return the Epic config path
    EXPECT_EQ(StatsApiConfig::FindConfigPathFromExecutable(epicExe), epicConfig.string());
}

TEST_F(StatsApiConfigTest, FindConfigPathFromExecutable_CandidateDirectoryWhenFileMissing) {
    auto gameRoot = tempDir / "RocketLeagueMissingIni";
    auto exePath = gameRoot / "Binaries" / "Win64" / "RocketLeague.exe";
    auto expectedConfig = gameRoot / "TAGame" / "Config" / "DefaultStatsAPI.ini";

    std::filesystem::create_directories(exePath.parent_path());
    std::filesystem::create_directories(expectedConfig.parent_path());
    {
        std::ofstream exeFile(exePath);
        exeFile << "dummy_exe";
    }

    // File doesn't exist yet, but TAGame/Config folder does exist
    EXPECT_FALSE(std::filesystem::exists(expectedConfig));
    std::string detected = StatsApiConfig::FindConfigPathFromExecutable(exePath);
    EXPECT_EQ(detected, expectedConfig.string());
}

TEST_F(StatsApiConfigTest, FindConfigPathFromExecutable_EmptyOrInvalidPath) {
    EXPECT_TRUE(StatsApiConfig::FindConfigPathFromExecutable("").empty());

    auto nonGameExe = tempDir / "OtherApp" / "bin" / "app.exe";
    std::filesystem::create_directories(nonGameExe.parent_path());
    {
        std::ofstream f(nonGameExe);
        f << "other";
    }
    EXPECT_TRUE(StatsApiConfig::FindConfigPathFromExecutable(nonGameExe).empty());
}
