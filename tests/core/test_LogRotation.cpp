#include <gtest/gtest.h>
#include "core/LogRotation.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

    void WriteFile(const std::filesystem::path& path, const std::string& content) {
        std::ofstream out(path, std::ios::binary);
        out << content;
    }

    std::string ReadFile(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open()) return "";
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    class LogRotationTest : public ::testing::Test {
      protected:
        void SetUp() override {
            testDir = std::filesystem::temp_directory_path() / ("omnistats_log_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
            std::error_code ec;
            std::filesystem::create_directories(testDir, ec);
        }

        void TearDown() override {
            std::error_code ec;
            std::filesystem::remove_all(testDir, ec);
        }

        std::filesystem::path testDir;
    };

    TEST_F(LogRotationTest, ShiftsNumberedLogsUpDropsOldestAndMovesCurrentTo1) {
        const std::string baseName = "omnistats_log.txt";

        WriteFile(testDir / "omnistats_log.txt", "content_current");
        WriteFile(testDir / "omnistats_log.1.txt", "content_1");
        WriteFile(testDir / "omnistats_log.2.txt", "content_2");
        WriteFile(testDir / "omnistats_log.3.txt", "content_3");
        WriteFile(testDir / "omnistats_log.4.txt", "content_4");
        WriteFile(testDir / "omnistats_log.5.txt", "content_5_oldest");

        const bool ok = LogRotation::RotateLogs(testDir, baseName, 5);
        EXPECT_TRUE(ok);

        EXPECT_EQ(ReadFile(testDir / "omnistats_log.5.txt"), "content_4");
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.4.txt"), "content_3");
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.3.txt"), "content_2");
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.2.txt"), "content_1");
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.1.txt"), "content_current");

        EXPECT_FALSE(std::filesystem::exists(testDir / "omnistats_log.txt"));
    }

    TEST_F(LogRotationTest, HandlesMissingIntermediateFiles) {
        const std::string baseName = "omnistats_log.txt";

        WriteFile(testDir / "omnistats_log.txt", "curr_data");
        WriteFile(testDir / "omnistats_log.1.txt", "log1_data");
        WriteFile(testDir / "omnistats_log.3.txt", "log3_data");

        const bool ok = LogRotation::RotateLogs(testDir, baseName, 5);
        EXPECT_TRUE(ok);

        EXPECT_EQ(ReadFile(testDir / "omnistats_log.4.txt"), "log3_data");
        EXPECT_FALSE(std::filesystem::exists(testDir / "omnistats_log.3.txt"));
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.2.txt"), "log1_data");
        EXPECT_EQ(ReadFile(testDir / "omnistats_log.1.txt"), "curr_data");
        EXPECT_FALSE(std::filesystem::exists(testDir / "omnistats_log.txt"));
    }

    TEST_F(LogRotationTest, HandlesMissingCurrentLogGracefully) {
        const std::string baseName = "omnistats_log.txt";

        WriteFile(testDir / "omnistats_log.1.txt", "log1_only");

        const bool ok = LogRotation::RotateLogs(testDir, baseName, 5);
        EXPECT_TRUE(ok);

        EXPECT_EQ(ReadFile(testDir / "omnistats_log.2.txt"), "log1_only");
        EXPECT_FALSE(std::filesystem::exists(testDir / "omnistats_log.1.txt"));
        EXPECT_FALSE(std::filesystem::exists(testDir / "omnistats_log.txt"));
    }

    TEST_F(LogRotationTest, HandlesInvalidArguments) {
        EXPECT_FALSE(LogRotation::RotateLogs("", "omnistats_log.txt", 5));
        EXPECT_FALSE(LogRotation::RotateLogs(testDir, "", 5));
        EXPECT_FALSE(LogRotation::RotateLogs(testDir, "omnistats_log.txt", 0));
        EXPECT_FALSE(LogRotation::RotateLogs(testDir, "omnistats_log.txt", -1));
    }

} // namespace
