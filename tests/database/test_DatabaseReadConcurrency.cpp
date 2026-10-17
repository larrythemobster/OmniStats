#include <gtest/gtest.h>

#include "database/DatabaseManager.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>

namespace {

    class DatabaseReadConcurrencyTest : public ::testing::Test {
      protected:
        void SetUp() override {
            directory = std::filesystem::temp_directory_path() /
                        ("omnistats_read_concurrency_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directory(directory);
            database = std::make_unique<DatabaseManager>(std::make_shared<SessionState>());
            ASSERT_TRUE(database->Initialize((directory / "history.db").string()));
        }

        void TearDown() override {
            database.reset();
            std::error_code error;
            std::filesystem::remove_all(directory, error);
        }

        std::filesystem::path directory;
        std::unique_ptr<DatabaseManager> database;
    };

    struct WriteGate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    };

    void BlockWrite(sqlite3_context* context, int, sqlite3_value**) {
        auto& gate = *static_cast<WriteGate*>(sqlite3_user_data(context));
        std::unique_lock lock(gate.mutex);
        gate.entered = true;
        gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.released; });
        sqlite3_result_int(context, 1);
    }

    TEST_F(DatabaseReadConcurrencyTest, HistoryAndAccountReadsSeeCommittedDataWhileWriterIsBlocked) {
        ASSERT_TRUE(database->SetSetting("guard", "committed"));
        auto identity = TrackedAccounts::FromPrimaryId("Steam|76561198000000001");
        ASSERT_TRUE(identity.has_value());
        ASSERT_EQ(database->RegisterTrackedAccount(TrackedAccounts::MakeRecord(
                      *identity, "main", "Player", TrackedAccounts::DiscoverySource::Manual)),
                  TrackedAccountInsertResult::Inserted);
        ASSERT_EQ(sqlite3_exec(database->GetRawDb(), R"(
        INSERT INTO Matches (id, arena, win, match_guid, gamemode, player_count, playlist_id)
        VALUES (1, 'DFH Stadium', 1, 'read-concurrency', '2v2', 4, 11);
        INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr)
        VALUES (1, 'Steam|76561198000000001', 'Player', 0, 1200);
    )",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);

        WriteGate gate;
        ASSERT_EQ(sqlite3_create_function_v2(database->GetRawDb(), "block_write", 0, SQLITE_UTF8,
                                             &gate, BlockWrite, nullptr, nullptr, nullptr),
                  SQLITE_OK);
        ASSERT_EQ(sqlite3_exec(database->GetRawDb(), R"(
        CREATE TEMP TRIGGER wait_before_setting BEFORE INSERT ON Settings
        BEGIN SELECT block_write(); END;
    )",
                               nullptr, nullptr, nullptr),
                  SQLITE_OK);

        auto writer = std::async(std::launch::async, [&] { return database->SetSetting("guard", "updated"); });
        bool entered;
        {
            std::unique_lock lock(gate.mutex);
            entered = gate.changed.wait_for(lock, std::chrono::seconds(2), [&] { return gate.entered; });
        }
        struct Results {
            std::string setting;
            std::vector<TrackedAccounts::AccountRecord> accounts;
            std::vector<SessionMatchSummary> history;
            std::vector<MatchRow> rows;
            int total = 0;
        };
        auto reader = std::async(std::launch::async, [&] {
            Results result;
            result.setting = database->GetSetting("guard");
            result.accounts = database->ListTrackedAccounts();
            database->GetRecentMatchHistory("Steam|76561198000000001", result.history);
            MatchQuery query;
            query.account = "Steam|76561198000000001";
            database->QueryMatches(query, result.rows, result.total);
            return result;
        });
        const bool readCompleted = reader.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
        {
            std::lock_guard lock(gate.mutex);
            gate.released = true;
        }
        gate.changed.notify_all();
        EXPECT_TRUE(writer.get());
        const auto result = reader.get();
        ASSERT_TRUE(entered);
        ASSERT_TRUE(readCompleted) << "Read query waited on the writer connection";
        EXPECT_EQ(result.setting, "committed");
        ASSERT_EQ(result.accounts.size(), 1u);
        EXPECT_EQ(result.accounts.front().primaryId, "Steam|76561198000000001");
        ASSERT_EQ(result.history.size(), 1u);
        EXPECT_EQ(result.total, 1);
        ASSERT_EQ(result.rows.size(), 1u);
        EXPECT_EQ(database->GetSetting("guard"), "updated");
    }

} // namespace
