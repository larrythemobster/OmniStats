#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"

namespace {
    const std::string me = "Steam|76561198000000001|0";
    const std::string rival = "Epic|0123456789abcdef0123456789abcdef|0";
    const std::string other = "Epic|fedcba9876543210fedcba9876543210|0";
    PlayerNotes::Identity Id(std::string_view primary) {
        return *PlayerNotes::IdentityFromPrimaryId(primary);
    }
    void Drain(DatabaseManager& db, const char* key) {
        db.AsyncSetSetting(key, "ok");
        for (int n = 0; n < 400 && db.GetSetting(key, "") != "ok"; ++n)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    void Save(DatabaseManager& db, const std::string& guid, int myTeam, int winner, const std::string& player,
              int playerTeam, bool pending = false) {
        MatchSaveSnapshot snap;
        snap.matchGuid = guid;
        snap.arenaName = "DFH Stadium";
        snap.playlistId = 11;
        snap.validResult = true;
        snap.resultPending = pending;
        snap.winnerTeam = winner;
        snap.myTeam = myTeam;
        snap.myPrimaryId = me;
        snap.endedAtUnixMs = 1'700'000'000'000LL + static_cast<int64_t>(guid.size()) * 60'000;
        snap.roster[me] = PlayerData{.primaryId = me, .name = "Local", .team = myTeam, .mmr = 1100};
        snap.roster[player] = PlayerData{.primaryId = player, .name = "Twin", .team = playerTeam, .mmr = 1200};
        db.SaveMatch(snap);
    }
    int Count(sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
        const int result = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return result;
    }
}

TEST(PlayerScout, TagsAreLocalUniqueNormalizedAndIndependentOfNotes) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    EXPECT_EQ(db.GetSchemaVersion(), 11);
    EXPECT_EQ(PlayerNotes::NormalizeTag("   Fast   aerials\n\t "), "Fast aerials");
    EXPECT_EQ(PlayerNotes::CodePointCount(PlayerNotes::NormalizeTag(std::string(100, 'a'))), PlayerNotes::kMaxTagCodePoints);

    std::string error;
    EXPECT_FALSE(db.SetPlayerTag(Id(rival), "  ", true, 1, &error));
    ASSERT_TRUE(db.SetPlayerTag(Id(rival), "Fast   aerials", true, 2));
    ASSERT_TRUE(db.SetPlayerTag(Id(rival), "fast aerials", true, 3)); // case-insensitive no-op
    auto tags = db.GetPlayerTags(Id(rival));
    ASSERT_TRUE(tags);
    ASSERT_EQ(tags->size(), 1u);
    EXPECT_EQ(tags->front(), "Fast aerials");
    EXPECT_FALSE(db.GetPlayerNote(Id(rival)).has_value());

    ASSERT_TRUE(db.SetPlayerTag(Id(other), "Fast aerials", true, 4));
    ASSERT_TRUE(db.SetPlayerTag(Id("Epic|0123456789abcdef0123456789abcdef|1"), "Fast aerials", true, 5));
    ASSERT_TRUE(db.SetPlayerTag(Id("Steam|76561198000000001|0"), "Fast aerials", true, 6));
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM PlayerTags;"), 4);
    ASSERT_TRUE(db.SetPlayerTag(Id(rival), "FAST AERIALS", false, 7));
    EXPECT_TRUE(db.GetPlayerTags(Id(rival))->empty());
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM PlayerTags;"), 3);
}

TEST(PlayerScout, TagLimitAndHostileTextAreHandledAsData) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    const auto id = Id(rival);
    for (size_t i = 0; i < PlayerNotes::kMaxTagsPerPlayer; ++i)
        ASSERT_TRUE(db.SetPlayerTag(id, "tag " + std::to_string(i), true, static_cast<int64_t>(i + 1)));
    std::string error;
    EXPECT_FALSE(db.SetPlayerTag(id, "one more", true, 15, &error));
    EXPECT_FALSE(error.empty());
    ASSERT_TRUE(db.SetPlayerTag(id, "tag 0", true, 16)); // duplicate remains idempotent
    ASSERT_TRUE(db.SetPlayerTag(id, "tag 0", false, 17));
    ASSERT_TRUE(db.SetPlayerTag(id, "<script>{{p}}</script>", true, 18));
    EXPECT_EQ(db.GetPlayerTags(id)->size(), PlayerNotes::kMaxTagsPerPlayer);
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM PlayerTags WHERE tag LIKE '%script%';"), 1);
}

TEST(PlayerScout, EncountersDifferentiateTeamsRecentMatchesAndUnknowns) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    Save(db, "rival-win-1", 0, 0, rival, 1);
    Save(db, "rival-win-2", 0, 0, rival, 0);
    Save(db, "rival-loss", 0, 1, rival, 0);
    Save(db, "other-match", 0, 0, other, 1);
    Save(db, "pending-rival", 0, 0, rival, 1, true);
    auto profile = db.GetPlayerScoutProfile(me, rival);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->record.winsWith, 1);
    EXPECT_EQ(profile->record.lossesWith, 1);
    EXPECT_EQ(profile->record.winsAgainst, 1);
    EXPECT_EQ(profile->record.lossesAgainst, 0);
    EXPECT_EQ(profile->recent.size(), 3u);
    EXPECT_FALSE(profile->mostFrequentPlaylist.empty());
    for (const auto& row : profile->recent)
        EXPECT_GT(row.matchId, 0);
    EXPECT_FALSE(db.GetPlayerScoutProfile("", rival));
    EXPECT_FALSE(db.GetPlayerScoutProfile(me, me));
    auto absent = db.GetPlayerScoutProfile(me, "Epic|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|0");
    ASSERT_TRUE(absent);
    EXPECT_TRUE(absent->recent.empty());
    EXPECT_EQ(absent->record.GamesAgainst(), 0);
}

TEST(PlayerScout, AsyncTagsAndProfilesCompleteOnWorker) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    Save(db, "async-scout", 0, 0, rival, 1);
    db.AsyncLoadPlayerNotes();
    Drain(db, "scout_load_barrier");
    {
        auto notesSnap = state->notes.GetSnapshot();
        ASSERT_NE(notesSnap, nullptr);
        EXPECT_TRUE(notesSnap->loaded);
    }
    db.AsyncSetPlayerTag(Id(rival), "Defensive", true, 42);
    db.AsyncGetPlayerScout(me, rival, 101);
    Drain(db, "scout_profile_barrier");
    {
        auto notesSnap = state->notes.GetSnapshot();
        ASSERT_NE(notesSnap, nullptr);
        EXPECT_EQ(notesSnap->completedTagRequestId, 42u);
        EXPECT_TRUE(notesSnap->lastTagRequestOk);
        ASSERT_EQ(notesSnap->tagsByKey.at(Id(rival).Key()).size(), 1u);
    }
    {
        auto scoutSnap = state->scout.GetSnapshot();
        ASSERT_NE(scoutSnap, nullptr);
        EXPECT_EQ(scoutSnap->requestId, 101u);
        EXPECT_TRUE(scoutSnap->loaded);
        EXPECT_TRUE(scoutSnap->success);
        EXPECT_EQ(scoutSnap->profile.recent.size(), 1u);
    }
}

TEST(PlayerScout, VersionNineMigrationPreservesNotesAndMatches) {
    const auto file = std::filesystem::temp_directory_path() / "omnistats_scout_upgrade_v9.db";
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", ".bak-v9"})
        std::filesystem::remove(file.string() + suffix, ec);
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.string()));
        ASSERT_TRUE(db.SavePlayerNote(Id(rival), "Known player", "Rival", 100));
        Save(db, "before-scout", 0, 0, rival, 1);
        ASSERT_EQ(sqlite3_exec(db.GetRawDb(), "DROP TABLE PlayerTags; PRAGMA user_version=9;", nullptr, nullptr, nullptr), SQLITE_OK);
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.string()));
        EXPECT_EQ(db.GetSchemaVersion(), 11);
        EXPECT_EQ(db.GetPlayerNote(Id(rival))->text, "Known player");
        EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 1);
        EXPECT_TRUE(db.GetPlayerTags(Id(rival))->empty());
        ASSERT_TRUE(db.SetPlayerTag(Id(rival), "Good teammate", true, 11));
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(file.string()));
        EXPECT_EQ(db.GetPlayerTags(Id(rival))->at(0), "Good teammate");
    }
    for (const char* suffix : {"", "-wal", "-shm", ".bak-v9"})
        std::filesystem::remove(file.string() + suffix, ec);
}
