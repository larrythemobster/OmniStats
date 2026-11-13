#include <gtest/gtest.h>
#include "TempDirectory.hpp"
#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include "utils/Storage.hpp"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <windows.h>

namespace {
    class ExportFailureTest : public ::testing::Test {
      protected:
        void SetUp() override {
            previousDataDirectory = Storage::GetDataDirectory();
            Storage::SetDataDirectoryOverride(directory.path.string() + "\\");
            db = std::make_unique<DatabaseManager>(std::make_shared<SessionState>());
            ASSERT_TRUE(db->Initialize(":memory:"));
            ASSERT_EQ(sqlite3_exec(db->GetRawDb(),
                                   "INSERT INTO Matches (match_guid) VALUES ('export-failure');",
                                   nullptr, nullptr, nullptr),
                      SQLITE_OK);
        }

        void TearDown() override {
            if (db) sqlite3_set_authorizer(db->GetRawDb(), nullptr, nullptr);
            if (lockedFile != INVALID_HANDLE_VALUE) CloseHandle(lockedFile);
            db.reset();
            Storage::SetDataDirectoryOverride(previousDataDirectory);
        }

        TempDirectory directory;
        std::string previousDataDirectory;
        std::unique_ptr<DatabaseManager> db;
        HANDLE lockedFile = INVALID_HANDLE_VALUE;
    };

    class ExportOutputFailureTest : public ExportFailureTest,
                                    public ::testing::WithParamInterface<const char*> {};

    TEST_P(ExportOutputFailureTest, RejectsAnUnwritableRequiredOutput) {
        std::string exportPath;
        std::string error;
        struct Context {
            const std::string& exportPath;
            const char* filename;
            bool attempted = false;
            std::error_code creationError;
        } context{exportPath, GetParam()};
        ASSERT_EQ(sqlite3_set_authorizer(
                      db->GetRawDb(),
                      [](void* data, int action, const char* table, const char*, const char*, const char*) {
                          auto& context = *static_cast<Context*>(data);
                          if (action == SQLITE_READ && table && std::strcmp(table, "Matches") == 0 && !context.attempted) {
                              context.attempted = true;
                              std::filesystem::create_directory(
                                  std::filesystem::path(context.exportPath) / context.filename,
                                  context.creationError);
                          }
                          return SQLITE_OK;
                      },
                      &context),
                  SQLITE_OK);

        const bool result = db->ExportLocalData(exportPath, error);

        ASSERT_TRUE(context.attempted);
        ASSERT_FALSE(context.creationError) << context.creationError.message();
        EXPECT_FALSE(result);
        EXPECT_FALSE(error.empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        RequiredOutputs, ExportOutputFailureTest,
        ::testing::Values("sessions.csv", "sessions.json", "replay_uploads.csv",
                          "replay_uploads.json", "tracked_accounts.json", "player_notes.json"));

    class ExportQueryFailureTest : public ExportFailureTest,
                                   public ::testing::WithParamInterface<const char*> {};

    TEST_P(ExportQueryFailureTest, RejectsAnUnreadableDataSet) {
        struct Context {
            const char* table;
            bool denied = false;
        } context{GetParam()};
        ASSERT_EQ(sqlite3_set_authorizer(
                      db->GetRawDb(),
                      [](void* data, int action, const char* table, const char*, const char*, const char*) {
                          auto& context = *static_cast<Context*>(data);
                          if (action == SQLITE_READ && table && std::strcmp(table, context.table) == 0) {
                              context.denied = true;
                              return SQLITE_DENY;
                          }
                          return SQLITE_OK;
                      },
                      &context),
                  SQLITE_OK);

        std::string exportPath;
        std::string error;
        const bool result = db->ExportLocalData(exportPath, error);

        ASSERT_TRUE(context.denied);
        EXPECT_FALSE(result);
        EXPECT_FALSE(error.empty());
    }

    INSTANTIATE_TEST_SUITE_P(
        RequiredDataSets, ExportQueryFailureTest,
        ::testing::Values("MatchLocalStats", "MatchPlayerStats", "MatchAnalytics", "MatchPlayers",
                          "Sessions", "ReplayUploads", "TrackedAccountRanks", "TrackedAccounts", "PlayerNotes"));

    TEST_F(ExportFailureTest, RejectsAWriteFailureWhenClosingAnOutput) {
        std::string exportPath;
        std::string error;
        struct Context {
            const std::string& exportPath;
            HANDLE& lockedFile;
            bool attempted = false;
            DWORD lockError = ERROR_SUCCESS;
            OVERLAPPED overlap{};
        } context{exportPath, lockedFile};
        ASSERT_EQ(sqlite3_set_authorizer(
                      db->GetRawDb(),
                      [](void* data, int action, const char* table, const char*, const char*, const char*) {
                          auto& context = *static_cast<Context*>(data);
                          if (action == SQLITE_READ && table && std::strcmp(table, "Matches") == 0 && !context.attempted) {
                              context.attempted = true;
                              const auto path = std::filesystem::path(context.exportPath) / "matches.csv";
                              context.lockedFile = CreateFileW(
                                  path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                              if (context.lockedFile == INVALID_HANDLE_VALUE ||
                                  !LockFileEx(context.lockedFile, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                                              0, MAXDWORD, MAXDWORD, &context.overlap)) {
                                  context.lockError = GetLastError();
                              }
                          }
                          return SQLITE_OK;
                      },
                      &context),
                  SQLITE_OK);

        const bool result = db->ExportLocalData(exportPath, error);

        ASSERT_TRUE(context.attempted);
        ASSERT_EQ(context.lockError, ERROR_SUCCESS);
        EXPECT_FALSE(result);
        EXPECT_FALSE(error.empty());
    }
} // namespace
