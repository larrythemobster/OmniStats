#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "core/SessionState.hpp"
#include "database/DatabaseManager.hpp"

namespace {
    PlayerNotes::Identity Id(const std::string& primaryId) {
        auto identity = PlayerNotes::IdentityFromPrimaryId(primaryId);
        EXPECT_TRUE(identity.has_value()) << primaryId;
        return identity.value_or(PlayerNotes::Identity{});
    }

    const std::string kEpicA = "Epic|0123456789abcdef0123456789abcdef|0";
    const std::string kEpicB = "Epic|fedcba9876543210fedcba9876543210|0";
    const std::string kSteam = "Steam|76561198000000001|0";

    int QueryInt(sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        int value = -1;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) value = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return value;
    }

    // The worker runs jobs in order, so a setting written after the job is a barrier.
    void Drain(DatabaseManager& db, const std::string& key) {
        db.AsyncSetSetting(key, "done");
        for (int i = 0; i < 400 && db.GetSetting(key, "") != "done"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    struct TempDb {
        std::filesystem::path path;
        explicit TempDb(const char* name) : path(std::filesystem::temp_directory_path() / name) {
            Clean();
        }
        ~TempDb() {
            Clean();
        }
        void Clean() const {
            std::error_code ec;
            for (const char* suffix : {"", "-wal", "-shm", ".bak-v5"})
                std::filesystem::remove(path.string() + suffix, ec);
        }
    };
}

class PlayerNotesDbTest : public ::testing::Test {
  protected:
    void SetUp() override {
        state = std::make_shared<SessionState>();
        db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
    }
    std::shared_ptr<SessionState> state;
    std::shared_ptr<DatabaseManager> db;
};

TEST_F(PlayerNotesDbTest, CreatesUpdatesAndDeletesANote) {
    const auto id = Id(kEpicA);
    EXPECT_FALSE(db->GetPlayerNote(id));

    ASSERT_TRUE(db->SavePlayerNote(id, "Good kickoffs", "ExamplePlayer", 1000));
    auto note = db->GetPlayerNote(id);
    ASSERT_TRUE(note);
    EXPECT_EQ(note->text, "Good kickoffs");
    EXPECT_EQ(note->lastKnownName, "ExamplePlayer");
    EXPECT_EQ(note->createdAtUnix, 1000);
    EXPECT_EQ(note->updatedAtUnix, 1000);

    ASSERT_TRUE(db->SavePlayerNote(id, "Good kickoffs\nOvercommits", "", 2000));
    note = db->GetPlayerNote(id);
    ASSERT_TRUE(note);
    EXPECT_EQ(note->text, "Good kickoffs\nOvercommits");
    EXPECT_EQ(note->lastKnownName, "ExamplePlayer");
    EXPECT_EQ(note->createdAtUnix, 1000);
    EXPECT_EQ(note->updatedAtUnix, 2000);
    EXPECT_EQ(QueryInt(db->GetRawDb(), "SELECT COUNT(*) FROM PlayerNotes;"), 1);

    ASSERT_TRUE(db->DeletePlayerNote(id));
    EXPECT_FALSE(db->GetPlayerNote(id));
    EXPECT_TRUE(db->DeletePlayerNote(id));
}

TEST_F(PlayerNotesDbTest, PlayersSharingADisplayNameKeepSeparateNotes) {
    ASSERT_TRUE(db->SavePlayerNote(Id(kEpicA), "Toxic", "Twin", 1));
    ASSERT_TRUE(db->SavePlayerNote(Id(kEpicB), "Strong teammate", "Twin", 2));
    EXPECT_EQ(db->GetPlayerNote(Id(kEpicA))->text, "Toxic");
    EXPECT_EQ(db->GetPlayerNote(Id(kEpicB))->text, "Strong teammate");
    EXPECT_FALSE(db->GetPlayerNote(Id(kSteam)));
    EXPECT_FALSE(db->GetPlayerNote(Id("Epic|0123456789abcdef0123456789abcdef|1")));
}

TEST_F(PlayerNotesDbTest, RejectsEmptyNotesAndStoresSpecialCharactersVerbatim) {
    std::string error;
    EXPECT_FALSE(db->SavePlayerNote(Id(kEpicA), "   \n\t", "Name", 1, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(db->GetPlayerNote(Id(kEpicA)));

    const std::string tricky = "<div onclick='x'>{{match_saves}}</div> & \"q\" ' ; DROP TABLE PlayerNotes; -- \xE2\x9C\x93";
    ASSERT_TRUE(db->SavePlayerNote(Id(kEpicA), tricky, "Name", 1));
    EXPECT_EQ(db->GetPlayerNote(Id(kEpicA))->text, tricky);
}

TEST_F(PlayerNotesDbTest, MaximumLengthIsEnforcedInCodePoints) {
    std::string longNote;
    for (size_t i = 0; i < PlayerNotes::kMaxNoteCodePoints + 10; ++i)
        longNote += "\xC3\xA9";
    ASSERT_TRUE(db->SavePlayerNote(Id(kEpicA), longNote, "Name", 1));
    EXPECT_EQ(PlayerNotes::CodePointCount(db->GetPlayerNote(Id(kEpicA))->text), PlayerNotes::kMaxNoteCodePoints);
}

TEST_F(PlayerNotesDbTest, AsyncOperationsPublishResultsToSessionState) {
    db->AsyncLoadPlayerNotes();
    Drain(*db, "barrier_load");
    {
        std::lock_guard lock(state->notes.mutex);
        EXPECT_TRUE(state->notes.loaded);
        EXPECT_TRUE(state->notes.byKey.empty());
    }

    db->AsyncSavePlayerNote(Id(kEpicA), "  Rotates well  ", "Mate", 7);
    Drain(*db, "barrier_save");
    {
        std::lock_guard lock(state->notes.mutex);
        EXPECT_EQ(state->notes.completedRequestId, 7u);
        EXPECT_TRUE(state->notes.lastRequestOk);
        ASSERT_EQ(state->notes.byKey.count(Id(kEpicA).Key()), 1u);
        EXPECT_EQ(state->notes.byKey.at(Id(kEpicA).Key()).text, "Rotates well");
    }

    // Saving an emptied editor removes the note rather than storing a blank row.
    db->AsyncSavePlayerNote(Id(kEpicA), "  ", "Mate", 8);
    Drain(*db, "barrier_clear");
    {
        std::lock_guard lock(state->notes.mutex);
        EXPECT_EQ(state->notes.completedRequestId, 8u);
        EXPECT_TRUE(state->notes.lastRequestOk);
        EXPECT_TRUE(state->notes.byKey.empty());
    }
    EXPECT_FALSE(db->GetPlayerNote(Id(kEpicA)));

    ASSERT_TRUE(db->SavePlayerNote(Id(kSteam), "Good mechanics", "Other", 3));
    db->AsyncDeletePlayerNote(Id(kSteam), 9);
    Drain(*db, "barrier_delete");
    std::lock_guard lock(state->notes.mutex);
    EXPECT_EQ(state->notes.completedRequestId, 9u);
    EXPECT_TRUE(state->notes.lastRequestOk);
    EXPECT_FALSE(db->GetPlayerNote(Id(kSteam)));
}

TEST_F(PlayerNotesDbTest, SaveFailureIsReportedWithoutTouchingTheCache) {
    {
        std::lock_guard lock(state->notes.mutex);
        state->notes.loaded = true;
    }
    ASSERT_EQ(sqlite3_exec(db->GetRawDb(), "DROP TABLE PlayerNotes;", nullptr, nullptr, nullptr), SQLITE_OK);
    db->AsyncSavePlayerNote(Id(kEpicA), "Will not persist", "Name", 11);
    Drain(*db, "barrier_fail");
    std::lock_guard lock(state->notes.mutex);
    EXPECT_EQ(state->notes.completedRequestId, 11u);
    EXPECT_FALSE(state->notes.lastRequestOk);
    EXPECT_FALSE(state->notes.lastRequestError.empty());
    EXPECT_TRUE(state->notes.byKey.empty());
}

TEST(PlayerNotesPersistenceTest, NotesSurviveARestart) {
    TempDb file("omnistats_notes_restart.db");
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.path.string()));
        ASSERT_TRUE(db.SavePlayerNote(Id(kEpicA), "Good mechanics, poor game sense", "ExamplePlayer", 42));
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.path.string()));
        const auto notes = db.ListPlayerNotes();
        ASSERT_TRUE(notes);
        ASSERT_EQ(notes->size(), 1u);
        EXPECT_EQ(notes->front().identity, Id(kEpicA));
        EXPECT_EQ(notes->front().text, "Good mechanics, poor game sense");
        EXPECT_EQ(notes->front().createdAtUnix, 42);
    }
}

TEST(PlayerNotesPersistenceTest, MigratesAVersion5DatabaseWithoutTouchingHistory) {
    TempDb file("omnistats_notes_v5.db");
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.path.string()));
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = "v5-guid";
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.myPrimaryId = kSteam;
        snap.roster[kSteam] = PlayerData{.primaryId = kSteam, .name = "Me", .team = 0, .mmr = 1000};
        snap.roster[kEpicA] = PlayerData{.primaryId = kEpicA, .name = "Rival", .team = 1, .mmr = 990};
        db.SaveMatch(snap);
        ASSERT_EQ(sqlite3_exec(db.GetRawDb(), "DROP TABLE PlayerNotes; PRAGMA user_version = 5;", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.path.string()));
        EXPECT_EQ(db.GetSchemaVersion(), 6);
        EXPECT_TRUE(std::filesystem::exists(file.path.string() + ".bak-v5"));
        EXPECT_EQ(QueryInt(db.GetRawDb(), "SELECT COUNT(*) FROM Matches WHERE match_guid = 'v5-guid';"), 1);
        EXPECT_EQ(QueryInt(db.GetRawDb(), "SELECT COUNT(*) FROM MatchPlayers;"), 2);
        const auto notes = db.ListPlayerNotes();
        ASSERT_TRUE(notes);
        EXPECT_TRUE(notes->empty());
        ASSERT_TRUE(db.SavePlayerNote(Id(kEpicA), "Played against several times", "Rival", 5));
        EXPECT_TRUE(db.GetPlayerNote(Id(kEpicA)));
    }
}
