#include "PlayerNotesRepository.hpp"
#include "HistoryRepository.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include <chrono>
#include <iostream>
#include <utility>

namespace database::repository {

    namespace {

        PlayerNotes::Note ReadPlayerNoteRow(const sqlite::Statement& stmt) {
            PlayerNotes::Note note;
            note.identity.platform = stmt.Get<std::string>(0);
            note.identity.accountId = stmt.Get<std::string>(1);
            note.identity.splitscreen = stmt.Get<int>(2);
            note.text = stmt.Get<std::string>(3);
            note.lastKnownName = stmt.Get<std::string>(4);
            note.createdAtUnix = stmt.Get<int64_t>(5);
            note.updatedAtUnix = stmt.Get<int64_t>(6);
            return note;
        }

    } // namespace

    std::optional<std::vector<PlayerNotes::Tag>> PlayerNotesRepository::ListPlayerTags(sqlite::Connection& db) {
        if (!db.IsOpen() || !db.TableExists("PlayerTags")) return std::nullopt;

        const char* sql = "SELECT platform,account_id,splitscreen,tag,created_at FROM PlayerTags ORDER BY created_at, tag;";
        try {
            sqlite::Statement stmt(db, sql);
            std::vector<PlayerNotes::Tag> result;
            while (stmt.Step()) {
                PlayerNotes::Tag tag;
                tag.identity.platform = stmt.Get<std::string>(0);
                tag.identity.accountId = stmt.Get<std::string>(1);
                tag.identity.splitscreen = stmt.Get<int>(2);
                tag.text = stmt.Get<std::string>(3);
                tag.createdAtUnix = stmt.Get<int64_t>(4);
                result.push_back(std::move(tag));
            }
            return result;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::vector<std::string>> PlayerNotesRepository::GetPlayerTags(sqlite::Connection& db, const PlayerNotes::Identity& identity) {
        if (!db.IsOpen() || !db.TableExists("PlayerTags")) return std::nullopt;

        const char* sql =
            "SELECT tag FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3 ORDER BY created_at, tag;";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, identity.platform);
            stmt.Bind(2, identity.accountId);
            stmt.Bind(3, identity.splitscreen);

            std::vector<std::string> tags;
            while (stmt.Step()) {
                tags.push_back(stmt.Get<std::string>(0));
            }
            return tags;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool PlayerNotesRepository::SetPlayerTag(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string_view text, bool add,
                                             int64_t nowUnix, std::string* error) {
        auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
        const std::string tag = PlayerNotes::NormalizeTag(text);
        if (identity.platform.empty() || identity.accountId.empty() || identity.splitscreen < 0 || identity.splitscreen > 99)
            return fail("Player identity is unavailable.");
        if (tag.empty()) return fail("Enter a tag first.");
        if (!db.IsOpen() || !db.TableExists("PlayerTags")) return fail("The local database is unavailable.");

        try {
            if (add) {
                const char* sql = "SELECT COUNT(*), MAX(CASE WHEN tag=?4 COLLATE NOCASE THEN 1 ELSE 0 END) "
                                  "FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3;";
                sqlite::Statement stmt(db, sql);
                stmt.Bind(1, identity.platform);
                stmt.Bind(2, identity.accountId);
                stmt.Bind(3, identity.splitscreen);
                stmt.Bind(4, tag);

                int count = 0;
                bool exists = false;
                if (stmt.Step()) {
                    count = stmt.Get<int>(0);
                    exists = !stmt.IsNull(1) && stmt.Get<int>(1) != 0;
                }
                if (exists) return true;
                if (count >= static_cast<int>(PlayerNotes::kMaxTagsPerPlayer)) return fail("This player already has the maximum number of tags.");

                const char* insertSql = "INSERT OR IGNORE INTO PlayerTags (platform, account_id, splitscreen, tag, created_at) "
                                        "VALUES (?1, ?2, ?3, ?4, ?5);";
                sqlite::Statement insStmt(db, insertSql);
                insStmt.Bind(1, identity.platform);
                insStmt.Bind(2, identity.accountId);
                insStmt.Bind(3, identity.splitscreen);
                insStmt.Bind(4, tag);
                insStmt.Bind(5, nowUnix);
                insStmt.Execute();
                return true;
            } else {
                const char* sql = "DELETE FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3 AND tag=?4 COLLATE NOCASE;";
                sqlite::Statement stmt(db, sql);
                stmt.Bind(1, identity.platform);
                stmt.Bind(2, identity.accountId);
                stmt.Bind(3, identity.splitscreen);
                stmt.Bind(4, tag);
                stmt.Execute();
                return true;
            }
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }

    std::optional<PlayerScoutProfile> PlayerNotesRepository::GetPlayerScoutProfile(sqlite::Connection& db, const std::string& account, const std::string& playerId) {
        if (!db.IsOpen() || account.empty() || playerId.empty() || account == playerId) return std::nullopt;

        PlayerScoutProfile profile;
        auto records = HistoryRepository::QueryEncounterRecords(db, account, playerId);
        if (!records) return std::nullopt;
        if (!records->empty()) profile.record = records->front();
        profile.record.primaryId = playerId;

        try {
            const char* playlistSql = R"(
            SELECT COALESCE(m.gamemode,''), COUNT(*) FROM Matches m
            WHERE COALESCE(m.result_pending,0)=0 AND m.win IS NOT NULL
              AND EXISTS (SELECT 1 FROM MatchPlayers me WHERE me.match_id=m.id AND me.primary_id=?1)
              AND EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id=m.id AND p.primary_id=?2)
            GROUP BY COALESCE(m.gamemode,'') ORDER BY COUNT(*) DESC, 1 ASC LIMIT 1;
        )";
            sqlite::Statement stmt(db, playlistSql);
            stmt.Bind(1, account);
            stmt.Bind(2, playerId);
            if (stmt.Step()) {
                profile.mostFrequentPlaylist = stmt.Get<std::string>(0);
            }

            const char* recentSql = R"(
            SELECT m.id, CAST(strftime('%s',m.timestamp) AS INTEGER), COALESCE(m.gamemode,''),
                CASE WHEN MIN(me.team) IN (0,1) AND MIN(p.team) IN (0,1)
                     THEN MIN(p.team) != MIN(me.team)
                     ELSE MAX(COALESCE(p.is_opponent,0)) != MIN(COALESCE(me.is_opponent,0)) END AS against,
                CASE WHEN MIN(COALESCE(me.is_opponent,0))=0 THEN m.win ELSE 1-m.win END AS won
            FROM Matches m
            JOIN MatchPlayers me ON me.match_id=m.id AND me.primary_id=?1
            JOIN MatchPlayers p ON p.match_id=m.id AND p.primary_id=?2
            WHERE COALESCE(m.result_pending,0)=0 AND m.win IS NOT NULL
            GROUP BY m.id ORDER BY m.timestamp DESC,m.id DESC LIMIT 8;
        )";
            sqlite::Statement recentStmt(db, recentSql);
            recentStmt.Bind(1, account);
            recentStmt.Bind(2, playerId);
            while (recentStmt.Step()) {
                PlayerScoutEncounter encounter;
                encounter.matchId = recentStmt.Get<int64_t>(0);
                encounter.playedAtUnix = recentStmt.Get<int64_t>(1);
                encounter.playlist = recentStmt.Get<std::string>(2);
                encounter.opponent = recentStmt.Get<int>(3) != 0;
                encounter.win = recentStmt.Get<int>(4) != 0;
                profile.recent.push_back(std::move(encounter));
            }
            return profile;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<std::vector<PlayerNotes::Note>> PlayerNotesRepository::ListPlayerNotes(sqlite::Connection& db) {
        if (!db.IsOpen() || !db.TableExists("PlayerNotes")) return std::nullopt;

        const char* sql =
            "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at "
            "FROM PlayerNotes ORDER BY updated_at DESC, created_at DESC;";
        try {
            sqlite::Statement stmt(db, sql);
            std::vector<PlayerNotes::Note> notes;
            while (stmt.Step()) {
                notes.push_back(ReadPlayerNoteRow(stmt));
            }
            return notes;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::optional<PlayerNotes::Note> PlayerNotesRepository::GetPlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity) {
        if (!db.IsOpen() || !db.TableExists("PlayerNotes")) return std::nullopt;

        const char* sql =
            "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at "
            "FROM PlayerNotes WHERE platform = ?1 AND account_id = ?2 AND splitscreen = ?3 LIMIT 1;";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, identity.platform);
            stmt.Bind(2, identity.accountId);
            stmt.Bind(3, identity.splitscreen);
            if (stmt.Step()) {
                return ReadPlayerNoteRow(stmt);
            }
            return std::nullopt;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool PlayerNotesRepository::SavePlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string_view text,
                                               std::string_view lastKnownName, int64_t nowUnix, std::string* error) {
        auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
        const std::string normalized = PlayerNotes::NormalizeNote(text);
        if (normalized.empty()) return fail("The note is empty.");
        if (identity.platform.empty() || identity.accountId.empty()) return fail("This player has no account ID to attach a note to.");
        const std::string name = TrackedAccounts::NormalizeLabel(lastKnownName, TrackedAccounts::kMaxDisplayNameLength);
        if (!db.IsOpen() || !db.TableExists("PlayerNotes")) return fail("The local database is unavailable.");

        const char* sql = R"(
        INSERT INTO PlayerNotes (platform, account_id, splitscreen, note, last_known_name, created_at, updated_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)
        ON CONFLICT(platform, account_id, splitscreen) DO UPDATE SET
            note = excluded.note,
            last_known_name = CASE WHEN excluded.last_known_name != '' THEN excluded.last_known_name
                                   ELSE PlayerNotes.last_known_name END,
            updated_at = excluded.updated_at;
    )";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, identity.platform);
            stmt.Bind(2, identity.accountId);
            stmt.Bind(3, identity.splitscreen);
            stmt.Bind(4, normalized);
            stmt.Bind(5, name);
            stmt.Bind(6, nowUnix);
            stmt.Execute();
            return true;
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }

    bool PlayerNotesRepository::DeletePlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string* error) {
        if (!db.IsOpen() || !db.TableExists("PlayerNotes")) {
            if (error) *error = "The local database is unavailable.";
            return false;
        }

        const char* sql = "DELETE FROM PlayerNotes WHERE platform = ?1 AND account_id = ?2 AND splitscreen = ?3;";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, identity.platform);
            stmt.Bind(2, identity.accountId);
            stmt.Bind(3, identity.splitscreen);
            stmt.Execute();
            return true;
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
    }

} // namespace database::repository
