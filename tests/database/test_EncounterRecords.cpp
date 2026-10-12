#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/SessionState.hpp"
#include "database/DatabaseManager.hpp"

namespace {
    const std::string kMe = "Steam|76561198000000001|0";
    const std::string kMyAlt = "Epic|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|0";

    struct Seat {
        std::string id;
        std::string name;
        int team;
    };
}

class EncounterRecordsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        state = std::make_shared<SessionState>();
        db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
    }

    // Saves a match from `saver`'s point of view; `winnerTeam` is absolute.
    void Save(const std::string& guid, const std::string& saver, int winnerTeam, const std::vector<Seat>& seats, bool pending = false) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.playlistId = 11;
        snap.winnerTeam = winnerTeam;
        snap.validResult = true;
        snap.resultPending = pending;
        snap.myPrimaryId = saver;
        snap.endedAtUnixMs = 1'700'000'000'000LL + static_cast<int64_t>(++saved) * 600'000LL;
        for (const auto& seat : seats) {
            snap.roster[seat.id] = PlayerData{.primaryId = seat.id, .name = seat.name, .team = seat.team, .mmr = 1000};
            if (seat.id == saver) snap.myTeam = seat.team;
        }
        db->SaveMatch(snap);
    }

    PersonRecord Record(const std::string& player, const std::string& account = kMe) {
        const auto record = db->GetPlayerEncounterRecord(account, player);
        EXPECT_TRUE(record.has_value());
        return record.value_or(PersonRecord{});
    }

    std::shared_ptr<SessionState> state;
    std::shared_ptr<DatabaseManager> db;
    int saved = 0;
};

TEST_F(EncounterRecordsTest, CountsTeammateOnlyOpponentOnlyAndBothRelationshipsIndependently) {
    const std::string mate = "Epic|11111111111111111111111111111111|0";
    const std::string rival = "Epic|22222222222222222222222222222222|0";
    const std::string both = "Epic|33333333333333333333333333333333|0";

    Save("m1", kMe, 0, {{kMe, "Me", 0}, {mate, "Mate", 0}, {rival, "Rival", 1}, {both, "Both", 1}});
    Save("m2", kMe, 1, {{kMe, "Me", 0}, {mate, "Mate", 0}, {rival, "Rival", 1}, {both, "Both", 0}});
    Save("m3", kMe, 1, {{kMe, "Me", 1}, {both, "Both", 1}, {rival, "Rival", 0}});

    const auto m = Record(mate);
    EXPECT_EQ(m.winsWith, 1);
    EXPECT_EQ(m.lossesWith, 1);
    EXPECT_EQ(m.GamesAgainst(), 0);

    const auto r = Record(rival);
    EXPECT_EQ(r.GamesWith(), 0);
    EXPECT_EQ(r.winsAgainst, 2);
    EXPECT_EQ(r.lossesAgainst, 1);

    // Both: opponent in a win, teammate in a loss, then teammate in a win.
    const auto b = Record(both);
    EXPECT_EQ(b.winsWith, 1);
    EXPECT_EQ(b.lossesWith, 1);
    EXPECT_EQ(b.winsAgainst, 1);
    EXPECT_EQ(b.lossesAgainst, 0);
}

TEST_F(EncounterRecordsTest, ConfirmedZeroIsDistinctFromUnavailable) {
    const auto none = db->GetPlayerEncounterRecord(kMe, "Epic|44444444444444444444444444444444|0");
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none->GamesWith() + none->GamesAgainst(), 0);

    EXPECT_FALSE(db->GetPlayerEncounterRecord("", "Epic|44444444444444444444444444444444|0"));
    DatabaseManager closed(state);
    EXPECT_FALSE(closed.GetPlayerEncounterRecord(kMe, "Epic|44444444444444444444444444444444|0"));
}

TEST_F(EncounterRecordsTest, DuplicateRowsAndProvisionalResultsAreNotCounted) {
    const std::string rival = "Epic|55555555555555555555555555555555|0";
    Save("dup", kMe, 0, {{kMe, "Me", 0}, {rival, "Rival", 1}});
    ASSERT_EQ(sqlite3_exec(db->GetRawDb(),
                           "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent) "
                           "SELECT match_id, primary_id, name, team, mmr, is_opponent FROM MatchPlayers;",
                           nullptr, nullptr, nullptr),
              SQLITE_OK);
    Save("pending", kMe, 0, {{kMe, "Me", 0}, {rival, "Rival", 1}}, true);

    const auto r = Record(rival);
    EXPECT_EQ(r.winsAgainst, 1);
    EXPECT_EQ(r.lossesAgainst, 0);
    EXPECT_EQ(r.GamesWith(), 0);
}

TEST_F(EncounterRecordsTest, PlayersWithTheSameNameStaySeparate) {
    const std::string twinA = "Epic|66666666666666666666666666666666|0";
    const std::string twinB = "PS4|1234567890|0";
    Save("t1", kMe, 0, {{kMe, "Me", 0}, {twinA, "Twin", 0}, {twinB, "Twin", 1}});

    EXPECT_EQ(Record(twinA).winsWith, 1);
    EXPECT_EQ(Record(twinA).GamesAgainst(), 0);
    EXPECT_EQ(Record(twinB).winsAgainst, 1);
    EXPECT_EQ(Record(twinB).GamesWith(), 0);
}

TEST_F(EncounterRecordsTest, RecordsAreScopedToTheLocalAccountAndItsSideOfTheMatch) {
    const std::string other = "Epic|77777777777777777777777777777777|0";
    // Saved by an alternate local account sitting opposite the main account:
    // stored win/is_opponent describe the alt's side.
    Save("alt", kMyAlt, 1, {{kMyAlt, "Alt", 1}, {kMe, "Me", 0}, {other, "Other", 0}});
    // A match the main account was not in.
    Save("absent", kMyAlt, 1, {{kMyAlt, "Alt", 1}, {other, "Other", 0}});

    const auto fromMain = Record(other, kMe);
    EXPECT_EQ(fromMain.GamesWith(), 1);
    EXPECT_EQ(fromMain.lossesWith, 1);
    EXPECT_EQ(fromMain.GamesAgainst(), 0);

    const auto fromAlt = Record(other, kMyAlt);
    EXPECT_EQ(fromAlt.winsAgainst, 2);
    EXPECT_EQ(fromAlt.GamesWith(), 0);
}

TEST_F(EncounterRecordsTest, PeopleListAgreesWithPerPlayerLookups) {
    const std::string a = "Epic|88888888888888888888888888888888|0";
    const std::string b = "Epic|99999999999999999999999999999999|0";
    Save("p1", kMe, 0, {{kMe, "Me", 0}, {a, "A", 0}, {b, "B", 1}, {"Unknown|Bot", "Bot", 1}});
    Save("p2", kMe, 1, {{kMe, "Me", 0}, {a, "A-renamed", 1}, {b, "B", 0}});

    std::vector<PersonRecord> people;
    db->GetPeopleRecords(kMe, people);
    ASSERT_EQ(people.size(), 2u);
    for (const auto& person : people) {
        const auto single = Record(person.primaryId);
        EXPECT_EQ(person.winsWith, single.winsWith) << person.primaryId;
        EXPECT_EQ(person.lossesWith, single.lossesWith) << person.primaryId;
        EXPECT_EQ(person.winsAgainst, single.winsAgainst) << person.primaryId;
        EXPECT_EQ(person.lossesAgainst, single.lossesAgainst) << person.primaryId;
    }
    const auto renamed = std::find_if(people.begin(), people.end(), [&](const PersonRecord& p) { return p.primaryId == a; });
    ASSERT_NE(renamed, people.end());
    EXPECT_EQ(renamed->name, "A-renamed");
}

TEST(EncounterRecordsMigrationTest, RecoversBothRelationshipsFromVersion1History) {
    const auto path = std::filesystem::temp_directory_path() / "omnistats_encounter_v1.db";
    std::error_code ec;
    for (const char* suffix : {"", "-wal", "-shm", ".bak-v1"})
        std::filesystem::remove(path.string() + suffix, ec);

    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open(path.string().c_str(), &raw), SQLITE_OK);
    const char* v1 = R"(
        CREATE TABLE Matches (id INTEGER PRIMARY KEY AUTOINCREMENT, timestamp DATETIME DEFAULT CURRENT_TIMESTAMP, arena TEXT,
            our_score INTEGER, their_score INTEGER, win BOOLEAN, match_guid TEXT, playlist_id INTEGER, gamemode TEXT,
            player_count INTEGER, result_pending BOOLEAN DEFAULT 0);
        CREATE TABLE MatchPlayers (id INTEGER PRIMARY KEY AUTOINCREMENT, match_id INTEGER, primary_id TEXT, name TEXT, team INTEGER,
            mmr INTEGER, mmr_estimated BOOLEAN DEFAULT 0, is_opponent BOOLEAN DEFAULT 0);
        CREATE TABLE Settings (key TEXT PRIMARY KEY, value TEXT);
        PRAGMA user_version = 1;
        INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, gamemode, player_count)
        VALUES ('2025-01-01 10:00:00', 'Mannfield', 3, 1, 1, 'old-1', '2v2', 4),
               ('2025-01-01 10:10:00', 'Mannfield', 0, 2, 0, 'old-2', '2v2', 4);
        INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, is_opponent) VALUES
            (1, 'Steam|76561198000000001|0', 'Me', 0, 1000, 0),
            (1, 'Epic|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|0', 'Old', 0, 990, 0),
            (2, 'Steam|76561198000000001|0', 'Me', 1, 995, 0),
            (2, 'Epic|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|0', 'Old', 0, 1001, 1);
    )";
    ASSERT_EQ(sqlite3_exec(raw, v1, nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close(raw);

    {
        auto state = std::make_shared<SessionState>();
        DatabaseManager db(state);
        ASSERT_TRUE(db.Initialize(path.string()));
        const auto record = db.GetPlayerEncounterRecord(kMe, "Epic|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|0");
        ASSERT_TRUE(record);
        EXPECT_EQ(record->winsWith, 1);
        EXPECT_EQ(record->lossesAgainst, 1);
        EXPECT_EQ(record->lossesWith + record->winsAgainst, 0);
    }
    for (const char* suffix : {"", "-wal", "-shm", ".bak-v1"})
        std::filesystem::remove(path.string() + suffix, ec);
}
