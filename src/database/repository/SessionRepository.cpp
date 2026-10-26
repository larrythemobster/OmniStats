#include "SessionRepository.hpp"
#include "core/config/Config.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/Insights.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include <algorithm>
#include <ctime>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace database::repository {

    namespace {

        std::string SerializeMmrChangeJson(const std::map<std::string, int>& changes) {
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& [playlist, delta] : changes) {
                if (!playlist.empty()) obj[playlist] = delta;
            }
            return obj.dump();
        }

        std::string SerializeTotalsJson(const SessionRecap& recap) {
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

        void ParseSessionRecapJson(const std::string& mmrJsonStr,
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

    } // namespace

    bool SessionRepository::HasSessionsTable(sqlite::Connection& db) {
        return db.TableExists("Sessions") && db.TableHasColumn("Matches", "session_id");
    }

    bool SessionRepository::HasUnlinkedMatches(sqlite::Connection& db, const std::string& account) {
        if (!db.IsOpen() || !HasSessionsTable(db)) return false;

        if (account.empty()) {
            const char* sql = "SELECT 1 FROM Matches WHERE session_id IS NULL AND COALESCE(result_pending, 0) = 0 LIMIT 1;";
            try {
                sqlite::Statement stmt(db, sql);
                return stmt.Step();
            } catch (...) {
                return false;
            }
        }

        const char* sql = R"(
        SELECT 1 FROM Matches m
        WHERE m.session_id IS NULL
          AND COALESCE(m.result_pending, 0) = 0
          AND EXISTS (SELECT 1 FROM MatchPlayers p WHERE p.match_id = m.id AND p.primary_id = ?1)
        LIMIT 1;
    )";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, account);
            return stmt.Step();
        } catch (...) {
            return false;
        }
    }

    void SessionRepository::LinkSessionMatches(sqlite::Connection& db, int64_t sessionId,
                                               const std::string& account,
                                               int64_t startedAt,
                                               int64_t endedAt,
                                               const std::vector<std::string>& matchGuids,
                                               int expectedGames,
                                               int64_t& outEarliestMatchTs,
                                               int64_t& outLatestMatchTs) {
        outEarliestMatchTs = 0;
        outLatestMatchTs = 0;
        if (!db.IsOpen() || !HasSessionsTable(db) || sessionId <= 0) return;

        if (!matchGuids.empty()) {
            const char* guidSql =
                "UPDATE Matches SET session_id = ?1 WHERE match_guid = ?2 AND (session_id IS NULL OR session_id = ?1);";
            try {
                sqlite::Statement guidStmt(db, guidSql);
                for (const auto& guid : matchGuids) {
                    if (guid.empty()) continue;
                    guidStmt.Reset();
                    guidStmt.Bind(1, sessionId);
                    guidStmt.Bind(2, guid);
                    (void)guidStmt.Step();
                }
            } catch (...) {
            }
        }

        if (!account.empty() && endedAt > 0) {
            const int64_t rangeStart = startedAt > 0 ? (std::min)(startedAt, endedAt)
                                                     : (endedAt - Insights::kSessionGapSeconds);
            const int64_t rangeEnd = (std::max)(startedAt, endedAt);
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
            try {
                sqlite::Statement rangeStmt(db, rangeSql);
                rangeStmt.Bind(1, sessionId);
                rangeStmt.Bind(2, rangeStart);
                rangeStmt.Bind(3, rangeEnd);
                rangeStmt.Bind(4, account);
                (void)rangeStmt.Step();
            } catch (...) {
            }
        }

        int linkedCount = 0;
        try {
            sqlite::Statement cntStmt(db, "SELECT COUNT(*) FROM Matches WHERE session_id = ?1;");
            cntStmt.Bind(1, sessionId);
            if (cntStmt.Step()) {
                linkedCount = cntStmt.Get<int>(0);
            }
        } catch (...) {
        }

        if (linkedCount == 0 && expectedGames > 0 && !account.empty()) {
            std::vector<std::pair<int64_t, int64_t>> candidates;
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
            try {
                sqlite::Statement candStmt(db, candSql);
                candStmt.Bind(1, account);
                candStmt.Bind(2, expectedGames);
                while (candStmt.Step()) {
                    candidates.emplace_back(candStmt.Get<int64_t>(0), candStmt.Get<int64_t>(1));
                }
            } catch (...) {
            }

            if (!candidates.empty()) {
                try {
                    sqlite::Statement linkStmt(db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;");
                    int64_t prevTs = candidates.front().second;
                    for (const auto& [mid, ts] : candidates) {
                        if (prevTs - ts > Insights::kSessionGapSeconds) break;
                        prevTs = ts;
                        linkStmt.Reset();
                        linkStmt.Bind(1, sessionId);
                        linkStmt.Bind(2, mid);
                        (void)linkStmt.Step();
                    }
                } catch (...) {
                }
            }
        }

        try {
            const char* minMaxSql =
                "SELECT MIN(CAST(strftime('%s', timestamp) AS INTEGER)), MAX(CAST(strftime('%s', timestamp) AS INTEGER)) "
                "FROM Matches WHERE session_id = ?1;";
            sqlite::Statement minMaxStmt(db, minMaxSql);
            minMaxStmt.Bind(1, sessionId);
            if (minMaxStmt.Step() && !minMaxStmt.IsNull(0)) {
                outEarliestMatchTs = minMaxStmt.Get<int64_t>(0);
                outLatestMatchTs = minMaxStmt.Get<int64_t>(1);
            }
        } catch (...) {
        }
    }

    int64_t SessionRepository::SaveSession(sqlite::Connection& db, const SessionRecap& recap,
                                           std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>>* sessionIdByGeneration) {
        if (!db.IsOpen() || !HasSessionsTable(db)) return 0;
        if (recap.totals.wins + recap.totals.losses <= 0 && !recap.valid) return 0;

        std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
        if (account.empty()) {
            account = Config::Read().last_primary_id;
        }

        const std::string source = !recap.source.empty() ? recap.source : "live";
        const auto genKey = std::make_pair(account, recap.sessionGeneration);
        if (source == "live" && sessionIdByGeneration) {
            auto it = sessionIdByGeneration->find(genKey);
            if (it != sessionIdByGeneration->end()) {
                SessionRecap updated = recap;
                updated.id = it->second.first;
                if (UpdateSession(db, updated, sessionIdByGeneration)) {
                    return it->second.first;
                }
            }
        }

        int64_t startedAt = recap.startedAtUnix;
        int64_t endedAt = recap.endedAtUnix;
        if (endedAt <= 0) {
            endedAt = static_cast<int64_t>(std::time(nullptr));
        }
        if (startedAt <= 0) {
            startedAt = endedAt;
        }

        const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
        const std::string totalsJson = SerializeTotalsJson(recap);

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            const char* insertSql = R"(
            INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);
        )";
            sqlite::Statement stmt(db, insertSql);
            stmt.Bind(1, account);
            stmt.Bind(2, startedAt);
            stmt.Bind(3, endedAt);
            stmt.Bind(4, recap.totals.wins);
            stmt.Bind(5, recap.totals.losses);
            stmt.Bind(6, mmrJson);
            stmt.Bind(7, totalsJson);
            stmt.Bind(8, source);
            stmt.Execute();

            const int64_t sessionId = db.LastInsertRowId();
            int64_t earliestTs = 0;
            int64_t latestTs = 0;
            LinkSessionMatches(db, sessionId, account, recap.startedAtUnix, endedAt,
                               recap.matchGuids, recap.totals.wins + recap.totals.losses,
                               earliestTs, latestTs);

            if (recap.startedAtUnix <= 0 && earliestTs > 0) {
                startedAt = (std::min)(earliestTs, endedAt);
                sqlite::Statement updStart(db, "UPDATE Sessions SET started_at = ?1 WHERE id = ?2;");
                updStart.Bind(1, startedAt);
                updStart.Bind(2, sessionId);
                updStart.Execute();
            }

            tx.Commit();

            if (source == "live" && sessionIdByGeneration) {
                (*sessionIdByGeneration)[genKey] = {sessionId, endedAt};
            }
            return sessionId;
        } catch (const std::exception& e) {
            std::cerr << "[Database] SaveSession failed: " << e.what() << "\n";
            return 0;
        }
    }

    bool SessionRepository::UpdateSession(sqlite::Connection& db, const SessionRecap& recap,
                                          const std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>>* sessionIdByGeneration) {
        if (!db.IsOpen() || !HasSessionsTable(db)) return false;

        std::string account = !recap.account.empty() ? recap.account : recap.mmrOwnerPrimaryId;
        if (account.empty()) {
            account = Config::Read().last_primary_id;
        }

        int64_t sessionId = recap.id;
        if (sessionId <= 0 && sessionIdByGeneration) {
            const auto it = sessionIdByGeneration->find(std::make_pair(account, recap.sessionGeneration));
            if (it != sessionIdByGeneration->end()) {
                sessionId = it->second.first;
            }
        }
        if (sessionId <= 0 && !account.empty()) {
            try {
                const char* findSql =
                    "SELECT id FROM Sessions WHERE account = ?1 AND source = 'live' ORDER BY id DESC LIMIT 1;";
                sqlite::Statement findStmt(db, findSql);
                findStmt.Bind(1, account);
                if (findStmt.Step()) {
                    sessionId = findStmt.Get<int64_t>(0);
                }
            } catch (...) {
            }
        }
        if (sessionId <= 0) {
            return SaveSession(db, recap, const_cast<std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>>*>(sessionIdByGeneration)) > 0;
        }

        const std::string mmrJson = SerializeMmrChangeJson(recap.totals.mmrChangeByPlaylist);
        const std::string totalsJson = SerializeTotalsJson(recap);

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            const char* updateSql = R"(
            UPDATE Sessions
            SET wins = ?1,
                losses = ?2,
                mmr_change_json = ?3,
                totals_json = ?4,
                ended_at = CASE WHEN ?5 > COALESCE(ended_at, 0) THEN ?5 ELSE ended_at END
            WHERE id = ?6;
        )";
            sqlite::Statement stmt(db, updateSql);
            stmt.Bind(1, recap.totals.wins);
            stmt.Bind(2, recap.totals.losses);
            stmt.Bind(3, mmrJson);
            stmt.Bind(4, totalsJson);
            stmt.Bind(5, recap.endedAtUnix);
            stmt.Bind(6, sessionId);
            stmt.Execute();

            int64_t earliestTs = 0;
            int64_t latestTs = 0;
            LinkSessionMatches(db, sessionId, account, recap.startedAtUnix, recap.endedAtUnix,
                               recap.matchGuids, recap.totals.wins + recap.totals.losses,
                               earliestTs, latestTs);

            tx.Commit();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] UpdateSession failed: " << e.what() << "\n";
            return false;
        }
    }

    bool SessionRepository::RecomputeBackfillSession(sqlite::Connection& db, int64_t sessionId, const std::string& account) {
        if (!db.IsOpen() || sessionId <= 0 || !HasSessionsTable(db)) return false;

        const bool hasStats = db.TableExists("MatchPlayerStats") && db.TableExists("MatchLocalStats");
        std::string sql =
            "SELECT m.id, CAST(strftime('%s', m.timestamp) AS INTEGER), m.win, "
            "       COALESCE(m.playlist_id, -1), COALESCE(m.gamemode, ''), "
            "       COALESCE(mp.mmr, 0), COALESCE(mp.mmr_estimated, 0), ";
        if (hasStats) {
            sql +=
                "COALESCE(mps.score, 0), COALESCE(mps.goals, 0), COALESCE(mps.assists, 0), "
                "COALESCE(mps.saves, 0), COALESCE(mps.shots, 0), COALESCE(mps.demos, 0), "
                "COALESCE(mps.touches, 0), COALESCE(mps.max_goal_speed, 0.0), COALESCE(mps.fastest_goal_time, 0.0), "
                "COALESCE(mls.boost_collected, 0), COALESCE(mls.demoed, 0), COALESCE(mls.crossbars, 0), "
                "COALESCE(mls.hardest_crossbar, 0.0), COALESCE(mls.max_ball_speed, 0.0), COALESCE(mls.own_goals, 0) ";
        } else {
            sql +=
                "0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, "
                "0, 0, 0, 0.0, 0.0, 0 ";
        }
        sql +=
            "FROM Matches m "
            "LEFT JOIN MatchPlayers mp ON mp.match_id = m.id AND mp.primary_id = ?1 "
            "LEFT JOIN MatchPlayerStats mps ON mps.match_id = m.id AND mps.primary_id = ?1 "
            "LEFT JOIN MatchLocalStats mls ON mls.match_id = m.id "
            "WHERE m.session_id = ?2 "
            "ORDER BY m.timestamp ASC, m.id ASC;";

        SessionRecap recap;
        recap.id = sessionId;
        recap.account = account;
        recap.mmrOwnerPrimaryId = account;
        recap.source = "backfill";
        recap.valid = true;

        struct PlaylistMmrTrack {
            int firstMmr = 0;
            int lastMmr = 0;
            int count = 0;
            bool moved = false;
        };
        std::map<std::string, PlaylistMmrTrack> mmrTracks;
        int rowCount = 0;

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, account);
            stmt.Bind(2, sessionId);

            while (stmt.Step()) {
                ++rowCount;
                const int64_t ts = stmt.Get<int64_t>(1);
                if (recap.startedAtUnix <= 0 || ts < recap.startedAtUnix) recap.startedAtUnix = ts;
                if (ts > recap.endedAtUnix) recap.endedAtUnix = ts;

                const bool win = stmt.Get<int>(2) != 0;
                if (win)
                    recap.totals.wins++;
                else
                    recap.totals.losses++;

                const int playlistId = stmt.Get<int>(3);
                const std::string gamemode = stmt.Get<std::string>(4);
                const int mmr = stmt.Get<int>(5);
                const bool mmrEstimated = stmt.Get<int>(6) != 0;

                if (hasStats) {
                    recap.totals.goals += stmt.Get<int>(8);
                    recap.totals.assists += stmt.Get<int>(9);
                    recap.totals.saves += stmt.Get<int>(10);
                    recap.totals.shots += stmt.Get<int>(11);
                    recap.totals.demos += stmt.Get<int>(12);
                    const float maxGoalSpeed = static_cast<float>(stmt.Get<double>(14));
                    const float fastestGoal = static_cast<float>(stmt.Get<double>(15));
                    recap.totals.maxGoalSpeed = (std::max)(recap.totals.maxGoalSpeed, maxGoalSpeed);
                    recap.totals.maxGoalSpeedSelf = (std::max)(recap.totals.maxGoalSpeedSelf, maxGoalSpeed);
                    if (fastestGoal > 0.0f) {
                        if (recap.totals.fastestGoalTime <= 0.0f || fastestGoal < recap.totals.fastestGoalTime) {
                            recap.totals.fastestGoalTime = fastestGoal;
                            recap.totals.fastestGoalTimeSelf = fastestGoal;
                        }
                    }
                    recap.totals.boostPickedUp += stmt.Get<int>(16);
                    recap.totals.demoed += stmt.Get<int>(17);
                    recap.totals.crossbars += stmt.Get<int>(18);
                    const float hardestCrossbar = static_cast<float>(stmt.Get<double>(19));
                    const float maxBallSpeed = static_cast<float>(stmt.Get<double>(20));
                    recap.totals.maxImpactForce = (std::max)(recap.totals.maxImpactForce, hardestCrossbar);
                    recap.totals.maxImpactForceSelf = (std::max)(recap.totals.maxImpactForceSelf, hardestCrossbar);
                    recap.totals.maxBallSpeed = (std::max)(recap.totals.maxBallSpeed, maxBallSpeed);
                    recap.totals.maxBallSpeedSelf = (std::max)(recap.totals.maxBallSpeedSelf, maxBallSpeed);
                    recap.totals.ownGoals += stmt.Get<int>(21);
                }

                const bool hasPlaylistId = playlistId >= 0;
                std::string modeKey = gamemode;
                std::string mmrKey;
                if (hasPlaylistId) {
                    const std::string metaKey = PlaylistMetadata::MmrKey(playlistId);
                    if (!metaKey.empty()) {
                        modeKey = metaKey;
                        mmrKey = metaKey;
                    }
                } else if (!gamemode.empty()) {
                    mmrKey = gamemode;
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
        } catch (const std::exception& e) {
            std::cerr << "[Database] RecomputeBackfillSession query failed: " << e.what() << "\n";
            return false;
        }

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

        try {
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
            sqlite::Statement updStmt(db, updSql);
            updStmt.Bind(1, recap.startedAtUnix);
            updStmt.Bind(2, recap.endedAtUnix);
            updStmt.Bind(3, recap.totals.wins);
            updStmt.Bind(4, recap.totals.losses);
            updStmt.Bind(5, mmrJson);
            updStmt.Bind(6, totalsJson);
            updStmt.Bind(7, sessionId);
            updStmt.Execute();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] RecomputeBackfillSession update failed: " << e.what() << "\n";
            return false;
        }
    }

    int SessionRepository::BackfillSessionsChunk(sqlite::Connection& db, int chunkSize, bool& outMoreRemaining, int& outLinked,
                                                 const std::string& preferredAccount,
                                                 const std::vector<std::string>& activeSessionGuids) {
        outMoreRemaining = false;
        outLinked = 0;
        if (!db.IsOpen() || !HasSessionsTable(db)) return 0;
        if (chunkSize <= 0) chunkSize = 500;

        std::unordered_set<std::string> activeGuidsSet(activeSessionGuids.begin(), activeSessionGuids.end());

        std::unordered_map<std::string, int> playerNonOpponentCounts;
        try {
            const char* freqSql = R"(
            SELECT primary_id, COUNT(DISTINCT match_id)
            FROM MatchPlayers
            WHERE COALESCE(is_opponent, 0) = 0
              AND primary_id IS NOT NULL
              AND primary_id != ''
              AND primary_id NOT LIKE 'Unknown|%'
            GROUP BY primary_id;
        )";
            sqlite::Statement freqStmt(db, freqSql);
            while (freqStmt.Step()) {
                playerNonOpponentCounts[freqStmt.Get<std::string>(0)] = freqStmt.Get<int>(1);
            }
        } catch (...) {
        }

        struct UnlinkedMatch {
            int64_t id = 0;
            int64_t endedAtUnix = 0;
            std::string matchGuid;
            std::string account;
        };
        std::vector<UnlinkedMatch> chunk;
        try {
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
            sqlite::Statement mStmt(db, mSql);
            mStmt.Bind(1, chunkSize + 1);
            while (mStmt.Step()) {
                UnlinkedMatch um;
                um.id = mStmt.Get<int64_t>(0);
                um.endedAtUnix = mStmt.Get<int64_t>(1);
                um.matchGuid = mStmt.Get<std::string>(2);
                if (!um.matchGuid.empty() && activeGuidsSet.count(um.matchGuid) > 0) {
                    continue;
                }
                chunk.push_back(std::move(um));
            }
        } catch (...) {
        }

        if (chunk.size() > static_cast<size_t>(chunkSize)) {
            outMoreRemaining = true;
            chunk.resize(static_cast<size_t>(chunkSize));
        }
        if (chunk.empty()) return 0;

        try {
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
            sqlite::Statement pStmt(db, pSql);
            for (auto& um : chunk) {
                pStmt.Reset();
                pStmt.Bind(1, um.id);
                std::string bestPid;
                int bestScore = -1;
                while (pStmt.Step()) {
                    const std::string pid = pStmt.Get<std::string>(0);
                    int score = playerNonOpponentCounts[pid];
                    if (!preferredAccount.empty() && pid == preferredAccount) score += 1'000'000;
                    if (score > bestScore) {
                        bestScore = score;
                        bestPid = pid;
                    }
                }
                um.account = std::move(bestPid);
            }
        } catch (...) {
        }

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            std::map<std::string, std::vector<UnlinkedMatch>> byAccount;
            const char* liveOverlapSql = R"(
            SELECT id FROM Sessions
            WHERE account = ?1 AND source = 'live' AND ?2 BETWEEN started_at AND ended_at
            ORDER BY id DESC LIMIT 1;
        )";
            sqlite::Statement liveOverlapStmt(db, liveOverlapSql);
            sqlite::Statement setMatchSessionStmt(db, "UPDATE Matches SET session_id = ?1 WHERE id = ?2;");

            int linked = 0;
            const auto linkMatch = [&](int64_t sessionId, int64_t matchId) {
                setMatchSessionStmt.Reset();
                setMatchSessionStmt.Bind(1, sessionId);
                setMatchSessionStmt.Bind(2, matchId);
                setMatchSessionStmt.Execute();
                ++linked;
            };

            for (auto& um : chunk) {
                if (um.account.empty()) continue;
                int64_t overlappingLiveId = 0;
                liveOverlapStmt.Reset();
                liveOverlapStmt.Bind(1, um.account);
                liveOverlapStmt.Bind(2, um.endedAtUnix);
                if (liveOverlapStmt.Step()) {
                    overlappingLiveId = liveOverlapStmt.Get<int64_t>(0);
                }
                if (overlappingLiveId > 0) {
                    linkMatch(overlappingLiveId, um.id);
                    continue;
                }
                byAccount[um.account].push_back(std::move(um));
            }

            int sessionsCreated = 0;
            const char* prevBackfillSql = R"(
            SELECT id, COALESCE(ended_at, 0)
            FROM Sessions
            WHERE account = ?1 AND source = 'backfill' AND COALESCE(ended_at, 0) <= ?2
            ORDER BY ended_at DESC, id DESC
            LIMIT 1;
        )";
            sqlite::Statement prevBackfillStmt(db, prevBackfillSql);

            const char* insertSessSql = R"(
            INSERT INTO Sessions (account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source)
            VALUES (?1, ?2, ?3, 0, 0, '{}', '{}', 'backfill');
        )";
            sqlite::Statement insertSessStmt(db, insertSessSql);

            for (const auto& [account, matches] : byAccount) {
                std::vector<int64_t> timestamps;
                timestamps.reserve(matches.size());
                for (const auto& m : matches)
                    timestamps.push_back(m.endedAtUnix);

                const std::vector<Insights::SessionSlice> slices = Insights::GroupSessionTimestamps(timestamps);
                for (size_t sliceIdx = 0; sliceIdx < slices.size(); ++sliceIdx) {
                    const auto& slice = slices[sliceIdx];
                    int64_t targetSessionId = 0;

                    if (sliceIdx == 0) {
                        prevBackfillStmt.Reset();
                        prevBackfillStmt.Bind(1, account);
                        prevBackfillStmt.Bind(2, slice.startedAtUnix);
                        if (prevBackfillStmt.Step()) {
                            const int64_t prevId = prevBackfillStmt.Get<int64_t>(0);
                            const int64_t prevEnded = prevBackfillStmt.Get<int64_t>(1);
                            if (prevEnded > 0 && !Insights::StartsNewSession(prevEnded, slice.startedAtUnix, true)) {
                                targetSessionId = prevId;
                            }
                        }
                    }

                    if (targetSessionId <= 0) {
                        insertSessStmt.Reset();
                        insertSessStmt.Bind(1, account);
                        insertSessStmt.Bind(2, slice.startedAtUnix);
                        insertSessStmt.Bind(3, slice.endedAtUnix);
                        insertSessStmt.Execute();
                        targetSessionId = db.LastInsertRowId();
                        ++sessionsCreated;
                    }

                    for (size_t idx = slice.beginIndex; idx < slice.endIndex; ++idx) {
                        linkMatch(targetSessionId, matches[idx].id);
                    }
                    (void)RecomputeBackfillSession(db, targetSessionId, account);
                }
            }

            tx.Commit();
            outLinked = linked;
            return sessionsCreated;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Session backfill failed: " << e.what() << "\n";
            return 0;
        }
    }

    std::vector<SessionRecap> SessionRepository::ListSessions(sqlite::Connection& db, const std::string& account, int offset, int limit) {
        std::vector<SessionRecap> out;
        if (account.empty() || !db.IsOpen() || !HasSessionsTable(db)) return out;

        const int clampedOffset = (std::max)(0, offset);
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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, account);
            stmt.Bind(2, clampedLimit);
            stmt.Bind(3, clampedOffset);

            while (stmt.Step()) {
                SessionRecap recap;
                recap.valid = true;
                recap.id = stmt.Get<int64_t>(0);
                recap.account = stmt.Get<std::string>(1);
                recap.mmrOwnerPrimaryId = recap.account;
                recap.startedAtUnix = stmt.Get<int64_t>(2);
                recap.endedAtUnix = stmt.Get<int64_t>(3);
                const int wins = stmt.Get<int>(4);
                const int losses = stmt.Get<int>(5);
                const std::string mmrJsonStr = stmt.Get<std::string>(6);
                const std::string totalsJsonStr = stmt.Get<std::string>(7);
                recap.source = stmt.Get<std::string>(8);
                ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
                recap.totals.wins = wins;
                recap.totals.losses = losses;
                out.push_back(std::move(recap));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] ListSessions failed: " << e.what() << "\n";
        }
        return out;
    }

    SessionRecap SessionRepository::GetSession(sqlite::Connection& db, int64_t id) {
        SessionRecap recap;
        if (id <= 0 || !db.IsOpen() || !HasSessionsTable(db)) return recap;

        const char* sql = R"(
        SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0),
               COALESCE(wins, 0), COALESCE(losses, 0),
               COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'),
               COALESCE(source, 'live')
        FROM Sessions
        WHERE id = ?1
        LIMIT 1;
    )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, id);
            if (stmt.Step()) {
                recap.valid = true;
                recap.id = stmt.Get<int64_t>(0);
                recap.account = stmt.Get<std::string>(1);
                recap.mmrOwnerPrimaryId = recap.account;
                recap.startedAtUnix = stmt.Get<int64_t>(2);
                recap.endedAtUnix = stmt.Get<int64_t>(3);
                const int wins = stmt.Get<int>(4);
                const int losses = stmt.Get<int>(5);
                const std::string mmrJsonStr = stmt.Get<std::string>(6);
                const std::string totalsJsonStr = stmt.Get<std::string>(7);
                recap.source = stmt.Get<std::string>(8);
                ParseSessionRecapJson(mmrJsonStr, totalsJsonStr, recap);
                recap.totals.wins = wins;
                recap.totals.losses = losses;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetSession failed: " << e.what() << "\n";
        }
        return recap;
    }

} // namespace database::repository
