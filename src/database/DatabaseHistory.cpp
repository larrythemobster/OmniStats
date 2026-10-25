#include "DatabaseManager.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "core/game/MatchAnalytics.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <shared_mutex>
#include <utility>

using namespace DatabaseDetail;

static std::string FormatPlaylistName(int playerCount) {
    if (playerCount <= 0) return "Unknown";
    if (playerCount <= 2) return "Duel";
    if (playerCount <= 4) return "Doubles";
    if (playerCount <= 6) return "Standard";
    if (playerCount <= 8) return "Chaos";
    return "Unknown";
}

static std::string FormatMatchHistoryMode(const std::string& gamemode, int playerCount) {
    if (gamemode == "1v1") return "Duel";
    if (gamemode == "2v2") return "Doubles";
    if (gamemode == "3v3") return "Standard";
    if (gamemode == "hoops") return "Hoops";
    if (gamemode == "rumble") return "Rumble";
    if (gamemode == "dropshot") return "Dropshot";
    if (gamemode == "snowday") return "Snow Day";
    if (gamemode == "heatseeker") return "Heatseeker";
    if (gamemode == "casual") return FormatPlaylistName(playerCount);
    if (gamemode == "t") return "Tournament Match";
    return FormatPlaylistName(playerCount);
}

// Human-readable playlist for a Matches row. Rows with Game.PlaylistId use the
// authoritative metadata; legacy rows fall back to gamemode/player count.
static std::string DescribeMatchPlaylist(sqlite3_stmt* stmt, int gamemodeColumn, int playerCountColumn, int playlistIdColumn, bool& ranked) {
    if (sqlite3_column_type(stmt, playlistIdColumn) != SQLITE_NULL) {
        const int playlistId = sqlite3_column_int(stmt, playlistIdColumn);
        if (const auto* info = PlaylistMetadata::Find(playlistId)) {
            ranked = info->playlistClass == PlaylistMetadata::PlaylistClass::Ranked ||
                     info->playlistClass == PlaylistMetadata::PlaylistClass::Tournament;
            return info->playlistClass == PlaylistMetadata::PlaylistClass::Casual ? "Casual" : info->displayName;
        }
        ranked = false;
        return "Unknown Playlist (" + std::to_string(playlistId) + ")";
    }
    const std::string gamemode = SqlColumnText(stmt, gamemodeColumn);
    ranked = gamemode != "casual";
    return FormatMatchHistoryMode(gamemode, sqlite3_column_int(stmt, playerCountColumn));
}

int DatabaseManager::GetSchemaVersion() {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    return ReadUserVersion(db);
}

void DatabaseManager::GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return;

    outX.clear();
    outY.clear();

    const char* sql;
    bool filtered = (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" || playlist == "casual" || playlist == "t" ||
                     playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" || playlist == "snowday" ||
                     playlist == "heatseeker");

    if (filtered) {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
            FROM MatchPlayers
            JOIN Matches ON MatchPlayers.match_id = Matches.id
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0
              AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
              AND Matches.gamemode = ?
            ORDER BY Matches.timestamp ASC;
        )";
    } else {
        sql = R"(
            SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
            FROM MatchPlayers
            JOIN Matches ON MatchPlayers.match_id = Matches.id
            WHERE MatchPlayers.primary_id = ? AND MatchPlayers.mmr > 0
              AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
            ORDER BY Matches.timestamp ASC;
        )";
    }

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    if (filtered) {
        sqlite3_bind_text(stmt, 2, playlist.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t epoch = sqlite3_column_int64(stmt, 0);
        float mmr = (float)sqlite3_column_int(stmt, 1);
        outX.push_back(epoch);
        outY.push_back(mmr);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    outMatches.clear();
    if (!db) return;

    if (limit <= 0) limit = kPreviousGamesDefaultLimit;
    limit = std::clamp(limit, 10, kPreviousGamesMaxLimit);

    const char* sql = R"(
        SELECT Matches.our_score, Matches.their_score, Matches.win, Matches.gamemode, Matches.player_count,
               strftime('%s', Matches.timestamp), COALESCE(MatchPlayers.mmr, 0), Matches.match_guid, Matches.playlist_id,
               COALESCE(MatchPlayers.mmr_estimated, 0), Matches.id
        FROM Matches
        LEFT JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?1
        WHERE EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id = Matches.id AND p.primary_id = ?1)
        ORDER BY Matches.timestamp DESC, Matches.id DESC
        LIMIT ?2;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionMatchSummary summary;
        summary.matchId = sqlite3_column_int64(stmt, 10);
        summary.mode = DescribeMatchPlaylist(stmt, 3, 4, 8, summary.ranked);
        summary.matchGuid = SqlColumnText(stmt, 7);
        summary.ourScore = sqlite3_column_int(stmt, 0);
        summary.theirScore = sqlite3_column_int(stmt, 1);
        summary.mmr = sqlite3_column_int(stmt, 6);
        summary.mmrEstimated = sqlite3_column_int(stmt, 9) != 0;
        summary.win = sqlite3_column_int(stmt, 2) != 0;
        summary.endedAtUnix = sqlite3_column_int64(stmt, 5);
        outMatches.push_back(std::move(summary));
    }

    sqlite3_finalize(stmt);
}

std::optional<std::vector<PersonRecord>> DatabaseManager::QueryEncounterRecordsLocked(sqlite3* db, const std::string& account,
                                                                                      const std::string& onlyPlayer,
                                                                                      int64_t excludeMatchId) {
    if (!db || account.empty()) return std::nullopt;

    // The inner query collapses each (match, player) pair to one row, so
    // duplicate MatchPlayers rows count once. Team and result are taken
    // relative to the account's own row: a match saved by a different local
    // account stores `win` and `is_opponent` from that account's side. Bare
    // `name` in the outer query comes from the row holding MAX(ts).
    std::string sql = R"(
        SELECT pid, name,
               SUM(CASE WHEN against = 0 AND won = 1 THEN 1 ELSE 0 END),
               SUM(CASE WHEN against = 0 AND won = 0 THEN 1 ELSE 0 END),
               SUM(CASE WHEN against = 1 AND won = 1 THEN 1 ELSE 0 END),
               SUM(CASE WHEN against = 1 AND won = 0 THEN 1 ELSE 0 END),
               MAX(ts)
        FROM (
            SELECT p.primary_id AS pid,
                   p.name AS name,
                   CASE WHEN MIN(me.team) IN (0, 1) AND MIN(p.team) IN (0, 1)
                        THEN MIN(p.team) != MIN(me.team)
                        ELSE MAX(COALESCE(p.is_opponent, 0)) != MIN(COALESCE(me.is_opponent, 0)) END AS against,
                   CASE WHEN MIN(COALESCE(me.is_opponent, 0)) = 0 THEN m.win ELSE 1 - m.win END AS won,
                   CAST(strftime('%s', m.timestamp) AS INTEGER) AS ts
            FROM MatchPlayers p
            JOIN MatchPlayers me ON me.match_id = p.match_id AND me.primary_id = ?1
            JOIN Matches m ON m.id = p.match_id
            WHERE p.primary_id != ?1
              AND p.primary_id NOT LIKE 'Unknown|%'
              AND COALESCE(m.result_pending, 0) = 0
              AND m.win IS NOT NULL
    )";
    if (!onlyPlayer.empty()) sql += " AND p.primary_id = ?2";
    if (excludeMatchId > 0) sql += " AND m.id != ?3";
    sql += R"(
            GROUP BY m.id, p.primary_id
        )
        GROUP BY pid
        ORDER BY COUNT(*) DESC, 7 DESC
        LIMIT 500;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Encounter query failed: " << sqlite3_errmsg(db) << "\n";
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    if (!onlyPlayer.empty()) sqlite3_bind_text(stmt, 2, onlyPlayer.c_str(), -1, SQLITE_TRANSIENT);
    if (excludeMatchId > 0) sqlite3_bind_int64(stmt, 3, excludeMatchId);

    std::vector<PersonRecord> people;
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        PersonRecord person;
        person.primaryId = SqlColumnText(stmt, 0);
        person.name = SqlColumnText(stmt, 1);
        person.winsWith = sqlite3_column_int(stmt, 2);
        person.lossesWith = sqlite3_column_int(stmt, 3);
        person.winsAgainst = sqlite3_column_int(stmt, 4);
        person.lossesAgainst = sqlite3_column_int(stmt, 5);
        person.lastSeenUnix = sqlite3_column_int64(stmt, 6);
        people.push_back(std::move(person));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return std::nullopt;
    return people;
}

std::optional<PersonRecord> DatabaseManager::GetPlayerEncounterRecord(const std::string& account, const std::string& primaryId) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (primaryId.empty()) return std::nullopt;
    auto records = QueryEncounterRecordsLocked(db, account, primaryId);
    if (!records) return std::nullopt;
    if (!records->empty()) return std::move(records->front());
    PersonRecord none;
    none.primaryId = primaryId;
    return none;
}

void DatabaseManager::GetPeopleRecords(const std::string& primaryId, std::vector<PersonRecord>& outPeople) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    outPeople.clear();
    if (auto records = QueryEncounterRecordsLocked(db, primaryId)) outPeople = std::move(*records);
}

void DatabaseManager::GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    outMatches.clear();
    if (!db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT strftime('%s', Matches.timestamp),
               CAST(strftime('%H', Matches.timestamp, 'localtime') AS INTEGER),
               Matches.win, Matches.gamemode, Matches.player_count, Matches.playlist_id
        FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?
        WHERE COALESCE(Matches.result_pending, 0) = 0
        ORDER BY Matches.timestamp ASC, Matches.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchOutcome outcome;
        outcome.endedAtUnix = sqlite3_column_int64(stmt, 0);
        outcome.localHour = sqlite3_column_int(stmt, 1);
        outcome.win = sqlite3_column_int(stmt, 2) != 0;
        bool ranked = false;
        outcome.playlist = DescribeMatchPlaylist(stmt, 3, 4, 5, ranked);
        outMatches.push_back(std::move(outcome));
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    out.clear();
    if (!db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT m.id, m.playlist_id, m.gamemode, m.player_count, m.win, strftime('%s', m.timestamp),
               COALESCE(me.mmr, 0) AS my_mmr, COALESCE(me.mmr_estimated, 0),
               COALESCE(AVG(CASE WHEN p.is_opponent = 0 AND p.mmr > 0 THEN p.mmr END), 0.0) AS team_avg,
               COALESCE(AVG(CASE WHEN p.is_opponent = 1 AND p.mmr > 0 THEN p.mmr END), 0.0) AS opp_avg,
               SUM(CASE WHEN p.is_opponent = 0 AND p.mmr > 0 THEN 1 ELSE 0 END) AS team_n,
               SUM(CASE WHEN p.is_opponent = 1 AND p.mmr > 0 THEN 1 ELSE 0 END) AS opp_n
        FROM Matches m
        JOIN MatchPlayers me ON me.match_id = m.id AND me.primary_id = ?1
        JOIN MatchPlayers p  ON p.match_id = m.id
        WHERE COALESCE(m.result_pending, 0) = 0
        GROUP BY m.id
        ORDER BY m.timestamp ASC, m.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchMmrContext ctx;
        bool ranked = false;
        std::string playlist = DescribeMatchPlaylist(stmt, 2, 3, 1, ranked);
        ctx.playlist = ranked ? std::move(playlist) : "Casual";
        ctx.win = sqlite3_column_int(stmt, 4) != 0;
        ctx.endedAtUnix = sqlite3_column_int64(stmt, 5);
        ctx.myMmr = sqlite3_column_int(stmt, 6);
        ctx.mmrEstimated = sqlite3_column_int(stmt, 7) != 0;
        ctx.teamAvg = sqlite3_column_double(stmt, 8);
        ctx.oppAvg = sqlite3_column_double(stmt, 9);
        ctx.teamCount = sqlite3_column_int(stmt, 10);
        ctx.oppCount = sqlite3_column_int(stmt, 11);
        out.push_back(std::move(ctx));
    }
    sqlite3_finalize(stmt);
}

static std::string NormalizeStatSeriesPlaylistFilter(const std::string& filter) {
    if (filter.empty() || filter == "all" || filter == "All" || filter == "best" || filter == "Best") {
        return "";
    }
    if (filter == "Duel") return "1v1";
    if (filter == "Doubles") return "2v2";
    if (filter == "Standard") return "3v3";
    if (filter == "Casual") return "casual";
    if (filter == "Tournament" || filter == "Tournament Match") return "t";
    if (filter == "Hoops") return "hoops";
    if (filter == "Rumble") return "rumble";
    if (filter == "Dropshot") return "dropshot";
    if (filter == "Snow Day") return "snowday";
    if (filter == "Heatseeker") return "heatseeker";
    return filter;
}

std::vector<PlayerStatRow> DatabaseManager::GetPlayerStatSeries(const std::string& primaryId,
                                                                const std::string& playlistFilter,
                                                                int limit) {
    std::vector<PlayerStatRow> rows;
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || primaryId.empty() || !HasStatsTables(db)) return rows;

    if (limit <= 0) limit = 100;
    const std::string normalizedFilter = NormalizeStatSeriesPlaylistFilter(playlistFilter);
    const bool filtered = !normalizedFilter.empty();

    std::string sql = R"(
        SELECT Matches.id, Matches.match_guid, strftime('%s', Matches.timestamp),
               Matches.gamemode, COALESCE(Matches.playlist_id, -1), Matches.win,
               Matches.our_score, Matches.their_score, COALESCE(MatchPlayers.mmr, 0),
               MatchPlayerStats.score, COALESCE(MatchPlayerStats.goals, 0),
               COALESCE(MatchPlayerStats.assists, 0), COALESCE(MatchPlayerStats.saves, 0),
               COALESCE(MatchPlayerStats.shots, 0), COALESCE(MatchPlayerStats.demos, 0),
               MatchPlayerStats.touches, MatchPlayerStats.car_touches,
               MatchPlayerStats.max_goal_speed, MatchPlayerStats.fastest_goal_time,
               MatchLocalStats.match_id, COALESCE(MatchLocalStats.boost_collected, 0),
               COALESCE(MatchLocalStats.demoed, 0), COALESCE(MatchLocalStats.crossbars, 0),
               MatchLocalStats.hardest_crossbar, MatchLocalStats.max_ball_speed,
               COALESCE(MatchLocalStats.own_goals, 0), MatchLocalStats.duration_seconds,
               MatchLocalStats.overtime_seconds, COALESCE(MatchLocalStats.stats_version, 1)
        FROM MatchPlayerStats
        JOIN Matches ON Matches.id = MatchPlayerStats.match_id
        LEFT JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?1
        LEFT JOIN MatchLocalStats ON MatchLocalStats.match_id = Matches.id
        WHERE MatchPlayerStats.primary_id = ?1
    )";
    if (filtered) {
        sql += " AND Matches.gamemode = ?2 ORDER BY Matches.timestamp DESC, Matches.id DESC LIMIT ?3;";
    } else {
        sql += " ORDER BY Matches.timestamp DESC, Matches.id DESC LIMIT ?2;";
    }

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    if (filtered) {
        sqlite3_bind_text(stmt, 2, normalizedFilter.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
    } else {
        sqlite3_bind_int(stmt, 2, limit);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PlayerStatRow row;
        row.matchId = sqlite3_column_int64(stmt, 0);
        row.matchGuid = SqlColumnText(stmt, 1);
        row.timestampUnix = sqlite3_column_int64(stmt, 2);
        row.playlist = SqlColumnText(stmt, 3);
        row.playlistId = sqlite3_column_int(stmt, 4);
        row.win = sqlite3_column_int(stmt, 5) != 0;
        row.ourScore = sqlite3_column_int(stmt, 6);
        row.theirScore = sqlite3_column_int(stmt, 7);
        row.mmr = sqlite3_column_int(stmt, 8);
        if (sqlite3_column_type(stmt, 9) != SQLITE_NULL) row.score = sqlite3_column_int(stmt, 9);
        row.goals = sqlite3_column_int(stmt, 10);
        row.assists = sqlite3_column_int(stmt, 11);
        row.saves = sqlite3_column_int(stmt, 12);
        row.shots = sqlite3_column_int(stmt, 13);
        row.demos = sqlite3_column_int(stmt, 14);
        if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) row.touches = sqlite3_column_int(stmt, 15);
        if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) row.carTouches = sqlite3_column_int(stmt, 16);
        if (sqlite3_column_type(stmt, 17) != SQLITE_NULL)
            row.maxGoalSpeed = static_cast<float>(sqlite3_column_double(stmt, 17));
        if (sqlite3_column_type(stmt, 18) != SQLITE_NULL)
            row.fastestGoalTime = static_cast<float>(sqlite3_column_double(stmt, 18));
        row.hasLocalStats = sqlite3_column_type(stmt, 19) != SQLITE_NULL;
        row.boostCollected = sqlite3_column_int(stmt, 20);
        row.demoed = sqlite3_column_int(stmt, 21);
        row.crossbars = sqlite3_column_int(stmt, 22);
        if (sqlite3_column_type(stmt, 23) != SQLITE_NULL)
            row.hardestCrossbar = static_cast<float>(sqlite3_column_double(stmt, 23));
        if (sqlite3_column_type(stmt, 24) != SQLITE_NULL)
            row.maxBallSpeed = static_cast<float>(sqlite3_column_double(stmt, 24));
        row.ownGoals = sqlite3_column_int(stmt, 25);
        if (sqlite3_column_type(stmt, 26) != SQLITE_NULL)
            row.durationSeconds = static_cast<float>(sqlite3_column_double(stmt, 26));
        if (sqlite3_column_type(stmt, 27) != SQLITE_NULL)
            row.overtimeSeconds = static_cast<float>(sqlite3_column_double(stmt, 27));
        row.statsVersion = sqlite3_column_int(stmt, 28);
        rows.push_back(std::move(row));
    }

    sqlite3_finalize(stmt);
    return rows;
}

void DatabaseManager::GetDetailedStatsSummary(int& outMatchCount, std::string& outSinceDate) {
    outMatchCount = 0;
    outSinceDate.clear();
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !HasStatsTables(db)) return;

    const char* sql = R"(
        SELECT COUNT(*), COALESCE(MIN(substr(Matches.timestamp, 1, 10)), '')
        FROM Matches
        WHERE EXISTS (SELECT 1 FROM MatchLocalStats s WHERE s.match_id = Matches.id)
           OR EXISTS (SELECT 1 FROM MatchPlayerStats p WHERE p.match_id = Matches.id);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        outMatchCount = sqlite3_column_int(stmt, 0);
        if (outMatchCount > 0) {
            outSinceDate = SqlColumnText(stmt, 1);
        }
    }
    sqlite3_finalize(stmt);
}

void DatabaseManager::GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    wins = 0;
    losses = 0;
    gamesPlayed = 0;
    if (!db) return;

    const char* sql = R"(
        SELECT
            SUM(CASE WHEN Matches.win = 1 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 THEN 1 ELSE 0 END),
            COUNT(*)
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ? AND Matches.gamemode = ?
          AND COALESCE(Matches.result_pending, 0) = 0;
    )";

    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, gamemode.c_str(), -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        wins = sqlite3_column_int(stmt, 0);
        losses = sqlite3_column_int(stmt, 1);
        gamesPlayed = sqlite3_column_int(stmt, 2);
    }

    sqlite3_finalize(stmt);
}

void DatabaseManager::GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss) {
    outCurWin = 0;
    outCurLoss = 0;
    outLongestWin = 0;
    outLongestLoss = 0;

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || primaryId.empty()) return;

    const char* sql = R"(
        SELECT Matches.win FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ? AND COALESCE(Matches.result_pending, 0) = 0
        ORDER BY Matches.timestamp DESC, Matches.id DESC;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);

    int previous = -1;
    int runLength = 0;
    bool initialRun = true;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const int win = sqlite3_column_int(stmt, 0);
        if (win != previous) {
            if (previous != -1) initialRun = false;
            previous = win;
            runLength = 0;
        }
        ++runLength;
        if (win == 1) {
            if (initialRun) outCurWin = runLength;
            outLongestWin = std::max(outLongestWin, runLength);
        } else {
            if (initialRun) outCurLoss = runLength;
            outLongestLoss = std::max(outLongestLoss, runLength);
        }
    }
    if (rc != SQLITE_DONE) {
        std::cerr << "[Database] Failed to read streak history: " << sqlite3_errmsg(db) << '\n';
        outCurWin = outCurLoss = outLongestWin = outLongestLoss = 0;
    }
    sqlite3_finalize(stmt);
}

std::string DatabaseManager::GetSetting(const std::string& key, const std::string& defaultValue) {
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return defaultValue;

    const char* sql = "SELECT value FROM Settings WHERE key = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return defaultValue;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);

    std::string result = defaultValue;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* val = sqlite3_column_text(stmt, 0);
        if (val) {
            result = reinterpret_cast<const char*>(val);
        }
    }

    sqlite3_finalize(stmt);
    return result;
}

namespace {
    std::string ToLowerAscii(std::string_view text) {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    std::string EscapeLikePattern(std::string_view input) {
        std::string out;
        out.reserve(input.size() + 4);
        for (char c : input) {
            if (c == '%' || c == '_' || c == '\\') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return "%" + out + "%";
    }

    std::string PlatformFromPrimaryId(const std::string& primaryId) {
        const size_t sep = primaryId.find('|');
        if (sep == std::string::npos) return {};
        const std::string raw = primaryId.substr(0, sep);
        const std::string lower = ToLowerAscii(raw);
        if (lower.empty()) return {};
        if (lower == "epic" || lower == "epicgames") return "Epic";
        if (lower == "steam") return "Steam";
        if (lower.rfind("ps", 0) == 0 || lower == "playstation") return "PlayStation";
        if (lower.rfind("xb", 0) == 0) return "Xbox";
        if (lower == "switch" || lower == "nintendo") return "Nintendo";
        if (lower == "unknown" || lower == "bot") return "Bot";
        return raw;
    }

    std::string TierPlaylistKey(int playlistId, const std::string& gamemode) {
        if (PlaylistMetadata::HasAuthoritativeId(playlistId) && PlaylistMetadata::IsKnown(playlistId)) {
            const std::string key = PlaylistMetadata::MmrKey(playlistId);
            if (!key.empty()) return key;
        }
        return gamemode;
    }

    struct BoundSqlParam {
        enum class Kind { Text,
                          Int,
                          Int64 };
        Kind kind = Kind::Text;
        std::string text;
        int intVal = 0;
        int64_t int64Val = 0;
    };

    void BindParams(sqlite3_stmt* stmt, const std::vector<BoundSqlParam>& params) {
        for (size_t i = 0; i < params.size(); ++i) {
            const int idx = static_cast<int>(i + 1);
            const auto& p = params[i];
            switch (p.kind) {
            case BoundSqlParam::Kind::Text:
                sqlite3_bind_text(stmt, idx, p.text.c_str(), -1, SQLITE_TRANSIENT);
                break;
            case BoundSqlParam::Kind::Int:
                sqlite3_bind_int(stmt, idx, p.intVal);
                break;
            case BoundSqlParam::Kind::Int64:
                sqlite3_bind_int64(stmt, idx, p.int64Val);
                break;
            }
        }
    }
}

void DatabaseManager::QueryMatches(const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount) {
    outRows.clear();
    outTotalCount = 0;
    if (query.account.empty()) return;

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return;

    std::vector<BoundSqlParam> params;
    auto addText = [&](std::string val) -> int {
        params.push_back({BoundSqlParam::Kind::Text, std::move(val), 0, 0});
        return static_cast<int>(params.size());
    };
    auto addInt = [&](int val) -> int {
        params.push_back({BoundSqlParam::Kind::Int, {}, val, 0});
        return static_cast<int>(params.size());
    };
    auto addInt64 = [&](int64_t val) -> int {
        params.push_back({BoundSqlParam::Kind::Int64, {}, 0, val});
        return static_cast<int>(params.size());
    };

    (void)addText(query.account); // ?1

    std::string whereSql;
    if (query.playlistLabel.has_value() && !query.playlistLabel->empty()) {
        const std::string lower = ToLowerAscii(*query.playlistLabel);
        if (lower != "all") {
            if (lower == "extra" || lower == "extras") {
                whereSql += " AND m.gamemode IN ('hoops', 'rumble', 'dropshot', 'snowday', 'heatseeker')";
            } else if (lower == "ranked") {
                whereSql += " AND COALESCE(m.gamemode, '') != 'casual'";
            } else if (lower == "casual") {
                whereSql += " AND m.gamemode = 'casual'";
            } else {
                std::string mapped = *query.playlistLabel;
                if (lower == "1v1" || lower == "duel")
                    mapped = "1v1";
                else if (lower == "2v2" || lower == "doubles")
                    mapped = "2v2";
                else if (lower == "3v3" || lower == "standard")
                    mapped = "3v3";
                else if (lower == "hoops")
                    mapped = "hoops";
                else if (lower == "rumble")
                    mapped = "rumble";
                else if (lower == "dropshot")
                    mapped = "dropshot";
                else if (lower == "snow day" || lower == "snowday" || lower == "snow_day")
                    mapped = "snowday";
                else if (lower == "heatseeker")
                    mapped = "heatseeker";
                else if (lower == "tournament" || lower == "tournament match" || lower == "t" || lower == "tourny")
                    mapped = "t";
                const int pMode = addText(std::move(mapped));
                whereSql += " AND m.gamemode = ?" + std::to_string(pMode);
            }
        }
    }

    if (query.ranked.has_value()) {
        if (*query.ranked) {
            whereSql += " AND COALESCE(m.gamemode, '') != 'casual'";
        } else {
            whereSql += " AND m.gamemode = 'casual'";
        }
    }

    if (query.win.has_value()) {
        const int pWin = addInt(*query.win ? 1 : 0);
        whereSql += " AND m.win = ?" + std::to_string(pWin);
    }

    if (query.fromUnix > 0) {
        const int pFrom = addInt64(query.fromUnix);
        whereSql += " AND CAST(strftime('%s', m.timestamp) AS INTEGER) >= ?" + std::to_string(pFrom);
    }

    if (query.toUnix > 0) {
        const int pTo = addInt64(query.toUnix);
        whereSql += " AND CAST(strftime('%s', m.timestamp) AS INTEGER) <= ?" + std::to_string(pTo);
    }

    if (!query.arena.empty() && ToLowerAscii(query.arena) != "all") {
        const int pExact = addText(query.arena);
        const int pLike = addText(EscapeLikePattern(query.arena));
        whereSql += " AND (LOWER(m.arena) = LOWER(?" + std::to_string(pExact) +
                    ") OR m.arena LIKE ?" + std::to_string(pLike) + " ESCAPE '\\')";
    }

    if (!query.withPlayer.empty()) {
        const int pId = addText(query.withPlayer);
        const int pLike = addText(EscapeLikePattern(query.withPlayer));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers wp WHERE wp.match_id = m.id"
                    " AND COALESCE(wp.is_opponent, 0) = 0 AND wp.primary_id != ?1"
                    " AND (wp.primary_id = ?" +
                    std::to_string(pId) +
                    " OR wp.name LIKE ?" + std::to_string(pLike) + " ESCAPE '\\'))";
    }

    if (!query.againstPlayer.empty()) {
        const int pId = addText(query.againstPlayer);
        const int pLike = addText(EscapeLikePattern(query.againstPlayer));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers ap WHERE ap.match_id = m.id"
                    " AND COALESCE(ap.is_opponent, 0) = 1"
                    " AND (ap.primary_id = ?" +
                    std::to_string(pId) +
                    " OR ap.name LIKE ?" + std::to_string(pLike) + " ESCAPE '\\'))";
    }

    if (!query.nameSearch.empty()) {
        const int pLike = addText(EscapeLikePattern(query.nameSearch));
        whereSql += " AND EXISTS (SELECT 1 FROM MatchPlayers np WHERE np.match_id = m.id"
                    " AND np.name LIKE ?" +
                    std::to_string(pLike) + " ESCAPE '\\')";
    }

    std::string orderSql;
    switch (query.sort) {
    case MatchQuery::Sort::Oldest:
        orderSql = "ORDER BY timestamp ASC, id ASC";
        break;
    case MatchQuery::Sort::BiggestWin:
        orderSql = "ORDER BY (mmr_delta IS NULL) ASC, mmr_delta DESC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::BiggestLoss:
        orderSql = "ORDER BY (mmr_delta IS NULL) ASC, mmr_delta ASC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::ScoreMargin:
        orderSql = "ORDER BY (our_score - their_score) DESC, ABS(our_score - their_score) DESC, timestamp DESC, id DESC";
        break;
    case MatchQuery::Sort::Newest:
    default:
        orderSql = "ORDER BY timestamp DESC, id DESC";
        break;
    }

    const std::vector<BoundSqlParam> filterParams = params;
    const int limit = query.limit > 0 ? query.limit : 100;
    const int offset = std::max(0, query.offset);
    const int pLimit = addInt(limit);
    const int pOffset = addInt(offset);

    std::string sql =
        "WITH AccountMatches AS ("
        "    SELECT m.id, m.timestamp, m.arena, m.our_score, m.their_score, m.win,"
        "           m.match_guid, m.playlist_id, m.gamemode, m.player_count,"
        "           COALESCE(me.team, 0) AS my_team,"
        "           COALESCE(me.mmr, 0) AS my_mmr, COALESCE(me.mmr_estimated, 0) AS mmr_estimated,"
        "           LAG(CASE WHEN me.mmr > 0 THEN me.mmr END) OVER ("
        "               PARTITION BY m.gamemode"
        "               ORDER BY m.timestamp ASC, m.id ASC"
        "           ) AS prev_mmr"
        "    FROM MatchPlayers me"
        "    JOIN Matches m ON m.id = me.match_id"
        "    WHERE me.primary_id = ?1 AND COALESCE(m.result_pending, 0) = 0"
        "),"
        "Filtered AS ("
        "    SELECT id, timestamp, arena, our_score, their_score, win, match_guid,"
        "           playlist_id, gamemode, player_count, my_team, my_mmr, mmr_estimated,"
        "           CASE WHEN my_mmr > 0 AND prev_mmr > 0 THEN my_mmr - prev_mmr ELSE NULL END AS mmr_delta,"
        "           COUNT(*) OVER () AS total_count"
        "    FROM AccountMatches m"
        "    WHERE 1=1" +
        whereSql +
        "),"
        "PageRows AS ("
        "    SELECT id, timestamp, arena, our_score, their_score, win, match_guid,"
        "           playlist_id, gamemode, player_count, my_team, my_mmr, mmr_estimated, mmr_delta, total_count"
        "    FROM Filtered " +
        orderSql +
        "    LIMIT ?" + std::to_string(pLimit) + " OFFSET ?" + std::to_string(pOffset) +
        ") ";
    const bool hasStats = HasStatsTables(db);
    const bool hasAnalytics = SqliteTableExists(db, "MatchAnalytics");
    const bool hasReplayUploads = SqliteTableExists(db, "ReplayUploads");

    sql +=
        "SELECT p.id, CAST(strftime('%s', p.timestamp) AS INTEGER), p.arena, p.our_score, p.their_score, p.win,"
        "       p.match_guid, p.playlist_id, p.gamemode, p.player_count, p.my_mmr, p.mmr_estimated, p.mmr_delta, p.total_count,"
        "       p.my_team, ";
    if (hasReplayUploads) {
        sql += "COALESCE(ru.ballchasing_url, ''), ";
    } else {
        sql += "'', ";
    }
    if (hasStats) {
        sql +=
            "mps.score, mps.goals, mps.assists, mps.saves, mps.shots,"
            "(CASE WHEN p.win = 1 AND mps.score > 0 AND mps.score = (SELECT MAX(score) FROM MatchPlayerStats WHERE match_id = p.id) THEN 1 ELSE 0 END),"
            "(COALESCE(mls.overtime_seconds, 0) > 0), ";
    } else {
        sql += "NULL, NULL, NULL, NULL, NULL, 0, 0, ";
    }
    if (hasAnalytics) {
        sql += "ma.data ";
    } else {
        sql += "NULL ";
    }
    sql += "FROM PageRows p ";
    if (hasReplayUploads) {
        sql += "LEFT JOIN (SELECT match_guid, ballchasing_url FROM ReplayUploads WHERE match_guid IS NOT NULL AND match_guid != '' AND ballchasing_url IS NOT NULL AND ballchasing_url != '' GROUP BY match_guid) ru ON ru.match_guid = p.match_guid ";
    }
    if (hasStats) {
        sql += "LEFT JOIN MatchPlayerStats mps ON mps.match_id = p.id AND mps.primary_id = ?1 ";
        sql += "LEFT JOIN MatchLocalStats mls ON mls.match_id = p.id ";
    }
    if (hasAnalytics) {
        sql += "LEFT JOIN MatchAnalytics ma ON ma.match_id = p.id ";
    }
    sql += ";";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return;
    }
    BindParams(stmt, params);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MatchRow row;
        row.matchId = sqlite3_column_int64(stmt, 0);
        row.timestampUnix = sqlite3_column_int64(stmt, 1);
        row.arena = SqlColumnText(stmt, 2);
        row.ourScore = sqlite3_column_int(stmt, 3);
        row.theirScore = sqlite3_column_int(stmt, 4);
        row.win = sqlite3_column_int(stmt, 5) != 0;
        row.matchGuid = SqlColumnText(stmt, 6);
        row.playlistId = sqlite3_column_type(stmt, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 7);
        row.gamemode = SqlColumnText(stmt, 8);
        row.playlist = DescribeMatchPlaylist(stmt, 8, 9, 7, row.ranked);
        row.myMmr = sqlite3_column_int(stmt, 10);
        row.mmrEstimated = sqlite3_column_int(stmt, 11) != 0;
        if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) {
            row.mmrDelta = sqlite3_column_int(stmt, 12);
        }
        outTotalCount = sqlite3_column_int(stmt, 13);
        const int myTeam = sqlite3_column_int(stmt, 14);
        row.ballchasingUrl = SqlColumnText(stmt, 15);
        row.hasBallchasing = !row.ballchasingUrl.empty();

        if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) row.myScore = sqlite3_column_int(stmt, 16);
        if (sqlite3_column_type(stmt, 17) != SQLITE_NULL) row.myGoals = sqlite3_column_int(stmt, 17);
        if (sqlite3_column_type(stmt, 18) != SQLITE_NULL) row.myAssists = sqlite3_column_int(stmt, 18);
        if (sqlite3_column_type(stmt, 19) != SQLITE_NULL) row.mySaves = sqlite3_column_int(stmt, 19);
        if (sqlite3_column_type(stmt, 20) != SQLITE_NULL) row.myShots = sqlite3_column_int(stmt, 20);
        row.mvp = sqlite3_column_int(stmt, 21) != 0;
        row.overtime = sqlite3_column_int(stmt, 22) != 0;

        if (sqlite3_column_type(stmt, 23) != SQLITE_NULL) {
            std::string text = SqlColumnText(stmt, 23);
            if (!text.empty()) {
                auto parsed = nlohmann::json::parse(text, nullptr, false);
                if (!parsed.is_discarded()) {
                    if (auto analytics = MatchAnalyticsFromJson(parsed)) {
                        row.endedEarly = analytics->EndedEarly();
                        row.scoreline = AnalyzeScoreline(*analytics, myTeam, row.win);
                        if (!row.overtime && row.scoreline && row.scoreline->overtime) {
                            row.overtime = true;
                        }
                    }
                }
            }
        }
        outRows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);

    if (outRows.empty() && offset > 0) {
        const std::string countSql =
            "SELECT COUNT(*) FROM MatchPlayers me "
            "JOIN Matches m ON m.id = me.match_id "
            "WHERE me.primary_id = ?1 AND COALESCE(m.result_pending, 0) = 0" +
            whereSql + ";";
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(db, countSql.c_str(), -1, &countStmt, nullptr) == SQLITE_OK) {
            BindParams(countStmt, filterParams);
            if (sqlite3_step(countStmt) == SQLITE_ROW) {
                outTotalCount = sqlite3_column_int(countStmt, 0);
            }
            sqlite3_finalize(countStmt);
        }
    }

    if (!outRows.empty()) {
        const char* tmSql =
            "SELECT name FROM MatchPlayers "
            "WHERE match_id = ?1 AND COALESCE(is_opponent, 0) = 0 AND primary_id != ?2 "
            "ORDER BY id ASC;";
        sqlite3_stmt* tmStmt = nullptr;
        if (sqlite3_prepare_v2(db, tmSql, -1, &tmStmt, nullptr) == SQLITE_OK) {
            for (auto& row : outRows) {
                sqlite3_reset(tmStmt);
                sqlite3_clear_bindings(tmStmt);
                sqlite3_bind_int64(tmStmt, 1, row.matchId);
                sqlite3_bind_text(tmStmt, 2, query.account.c_str(), -1, SQLITE_TRANSIENT);
                while (sqlite3_step(tmStmt) == SQLITE_ROW) {
                    std::string tmName = SqlColumnText(tmStmt, 0);
                    if (!tmName.empty()) row.teammates.push_back(std::move(tmName));
                }
            }
            sqlite3_finalize(tmStmt);
        }
    }
}

bool DatabaseManager::GetMatchDetail(int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    outDetail = {};
    if (matchId <= 0) return false;

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return false;

    const char* matchSql = R"(
        SELECT id, timestamp, CAST(strftime('%s', timestamp) AS INTEGER), arena,
               our_score, their_score, win, match_guid, playlist_id, gamemode, player_count
        FROM Matches
        WHERE id = ?1;
    )";
    sqlite3_stmt* matchStmt = nullptr;
    if (sqlite3_prepare_v2(db, matchSql, -1, &matchStmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(matchStmt, 1, matchId);

    std::string rawTimestamp;
    if (sqlite3_step(matchStmt) != SQLITE_ROW) {
        sqlite3_finalize(matchStmt);
        return false;
    }

    outDetail.found = true;
    outDetail.matchId = sqlite3_column_int64(matchStmt, 0);
    rawTimestamp = SqlColumnText(matchStmt, 1);
    outDetail.timestampUnix = sqlite3_column_int64(matchStmt, 2);
    outDetail.arena = SqlColumnText(matchStmt, 3);
    outDetail.ourScore = sqlite3_column_int(matchStmt, 4);
    outDetail.theirScore = sqlite3_column_int(matchStmt, 5);
    outDetail.win = sqlite3_column_int(matchStmt, 6) != 0;
    outDetail.matchGuid = SqlColumnText(matchStmt, 7);
    outDetail.playlistId = sqlite3_column_type(matchStmt, 8) == SQLITE_NULL ? -1 : sqlite3_column_int(matchStmt, 8);
    outDetail.gamemode = SqlColumnText(matchStmt, 9);
    outDetail.playlist = DescribeMatchPlaylist(matchStmt, 9, 10, 8, outDetail.ranked);
    sqlite3_finalize(matchStmt);
    if (!outDetail.matchGuid.empty() && SqliteTableExists(db, "ReplayUploads")) {
        const char* bcSql =
            "SELECT ballchasing_id, ballchasing_url FROM ReplayUploads WHERE match_guid = ?1 AND ballchasing_url IS NOT NULL AND ballchasing_url != '' ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt* bcStmt = nullptr;
        if (sqlite3_prepare_v2(db, bcSql, -1, &bcStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(bcStmt, 1, outDetail.matchGuid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(bcStmt) == SQLITE_ROW) {
                outDetail.ballchasingId = SqlColumnText(bcStmt, 0);
                outDetail.ballchasingUrl = SqlColumnText(bcStmt, 1);
            }
            sqlite3_finalize(bcStmt);
        }
    }

    if (SqliteTableExists(db, "MatchEvents")) {
        const char* eventsSql = R"(
            SELECT event_type, observed_at_ms, match_clock_seconds, actor_id, actor_name,
                   secondary_id, secondary_name, team, value, ball_speed, source, event_key
            FROM MatchEvents WHERE match_id = ? ORDER BY observed_at_ms, id LIMIT 512;
        )";
        sqlite3_stmt* eventsStmt = nullptr;
        if (sqlite3_prepare_v2(db, eventsSql, -1, &eventsStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(eventsStmt, 1, matchId);
            while (sqlite3_step(eventsStmt) == SQLITE_ROW) {
                MatchJournalEvent e;
                e.type = SqlColumnText(eventsStmt, 0);
                e.observedAtUnixMs = sqlite3_column_int64(eventsStmt, 1);
                if (sqlite3_column_type(eventsStmt, 2) != SQLITE_NULL)
                    e.matchClockSeconds = sqlite3_column_double(eventsStmt, 2);
                e.actorId = SqlColumnText(eventsStmt, 3);
                e.actorName = SqlColumnText(eventsStmt, 4);
                e.secondaryId = SqlColumnText(eventsStmt, 5);
                e.secondaryName = SqlColumnText(eventsStmt, 6);
                if (sqlite3_column_type(eventsStmt, 7) != SQLITE_NULL) e.team = sqlite3_column_int(eventsStmt, 7);
                if (sqlite3_column_type(eventsStmt, 8) != SQLITE_NULL) e.value = sqlite3_column_double(eventsStmt, 8);
                if (sqlite3_column_type(eventsStmt, 9) != SQLITE_NULL) e.ballSpeed = sqlite3_column_double(eventsStmt, 9);
                e.source = SqlColumnText(eventsStmt, 10);
                e.eventKey = SqlColumnText(eventsStmt, 11);
                outDetail.events.push_back(std::move(e));
            }
            sqlite3_finalize(eventsStmt);
        }
    }
    const bool hasStatsTables = HasStatsTables(db);
    if (hasStatsTables) {
        const char* localSql =
            "SELECT duration_seconds, overtime_seconds, boost_collected, demoed, crossbars, "
            "hardest_crossbar, max_ball_speed, own_goals FROM MatchLocalStats WHERE match_id = ?1;";
        sqlite3_stmt* localStmt = nullptr;
        if (sqlite3_prepare_v2(db, localSql, -1, &localStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(localStmt, 1, matchId);
            if (sqlite3_step(localStmt) == SQLITE_ROW) {
                outDetail.hasLocalStats = true;
                if (sqlite3_column_type(localStmt, 0) != SQLITE_NULL) {
                    outDetail.durationSeconds = static_cast<float>(sqlite3_column_double(localStmt, 0));
                }
                if (sqlite3_column_type(localStmt, 1) != SQLITE_NULL) {
                    outDetail.overtimeSeconds = static_cast<float>(sqlite3_column_double(localStmt, 1));
                }
                MatchLocalStatsRow lstats;
                lstats.boostCollected = sqlite3_column_int(localStmt, 2);
                lstats.demoed = sqlite3_column_int(localStmt, 3);
                lstats.crossbars = sqlite3_column_int(localStmt, 4);
                if (sqlite3_column_type(localStmt, 5) != SQLITE_NULL) {
                    lstats.hardestCrossbar = static_cast<float>(sqlite3_column_double(localStmt, 5));
                }
                if (sqlite3_column_type(localStmt, 6) != SQLITE_NULL) {
                    lstats.maxBallSpeed = static_cast<float>(sqlite3_column_double(localStmt, 6));
                }
                lstats.ownGoals = sqlite3_column_int(localStmt, 7);
                outDetail.localStats = lstats;
            }
            sqlite3_finalize(localStmt);
        }
    }

    if (SqliteTableExists(db, "MatchAnalytics")) {
        const char* analyticsSql = "SELECT data FROM MatchAnalytics WHERE match_id = ?1;";
        sqlite3_stmt* analyticsStmt = nullptr;
        if (sqlite3_prepare_v2(db, analyticsSql, -1, &analyticsStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(analyticsStmt, 1, matchId);
            if (sqlite3_step(analyticsStmt) == SQLITE_ROW) {
                std::string text = SqlColumnText(analyticsStmt, 0);
                auto parsed = nlohmann::json::parse(text, nullptr, false);
                if (!parsed.is_discarded()) {
                    outDetail.analytics = MatchAnalyticsFromJson(parsed);
                }
            }
            sqlite3_finalize(analyticsStmt);
        }
    }

    const std::string tierKey = TierPlaylistKey(outDetail.playlistId, outDetail.gamemode);
    std::string playersSql;
    if (hasStatsTables) {
        playersSql = R"(
            SELECT mp.primary_id, mp.name, COALESCE(mp.team, 0), COALESCE(mp.mmr, 0),
                   COALESCE(mp.mmr_estimated, 0), COALESCE(mp.is_opponent, 0),
                   mps.match_id, mps.score, mps.goals, mps.assists, mps.saves, mps.shots, mps.demos,
                   mps.touches, mps.car_touches, mps.max_goal_speed, mps.fastest_goal_time
            FROM MatchPlayers mp
            LEFT JOIN MatchPlayerStats mps ON mps.match_id = mp.match_id AND mps.primary_id = mp.primary_id
            WHERE mp.match_id = ?1
            ORDER BY COALESCE(mp.is_opponent, 0) ASC, mp.id ASC;
        )";
    } else {
        playersSql = R"(
            SELECT mp.primary_id, mp.name, COALESCE(mp.team, 0), COALESCE(mp.mmr, 0),
                   COALESCE(mp.mmr_estimated, 0), COALESCE(mp.is_opponent, 0)
            FROM MatchPlayers mp
            WHERE mp.match_id = ?1
            ORDER BY COALESCE(mp.is_opponent, 0) ASC, mp.id ASC;
        )";
    }

    sqlite3_stmt* playerStmt = nullptr;
    if (sqlite3_prepare_v2(db, playersSql.c_str(), -1, &playerStmt, nullptr) != SQLITE_OK) {
        return true;
    }
    sqlite3_bind_int64(playerStmt, 1, matchId);

    std::vector<MatchDetailPlayer> allPlayers;
    while (sqlite3_step(playerStmt) == SQLITE_ROW) {
        MatchDetailPlayer p;
        p.primaryId = SqlColumnText(playerStmt, 0);
        p.name = SqlColumnText(playerStmt, 1);
        p.platform = PlatformFromPrimaryId(p.primaryId);
        p.team = sqlite3_column_int(playerStmt, 2);
        p.mmr = sqlite3_column_int(playerStmt, 3);
        p.mmrEstimated = sqlite3_column_int(playerStmt, 4) != 0;
        p.isOpponent = sqlite3_column_int(playerStmt, 5) != 0;
        p.tier = MMRFetcher::GetRankTierForPlaylistMmr(tierKey, p.mmr);
        if (hasStatsTables && sqlite3_column_type(playerStmt, 6) != SQLITE_NULL) {
            p.hasStats = true;
            outDetail.hasPlayerStats = true;
            if (sqlite3_column_type(playerStmt, 7) != SQLITE_NULL) p.score = sqlite3_column_int(playerStmt, 7);
            if (sqlite3_column_type(playerStmt, 8) != SQLITE_NULL) p.goals = sqlite3_column_int(playerStmt, 8);
            if (sqlite3_column_type(playerStmt, 9) != SQLITE_NULL) p.assists = sqlite3_column_int(playerStmt, 9);
            if (sqlite3_column_type(playerStmt, 10) != SQLITE_NULL) p.saves = sqlite3_column_int(playerStmt, 10);
            if (sqlite3_column_type(playerStmt, 11) != SQLITE_NULL) p.shots = sqlite3_column_int(playerStmt, 11);
            if (sqlite3_column_type(playerStmt, 12) != SQLITE_NULL) p.demos = sqlite3_column_int(playerStmt, 12);
            if (sqlite3_column_type(playerStmt, 13) != SQLITE_NULL) p.touches = sqlite3_column_int(playerStmt, 13);
            if (sqlite3_column_type(playerStmt, 14) != SQLITE_NULL) p.carTouches = sqlite3_column_int(playerStmt, 14);
            if (sqlite3_column_type(playerStmt, 15) != SQLITE_NULL)
                p.maxGoalSpeed = static_cast<float>(sqlite3_column_double(playerStmt, 15));
            if (sqlite3_column_type(playerStmt, 16) != SQLITE_NULL)
                p.fastestGoalTime = static_cast<float>(sqlite3_column_double(playerStmt, 16));
        }
        allPlayers.push_back(std::move(p));
    }
    sqlite3_finalize(playerStmt);

    auto hasPlayerInMatch = [&](const std::string& pid) {
        if (pid.empty()) return false;
        return std::any_of(allPlayers.begin(), allPlayers.end(), [&](const MatchDetailPlayer& p) {
            return p.primaryId == pid;
        });
    };

    std::string resolvedAccount;
    if (hasPlayerInMatch(accountPrimaryId)) {
        resolvedAccount = accountPrimaryId;
    } else if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        const std::string statePid = liveSnap ? liveSnap->myPrimaryId : std::string{};
        if (hasPlayerInMatch(statePid)) {
            resolvedAccount = statePid;
        }
    }
    if (resolvedAccount.empty()) {
        const std::string cfgPid = Config::Read().last_primary_id;
        if (hasPlayerInMatch(cfgPid)) {
            resolvedAccount = cfgPid;
        }
    }
    if (resolvedAccount.empty()) {
        const char* inferSql = R"(
            SELECT mp.primary_id
            FROM MatchPlayers mp
            WHERE mp.match_id = ?1 AND COALESCE(mp.is_opponent, 0) = 0 AND mp.primary_id != ''
            ORDER BY (SELECT COUNT(*) FROM MatchPlayers all_mp WHERE all_mp.primary_id = mp.primary_id) DESC,
                     mp.id ASC
            LIMIT 1;
        )";
        sqlite3_stmt* inferStmt = nullptr;
        if (sqlite3_prepare_v2(db, inferSql, -1, &inferStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(inferStmt, 1, matchId);
            if (sqlite3_step(inferStmt) == SQLITE_ROW) {
                resolvedAccount = SqlColumnText(inferStmt, 0);
            }
            sqlite3_finalize(inferStmt);
        }
    }
    outDetail.accountPrimaryId = resolvedAccount;

    for (auto& p : allPlayers) {
        p.isMe = (!resolvedAccount.empty() && p.primaryId == resolvedAccount);
        if (p.isMe) {
            outDetail.mmrEstimated = p.mmrEstimated;
            if (p.mmr > 0) {
                outDetail.mmrAfter = p.mmr;
            }
        } else if (!resolvedAccount.empty() && !p.primaryId.empty() && p.primaryId.rfind("Unknown|", 0) != 0) {
            if (auto records = QueryEncounterRecordsLocked(db, resolvedAccount, p.primaryId, matchId)) {
                if (!records->empty()) {
                    p.encounter = std::move(records->front());
                } else {
                    p.encounter = PersonRecord{.primaryId = p.primaryId, .name = p.name};
                }
            }
        }

        if (p.isOpponent) {
            outDetail.theirTeam.push_back(std::move(p));
        } else {
            outDetail.ourTeam.push_back(std::move(p));
        }
    }

    if (!resolvedAccount.empty()) {
        const std::string partitionKey = !outDetail.gamemode.empty()
                                             ? outDetail.gamemode
                                             : (outDetail.playlistId >= 0 ? std::to_string(outDetail.playlistId) : "");
        const char* prevSql = R"(
            SELECT me.mmr
            FROM MatchPlayers me
            JOIN Matches m ON m.id = me.match_id
            WHERE me.primary_id = ?1
              AND COALESCE(m.result_pending, 0) = 0
              AND COALESCE(NULLIF(m.gamemode, ''), CAST(m.playlist_id AS TEXT), '') = ?2
              AND me.mmr > 0
              AND (m.timestamp < ?3 OR (m.timestamp = ?3 AND m.id < ?4))
            ORDER BY m.timestamp DESC, m.id DESC
            LIMIT 1;
        )";
        sqlite3_stmt* prevStmt = nullptr;
        if (sqlite3_prepare_v2(db, prevSql, -1, &prevStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(prevStmt, 1, resolvedAccount.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(prevStmt, 2, partitionKey.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(prevStmt, 3, rawTimestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(prevStmt, 4, matchId);
            if (sqlite3_step(prevStmt) == SQLITE_ROW) {
                const int prevMmr = sqlite3_column_int(prevStmt, 0);
                if (prevMmr > 0) {
                    outDetail.mmrBefore = prevMmr;
                    if (outDetail.mmrAfter.has_value()) {
                        outDetail.mmrDelta = *outDetail.mmrAfter - prevMmr;
                    }
                }
            }
            sqlite3_finalize(prevStmt);
        }
    }

    return true;
}

bool DatabaseManager::GetMatchDetailByGuid(const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    outDetail = {};
    if (matchGuid.empty()) return false;

    int64_t matchId = 0;
    {
        auto read = AcquireRead();
        sqlite3* db = read.db;
        if (!db) return false;
        const char* sql = "SELECT id FROM Matches WHERE match_guid = ?1 ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, matchGuid.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                matchId = sqlite3_column_int64(stmt, 0);
            }
            sqlite3_finalize(stmt);
        }
    }
    if (matchId <= 0) return false;
    return GetMatchDetail(matchId, outDetail, accountPrimaryId);
}

std::vector<AnalyticsMatchRow> DatabaseManager::GetAnalyticsMatches(const std::string& primaryId, int limit) {
    std::vector<AnalyticsMatchRow> rows;
    if (primaryId.empty()) return rows;
    if (limit <= 0) limit = 100;

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db) return rows;

    const bool hasStats = HasStatsTables(db);
    const bool hasAnalytics = SqliteTableExists(db, "MatchAnalytics");

    std::string sql =
        "SELECT m.id, CAST(strftime('%s', m.timestamp) AS INTEGER), "
        "       m.arena, m.our_score, m.their_score, m.win, "
        "       m.playlist_id, m.gamemode, m.player_count, "
        "       COALESCE(me.team, -1), ";
    if (hasStats) {
        sql +=
            "mps.match_id, mps.score, mps.goals, mps.assists, mps.saves, mps.shots, mps.demos, mps.touches, "
            "mls.match_id, mls.boost_collected, mls.demoed, mls.crossbars, mls.hardest_crossbar, mls.max_ball_speed, mls.own_goals, "
            "mls.duration_seconds, mls.overtime_seconds, ";
    } else {
        sql +=
            "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "
            "NULL, NULL, NULL, NULL, NULL, NULL, NULL, "
            "NULL, NULL, ";
    }
    if (hasAnalytics) {
        sql += "ma.data ";
    } else {
        sql += "NULL ";
    }
    sql +=
        "FROM Matches m "
        "JOIN MatchPlayers me ON me.match_id = m.id AND me.primary_id = ?1 ";
    if (hasStats) {
        sql += "LEFT JOIN MatchPlayerStats mps ON mps.match_id = m.id AND mps.primary_id = ?1 ";
        sql += "LEFT JOIN MatchLocalStats mls ON mls.match_id = m.id ";
    }
    if (hasAnalytics) {
        sql += "LEFT JOIN MatchAnalytics ma ON ma.match_id = m.id ";
    }
    sql +=
        "WHERE COALESCE(m.result_pending, 0) = 0 "
        "ORDER BY m.timestamp DESC, m.id DESC "
        "LIMIT ?2;";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return rows;
    }

    sqlite3_bind_text(stmt, 1, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        AnalyticsMatchRow row;
        row.matchId = sqlite3_column_int64(stmt, 0);
        row.timestampUnix = sqlite3_column_int64(stmt, 1);
        row.arena = SqlColumnText(stmt, 2);
        row.ourScore = sqlite3_column_int(stmt, 3);
        row.theirScore = sqlite3_column_int(stmt, 4);
        row.win = sqlite3_column_int(stmt, 5) != 0;
        row.playlistId = sqlite3_column_type(stmt, 6) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 6);
        row.playlist = DescribeMatchPlaylist(stmt, 7, 8, 6, row.ranked);
        row.myTeam = sqlite3_column_int(stmt, 9);

        if (sqlite3_column_type(stmt, 10) != SQLITE_NULL) {
            if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) row.score = sqlite3_column_int(stmt, 11);
            if (sqlite3_column_type(stmt, 12) != SQLITE_NULL) row.goals = sqlite3_column_int(stmt, 12);
            if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) row.assists = sqlite3_column_int(stmt, 13);
            if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) row.saves = sqlite3_column_int(stmt, 14);
            if (sqlite3_column_type(stmt, 15) != SQLITE_NULL) row.shots = sqlite3_column_int(stmt, 15);
            if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) row.demos = sqlite3_column_int(stmt, 16);
            if (sqlite3_column_type(stmt, 17) != SQLITE_NULL) row.touches = sqlite3_column_int(stmt, 17);
        }

        if (sqlite3_column_type(stmt, 18) != SQLITE_NULL) {
            MatchLocalStatsRow lstats;
            lstats.boostCollected = sqlite3_column_int(stmt, 19);
            lstats.demoed = sqlite3_column_int(stmt, 20);
            lstats.crossbars = sqlite3_column_int(stmt, 21);
            if (sqlite3_column_type(stmt, 22) != SQLITE_NULL) {
                lstats.hardestCrossbar = static_cast<float>(sqlite3_column_double(stmt, 22));
            }
            if (sqlite3_column_type(stmt, 23) != SQLITE_NULL) {
                lstats.maxBallSpeed = static_cast<float>(sqlite3_column_double(stmt, 23));
            }
            lstats.ownGoals = sqlite3_column_int(stmt, 24);
            row.localStats = lstats;

            if (sqlite3_column_type(stmt, 25) != SQLITE_NULL) {
                row.durationSeconds = static_cast<float>(sqlite3_column_double(stmt, 25));
            }
            if (sqlite3_column_type(stmt, 26) != SQLITE_NULL) {
                row.overtimeSeconds = static_cast<float>(sqlite3_column_double(stmt, 26));
            }
        }

        if (sqlite3_column_type(stmt, 27) != SQLITE_NULL) {
            std::string text = SqlColumnText(stmt, 27);
            if (!text.empty()) {
                auto parsed = nlohmann::json::parse(text, nullptr, false);
                if (!parsed.is_discarded()) {
                    row.analytics = MatchAnalyticsFromJson(parsed);
                }
            }
        }

        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}
