#include <gtest/gtest.h>
#include "ReplayDriver.hpp"
#include "network/telemetry/TelemetryRecorder.hpp"
#include "utils/Env.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace omnistats::replay {
    namespace {

        std::filesystem::path FindFixturesDirectory() {
#ifdef OMNISTATS_SOURCE_DIR
            const std::filesystem::path sourceRoot(OMNISTATS_SOURCE_DIR);
            const auto candidate = sourceRoot / "tests" / "fixtures" / "replay";
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
#endif
            std::filesystem::path cur = std::filesystem::current_path();
            for (int i = 0; i < 5; ++i) {
                const auto curCandidate = cur / "tests" / "fixtures" / "replay";
                if (std::filesystem::exists(curCandidate)) {
                    return curCandidate;
                }
                if (!cur.has_parent_path() || cur.parent_path() == cur) break;
                cur = cur.parent_path();
            }
            return std::filesystem::path("tests") / "fixtures" / "replay";
        }

        bool IsUpdateGoldensEnabled() {
            const auto envVal = Env::Get("OMNISTATS_UPDATE_GOLDENS");
            return envVal.has_value() && (*envVal == "1" || *envVal == "true" || *envVal == "TRUE");
        }

        std::string ReadFileContent(const std::filesystem::path& path) {
            std::ifstream is(path, std::ios::in | std::ios::binary);
            if (!is.is_open()) return {};
            std::ostringstream ss;
            ss << is.rdbuf();
            std::string content = ss.str();
            content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());
            return content;
        }

        void WriteFileContent(const std::filesystem::path& path, const std::string& content) {
            std::error_code ec;
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            std::ofstream os(path, std::ios::out | std::ios::trunc | std::ios::binary);
            os << content;
        }

        std::string CreateReadableDiff(const std::string& expected, const std::string& actual) {
            std::istringstream expStream(expected);
            std::istringstream actStream(actual);
            std::vector<std::string> expLines;
            std::vector<std::string> actLines;
            std::string line;
            while (std::getline(expStream, line))
                expLines.push_back(line);
            while (std::getline(actStream, line))
                actLines.push_back(line);

            std::ostringstream diff;
            diff << "\n--- Expected Golden\n+++ Actual Output\n";
            const size_t maxLines = std::max(expLines.size(), actLines.size());
            int mismatches = 0;
            for (size_t i = 0; i < maxLines && mismatches < 5; ++i) {
                const std::string& exp = (i < expLines.size()) ? expLines[i] : "<EOF>";
                const std::string& act = (i < actLines.size()) ? actLines[i] : "<EOF>";
                if (exp != act) {
                    mismatches++;
                    diff << "@@ Line " << (i + 1) << " @@\n";
                    diff << "- " << exp << "\n";
                    diff << "+ " << act << "\n";
                }
            }
            return diff.str();
        }

        void RunAndVerifyFixture(const std::string& fixtureFilename) {
            const auto fixtureDir = FindFixturesDirectory();
            std::filesystem::path fixturePath = fixtureDir / fixtureFilename;
            if (!std::filesystem::exists(fixturePath)) {
                if (fixturePath.extension() == ".log") {
                    auto alt = fixturePath;
                    alt.replace_extension(".wire");
                    if (std::filesystem::exists(alt)) fixturePath = alt;
                } else if (fixturePath.extension() == ".wire") {
                    auto alt = fixturePath;
                    alt.replace_extension(".log");
                    if (std::filesystem::exists(alt)) fixturePath = alt;
                }
            }

            ASSERT_TRUE(std::filesystem::exists(fixturePath)) << "Fixture file not found: " << fixturePath.string();

            const auto goldenPath = fixtureDir / (fixturePath.stem().string() + ".golden.json");

            ReplayDriver driver;
            const auto result = driver.ReplayFile(fixturePath);
            ASSERT_TRUE(result.success) << "Replay execution failed for " << fixtureFilename << ": " << result.errorMessage;

            std::string normalizedActual = result.goldenJson;
            normalizedActual.erase(std::remove(normalizedActual.begin(), normalizedActual.end(), '\r'), normalizedActual.end());

            if (IsUpdateGoldensEnabled()) {
                std::cout << "[ReplayTest] Writing golden file: " << goldenPath.string() << "\n";
                WriteFileContent(goldenPath, normalizedActual);
                return;
            }

            if (!std::filesystem::exists(goldenPath)) {
                FAIL() << "Golden file does not exist: " << goldenPath.string()
                       << "\nGenerate golden files by running the test suite with OMNISTATS_UPDATE_GOLDENS=1.";
            }

            const std::string expectedGolden = ReadFileContent(goldenPath);
            EXPECT_EQ(normalizedActual, expectedGolden)
                << "Golden mismatch for " << fixtureFilename << " at " << goldenPath.string()
                << CreateReadableDiff(expectedGolden, normalizedActual);
        }

    } // namespace

    TEST(ReplayHarness, Ranked2v2Win) {
        RunAndVerifyFixture("01_ranked_2v2_win.wire");
    }

    TEST(ReplayHarness, Ranked2v2Loss) {
        RunAndVerifyFixture("02_ranked_2v2_loss.wire");
    }

    TEST(ReplayHarness, Ranked2v2Overtime) {
        RunAndVerifyFixture("03_ranked_2v2_overtime.wire");
    }

    TEST(ReplayHarness, Ranked2v2Forfeit) {
        RunAndVerifyFixture("04_ranked_2v2_forfeit.wire");
    }

    TEST(ReplayHarness, PlayerLeavesMidMatch) {
        RunAndVerifyFixture("05_player_leaves_mid_match.wire");
    }

    TEST(ReplayHarness, LocalPlayerDisconnectReconnect) {
        RunAndVerifyFixture("06_local_disconnect_reconnect.wire");
    }

    TEST(ReplayHarness, DestroyedAbandonedCompetitive) {
        RunAndVerifyFixture("07_destroyed_abandoned_competitive.wire");
    }

    TEST(ReplayHarness, CasualNonRecordable) {
        RunAndVerifyFixture("08_casual_non_recordable.wire");
    }

    TEST(ReplayHarness, ReplaySpectatorSegments) {
        RunAndVerifyFixture("09_replay_spectator.wire");
    }

    TEST(ReplayHarness, TwoConsecutiveMatches) {
        RunAndVerifyFixture("10_two_consecutive_matches.wire");
    }

    TEST(ReplayHarness, SessionResetByInactivity) {
        RunAndVerifyFixture("11_session_reset_inactivity.wire");
    }

    TEST(ReplayHarness, AllDiscoveredFixtures) {
        const auto fixtureDir = FindFixturesDirectory();
        ASSERT_TRUE(std::filesystem::exists(fixtureDir)) << "Fixture dir not found: " << fixtureDir.string();

        std::vector<std::string> fixtures;
        for (const auto& entry : std::filesystem::directory_iterator(fixtureDir)) {
            if (entry.is_regular_file() && (entry.path().extension() == ".wire" || entry.path().extension() == ".log")) {
                fixtures.push_back(entry.path().filename().string());
            }
        }
        std::sort(fixtures.begin(), fixtures.end());
        ASSERT_GE(fixtures.size(), 11u);

        for (const auto& f : fixtures) {
            RunAndVerifyFixture(f);
        }
    }

    TEST(TelemetryRecorder, RecordAndReadBack) {
        const auto tempDir = std::filesystem::temp_directory_path() /
                             ("omnistats_rec_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);
        const auto logPath = tempDir / "test_recording.log";

        const std::string line1 = R"({"Event":"MatchCreated","Data":{"MatchGuid":"rec-guid-1"}})";
        const std::string line2 = R"({"Event":"RoundStarted","Data":{}})";
        const std::string line3 = R"({"Event":"MatchEnded","Data":{"WinnerTeamNum":0}})";

        {
            TelemetryRecorder recorder(logPath);
            recorder.Record(line1);
            recorder.Record(line2);
            recorder.Record(line3);

            // Test failure resilience (invalid path must not crash)
            TelemetryRecorder invalidRecorder("");
            EXPECT_NO_THROW(invalidRecorder.Record("{\"Event\":\"Dummy\"}"));
        }

        const auto records = ReadRecording(logPath);
        ASSERT_EQ(records.size(), 3u);

        EXPECT_GT(records[0].timestampMs, 0);
        EXPECT_GE(records[1].timestampMs, records[0].timestampMs);
        EXPECT_GE(records[2].timestampMs, records[1].timestampMs);

        EXPECT_EQ(records[0].line, line1);
        EXPECT_EQ(records[1].line, line2);
        EXPECT_EQ(records[2].line, line3);

        std::filesystem::remove_all(tempDir, ec);
    }

} // namespace omnistats::replay
