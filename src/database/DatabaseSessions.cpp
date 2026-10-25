#include "DatabaseManager.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <shared_mutex>
#include <utility>

using namespace DatabaseDetail;

static std::string SerializeMmrChangeJson(const std::map<std::string, int>& changes) {
    nlohmann::json obj = nlohmann::json::object();
    for (const auto& [playlist, delta] : changes) {
        if (!playlist.empty()) obj[playlist] = delta;
    }
    return obj.dump();
}

static std::string SerializeTotalsJson(const SessionRecap& recap) {
    const auto& t = recap.totals;
    nlohmann::json gamemodesObj = nlohmann::json::object();
    for (const auto& [mode, gm] : recap.gamemodes) {
        if (!mode.empty()) {
            gamemodesObj[mode] = {{"wins", gm.wins}, {"losses", gm.losses}, {"total", gm.total}};
        }
    }
    nlohmann::json obj = {
        {"wins", t.wins},
        {"losses", t.losses},
        {"goals", t.goals},
        {"saves", t.saves},
        {"savesTotal", t.savesTotal},
        {"shots", t.shots},
        {"shotsTotal", t.shotsTotal},
        {"demos", t.demos},
        {"demosTotal", t.demosTotal},
        {"demoed", t.demoed},
        {"assists", t.assists},
        {"assistsTotal", t.assistsTotal},
        {"crossbars", t.crossbars},
        {"crossbarsTotal", t.crossbarsTotal},
        {"maxGoalSpeed", t.maxGoalSpeed},
        {"maxGoalSpeedSelf", t.maxGoalSpeedSelf},
        {"maxBallSpeed", t.maxBallSpeed},
        {"maxBallSpeedSelf", t.maxBallSpeedSelf},
        {"maxImpactForce", t.maxImpactForce},
        {"maxImpactForceSelf", t.maxImpactForceSelf},
        {"fastestGoalTime", t.fastestGoalTime},
        {"fastestGoalTimeSelf", t.fastestGoalTimeSelf},
        {"ownGoals", t.ownGoals},
        {"ownGoalsSelf", t.ownGoalsSelf},
        {"boostPickedUp", t.boostPickedUp},
        {"totalMmrChange", t.totalMmrChange},
        {"teamGoals", t.teamGoals},
        {"goalParticipations", t.goalParticipations},
        {"sessionGeneration", recap.sessionGeneration},
        {"gamemodes", std::move(gamemodesObj)},
    };
    return obj.dump();
}

static void ParseSessionRecapJson(const std::string& mmrJsonStr,
                                  const std::string& totalsJsonStr,
                                  SessionRecap& recap) {
    if (!totalsJsonStr.empty()) {
        const nlohmann::json obj = nlohmann::json::parse(totalsJsonStr, nullptr, false);
        if (obj.is_object()) {
            auto& t = recap.totals;
            t.wins = obj.value("wins", t.wins);
            t.losses = obj.value("losses", t.losses);
            t.goals = obj.value("goals", 0);
            t.saves = obj.value("saves", 0);
            t.savesTotal = obj.value("savesTotal", 0);
            t.shots = obj.value("shots", 0);
            t.shotsTotal = obj.value("shotsTotal", 0);
            t.demos = obj.value("demos", 0);
            t.demosTotal = obj.value("demosTotal", 0);
            t.demoed = obj.value("demoed", 0);
            t.assists = obj.value("assists", 0);
            t.assistsTotal = obj.value("assistsTotal", 0);
            t.crossbars = obj.value("crossbars", 0);
            t.crossbarsTotal = obj.value("crossbarsTotal", 0);
            t.maxGoalSpeed = obj.value("maxGoalSpeed", 0.0f);
            t.maxGoalSpeedSelf = obj.value("maxGoalSpeedSelf", 0.0f);
            t.maxBallSpeed = obj.value("maxBallSpeed", 0.0f);
            t.maxBallSpeedSelf = obj.value("maxBallSpeedSelf", 0.0f);
            t.maxImpactForce = obj.value("maxImpactForce", 0.0f);
            t.maxImpactForceSelf = obj.value("maxImpactForceSelf", 0.0f);
            t.fastestGoalTime = obj.value("fastestGoalTime", 0.0f);
            t.fastestGoalTimeSelf = obj.value("fastestGoalTimeSelf", 0.0f);
            t.ownGoals = obj.value("ownGoals", 0);
            t.ownGoalsSelf = obj.value("ownGoalsSelf", 0);
            t.boostPickedUp = obj.value("boostPickedUp", 0);
            t.teamGoals = obj.value("teamGoals", 0);
            t.goalParticipations = obj.value("goalParticipations", 0);
            recap.sessionGeneration = obj.value("sessionGeneration", uint64_t{0});
            if (obj.contains("gamemodes") && obj["gamemodes"].is_object()) {
                for (auto it = obj["gamemodes"].begin(); it != obj["gamemodes"].end(); ++it) {
                    if (it.value().is_object()) {
                        GamemodeStat gm;
                        gm.wins = it.value().value("wins", 0);
                        gm.losses = it.value().value("losses", 0);
                        gm.total = it.value().value("total", gm.wins + gm.losses);
                        recap.gamemodes[it.key()] = gm;
                    }
                }
            }
        }
    }
    if (!mmrJsonStr.empty()) {
        const nlohmann::json mmrObj = nlohmann::json::parse(mmrJsonStr, nullptr, false);
        if (mmrObj.is_object()) {
            for (auto it = mmrObj.begin(); it != mmrObj.end(); ++it) {
                if (it.value().is_number_integer()) {
                    recap.totals.mmrChangeByPlaylist[it.key()] = it.value().get<int>();
                }
            }
        }
    }
    recap.totals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(recap.totals.mmrChangeByPlaylist));
}

void DatabaseManager::LinkSessionMatchesLocked(sqlite3_int64 sessionId,
                                               const std::string& account,
                                               int64_t startedAt,
                                               int64_t endedAt,
                                               const std::vector<std::string>& matchGuids,
                                               int expectedGames,
                                               int64_t& outEarliestMatchTs,
                                               int64_t& outLatestMatchTs) {
    outEarliestMatchTs = 0;
    outLatestMatchTs = 0;
    if (!m_db || !HasSessionsTableLocked() || sessionId <= 0) return;

    if (!matchGuids.empty()) {
        sqlite3_stmt* guidStmt = nullptr;
        const char* guidSql =
            "UPDATE Matches SET session_id = ?1 WHERE match_guid = ?2 AND (session_id IS NULL OR session_id = ?1);";
        if (sqlite3_prepare_v2(m_db, guidSql, -1, &guidStmt, nullptr) == SQLITE_OK) {
            for (const auto& guid : matchGuids) {
                if (guid.empty()) continue;
                sqlite3_bind_int64(guidStmt, 1, sessionId);
                sqlite3_bind_text(guidStmt, 2, guid.c_str(), -1, SQLITE_TRANSIENT);
                (void)sqlite3_step(guidStmt);
                sqlite3_reset(guidStmt);
                sqlite3_clear_bindings(guidStmt);
            }
            sqlite3_finalize(guidStmt);
        }
    }

    if (!account.empty() && endedAt > 0) {
        const int64_t rangeStart = startedAt > 0 ? std::min(startedAt, endedAt)
                                                 : (endedAt - Insights::kSessionGapSeconds);
        const int64_t rangeEnd = std::max(startedAt, endedAt);
        sqlite3_stmt* rangeStmt = nullptr;
        const char* rangeSql = R"(
            UPDATE Matches
            SET session_id = ?1
            WHERE session_id IS NULL
              AND CAST(strftime('%s', timestamp) AS INTEGER) BETWEEN ?2 AND ?3
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers p
                  WHERE p.match_id = Matches.id AND p.primary_id = ?4
              );
        )";
        if (sqlite3_prepare_v2(m_db, rangeSql, -1, &rangeStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(rangeStmt, 1, sessionId);
            sqlite3_bind_int64(rangeStmt, 2, rangeStart);
            sqlite3_bind_int64(rangeStmt, 3, rangeEnd);
            sqlite3_bind_text(rangeStmt, 4, account.c_str(), -1, SQLITE_TRANSIENT);
            (void)sqlite3_step(rangeStmt);
            sqlite3_finalize(rangeStmt);
        }
    }

    int linkedCount = 0;
    {
        sqlite3_stmt* cntStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT COUNT(*) FROM Matches WHERE session_id = ?1;", -1, &cntStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(cntStmt, 1, sessionId);
            if (sqlite3_step(cntStmt) == SQLITE_ROW) linkedCount = sqlite3_column_int(cntStmt, 0);
            sqlite3_finalize(cntStmt);
        }
    }

    if (linkedCount == 0 && expectedGames > 0 && !account.empty()) {
        std::vector<std::pair<sqlite3_int64, int64_t>> candidates;
        sqlite3_stmt* candStmt = nullptr;
        const char* candSql = R"(
            SELECT Matches.id, CAST(strftime('%s', Matches.timestamp) AS INTEGER)
            FROM Matches
            WHERE Matches.session_id IS NULL
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers p
                  WHERE p.match_id = Matches.id AND p.primary_id = ?1
              )
            ORDER BY Matches.timestamp DESC, Matches.id DESC
            LIMIT ?2;
        )";
        if (sqlite3_prepare_v2(m_db, candSql, -1, &candStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(candStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(candStmt, 2, expectedGames);
            while (sqlite3_step(candStmt) == SQLITE_ROW) {
                candidates.emplace_back(sqlite3_column_int64(candStmt, 0), sqlite3_column_int64(candStmt, 1));
            }
            sqlite3_finalize(candStmt);
        }
        if (!candidates.empty()) {
            sqlite3_stmt* linkStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;", -1, &linkStmt, nullptr) == SQLITE_OK) {
                int64_t prevTs = candidates.front().second;
                for (const auto& [mid, ts] : candidates) {
                    if (prevTs - ts > Insights::kSessionGapSeconds) break;
                    prevTs = ts;
                    sqlite3_bind_int64(linkStmt, 1, sessionId);
                    sqlite3_bind_int64(linkStmt, 2, mid);
                    (void)sqlite3_step(linkStmt);
                    sqlite3_reset(linkStmt);
                    sqlite3_clear_bindings(linkStmt);
                }
                sqlite3_finalize(linkStmt);
            }
        }
    }

    sqlite3_stmt* minMaxStmt = nullptr;
    const char* minMaxSql =
        "SELECT MIN(CAST(strftime('%s', timestamp) AS INTEGER)), MAX(CAST(strftime('%s', timestamp) AS INTEGER)) "
        "FROM Matches WHERE session_id = ?1;";
    if (sqlite3_prepare_v2(m_db, minMaxSql, -1, &minMaxStmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(minMaxStmt, 1, sessionId);
        if (sqlite3_step(minMaxStmt) == SQLITE_ROW && sqlite3_column_type(minMaxStmt, 0) != SQLITE_NULL) {
            outEarliestMatchTs = sqlite3_column_int64(minMaxStmt, 0);
            outLatestMatchTs = sqlite3_column_int64(minMaxStmt, 1);
        }
        sqlite3_finalize(minMaxStmt);
    }
}

int64_t DatabaseManager::SaveSessionLocked(const SessionRecap& recap) {
    if (!m_db || !HasSessionsTableLocked()) return 0;
    if (recap.totals.wins + recap.totals.losses <= 0 && !recap.valid) return 0;

    std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
    if (account.empty() && m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) account = liveSnap->myPrimaryId;
    }
    if (account.empty()) {
        account = Config::Read().last_primary_id;
    }

    const std::string source = recap.source.empty() ? "live" : recap.source;
    const auto genKey = std::make_pair(account, recap.sessionGeneration);
    if (source == "live" && recap.id == 0) {
        const auto existingIt = m_sessionIdByGeneration.find(genKey);
        if (existingIt != m_sessionIdByGeneration.end() &&
            existingIt->second.sessionId > 0 &&
            existingIt->second.endedAtUnix == recap.endedAtUnix) {
            SessionRecap withId = recap;
            withId.id = existingIt->second.sessionId;
            withId.account = account;
            UpdateSessionLocked(withId);
            return existingIt->second.sessionId;
        }
    }

    const int64_t endedAt = recap.endedAtUnix > 0 ? recap.endedAtUnix : static_cast<int64_t>(std::time(nullptr));
    int64_t startedAt = recap.startedAtUnix > 0 ? std::min(recap.startedAtUnix, endedAt) : endedAt;
    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return 0;
    }

    const char* insertSql = R"(
        INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?);
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, insertSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, startedAt);
    sqlite3_bind_int64(stmt, 3, endedAt);
    sqlite3_bind_int(stmt, 4, recap.totals.wins);
    sqlite3_bind_int(stmt, 5, recap.totals.losses);
    sqlite3_bind_text(stmt, 6, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 8, source.c_str(), -1, SQLITE_TRANSIENT);

    const bool inserted = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!inserted) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    const sqlite3_int64 sessionId = sqlite3_last_insert_rowid(m_db);
    int64_t earliestTs = 0;
    int64_t latestTs = 0;
    LinkSessionMatchesLocked(sessionId,
                             account,
                             recap.startedAtUnix,
                             endedAt,
                             recap.matchGuids,
                             recap.totals.wins + recap.totals.losses,
                             earliestTs,
                             latestTs);
    if (recap.startedAtUnix <= 0 && earliestTs > 0) {
        startedAt = std::min(earliestTs, endedAt);
        sqlite3_stmt* updStart = nullptr;
        if (sqlite3_prepare_v2(m_db, "UPDATE Sessions SET started_at = ?1 WHERE id = ?2;", -1, &updStart, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(updStart, 1, startedAt);
            sqlite3_bind_int64(updStart, 2, sessionId);
            (void)sqlite3_step(updStart);
            sqlite3_finalize(updStart);
        }
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }

    if (source == "live") {
        m_sessionIdByGeneration[genKey] = {sessionId, endedAt};
    }
    return sessionId;
}

bool DatabaseManager::UpdateSessionLocked(const SessionRecap& recap) {
    if (!m_db || !HasSessionsTableLocked()) return false;

    std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
    if (account.empty() && m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) account = liveSnap->myPrimaryId;
    }
    if (account.empty()) {
        account = Config::Read().last_primary_id;
    }

    sqlite3_int64 sessionId = recap.id;
    if (sessionId <= 0) {
        const auto it = m_sessionIdByGeneration.find(std::make_pair(account, recap.sessionGeneration));
        if (it != m_sessionIdByGeneration.end()) {
            sessionId = it->second.sessionId;
        }
    }
    if (sessionId <= 0 && !account.empty()) {
        sqlite3_stmt* findStmt = nullptr;
        const char* findSql =
            "SELECT id FROM Sessions WHERE account = ?1 AND source = 'live' ORDER BY id DESC LIMIT 1;";
        if (sqlite3_prepare_v2(m_db, findSql, -1, &findStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(findStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(findStmt) == SQLITE_ROW) {
                sessionId = sqlite3_column_int64(findStmt, 0);
            }
            sqlite3_finalize(findStmt);
        }
    }
    if (sessionId <= 0) {
        return SaveSessionLocked(recap) > 0;
    }

    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return false;
    }

    const char* updateSql = R"(
        UPDATE Sessions
        SET wins = ?1,
            losses = ?2,
            mmr_change_json = ?3,
            totals_json = ?4,
            ended_at = CASE WHEN ?5 > COALESCE(ended_at, 0) THEN ?5 ELSE ended_at END
        WHERE id = ?6;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, updateSql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    sqlite3_bind_int(stmt, 1, recap.totals.wins);
    sqlite3_bind_int(stmt, 2, recap.totals.losses);
    sqlite3_bind_text(stmt, 3, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, recap.endedAtUnix);
    sqlite3_bind_int64(stmt, 6, sessionId);

    const bool updated = (sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    if (!updated) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }

    int64_t earliestTs = 0;
    int64_t latestTs = 0;
    LinkSessionMatchesLocked(sessionId,
                             account,
                             recap.startedAtUnix,
                             recap.endedAtUnix,
                             recap.matchGuids,
                             recap.totals.wins + recap.totals.losses,
                             earliestTs,
                             latestTs);

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

int64_t DatabaseManager::SaveSession(const SessionRecap& recap) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return SaveSessionLocked(recap);
}

bool DatabaseManager::UpdateSession(const SessionRecap& recap) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    return UpdateSessionLocked(recap);
}

void DatabaseManager::AsyncSaveSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        const int64_t id = SaveSession(r);
        if (id > 0 && m_state) {
            std::string pid;
            {
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.loaded) pid = m_state->insights.primaryId;
            }
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.primaryId == pid) {
                    m_state->insights.sessions = std::move(sessions);
                    m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    },
                       DbJobPriority::Critical);
}

void DatabaseManager::AsyncUpdateSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        if (UpdateSession(r) && m_state) {
            std::string pid;
            {
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.loaded) pid = m_state->insights.primaryId;
            }
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                std::lock_guard<std::mutex> lock(m_state->insights.mutex);
                if (m_state->insights.primaryId == pid) {
                    m_state->insights.sessions = std::move(sessions);
                    m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    },
                       DbJobPriority::Critical);
}

bool DatabaseManager::RecomputeBackfillSessionLocked(sqlite3_int64 sessionId, const std::string& account) {
    if (!m_db || !HasSessionsTableLocked() || sessionId <= 0) return false;
    const bool hasStats = HasStatsTablesLocked();
    const char* sql = hasStats ? R"(
        SELECT m.id,
               CAST(strftime('%s', m.timestamp) AS INTEGER),
               COALESCE(m.win, 0),
               COALESCE(m.our_score, 0),
               COALESCE(m.their_score, 0),
               m.gamemode,
               COALESCE(m.player_count, 0),
               m.playlist_id,
               COALESCE(mp.mmr, 0),
               COALESCE(mp.mmr_estimated, 0),
               COALESCE(mps.goals, 0),
               COALESCE(mps.assists, 0),
               COALESCE(mps.saves, 0),
               COALESCE(mps.shots, 0),
               COALESCE(mps.demos, 0),
               COALESCE(mps.max_goal_speed, 0.0),
               COALESCE(mps.fastest_goal_time, 0.0),
               COALESCE(mls.boost_collected, 0),
               COALESCE(mls.demoed, 0),
               COALESCE(mls.crossbars, 0),
               COALESCE(mls.hardest_crossbar, 0.0),
               COALESCE(mls.max_ball_speed, 0.0),
               COALESCE(mls.own_goals, 0)
        FROM Matches m
        LEFT JOIN MatchPlayers mp ON mp.match_id = m.id AND mp.primary_id = ?2
        LEFT JOIN MatchPlayerStats mps ON mps.match_id = m.id AND mps.primary_id = ?2
        LEFT JOIN MatchLocalStats mls ON mls.match_id = m.id
        WHERE m.session_id = ?1
        ORDER BY m.timestamp ASC, m.id ASC;
    )"
                               : R"(
        SELECT m.id,
               CAST(strftime('%s', m.timestamp) AS INTEGER),
               COALESCE(m.win, 0),
               COALESCE(m.our_score, 0),
               COALESCE(m.their_score, 0),
               m.gamemode,
               COALESCE(m.player_count, 0),
               m.playlist_id,
               COALESCE(mp.mmr, 0),
               COALESCE(mp.mmr_estimated, 0),
               0, 0, 0, 0, 0, 0.0, 0.0,
               0, 0, 0, 0.0, 0.0, 0
        FROM Matches m
        LEFT JOIN MatchPlayers mp ON mp.match_id = m.id AND mp.primary_id = ?2
        WHERE m.session_id = ?1
        ORDER BY m.timestamp ASC, m.id ASC;
    )";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(stmt, 1, sessionId);
    sqlite3_bind_text(stmt, 2, account.c_str(), -1, SQLITE_TRANSIENT);

    SessionRecap recap;
    recap.valid = true;
    recap.id = sessionId;
    recap.account = account;
    recap.source = "backfill";

    struct PlaylistMmrTrack {
        int firstMmr = 0;
        int lastMmr = 0;
        int count = 0;
        bool moved = false;
    };
    std::map<std::string, PlaylistMmrTrack> mmrTracks;
    int rowCount = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int64_t ts = sqlite3_column_int64(stmt, 1);
        const bool win = sqlite3_column_int(stmt, 2) != 0;
        const int ourScore = sqlite3_column_int(stmt, 3);
        const std::string rawGamemode = SqlColumnText(stmt, 5);
        const int playerCount = sqlite3_column_int(stmt, 6);
        const bool hasPlaylistId = sqlite3_column_type(stmt, 7) != SQLITE_NULL;
        const int playlistId = hasPlaylistId ? sqlite3_column_int(stmt, 7) : -1;
        const int mmr = sqlite3_column_int(stmt, 8);
        const bool mmrEstimated = sqlite3_column_int(stmt, 9) != 0;
        const int goals = sqlite3_column_int(stmt, 10);
        const int assists = sqlite3_column_int(stmt, 11);
        const int saves = sqlite3_column_int(stmt, 12);
        const int shots = sqlite3_column_int(stmt, 13);
        const int demos = sqlite3_column_int(stmt, 14);
        const float maxGoalSpeed = static_cast<float>(sqlite3_column_double(stmt, 15));
        const float fastestGoalTime = static_cast<float>(sqlite3_column_double(stmt, 16));
        const int boostCollected = sqlite3_column_int(stmt, 17);
        const int demoed = sqlite3_column_int(stmt, 18);
        const int crossbars = sqlite3_column_int(stmt, 19);
        const float hardestCrossbar = static_cast<float>(sqlite3_column_double(stmt, 20));
        const float maxBallSpeed = static_cast<float>(sqlite3_column_double(stmt, 21));
        const int ownGoals = sqlite3_column_int(stmt, 22);

        if (rowCount == 0) {
            recap.startedAtUnix = ts;
            recap.endedAtUnix = ts;
        } else {
            recap.startedAtUnix = std::min(recap.startedAtUnix, ts);
            recap.endedAtUnix = std::max(recap.endedAtUnix, ts);
        }
        ++rowCount;

        auto& t = recap.totals;
        if (win)
            t.wins++;
        else
            t.losses++;
        t.goals += goals;
        t.assists += assists;
        t.saves += saves;
        t.shots += shots;
        t.demos += demos;
        t.boostPickedUp += boostCollected;
        t.demoed += demoed;
        t.crossbars += crossbars;
        t.ownGoalsSelf += ownGoals;
        t.maxGoalSpeedSelf = std::max(t.maxGoalSpeedSelf, maxGoalSpeed);
        t.maxGoalSpeed = std::max(t.maxGoalSpeed, maxGoalSpeed);
        t.maxImpactForceSelf = std::max(t.maxImpactForceSelf, hardestCrossbar);
        t.maxImpactForce = std::max(t.maxImpactForce, hardestCrossbar);
        t.maxBallSpeedSelf = std::max(t.maxBallSpeedSelf, maxBallSpeed);
        t.maxBallSpeed = std::max(t.maxBallSpeed, maxBallSpeed);
        if (fastestGoalTime > 0.0f && (t.fastestGoalTimeSelf == 0.0f || fastestGoalTime < t.fastestGoalTimeSelf)) {
            t.fastestGoalTimeSelf = fastestGoalTime;
            t.fastestGoalTime = fastestGoalTime;
        }

        std::string modeKey = rawGamemode;
        std::string mmrKey = rawGamemode;
        if (hasPlaylistId && PlaylistMetadata::IsKnown(playlistId)) {
            modeKey = PlaylistMetadata::StorageMode(playlistId);
            mmrKey = PlaylistMetadata::MmrKey(playlistId);
        } else if (modeKey.empty()) {
            modeKey = GamemodeUtils::InferFromSnapshot(playerCount, playerCount, MmrCategory::Best, "");
            mmrKey = modeKey;
        }

        if (modeKey != "1v1") {
            const int teamGoalsThisMatch = std::max(0, ourScore);
            t.teamGoals += teamGoalsThisMatch;
            t.goalParticipations += std::clamp(goals + assists, 0, teamGoalsThisMatch);
        }

        const bool isCasual = (hasPlaylistId && PlaylistMetadata::IsCasual(playlistId)) || modeKey == "casual";
        if (!isCasual && GamemodeUtils::IsTrackedCompetitiveMode(modeKey)) {
            auto& gm = recap.gamemodes[modeKey];
            if (win)
                gm.wins++;
            else
                gm.losses++;
            gm.total++;

            if (!mmrKey.empty() && mmr > 0 && !mmrEstimated) {
                auto& track = mmrTracks[mmrKey];
                if (track.count == 0) {
                    track.firstMmr = mmr;
                    track.lastMmr = mmr;
                } else {
                    if (mmr != track.firstMmr) track.moved = true;
                    track.lastMmr = mmr;
                }
                track.count++;
            }
        }
    }
    sqlite3_finalize(stmt);
    if (rowCount == 0) return false;

    for (const auto& [playlist, track] : mmrTracks) {
        if (track.count >= 2 && track.moved) {
            recap.totals.mmrChangeByPlaylist[playlist] = track.lastMmr - track.firstMmr;
        }
    }
    recap.totals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(recap.totals.mmrChangeByPlaylist));

    const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
    const std::string totalsJson = SerializeTotalsJson(recap);

    sqlite3_stmt* updStmt = nullptr;
    const char* updSql = R"(
        UPDATE Sessions
        SET started_at = ?1,
            ended_at = ?2,
            wins = ?3,
            losses = ?4,
            mmr_change_json = ?5,
            totals_json = ?6
        WHERE id = ?7;
    )";
    if (sqlite3_prepare_v2(m_db, updSql, -1, &updStmt, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(updStmt, 1, recap.startedAtUnix);
    sqlite3_bind_int64(updStmt, 2, recap.endedAtUnix);
    sqlite3_bind_int(updStmt, 3, recap.totals.wins);
    sqlite3_bind_int(updStmt, 4, recap.totals.losses);
    sqlite3_bind_text(updStmt, 5, mmrJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(updStmt, 6, totalsJson.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(updStmt, 7, sessionId);
    const bool ok = (sqlite3_step(updStmt) == SQLITE_DONE);
    sqlite3_finalize(updStmt);
    return ok;
}

int DatabaseManager::BackfillSessionsChunkLocked(int chunkSize, bool& outMoreRemaining, int& outLinked) {
    outMoreRemaining = false;
    outLinked = 0;
    if (!m_db || !HasSessionsTableLocked()) return 0;
    if (chunkSize <= 0) chunkSize = 500;

    std::unordered_set<std::string> activeSessionGuids;
    std::string preferredAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) {
            preferredAccount = liveSnap->myPrimaryId;
            if (liveSnap->sessionTotals.wins + liveSnap->sessionTotals.losses > 0) {
                for (const auto& g : liveSnap->sessionMatchGuids) {
                    if (!g.empty()) activeSessionGuids.insert(g);
                }
            }
        }
    }
    if (preferredAccount.empty()) {
        preferredAccount = Config::Read().last_primary_id;
    }

    std::unordered_map<std::string, int> playerNonOpponentCounts;
    {
        sqlite3_stmt* freqStmt = nullptr;
        const char* freqSql = R"(
            SELECT primary_id, COUNT(DISTINCT match_id)
            FROM MatchPlayers
            WHERE COALESCE(is_opponent, 0) = 0
              AND primary_id IS NOT NULL
              AND primary_id != ''
              AND primary_id NOT LIKE 'Unknown|%'
            GROUP BY primary_id;
        )";
        if (sqlite3_prepare_v2(m_db, freqSql, -1, &freqStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(freqStmt) == SQLITE_ROW) {
                playerNonOpponentCounts[SqlColumnText(freqStmt, 0)] = sqlite3_column_int(freqStmt, 1);
            }
            sqlite3_finalize(freqStmt);
        }
    }

    struct UnlinkedMatch {
        sqlite3_int64 id = 0;
        int64_t endedAtUnix = 0;
        std::string matchGuid;
        std::string account;
    };
    std::vector<UnlinkedMatch> chunk;
    {
        sqlite3_stmt* mStmt = nullptr;
        const char* mSql = R"(
            SELECT Matches.id, CAST(strftime('%s', Matches.timestamp) AS INTEGER), COALESCE(Matches.match_guid, '')
            FROM Matches
            WHERE Matches.session_id IS NULL
              AND COALESCE(Matches.result_pending, 0) = 0
              AND EXISTS (
                  SELECT 1 FROM MatchPlayers mp
                  WHERE mp.match_id = Matches.id
                    AND COALESCE(mp.is_opponent, 0) = 0
                    AND mp.primary_id IS NOT NULL
                    AND mp.primary_id != ''
                    AND mp.primary_id NOT LIKE 'Unknown|%'
              )
            ORDER BY Matches.timestamp ASC, Matches.id ASC
            LIMIT ?1;
        )";
        if (sqlite3_prepare_v2(m_db, mSql, -1, &mStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(mStmt, 1, chunkSize + 1);
            while (sqlite3_step(mStmt) == SQLITE_ROW) {
                UnlinkedMatch um;
                um.id = sqlite3_column_int64(mStmt, 0);
                um.endedAtUnix = sqlite3_column_int64(mStmt, 1);
                um.matchGuid = SqlColumnText(mStmt, 2);
                if (!um.matchGuid.empty() && activeSessionGuids.count(um.matchGuid) > 0) {
                    continue;
                }
                chunk.push_back(std::move(um));
            }
            sqlite3_finalize(mStmt);
        }
    }

    if (chunk.size() > static_cast<size_t>(chunkSize)) {
        outMoreRemaining = true;
        chunk.resize(static_cast<size_t>(chunkSize));
    }
    if (chunk.empty()) return 0;

    {
        sqlite3_stmt* pStmt = nullptr;
        const char* pSql = R"(
            SELECT primary_id, id
            FROM MatchPlayers
            WHERE match_id = ?1
              AND COALESCE(is_opponent, 0) = 0
              AND primary_id IS NOT NULL
              AND primary_id != ''
              AND primary_id NOT LIKE 'Unknown|%'
            ORDER BY id ASC;
        )";
        if (sqlite3_prepare_v2(m_db, pSql, -1, &pStmt, nullptr) == SQLITE_OK) {
            for (auto& um : chunk) {
                sqlite3_bind_int64(pStmt, 1, um.id);
                std::string bestPid;
                int bestScore = -1;
                while (sqlite3_step(pStmt) == SQLITE_ROW) {
                    const std::string pid = SqlColumnText(pStmt, 0);
                    int score = playerNonOpponentCounts[pid];
                    if (!preferredAccount.empty() && pid == preferredAccount) score += 1'000'000;
                    if (score > bestScore) {
                        bestScore = score;
                        bestPid = pid;
                    }
                }
                sqlite3_reset(pStmt);
                sqlite3_clear_bindings(pStmt);
                um.account = std::move(bestPid);
            }
            sqlite3_finalize(pStmt);
        }
    }

    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        return 0;
    }

    std::map<std::string, std::vector<UnlinkedMatch>> byAccount;
    sqlite3_stmt* liveOverlapStmt = nullptr;
    const char* liveOverlapSql = R"(
        SELECT id FROM Sessions
        WHERE account = ?1 AND source = 'live' AND ?2 BETWEEN started_at AND ended_at
        ORDER BY id DESC LIMIT 1;
    )";
    sqlite3_prepare_v2(m_db, liveOverlapSql, -1, &liveOverlapStmt, nullptr);

    sqlite3_stmt* setMatchSessionStmt = nullptr;
    sqlite3_prepare_v2(m_db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;", -1, &setMatchSessionStmt, nullptr);
    bool linkFailed = setMatchSessionStmt == nullptr;
    std::string linkError = linkFailed ? sqlite3_errmsg(m_db) : "";
    int linked = 0;
    const auto linkMatch = [&](sqlite3_int64 sessionId, sqlite3_int64 matchId) {
        sqlite3_bind_int64(setMatchSessionStmt, 1, sessionId);
        sqlite3_bind_int64(setMatchSessionStmt, 2, matchId);
        if (sqlite3_step(setMatchSessionStmt) == SQLITE_DONE) {
            ++linked;
        } else if (!linkFailed) {
            linkFailed = true;
            linkError = sqlite3_errmsg(m_db);
        }
        sqlite3_reset(setMatchSessionStmt);
        sqlite3_clear_bindings(setMatchSessionStmt);
    };

    for (auto& um : chunk) {
        if (um.account.empty()) continue;
        sqlite3_int64 overlappingLiveId = 0;
        if (liveOverlapStmt) {
            sqlite3_bind_text(liveOverlapStmt, 1, um.account.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(liveOverlapStmt, 2, um.endedAtUnix);
            if (sqlite3_step(liveOverlapStmt) == SQLITE_ROW) {
                overlappingLiveId = sqlite3_column_int64(liveOverlapStmt, 0);
            }
            sqlite3_reset(liveOverlapStmt);
            sqlite3_clear_bindings(liveOverlapStmt);
        }
        if (overlappingLiveId > 0 && setMatchSessionStmt) {
            linkMatch(overlappingLiveId, um.id);
            continue;
        }
        byAccount[um.account].push_back(std::move(um));
    }
    if (liveOverlapStmt) sqlite3_finalize(liveOverlapStmt);

    int sessionsCreated = 0;
    sqlite3_stmt* prevBackfillStmt = nullptr;
    const char* prevBackfillSql = R"(
        SELECT id, COALESCE(ended_at, 0)
        FROM Sessions
        WHERE account = ?1 AND source = 'backfill' AND COALESCE(ended_at, 0) <= ?2
        ORDER BY ended_at DESC, id DESC
        LIMIT 1;
    )";
    sqlite3_prepare_v2(m_db, prevBackfillSql, -1, &prevBackfillStmt, nullptr);

    sqlite3_stmt* insertSessStmt = nullptr;
    const char* insertSessSql = R"(
        INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
        VALUES (?1, ?2, ?3, 0, 0, '{}', '{}', 'backfill');
    )";
    sqlite3_prepare_v2(m_db, insertSessSql, -1, &insertSessStmt, nullptr);

    for (const auto& [account, matches] : byAccount) {
        std::vector<int64_t> timestamps;
        timestamps.reserve(matches.size());
        for (const auto& m : matches)
            timestamps.push_back(m.endedAtUnix);

        const std::vector<Insights::SessionSlice> slices = Insights::GroupSessionTimestamps(timestamps);
        for (size_t sliceIdx = 0; sliceIdx < slices.size(); ++sliceIdx) {
            const auto& slice = slices[sliceIdx];
            sqlite3_int64 targetSessionId = 0;

            if (sliceIdx == 0 && prevBackfillStmt) {
                sqlite3_bind_text(prevBackfillStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(prevBackfillStmt, 2, slice.startedAtUnix);
                if (sqlite3_step(prevBackfillStmt) == SQLITE_ROW) {
                    const sqlite3_int64 prevId = sqlite3_column_int64(prevBackfillStmt, 0);
                    const int64_t prevEnded = sqlite3_column_int64(prevBackfillStmt, 1);
                    if (prevEnded > 0 && !Insights::StartsNewSession(prevEnded, slice.startedAtUnix, true)) {
                        targetSessionId = prevId;
                    }
                }
                sqlite3_reset(prevBackfillStmt);
                sqlite3_clear_bindings(prevBackfillStmt);
            }

            if (targetSessionId <= 0 && insertSessStmt) {
                sqlite3_bind_text(insertSessStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(insertSessStmt, 2, slice.startedAtUnix);
                sqlite3_bind_int64(insertSessStmt, 3, slice.endedAtUnix);
                if (sqlite3_step(insertSessStmt) == SQLITE_DONE) {
                    targetSessionId = sqlite3_last_insert_rowid(m_db);
                    ++sessionsCreated;
                }
                sqlite3_reset(insertSessStmt);
                sqlite3_clear_bindings(insertSessStmt);
            }

            if (targetSessionId <= 0 || !setMatchSessionStmt) {
                if (!linkFailed) linkError = "could not create a backfill session";
                linkFailed = true;
                continue;
            }
            for (size_t idx = slice.beginIndex; idx < slice.endIndex; ++idx)
                linkMatch(targetSessionId, matches[idx].id);
            (void)RecomputeBackfillSessionLocked(targetSessionId, account);
        }
    }

    if (insertSessStmt) sqlite3_finalize(insertSessStmt);
    if (prevBackfillStmt) sqlite3_finalize(prevBackfillStmt);
    if (setMatchSessionStmt) sqlite3_finalize(setMatchSessionStmt);

    // Keep a chunk all-or-nothing so a failing link never leaves empty backfill sessions behind.
    if (linkFailed) {
        std::cerr << "[Database] Session backfill could not link matches: " << linkError << '\n';
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "[Database] Session backfill commit failed: " << sqlite3_errmsg(m_db) << '\n';
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return 0;
    }
    outLinked = linked;
    return sessionsCreated;
}

int DatabaseManager::BackfillSessions(int chunkSize) {
    int totalCreated = 0;
    while (true) {
        bool moreRemaining = false;
        int linked = 0;
        std::lock_guard<std::mutex> lock(m_dbMutex);
        totalCreated += BackfillSessionsChunkLocked(chunkSize, moreRemaining, linked);
        if (!moreRemaining || linked == 0) {
            m_backfillPending.store(false, std::memory_order_release);
            break;
        }
    }
    return totalCreated;
}

std::vector<SessionRecap> DatabaseManager::ListSessions(const std::string& account, int offset, int limit) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    std::vector<SessionRecap> out;
    if (account.empty()) return out;

    bool shouldBackfillAccount = false;
    {
        auto read = AcquireRead();
        sqlite3* db = read.db;
        if (!db || !HasSessionsTable(db)) return out;
        bool liveOrEndedInMemory = false;
        if (m_state && m_state->liveMatch) {
            auto liveSnap = m_state->liveMatch->GetSnapshot();
            if (liveSnap) {
                liveOrEndedInMemory =
                    (liveSnap->sessionTotals.wins + liveSnap->sessionTotals.losses > 0) ||
                    liveSnap->lastSessionRecap.valid ||
                    liveSnap->inMatch;
            }
        }
        if (!liveOrEndedInMemory) {
            sqlite3_stmt* chkStmt = nullptr;
            const char* chkSql = R"(
                SELECT 1 FROM Matches m
                WHERE m.session_id IS NULL
                  AND COALESCE(m.result_pending, 0) = 0
                  AND EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id = m.id AND p.primary_id = ?1)
                LIMIT 1;
            )";
            if (sqlite3_prepare_v2(db, chkSql, -1, &chkStmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(chkStmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
                shouldBackfillAccount = (sqlite3_step(chkStmt) == SQLITE_ROW);
                sqlite3_finalize(chkStmt);
            }
        }
    }
    if (shouldBackfillAccount) {
        BackfillSessions(500);
    }

    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !HasSessionsTable(db)) return out;

    const int clampedOffset = std::max(0, offset);
    const int clampedLimit = limit <= 0 ? 50 : limit;

    const char* sql = R"(
        SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0),
               COALESCE(wins, 0), COALESCE(losses, 0),
               COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'),
               COALESCE(source, 'live')
        FROM Sessions
        WHERE account = ?1
        ORDER BY ended_at DESC, id DESC
        LIMIT ?2 OFFSET ?3;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_text(stmt, 1, account.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, clampedLimit);
    sqlite3_bind_int(stmt, 3, clampedOffset);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SessionRecap recap;
        recap.valid = true;
        recap.id = sqlite3_column_int64(stmt, 0);
        recap.account = SqlColumnText(stmt, 1);
        recap.mmrOwnerPrimaryId = recap.account;
        recap.startedAtUnix = sqlite3_column_int64(stmt, 2);
        recap.endedAtUnix = sqlite3_column_int64(stmt, 3);
        const int wins = sqlite3_column_int(stmt, 4);
        const int losses = sqlite3_column_int(stmt, 5);
        const std::string mmrJsonStr = SqlColumnText(stmt, 6);
        const std::string totalsJsonStr = SqlColumnText(stmt, 7);
        recap.source = SqlColumnText(stmt, 8);
        ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
        recap.totals.wins = wins;
        recap.totals.losses = losses;
        out.push_back(std::move(recap));
    }
    sqlite3_finalize(stmt);
    return out;
}

SessionRecap DatabaseManager::GetSession(int64_t id) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    SessionRecap recap;
    if (id <= 0) return recap;
    auto read = AcquireRead();
    sqlite3* db = read.db;
    if (!db || !HasSessionsTable(db)) return recap;

    const char* sql = R"(
        SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0),
               COALESCE(wins, 0), COALESCE(losses, 0),
               COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'),
               COALESCE(source, 'live')
        FROM Sessions
        WHERE id = ?1
        LIMIT 1;
    )";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return recap;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        recap.valid = true;
        recap.id = sqlite3_column_int64(stmt, 0);
        recap.account = SqlColumnText(stmt, 1);
        recap.mmrOwnerPrimaryId = recap.account;
        recap.startedAtUnix = sqlite3_column_int64(stmt, 2);
        recap.endedAtUnix = sqlite3_column_int64(stmt, 3);
        const int wins = sqlite3_column_int(stmt, 4);
        const int losses = sqlite3_column_int(stmt, 5);
        const std::string mmrJsonStr = SqlColumnText(stmt, 6);
        const std::string totalsJsonStr = SqlColumnText(stmt, 7);
        recap.source = SqlColumnText(stmt, 8);
        ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
        recap.totals.wins = wins;
        recap.totals.losses = losses;
    }
    sqlite3_finalize(stmt);
    return recap;
}
