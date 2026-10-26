#include "MatchRepository.hpp"
#include "core/config/Config.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include "network/telemetry/TelemetryManager.hpp"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <utility>

namespace database::repository {

    bool MatchRepository::UpsertMatchStats(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot) {
        if (!db.TableExists("MatchPlayerStats") || !db.TableExists("MatchLocalStats")) {
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

        try {
            sqlite::Statement stmtPlayerStats(db, sqlPlayerStats);
            for (const auto& [id, p] : snapshot.roster) {
                const std::string& primaryId = p.primaryId.empty() ? id : p.primaryId;
                if (primaryId.empty()) continue;

                stmtPlayerStats.Reset();
                stmtPlayerStats.Bind(1, matchId);
                stmtPlayerStats.Bind(2, primaryId);
                if (p.score >= 0)
                    stmtPlayerStats.Bind(3, p.score);
                else
                    stmtPlayerStats.BindNull(3);
                stmtPlayerStats.Bind(4, p.goals);
                stmtPlayerStats.Bind(5, p.assists);
                stmtPlayerStats.Bind(6, p.saves);
                stmtPlayerStats.Bind(7, p.shots);
                stmtPlayerStats.Bind(8, p.demos);
                if (p.touches.has_value())
                    stmtPlayerStats.Bind(9, *p.touches);
                else
                    stmtPlayerStats.BindNull(9);
                if (p.carTouches.has_value())
                    stmtPlayerStats.Bind(10, *p.carTouches);
                else
                    stmtPlayerStats.BindNull(10);
                if (p.maxGoalSpeed > 0.0f)
                    stmtPlayerStats.Bind(11, p.maxGoalSpeed);
                else
                    stmtPlayerStats.BindNull(11);
                if (p.fastestGoalTime > 0.0f)
                    stmtPlayerStats.Bind(12, p.fastestGoalTime);
                else
                    stmtPlayerStats.BindNull(12);

                stmtPlayerStats.Execute();
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to upsert MatchPlayerStats: " << e.what() << "\n";
            return false;
        }

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

        try {
            sqlite::Statement stmtLocalStats(db, sqlLocalStats);
            const auto& local = snapshot.localStats;
            stmtLocalStats.Bind(1, matchId);
            stmtLocalStats.Bind(2, local.boostPickedUpSelf);
            stmtLocalStats.Bind(3, local.demoedSelf);
            stmtLocalStats.Bind(4, local.crossbarsSelf);
            if (local.maxImpactForceSelf > 0.0f)
                stmtLocalStats.Bind(5, local.maxImpactForceSelf);
            else
                stmtLocalStats.BindNull(5);
            if (local.maxBallSpeedSelf > 0.0f)
                stmtLocalStats.Bind(6, local.maxBallSpeedSelf);
            else
                stmtLocalStats.BindNull(6);
            stmtLocalStats.Bind(7, local.ownGoalsSelf);
            if (snapshot.durationSeconds > 0.0f)
                stmtLocalStats.Bind(8, snapshot.durationSeconds);
            else
                stmtLocalStats.BindNull(8);
            if (snapshot.overtimeSeconds > 0.0f)
                stmtLocalStats.Bind(9, snapshot.overtimeSeconds);
            else
                stmtLocalStats.BindNull(9);

            stmtLocalStats.Execute();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to upsert MatchLocalStats: " << e.what() << "\n";
            return false;
        }
    }

    bool MatchRepository::InsertMatchEvents(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot) {
        if (!db.TableExists("MatchEvents")) return snapshot.events.empty();

        const char* sql = R"(
        INSERT OR IGNORE INTO MatchEvents
          (match_id, match_guid, event_type, observed_at_ms, match_clock_seconds,
           actor_id, actor_name, secondary_id, secondary_name, team,
           value, ball_speed, source, event_key)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);
    )";

        try {
            sqlite::Statement stmt(db, sql);
            size_t count = 0;
            for (const auto& event : snapshot.events) {
                if (++count > 512) break;
                stmt.Reset();
                stmt.Bind(1, matchId);
                stmt.Bind(2, snapshot.matchGuid);
                stmt.Bind(3, event.type);
                stmt.Bind(4, event.observedAtUnixMs);
                if (event.matchClockSeconds)
                    stmt.Bind(5, *event.matchClockSeconds);
                else
                    stmt.BindNull(5);
                stmt.Bind(6, event.actorId);
                stmt.Bind(7, event.actorName);
                stmt.Bind(8, event.secondaryId);
                stmt.Bind(9, event.secondaryName);
                if (event.team)
                    stmt.Bind(10, *event.team);
                else
                    stmt.BindNull(10);
                if (event.value)
                    stmt.Bind(11, *event.value);
                else
                    stmt.BindNull(11);
                if (event.ballSpeed)
                    stmt.Bind(12, *event.ballSpeed);
                else
                    stmt.BindNull(12);
                stmt.Bind(13, event.source);
                if (!event.eventKey.empty())
                    stmt.Bind(14, event.eventKey);
                else
                    stmt.BindNull(14);

                stmt.Execute();
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to insert MatchEvents: " << e.what() << "\n";
            return false;
        }
    }

    bool MatchRepository::UpsertMatchAnalytics(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot) {
        if (!snapshot.analytics.has_value() || snapshot.analytics->Empty()) {
            return true;
        }
        if (!db.TableExists("MatchAnalytics")) {
            return true;
        }

        const char* sql = R"(
        INSERT OR REPLACE INTO MatchAnalytics (match_id, format_version, data)
        VALUES (?, ?, ?);
    )";

        try {
            sqlite::Statement stmt(db, sql);
            const std::string text = MatchAnalyticsToJson(*snapshot.analytics).dump();
            stmt.Bind(1, matchId);
            stmt.Bind(2, snapshot.analytics->formatVersion);
            stmt.Bind(3, text);
            stmt.Execute();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to upsert MatchAnalytics row: " << e.what() << "\n";
            return false;
        }
    }

    void MatchRepository::ResolvePendingMatch(sqlite::Connection& db, int64_t matchId, const MatchSaveSnapshot& snapshot, bool hasStatsTables) {
        const bool validResult =
            snapshot.validResult &&
            (snapshot.myTeam == 0 || snapshot.myTeam == 1) &&
            (snapshot.winnerTeam == 0 || snapshot.winnerTeam == 1);

        if (!validResult) {
            try {
                sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
                if (hasStatsTables) {
                    for (const char* sql : {"DELETE FROM MatchEvents WHERE match_id = ?;",
                                            "DELETE FROM MatchPlayerStats WHERE match_id = ?;",
                                            "DELETE FROM MatchLocalStats WHERE match_id = ?;"}) {
                        sqlite::Statement stmt(db, sql);
                        stmt.Bind(1, matchId);
                        stmt.Execute();
                    }
                }
                if (db.TableExists("MatchAnalytics")) {
                    sqlite::Statement stmt(db, "DELETE FROM MatchAnalytics WHERE match_id = ?;");
                    stmt.Bind(1, matchId);
                    stmt.Execute();
                }
                for (const char* sql : {"DELETE FROM MatchPlayers WHERE match_id = ?;",
                                        "DELETE FROM Matches WHERE id = ?;"}) {
                    sqlite::Statement stmt(db, sql);
                    stmt.Bind(1, matchId);
                    stmt.Execute();
                }
                tx.Commit();
                std::cout << "[Database] Removed pending match voided on confirmation. ID: " << matchId
                          << (snapshot.voidReason.empty() ? "" : ", reason=" + snapshot.voidReason) << "\n";
            } catch (const std::exception& e) {
                std::cerr << "[Database] Failed to remove voided pending match: " << e.what() << "\n";
            }
            return;
        }

        const bool win = snapshot.winnerTeam == snapshot.myTeam;
        const int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
        const int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            const char* sql = "UPDATE Matches SET win = ?, our_score = ?, their_score = ?, result_pending = 0 WHERE id = ?;";
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, win ? 1 : 0);
            stmt.Bind(2, ourScore);
            stmt.Bind(3, theirScore);
            stmt.Bind(4, matchId);
            stmt.Execute();

            if (UpsertMatchStats(db, matchId, snapshot) &&
                InsertMatchEvents(db, matchId, snapshot) &&
                UpsertMatchAnalytics(db, matchId, snapshot)) {
                tx.Commit();
                std::cout << "Pending match confirmed in database. ID: " << matchId
                          << " Result: " << (win ? "win" : "loss") << "\n";
            } else {
                tx.Rollback();
                std::cerr << "[Database] Failed to confirm pending match.\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to confirm pending match: " << e.what() << "\n";
        }
    }

    void MatchRepository::SaveMatch(sqlite::Connection& db, const MatchSaveSnapshot& snapshot, bool hasStatsTables, TelemetryManager* telemetry) {
        if (!db.IsOpen() || snapshot.arenaName.empty()) return;

        int64_t pendingMatchId = 0;
        if (!snapshot.matchGuid.empty()) {
            try {
                const char* duplicateSql =
                    "SELECT id, COALESCE(result_pending, 0) FROM Matches "
                    "WHERE match_guid = ? ORDER BY id DESC LIMIT 1;";
                sqlite::Statement duplicateCheck(db, duplicateSql);
                duplicateCheck.Bind(1, snapshot.matchGuid);
                bool alreadySaved = false;
                if (duplicateCheck.Step()) {
                    alreadySaved = true;
                    if (duplicateCheck.Get<int>(1) != 0) {
                        pendingMatchId = duplicateCheck.Get<int64_t>(0);
                    }
                }
                if (alreadySaved && (pendingMatchId == 0 || snapshot.resultPending)) {
                    std::cout << "[Database] Skipping duplicate match GUID.\n";
                    return;
                }
            } catch (...) {
            }
        }

        if (pendingMatchId != 0) {
            ResolvePendingMatch(db, pendingMatchId, snapshot, hasStatsTables);
            return;
        }

        if (!snapshot.validResult) {
            std::cout << "[Database] Skipping void match"
                      << (snapshot.voidReason.empty() ? "" : ": " + snapshot.voidReason) << "\n";
            return;
        }

        if (snapshot.myTeam != 0 && snapshot.myTeam != 1) return;
        if (snapshot.winnerTeam != 0 && snapshot.winnerTeam != 1) return;

        const bool win = (snapshot.winnerTeam == snapshot.myTeam);
        const int ourScore = snapshot.myTeam == 1 ? snapshot.score[1] : snapshot.score[0];
        const int theirScore = snapshot.myTeam == 1 ? snapshot.score[0] : snapshot.score[1];

        const bool hasPlaylistId = PlaylistMetadata::HasAuthoritativeId(snapshot.playlistId);
        const int playerCount = hasPlaylistId ? 0 : (snapshot.legacyPlayerCount > 0 ? snapshot.legacyPlayerCount : static_cast<int>(snapshot.roster.size()));
        const std::string arenaKey = !snapshot.arenaAsset.empty() ? snapshot.arenaAsset : snapshot.arenaName;

        std::string gamemode;
        std::string mmrKey;
        if (hasPlaylistId) {
            if (PlaylistMetadata::IsKnown(snapshot.playlistId)) {
                gamemode = PlaylistMetadata::StorageMode(snapshot.playlistId);
                mmrKey = PlaylistMetadata::MmrKey(snapshot.playlistId);
            } else {
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

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            const char* sqlMatches =
                "INSERT INTO Matches (arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, timestamp, result_pending) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, COALESCE(strftime('%Y-%m-%d %H:%M:%f', NULLIF(?, 0) / 1000.0, 'unixepoch'), CURRENT_TIMESTAMP), ?);";

            sqlite::Statement stmtMatches(db, sqlMatches);
            stmtMatches.Bind(1, snapshot.arenaName);
            stmtMatches.Bind(2, ourScore);
            stmtMatches.Bind(3, theirScore);
            stmtMatches.Bind(4, win ? 1 : 0);
            stmtMatches.Bind(5, snapshot.matchGuid);
            if (hasPlaylistId)
                stmtMatches.Bind(6, snapshot.playlistId);
            else
                stmtMatches.BindNull(6);
            stmtMatches.Bind(7, gamemode);
            stmtMatches.Bind(8, playerCount);
            stmtMatches.Bind(9, snapshot.endedAtUnixMs);
            stmtMatches.Bind(10, snapshot.resultPending ? 1 : 0);

            stmtMatches.Execute();
            const int64_t matchId = db.LastInsertRowId();

            const char* sqlPlayers = "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) VALUES (?, ?, ?, ?, ?, ?, ?);";
            sqlite::Statement stmtPlayers(db, sqlPlayers);

            auto insertPlayerRow = [&](const PlayerData& p) {
                const bool isOpponent = (p.team != snapshot.myTeam);
                stmtPlayers.Reset();
                stmtPlayers.Bind(1, matchId);
                stmtPlayers.Bind(2, p.primaryId);
                stmtPlayers.Bind(3, p.name);
                stmtPlayers.Bind(4, p.team);
                int mmrVal = p.mmr;
                if (!mmrKey.empty() && p.playlists.count(mmrKey)) {
                    mmrVal = p.playlists.at(mmrKey);
                }
                stmtPlayers.Bind(5, mmrVal);
                const bool mmrEstimated = snapshot.localMmrNeedsReconciliation && p.primaryId == snapshot.myPrimaryId;
                stmtPlayers.Bind(6, mmrEstimated ? 1 : 0);
                stmtPlayers.Bind(7, isOpponent ? 1 : 0);
                stmtPlayers.Execute();
            };

            if (!snapshot.myPrimaryId.empty()) {
                const auto selfIt = snapshot.roster.find(snapshot.myPrimaryId);
                if (selfIt != snapshot.roster.end()) {
                    insertPlayerRow(selfIt->second);
                }
            }
            for (const auto& [id, p] : snapshot.roster) {
                if (!snapshot.myPrimaryId.empty() && id == snapshot.myPrimaryId) continue;
                insertPlayerRow(p);
            }

            if (!UpsertMatchStats(db, matchId, snapshot) ||
                !InsertMatchEvents(db, matchId, snapshot) ||
                !UpsertMatchAnalytics(db, matchId, snapshot)) {
                tx.Rollback();
                return;
            }

            tx.Commit();
            std::cout << "Match saved to database. ID: " << matchId << " Gamemode: " << gamemode << "\n";
            if (telemetry) telemetry->RecordMilestone(TelemetryManager::Milestone::FirstMatchTracked);
        } catch (const std::exception& e) {
            std::cerr << "Failed to save match: " << e.what() << "\n";
        }
    }

    bool MatchRepository::UpdateMatchPlayerMmr(sqlite::Connection& db, const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated) {
        if (!db.IsOpen() || matchGuid.empty() || primaryId.empty() || mmr <= 0) return false;

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, mmr);
            stmt.Bind(2, estimated ? 1 : 0);
            stmt.Bind(3, primaryId);
            stmt.Bind(4, matchGuid);
            stmt.Execute();
            return db.Changes() > 0;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to update post-match MMR: " << e.what() << "\n";
            return false;
        }
    }

} // namespace database::repository
