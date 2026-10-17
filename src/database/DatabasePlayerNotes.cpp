#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include <chrono>
#include <utility>

using namespace DatabaseDetail;

std::optional<std::vector<PlayerNotes::Tag>> DatabaseManager::ListPlayerTags() {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "PlayerTags")) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT platform,account_id,splitscreen,tag,created_at FROM PlayerTags ORDER BY created_at, tag;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    std::vector<PlayerNotes::Tag> result;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PlayerNotes::Tag tag;
        tag.identity.platform = SqlColumnText(stmt, 0);
        tag.identity.accountId = SqlColumnText(stmt, 1);
        tag.identity.splitscreen = sqlite3_column_int(stmt, 2);
        tag.text = SqlColumnText(stmt, 3);
        tag.createdAtUnix = sqlite3_column_int64(stmt, 4);
        result.push_back(std::move(tag));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? std::optional{std::move(result)} : std::nullopt;
}

std::optional<std::vector<std::string>> DatabaseManager::GetPlayerTags(const PlayerNotes::Identity& identity) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "PlayerTags")) return std::nullopt;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db,
                           "SELECT tag FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3 ORDER BY created_at, tag;",
                           -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, identity.splitscreen);
    std::vector<std::string> tags;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        tags.push_back(SqlColumnText(stmt, 0));
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? std::optional{std::move(tags)} : std::nullopt;
}

bool DatabaseManager::SetPlayerTag(const PlayerNotes::Identity& identity, std::string_view text, bool add,
                                   int64_t nowUnix, std::string* error) {
    auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
    const std::string tag = PlayerNotes::NormalizeTag(text);
    if (identity.platform.empty() || identity.accountId.empty() || identity.splitscreen < 0 || identity.splitscreen > 99)
        return fail("Player identity is unavailable.");
    if (tag.empty()) return fail("Enter a tag first.");
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "PlayerTags")) return fail("The local database is unavailable.");
    if (add) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT COUNT(*), MAX(CASE WHEN tag=?4 COLLATE NOCASE THEN 1 ELSE 0 END) "
                          "FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3;";
        if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(m_db));
        sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, identity.splitscreen);
        sqlite3_bind_text(stmt, 4, tag.c_str(), -1, SQLITE_TRANSIENT);
        int count = 0, exists = 0;
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
            exists = sqlite3_column_int(stmt, 1);
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) return fail("Could not check current tags.");
        if (exists) return true;
        if (count >= static_cast<int>(PlayerNotes::kMaxTagsPerPlayer)) return fail("This player already has the maximum number of tags.");
    }
    sqlite3_stmt* stmt = nullptr;
    const char* sql = add ? "INSERT OR IGNORE INTO PlayerTags(platform,account_id,splitscreen,tag,created_at) VALUES(?1,?2,?3,?4,?5);"
                          : "DELETE FROM PlayerTags WHERE platform=?1 AND account_id=?2 AND splitscreen=?3 AND tag=?4 COLLATE NOCASE;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(m_db));
    sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, identity.splitscreen);
    sqlite3_bind_text(stmt, 4, tag.c_str(), -1, SQLITE_TRANSIENT);
    if (add) sqlite3_bind_int64(stmt, 5, nowUnix);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? true : fail(sqlite3_errmsg(m_db));
}

void DatabaseManager::AsyncSetPlayerTag(PlayerNotes::Identity identity, std::string tag, bool add, uint64_t requestId) {
    if (!m_state) return;
    const bool queued = EnqueueDbJob([this, identity = std::move(identity), tag = std::move(tag), add, requestId]() {
        std::string error;
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        bool ok = SetPlayerTag(identity, tag, add, now, &error);
        std::optional<std::vector<std::string>> tags;
        if (ok) {
            tags = GetPlayerTags(identity);
            if (!tags) {
                ok = false;
                error = "Could not read saved tags.";
            }
        }
        std::lock_guard<std::mutex> lock(m_state->notes.mutex);
        if (ok) {
            if (tags->empty())
                m_state->notes.tagsByKey.erase(identity.Key());
            else
                m_state->notes.tagsByKey[identity.Key()] = std::move(*tags);
        }
        m_state->notes.completedTagRequestId = requestId;
        m_state->notes.lastTagRequestOk = ok;
        m_state->notes.lastTagRequestError = ok ? std::string{} : error;
        m_state->notes.version.fetch_add(1, std::memory_order_relaxed);
    },
                                     DbJobPriority::Critical);
    if (!queued) {
        std::lock_guard<std::mutex> lock(m_state->notes.mutex);
        m_state->notes.completedTagRequestId = requestId;
        m_state->notes.lastTagRequestOk = false;
        m_state->notes.lastTagRequestError = "The database is shutting down.";
        m_state->notes.version.fetch_add(1, std::memory_order_relaxed);
    }
}

std::optional<PlayerScoutProfile> DatabaseManager::GetPlayerScoutProfile(const std::string& account, const std::string& playerId) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || account.empty() || playerId.empty() || account == playerId) return std::nullopt;
    PlayerScoutProfile profile;
    auto records = QueryEncounterRecordsLocked(db, account, playerId);
    if (!records) return std::nullopt;
    if (!records->empty()) profile.record = records->front();
    profile.record.primaryId = playerId;

    // Each saved match is counted once, even if MatchPlayers contains duplicate rows.
    sqlite3_stmt* stmt = nullptr;
    const char* playlistSql = R"(
        SELECT COALESCE(m.gamemode,''), COUNT(*) FROM Matches m
        WHERE COALESCE(m.result_pending,0)=0 AND m.win IS NOT NULL
          AND EXISTS (SELECT 1 FROM MatchPlayers me WHERE me.match_id=m.id AND me.primary_id=?1)
          AND EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id=m.id AND p.primary_id=?2)
        GROUP BY COALESCE(m.gamemode,'') ORDER BY COUNT(*) DESC, 1 ASC LIMIT 1;
    )";
    if (sqlite3_prepare_v2(db, playlistSql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, playerId.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) profile.mostFrequentPlaylist = SqlColumnText(stmt, 0);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return std::nullopt;

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
    if (sqlite3_prepare_v2(db, recentSql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, playerId.c_str(), -1, SQLITE_TRANSIENT);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PlayerScoutEncounter encounter;
        encounter.matchId = sqlite3_column_int64(stmt, 0);
        encounter.playedAtUnix = sqlite3_column_int64(stmt, 1);
        encounter.playlist = SqlColumnText(stmt, 2);
        encounter.opponent = sqlite3_column_int(stmt, 3) != 0;
        encounter.win = sqlite3_column_int(stmt, 4) != 0;
        profile.recent.push_back(std::move(encounter));
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? std::optional{std::move(profile)} : std::nullopt;
}

void DatabaseManager::AsyncGetPlayerScout(std::string account, std::string playerId, uint64_t requestId) {
    if (!m_state) return;
    auto publish = [this, requestId, playerId](std::optional<PlayerScoutProfile> profile) {
        std::lock_guard<std::mutex> lock(m_state->scout.mutex);
        if (requestId < m_state->scout.requestId) return;
        m_state->scout.requestId = requestId;
        m_state->scout.playerId = playerId;
        m_state->scout.loaded = true;
        m_state->scout.success = profile.has_value();
        m_state->scout.profile = profile.value_or(PlayerScoutProfile{});
        m_state->scout.version.fetch_add(1, std::memory_order_relaxed);
    };
    if (!EnqueueDbJob([this, account = std::move(account), playerId, publish = std::move(publish)]() {
            publish(GetPlayerScoutProfile(account, playerId));
        },
                      DbJobPriority::Normal)) {
        std::lock_guard<std::mutex> lock(m_state->scout.mutex);
        if (requestId >= m_state->scout.requestId) {
            m_state->scout.requestId = requestId;
            m_state->scout.playerId = playerId;
            m_state->scout.loaded = true;
            m_state->scout.success = false;
            m_state->scout.version.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

static PlayerNotes::Note ReadPlayerNoteRow(sqlite3_stmt* stmt) {
    PlayerNotes::Note note;
    note.identity.platform = SqlColumnText(stmt, 0);
    note.identity.accountId = SqlColumnText(stmt, 1);
    note.identity.splitscreen = sqlite3_column_int(stmt, 2);
    note.text = SqlColumnText(stmt, 3);
    note.lastKnownName = SqlColumnText(stmt, 4);
    note.createdAtUnix = sqlite3_column_int64(stmt, 5);
    note.updatedAtUnix = sqlite3_column_int64(stmt, 6);
    return note;
}

std::optional<std::vector<PlayerNotes::Note>> DatabaseManager::ListPlayerNotes() {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "PlayerNotes")) return std::nullopt;
    const char* sql =
        "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at FROM PlayerNotes;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    std::vector<PlayerNotes::Note> notes;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        notes.push_back(ReadPlayerNoteRow(stmt));
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return std::nullopt;
    return notes;
}

std::optional<PlayerNotes::Note> DatabaseManager::GetPlayerNote(const PlayerNotes::Identity& identity) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !SqliteTableExists(db, "PlayerNotes")) return std::nullopt;
    const char* sql =
        "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at FROM PlayerNotes "
        "WHERE platform = ?1 AND account_id = ?2 AND splitscreen = ?3;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return std::nullopt;
    sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, identity.splitscreen);
    std::optional<PlayerNotes::Note> note;
    if (sqlite3_step(stmt) == SQLITE_ROW) note = ReadPlayerNoteRow(stmt);
    sqlite3_finalize(stmt);
    return note;
}

bool DatabaseManager::SavePlayerNote(const PlayerNotes::Identity& identity, std::string_view text, std::string_view lastKnownName,
                                     int64_t nowUnix, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    const std::string normalized = PlayerNotes::NormalizeNote(text);
    if (normalized.empty()) return fail("The note is empty.");
    if (identity.platform.empty() || identity.accountId.empty()) return fail("This player has no account ID to attach a note to.");
    const std::string name = TrackedAccounts::NormalizeLabel(lastKnownName, TrackedAccounts::kMaxDisplayNameLength);

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "PlayerNotes")) return fail("The local database is unavailable.");
    const char* sql = R"(
        INSERT INTO PlayerNotes (platform, account_id, splitscreen, note, last_known_name, created_at, updated_at)
        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?6)
        ON CONFLICT(platform, account_id, splitscreen) DO UPDATE SET
            note = excluded.note,
            last_known_name = CASE WHEN excluded.last_known_name != '' THEN excluded.last_known_name
                                   ELSE PlayerNotes.last_known_name END,
            updated_at = excluded.updated_at;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return fail(sqlite3_errmsg(m_db));
    sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, identity.splitscreen);
    sqlite3_bind_text(stmt, 4, normalized.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 6, nowUnix);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!ok) return fail(sqlite3_errmsg(m_db));
    return true;
}

bool DatabaseManager::DeletePlayerNote(const PlayerNotes::Identity& identity, std::string* error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db || !SqliteTableExists(m_db, "PlayerNotes")) {
        if (error) *error = "The local database is unavailable.";
        return false;
    }
    const char* sql = "DELETE FROM PlayerNotes WHERE platform = ?1 AND account_id = ?2 AND splitscreen = ?3;";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(stmt, 1, identity.platform.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, identity.accountId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, identity.splitscreen);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok && error) *error = sqlite3_errmsg(m_db);
    return ok;
}

void DatabaseManager::AsyncLoadPlayerNotes() {
    if (!m_state) return;
    (void)EnqueueDbJob([this]() {
        auto notes = ListPlayerNotes();
        auto tags = ListPlayerTags();
        std::lock_guard<std::mutex> lock(m_state->notes.mutex);
        // On failure, do not erase the last known notes/tags in memory.
        if (notes && tags) {
            m_state->notes.byKey.clear();
            for (auto& note : *notes)
                m_state->notes.byKey.emplace(note.identity.Key(), std::move(note));
            m_state->notes.tagsByKey.clear();
            for (const auto& tag : *tags)
                m_state->notes.tagsByKey[tag.identity.Key()].push_back(tag.text);
        }
        m_state->notes.loaded = notes.has_value() && tags.has_value();
        m_state->notes.loadFailed = !m_state->notes.loaded;
        m_state->notes.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Coalescable, "player_notes_load");
}

void DatabaseManager::AsyncSavePlayerNote(PlayerNotes::Identity identity, std::string text, std::string lastKnownName, uint64_t requestId) {
    if (!m_state) return;
    const bool queued = EnqueueDbJob([this, identity = std::move(identity), text = std::move(text),
                                      lastKnownName = std::move(lastKnownName), requestId]() {
        std::string error;
        const std::string normalized = PlayerNotes::NormalizeNote(text);
        std::optional<PlayerNotes::Note> stored;
        bool ok = false;
        if (normalized.empty()) {
            ok = DeletePlayerNote(identity, &error);
        } else {
            const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count();
            ok = SavePlayerNote(identity, normalized, lastKnownName, now, &error);
            if (ok) {
                stored = GetPlayerNote(identity);
                if (!stored) {
                    ok = false;
                    error = "The note was written but could not be read back.";
                }
            }
        }
        std::lock_guard<std::mutex> lock(m_state->notes.mutex);
        if (ok) {
            if (stored)
                m_state->notes.byKey[identity.Key()] = std::move(*stored);
            else
                m_state->notes.byKey.erase(identity.Key());
        }
        m_state->notes.completedRequestId = requestId;
        m_state->notes.lastRequestOk = ok;
        m_state->notes.lastRequestError = ok ? std::string{} : error;
        m_state->notes.version.fetch_add(1, std::memory_order_relaxed);
    },
                                     DbJobPriority::Critical);
    if (!queued) {
        std::lock_guard<std::mutex> lock(m_state->notes.mutex);
        m_state->notes.completedRequestId = requestId;
        m_state->notes.lastRequestOk = false;
        m_state->notes.lastRequestError = "The database is shutting down.";
        m_state->notes.version.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatabaseManager::AsyncDeletePlayerNote(PlayerNotes::Identity identity, uint64_t requestId) {
    AsyncSavePlayerNote(std::move(identity), {}, {}, requestId);
}
