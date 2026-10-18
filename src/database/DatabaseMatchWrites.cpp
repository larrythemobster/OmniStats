#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/game/MatchAnalytics.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <shared_mutex>
#include <utility>

using namespace DatabaseDetail;

bool DatabaseManager::UpsertMatchStatsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    if (!HasStatsTablesLocked()) {
        return true;
    }

    const char* sqlPlayerStats = R"(
        INSERT INTO MatchPlayerStats (
            match_id, primary_id, score, goals, assists, saves, shots, demos,
            touches, car_touches, max_goal_speed, fastest_goal_time
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        ON CONFLICT(match_id, primary_id) DO UPDATE SET
            score = excluded.score,
            goals = excluded.goals,
            assists = excluded.assists,
            saves = excluded.saves,
            shots = excluded.shots,
            demos = excluded.demos,
            touches = excluded.touches,
            car_touches = excluded.car_touches,
            max_goal_speed = excluded.max_goal_speed,
            fastest_goal_time = excluded.fastest_goal_time;
    )";

    sqlite3_stmt* stmtPlayerStats = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlPlayerStats, -1, &stmtPlayerStats, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare MatchPlayerStats upsert.\n";
        return false;
    }

    bool ok = true;
    for (const auto& [id, p] : snapshot.roster) {
        const std::string& primaryId = p.primaryId.empty() ? id : p.primaryId;
        if (primaryId.empty()) continue;

        sqlite3_bind_int64(stmtPlayerStats, 1, matchId);
        sqlite3_bind_text(stmtPlayerStats, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
        if (p.score >= 0) {
            sqlite3_bind_int(stmtPlayerStats, 3, p.score);
        } else {
            sqlite3_bind_null(stmtPlayerStats, 3);
        }
        sqlite3_bind_int(stmtPlayerStats, 4, p.goals);
        sqlite3_bind_int(stmtPlayerStats, 5, p.assists);
        sqlite3_bind_int(stmtPlayerStats, 6, p.saves);
        sqlite3_bind_int(stmtPlayerStats, 7, p.shots);
        sqlite3_bind_int(stmtPlayerStats, 8, p.demos);
        if (p.touches.has_value()) {
            sqlite3_bind_int(stmtPlayerStats, 9, *p.touches);
        } else {
            sqlite3_bind_null(stmtPlayerStats, 9);
        }
        if (p.carTouches.has_value()) {
            sqlite3_bind_int(stmtPlayerStats, 10, *p.carTouches);
        } else {
            sqlite3_bind_null(stmtPlayerStats, 10);
        }
        if (p.maxGoalSpeed > 0.0f) {
            sqlite3_bind_double(stmtPlayerStats, 11, static_cast<double>(p.maxGoalSpeed));
        } else {
            sqlite3_bind_null(stmtPlayerStats, 11);
        }
        if (p.fastestGoalTime > 0.0f) {
            sqlite3_bind_double(stmtPlayerStats, 12, static_cast<double>(p.fastestGoalTime));
        } else {
            sqlite3_bind_null(stmtPlayerStats, 12);
        }

        if (sqlite3_step(stmtPlayerStats) != SQLITE_DONE) {
            std::cerr << "[Database] Failed to upsert MatchPlayerStats row.\n";
            ok = false;
            break;
        }
        sqlite3_reset(stmtPlayerStats);
        sqlite3_clear_bindings(stmtPlayerStats);
    }
    sqlite3_finalize(stmtPlayerStats);
    if (!ok) return false;

    const char* sqlLocalStats = R"(
        INSERT INTO MatchLocalStats (
            match_id, boost_collected, demoed, crossbars, hardest_crossbar,
            max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version
        ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1)
        ON CONFLICT(match_id) DO UPDATE SET
            boost_collected = excluded.boost_collected,
            demoed = excluded.demoed,
            crossbars = excluded.crossbars,
            hardest_crossbar = excluded.hardest_crossbar,
            max_ball_speed = excluded.max_ball_speed,
            own_goals = excluded.own_goals,
            duration_seconds = excluded.duration_seconds,
            overtime_seconds = excluded.overtime_seconds,
            stats_version = excluded.stats_version;
    )";

    sqlite3_stmt* stmtLocalStats = nullptr;
    if (sqlite3_prepare_v2(m_db, sqlLocalStats, -1, &stmtLocalStats, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare MatchLocalStats upsert.\n";
        return false;
    }

    const auto& local = snapshot.localStats;
    sqlite3_bind_int64(stmtLocalStats, 1, matchId);
    sqlite3_bind_int(stmtLocalStats, 2, local.boostPickedUpSelf);
    sqlite3_bind_int(stmtLocalStats, 3, local.demoedSelf);
    sqlite3_bind_int(stmtLocalStats, 4, local.crossbarsSelf);
    if (local.maxImpactForceSelf > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 5, static_cast<double>(local.maxImpactForceSelf));
    } else {
        sqlite3_bind_null(stmtLocalStats, 5);
    }
    if (local.maxBallSpeedSelf > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 6, static_cast<double>(local.maxBallSpeedSelf));
    } else {
        sqlite3_bind_null(stmtLocalStats, 6);
    }
    sqlite3_bind_int(stmtLocalStats, 7, local.ownGoalsSelf);
    if (snapshot.durationSeconds > 0.0f) {
        sqlite3_bind_double(stmtLocalStats, 8, static_cast<double>(snapshot.durationSeconds));
        sqlite3_bind_double(stmtLocalStats, 9, static_cast<double>(std::max(0.0f, snapshot.overtimeSeconds)));
    } else {
        sqlite3_bind_null(stmtLocalStats, 8);
        sqlite3_bind_null(stmtLocalStats, 9);
    }

    ok = (sqlite3_step(stmtLocalStats) == SQLITE_DONE);
    sqlite3_finalize(stmtLocalStats);
    if (!ok) {
        std::cerr << "[Database] Failed to upsert MatchLocalStats row.\n";
    }
    return ok;
}

bool DatabaseManager::InsertMatchEventsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    if (!SqliteTableExists(m_db, "MatchEvents")) return snapshot.events.empty();
    const char* sql = R"(
        INSERT OR IGNORE INTO MatchEvents
          (match_id, match_guid, event_type, observed_at_ms, match_clock_seconds,
           actor_id, actor_name, secondary_id, secondary_name, team,
           value, ball_speed, source, event_key)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    bool ok = true;
    size_t count = 0;
    for (const auto& event : snapshot.events) {
        if (++count > 512) break;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_int64(stmt, 1, matchId);
        sqlite3_bind_text(stmt, 2, snapshot.matchGuid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, event.type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, event.observedAtUnixMs);
        if (event.matchClockSeconds) sqlite3_bind_double(stmt, 5, *event.matchClockSeconds);
        sqlite3_bind_text(stmt, 6, event.actorId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, event.actorName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, event.secondaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, event.secondaryName.c_str(), -1, SQLITE_TRANSIENT);
        if (event.team) sqlite3_bind_int(stmt, 10, *event.team);
        if (event.value) sqlite3_bind_double(stmt, 11, *event.value);
        if (event.ballSpeed) sqlite3_bind_double(stmt, 12, *event.ballSpeed);
        sqlite3_bind_text(stmt, 13, event.source.c_str(), -1, SQLITE_TRANSIENT);
        if (!event.eventKey.empty()) sqlite3_bind_text(stmt, 14, event.eventKey.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            ok = false;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

bool DatabaseManager::UpsertMatchAnalyticsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    if (!snapshot.analytics.has_value() || snapshot.analytics->Empty()) {
        return true;
    }
    if (!SqliteTableExists(m_db, "MatchAnalytics")) {
        return true;
    }

    const char* sql = R"(
        INSERT OR REPLACE INTO MatchAnalytics (match_id, format_version, data)
        VALUES (?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare MatchAnalytics upsert.\n";
        return false;
    }

    const std::string text = MatchAnalyticsToJson(*snapshot.analytics).dump();
    sqlite3_bind_int64(stmt, 1, matchId);
    sqlite3_bind_int(stmt, 2, snapshot.analytics->formatVersion);
    sqlite3_bind_text(stmt, 3, text.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!ok) {
        std::cerr << "[Database] Failed to upsert MatchAnalytics row.\n";
    }
    return ok;
}

void DatabaseManager::SaveMatch(const MatchSaveSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return;

    if (snapshot.arenaName.empty()) return;

    sqlite3_int64 pendingMatchId = 0;
    if (!snapshot.matchGuid.empty()) {
        sqlite3_stmt* duplicateCheck = nullptr;
        const char* duplicateSql =
            "SELECT id, COALESCE(result_pending, 0) FROM Matches "
            "WHERE match_guid = ? ORDER BY id DESC LIMIT 1;";
        bool alreadySaved = false;
        if (sqlite3_prepare_v2(
                m_db,
                duplicateSql,
                -1,
                &duplicateCheck,
                nullptr) == SQLITE_OK) {
            sqlite3_bind_text(
                duplicateCheck,
                1,
                snapshot.matchGuid.c_str(),
                -1,
                SQLITE_TRANSIENT);
            if (sqlite3_step(duplicateCheck) == SQLITE_ROW) {
                alreadySaved = true;
                if (sqlite3_column_int(duplicateCheck, 1) != 0) {
                    pendingMatchId = sqlite3_column_int64(duplicateCheck, 0);
                }
            }
            sqlite3_finalize(duplicateCheck);
        } else if (duplicateCheck) {
            sqlite3_finalize(duplicateCheck);
        }
        if (alreadySaved &&
            (pendingMatchId == 0 || snapshot.resultPending)) {
            std::cout
                << "[Database] Skipping duplicate match GUID.\n";
            return;
        }
    }

    if (pendingMatchId != 0) {
        ResolvePendingMatchLocked(pendingMatchId, snapshot);
        return;
    }

    if (!snapshot.validResult) {
        std::cout << "[Database] Skipping void match"
                  << (snapshot.voidReason.empty() ? "" : ": " + snapshot.voidReason)
                  << "\n";
        return;
    }

    if (snapshot.myTeam != 0 && snapshot.myTeam != 1) return;
    if (snapshot.winnerTeam != 0 && snapshot.winnerTeam != 1) return;

    bool win = (snapshot.winnerTeam == snapshot.myTeam);

    int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
    int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

    const bool hasPlaylistId =
        PlaylistMetadata::HasAuthoritativeId(snapshot.playlistId);
    const int playerCount = hasPlaylistId
                                ? 0
                                : (snapshot.legacyPlayerCount > 0
                                       ? snapshot.legacyPlayerCount
                                       : static_cast<int>(snapshot.roster.size()));
    const std::string arenaKey = !snapshot.arenaAsset.empty()
                                     ? snapshot.arenaAsset
                                     : snapshot.arenaName;

    std::string gamemode;
    std::string mmrKey;
    if (hasPlaylistId) {
        if (PlaylistMetadata::IsKnown(snapshot.playlistId)) {
            gamemode = PlaylistMetadata::StorageMode(snapshot.playlistId);
            mmrKey = PlaylistMetadata::MmrKey(snapshot.playlistId);
        } else {
            // Preserve the fact that the ID was unknown rather than inventing
            // a ranked mode from UI state, arena, or roster size.
            gamemode = "unknown";
        }
    } else {
        gamemode = snapshot.gamemode;
        if (gamemode.empty()) {
            gamemode = GamemodeUtils::InferFromSnapshot(
                playerCount,
                static_cast<int>(snapshot.roster.size()),
                snapshot.rosterMmrCategory,
                arenaKey);
        }
        mmrKey = gamemode;
    }

    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to begin transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        return;
    }

    std::string sqlMatches =
        "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, timestamp, result_pending) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, COALESCE(strftime('%Y-%m-%d %H:%M:%f', NULLIF(?, 0) / 1000.0, 'unixepoch'), CURRENT_TIMESTAMP), ?);";
    sqlite3_stmt* stmtMatches;

    if (sqlite3_prepare_v2(m_db, sqlMatches.c_str(), -1, &stmtMatches, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare insert statement\n";
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    sqlite3_bind_text(stmtMatches, 1, snapshot.arenaName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMatches, 2, ourScore);
    sqlite3_bind_int(stmtMatches, 3, theirScore);
    sqlite3_bind_int(stmtMatches, 4, win ? 1 : 0);
    sqlite3_bind_text(stmtMatches, 5, snapshot.matchGuid.c_str(), -1, SQLITE_TRANSIENT);
    if (hasPlaylistId) {
        sqlite3_bind_int(stmtMatches, 6, snapshot.playlistId);
    } else {
        sqlite3_bind_null(stmtMatches, 6);
    }
    sqlite3_bind_text(stmtMatches, 7, gamemode.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmtMatches, 8, playerCount);
    sqlite3_bind_int64(
        stmtMatches, 9, snapshot.endedAtUnixMs);
    sqlite3_bind_int(stmtMatches, 10, snapshot.resultPending ? 1 : 0);

    bool ok = true;
    if (sqlite3_step(stmtMatches) != SQLITE_DONE) {
        std::cerr << "Failed to insert match record\n";
        ok = false;
    }
    sqlite3_finalize(stmtMatches);

    if (!ok) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    // Get the ID of the match we just inserted
    sqlite3_int64 matchId = sqlite3_last_insert_rowid(m_db);

    // Save players
    std::string sqlPlayers = "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* stmtPlayers;

    if (sqlite3_prepare_v2(m_db, sqlPlayers.c_str(), -1, &stmtPlayers, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare player insert statement\n";
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    auto insertPlayerRow = [&](const PlayerData& p) -> bool {
        const bool isOpponent = (p.team != snapshot.myTeam);
        sqlite3_bind_int64(stmtPlayers, 1, matchId);
        sqlite3_bind_text(stmtPlayers, 2, p.primaryId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmtPlayers, 3, p.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmtPlayers, 4, p.team);
        int mmrVal = p.mmr;
        if (!mmrKey.empty() && p.playlists.count(mmrKey)) {
            mmrVal = p.playlists.at(mmrKey);
        }
        sqlite3_bind_int(stmtPlayers, 5, mmrVal);
        const bool mmrEstimated =
            snapshot.localMmrNeedsReconciliation &&
            p.primaryId == snapshot.myPrimaryId;
        sqlite3_bind_int(stmtPlayers, 6, mmrEstimated ? 1 : 0);
        sqlite3_bind_int(stmtPlayers, 7, isOpponent ? 1 : 0);

        if (sqlite3_step(stmtPlayers) != SQLITE_DONE) {
            std::cerr << "Failed to insert player record\n";
            return false;
        }
        sqlite3_reset(stmtPlayers);
        sqlite3_clear_bindings(stmtPlayers);
        return true;
    };

    if (!snapshot.myPrimaryId.empty()) {
        const auto selfIt = snapshot.roster.find(snapshot.myPrimaryId);
        if (selfIt != snapshot.roster.end()) {
            ok = insertPlayerRow(selfIt->second);
        }
    }
    if (ok) {
        for (const auto& [id, p] : snapshot.roster) {
            if (!snapshot.myPrimaryId.empty() && id == snapshot.myPrimaryId) continue;
            if (!insertPlayerRow(p)) {
                ok = false;
                break;
            }
        }
    }
    sqlite3_finalize(stmtPlayers);

    if (!ok || !UpsertMatchStatsLocked(matchId, snapshot) ||
        !InsertMatchEventsLocked(matchId, snapshot) ||
        !UpsertMatchAnalyticsLocked(matchId, snapshot)) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        std::cerr << "Failed to commit transaction: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    std::cout << "Match saved to database. ID: " << matchId << " Gamemode: " << gamemode << "\n";
}

void DatabaseManager::ResolvePendingMatchLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot) {
    const bool validResult =
        snapshot.validResult &&
        (snapshot.myTeam == 0 || snapshot.myTeam == 1) &&
        (snapshot.winnerTeam == 0 || snapshot.winnerTeam == 1);

    if (!validResult) {
        sqlite3_stmt* stmt = nullptr;
        bool ok = sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) == SQLITE_OK;
        if (ok && HasStatsTablesLocked()) {
            for (const char* sql : {"DELETE FROM MatchEvents WHERE match_id = ?;",
                                    "DELETE FROM MatchPlayerStats WHERE match_id = ?;",
                                    "DELETE FROM MatchLocalStats WHERE match_id = ?;"}) {
                if (!ok) break;
                ok = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK;
                if (ok) {
                    sqlite3_bind_int64(stmt, 1, matchId);
                    ok = sqlite3_step(stmt) == SQLITE_DONE;
                }
                sqlite3_finalize(stmt);
                stmt = nullptr;
            }
        }
        if (ok && SqliteTableExists(m_db, "MatchAnalytics")) {
            ok = sqlite3_prepare_v2(m_db, "DELETE FROM MatchAnalytics WHERE match_id = ?;", -1, &stmt, nullptr) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_int64(stmt, 1, matchId);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        for (const char* sql : {"DELETE FROM MatchPlayers WHERE match_id = ?;",
                                "DELETE FROM Matches WHERE id = ?;"}) {
            if (!ok) break;
            ok = sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_int64(stmt, 1, matchId);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
            stmt = nullptr;
        }
        if (!ok || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            std::cerr << "[Database] Failed to remove voided pending match.\n";
            return;
        }
        std::cout << "[Database] Removed pending match voided on confirmation. ID: " << matchId
                  << (snapshot.voidReason.empty() ? "" : ", reason=" + snapshot.voidReason) << "\n";
        return;
    }

    const bool win = snapshot.winnerTeam == snapshot.myTeam;
    const int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
    const int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to begin pending match confirmation.\n";
        return;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE Matches SET win = ?, our_score = ?, their_score = ?, result_pending = 0 WHERE id = ?;";
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        std::cerr << "[Database] Failed to prepare pending match confirmation.\n";
        return;
    }
    sqlite3_bind_int(stmt, 1, win ? 1 : 0);
    sqlite3_bind_int(stmt, 2, ourScore);
    sqlite3_bind_int(stmt, 3, theirScore);
    sqlite3_bind_int64(stmt, 4, matchId);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && UpsertMatchStatsLocked(matchId, snapshot) &&
                    InsertMatchEventsLocked(matchId, snapshot) &&
                    UpsertMatchAnalyticsLocked(matchId, snapshot);
    sqlite3_finalize(stmt);
    if (!ok || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        std::cerr << "[Database] Failed to confirm pending match.\n";
        return;
    }
    std::cout << "Pending match confirmed in database. ID: " << matchId
              << " Result: " << (win ? "win" : "loss") << "\n";
}

bool DatabaseManager::UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated) {
    if (matchGuid.empty() || primaryId.empty() || mmr <= 0) return false;

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return false;

    const char* sql = R"(
        UPDATE MatchPlayers
        SET mmr = ?, mmr_estimated = ?
        WHERE primary_id = ?
          AND match_id = (
              SELECT id
              FROM Matches
              WHERE match_guid = ?
              ORDER BY id DESC
              LIMIT 1
          );
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Failed to prepare post-match MMR update.\n";
        return false;
    }

    sqlite3_bind_int(stmt, 1, mmr);
    sqlite3_bind_int(stmt, 2, estimated ? 1 : 0);
    sqlite3_bind_text(stmt, 3, primaryId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, matchGuid.c_str(), -1, SQLITE_TRANSIENT);

    const bool ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(m_db) > 0;
    sqlite3_finalize(stmt);
    return ok;
}

void DatabaseManager::AsyncUpdateMatchPlayerMmr(std::string matchGuid, std::string primaryId, int mmr, bool estimated) {
    if (matchGuid.empty() || primaryId.empty() || mmr <= 0) return;

    (void)EnqueueDbJob([this,
                        matchGuid = std::move(matchGuid),
                        primaryId = std::move(primaryId),
                        mmr,
                        estimated]() {
        if (!UpdateMatchPlayerMmr(matchGuid, primaryId, mmr, estimated)) {
            std::cout << "[Database] Post-match MMR update found no saved row for match GUID.\n";
            return;
        }

        ConfigData conf = Config::Read();
        std::vector<SessionMatchSummary> matches;
        GetRecentMatchHistory(
            primaryId, matches, conf.previous_games_limit);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(
                m_state->history.mutex);
            m_state->history.recentSavedMatches = std::move(matches);
            std::erase_if(
                m_state->history.pendingRecentMatches,
                [&](const SessionMatchSummary& summary) {
                    return summary.matchGuid == matchGuid;
                });
            m_state->history.recentSavedMatchesLoaded = true;
            m_state->history.version++;
        }
    },
                       DbJobPriority::Critical);
}
