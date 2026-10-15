#include <gtest/gtest.h>
#include <filesystem>
#include <memory>
#include <string>
#include <sqlite3.h>
#include "database/DatabaseManager.hpp"
#include "core/state/SessionState.hpp"

namespace {
    int Count(sqlite3* db, const char* sql) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return -1;
        int answer = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
        sqlite3_finalize(stmt);
        return answer;
    }
    MatchSaveSnapshot MakeMatch(const char* guid = "journal-match") {
        MatchSaveSnapshot snapshot;
        snapshot.arenaName = "DFH Stadium";
        snapshot.matchGuid = guid;
        snapshot.myTeam = 0;
        snapshot.winnerTeam = 0;
        snapshot.validResult = true;
        snapshot.playlistId = 11;
        snapshot.myPrimaryId = "Steam|1";
        snapshot.score[0] = 2;
        snapshot.score[1] = 1;
        snapshot.roster["Steam|1"] = PlayerData{.primaryId = "Steam|1", .name = "Me", .team = 0, .mmr = 1500};
        return snapshot;
    }
    MatchJournalEvent MakeEvent(std::string type, int64_t at) {
        MatchJournalEvent e;
        e.type = std::move(type);
        e.observedAtUnixMs = at;
        e.actorId = "Steam|1";
        e.actorName = "Me";
        e.source = "statfeed";
        return e;
    }
    void Cleanup(const std::string& path) {
        std::error_code ec;
        for (const char* suffix : {"", "-wal", "-shm", ".bak-v7", ".bak-v8"})
            std::filesystem::remove(path + suffix, ec);
    }
}

TEST(MatchEvents, RoundTripChronologyNullsAndDuplicateMatch) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    EXPECT_EQ(db.GetSchemaVersion(), 9);
    auto snap = MakeMatch();
    auto later = MakeEvent("goal", 2000);
    later.ballSpeed = 112.2;
    later.team = 0;
    later.matchClockSeconds = 180;
    later.eventKey = "goal:123";
    auto earlier = MakeEvent("shot", 1000);
    earlier.actorId.clear();
    earlier.actorName.clear();
    snap.events = {later, earlier, later};
    db.SaveMatch(snap);
    db.SaveMatch(snap); // duplicate GUID must not double-insert anything
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 1);
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents;"), 2);
    MatchDetail detail;
    ASSERT_TRUE(db.GetMatchDetail(1, detail, "Steam|1"));
    ASSERT_EQ(detail.events.size(), 2u);
    EXPECT_EQ(detail.events[0].type, "shot");
    EXPECT_FALSE(detail.events[0].team.has_value());
    EXPECT_FALSE(detail.events[0].matchClockSeconds.has_value());
    EXPECT_TRUE(detail.events[0].actorId.empty());
    EXPECT_EQ(detail.events[1].type, "goal");
    ASSERT_TRUE(detail.events[1].ballSpeed);
    EXPECT_DOUBLE_EQ(*detail.events[1].ballSpeed, 112.2);
    EXPECT_EQ(detail.events[1].eventKey, "goal:123");
}

TEST(MatchEvents, ForeignKeyCascadeAndFullDeletion) {
    auto state = std::make_shared<SessionState>();
    DatabaseManager db(state);
    ASSERT_TRUE(db.Initialize(":memory:"));
    auto snap = MakeMatch();
    snap.events.push_back(MakeEvent("save", 1234));
    db.SaveMatch(snap);
    ASSERT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents;"), 1);
    ASSERT_EQ(sqlite3_exec(db.GetRawDb(), "DELETE FROM MatchPlayers; DELETE FROM Matches;", nullptr, nullptr, nullptr), SQLITE_OK);
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents;"), 0);
    snap.matchGuid = "journal-second";
    db.SaveMatch(snap);
    std::string error;
    ASSERT_TRUE(db.DeleteLocalMatchHistory(error)) << error;
    EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents;"), 0);
}

TEST(MatchEvents, MigrationFromSevenPreservesMatchHistoryAndNewEventsAcrossRestart) {
    const std::string path = "test_match_events_upgrade.db";
    Cleanup(path);
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        db.SaveMatch(MakeMatch("preupgrade"));
        ASSERT_EQ(sqlite3_exec(db.GetRawDb(),
                               "DROP TABLE MatchEvents; PRAGMA user_version=7;", nullptr, nullptr, nullptr),
                  SQLITE_OK);
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        EXPECT_EQ(db.GetSchemaVersion(), 9);
        EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM Matches;"), 1);
        MatchDetail legacy;
        ASSERT_TRUE(db.GetMatchDetail(1, legacy, "Steam|1"));
        EXPECT_TRUE(legacy.events.empty());
        auto snap = MakeMatch("postupgrade");
        snap.events.push_back(MakeEvent("demolition", 142857));
        db.SaveMatch(snap);
        EXPECT_EQ(Count(db.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents;"), 1);
    }
    {
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path));
        MatchDetail detail;
        ASSERT_TRUE(db.GetMatchDetailByGuid("postupgrade", detail, "Steam|1"));
        ASSERT_EQ(detail.events.size(), 1u);
        EXPECT_EQ(detail.events[0].observedAtUnixMs, 142857);
    }
    Cleanup(path);
}

TEST(MatchEvents, ImportPreservesOriginalMatchAssociation) {
    const std::string path = "test_journal_merge_source.db";
    Cleanup(path);
    auto state = std::make_shared<SessionState>();
    {
        DatabaseManager source(state);
        ASSERT_TRUE(source.Initialize(path));
        auto snap = MakeMatch("import-source-guid");
        // Same-second timestamp + arena + score would hit the merge's legacy duplicate fallback.
        snap.arenaName = "Mannfield";
        snap.events.push_back(MakeEvent("epic_save", 800));
        source.SaveMatch(snap);
    }
    {
        DatabaseManager target(state);
        ASSERT_TRUE(target.Initialize(":memory:"));
        target.SaveMatch(MakeMatch("import-existing-guid"));
        auto result = target.MergeDatabase(path);
        ASSERT_TRUE(result.success) << result.error;
        MatchDetail detail;
        ASSERT_TRUE(target.GetMatchDetailByGuid("import-source-guid", detail, "Steam|1"));
        ASSERT_EQ(detail.events.size(), 1u);
        EXPECT_EQ(detail.events[0].type, "epic_save");
        EXPECT_EQ(Count(target.GetRawDb(), "SELECT COUNT(*) FROM MatchEvents WHERE match_id = 1;"), 0);
    }
    Cleanup(path);
}
