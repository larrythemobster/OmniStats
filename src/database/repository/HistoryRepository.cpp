#include "HistoryRepository.hpp"
#include "core/config/Config.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include "network/mmr/MmrService.hpp"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <nlohmann/json.hpp>
#include <utility>

namespace database::repository {

    namespace {

        std::string FormatPlaylistName(int playerCount) {
            if (playerCount <= 0) return "Unknown";
            if (playerCount <= 2) return "Duel";
            if (playerCount <= 4) return "Doubles";
            if (playerCount <= 6) return "Standard";
            if (playerCount <= 8) return "Chaos";
            return "Unknown";
        }

        std::string FormatMatchHistoryMode(const std::string& gamemode, int playerCount) {
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

        std::string DescribeMatchPlaylist(const sqlite::Statement& stmt, int gamemodeColumn, int playerCountColumn, int playlistIdColumn, bool& ranked) {
            if (!stmt.IsNull(playlistIdColumn)) {
                const int playlistId = stmt.Get<int>(playlistIdColumn);
                if (const auto* info = PlaylistMetadata::Find(playlistId)) {
                    ranked = info->playlistClass == PlaylistMetadata::PlaylistClass::Ranked ||
                             info->playlistClass == PlaylistMetadata::PlaylistClass::Tournament;
                    return info->playlistClass == PlaylistMetadata::PlaylistClass::Casual ? "Casual" : info->displayName;
                }
                ranked = false;
                return "Unknown Playlist (" + std::to_string(playlistId) + ")";
            }
            const std::string gamemode = stmt.Get<std::string>(gamemodeColumn);
            ranked = gamemode != "casual";
            return FormatMatchHistoryMode(gamemode, stmt.Get<int>(playerCountColumn));
        }

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

        std::string NormalizeStatSeriesPlaylistFilter(const std::string& filter) {
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

        bool HasStatsTables(sqlite::Connection& db) {
            return db.TableExists("MatchPlayerStats") && db.TableExists("MatchLocalStats");
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

        void BindParams(sqlite::Statement& stmt, const std::vector<BoundSqlParam>& params) {
            for (size_t i = 0; i < params.size(); ++i) {
                const int idx = static_cast<int>(i + 1);
                const auto& p = params[i];
                switch (p.kind) {
                case BoundSqlParam::Kind::Text:
                    stmt.Bind(idx, p.text);
                    break;
                case BoundSqlParam::Kind::Int:
                    stmt.Bind(idx, p.intVal);
                    break;
                case BoundSqlParam::Kind::Int64:
                    stmt.Bind(idx, p.int64Val);
                    break;
                }
            }
        }

    } // namespace

    int HistoryRepository::GetSchemaVersion(sqlite::Connection& db) {
        if (!db.IsOpen()) return 0;
        return db.UserVersion();
    }

    void HistoryRepository::GetLifetimeMmrHistory(sqlite::Connection& db, const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY) {
        outX.clear();
        outY.clear();
        if (!db.IsOpen() || primaryId.empty()) return;

        bool filtered = (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" || playlist == "casual" || playlist == "t" ||
                         playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" || playlist == "snowday" ||
                         playlist == "heatseeker");

        const char* sql = filtered ? R"(
        SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?1 AND MatchPlayers.mmr > 0
          AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
          AND Matches.gamemode = ?2
        ORDER BY Matches.timestamp ASC;
    )"
                                   : R"(
        SELECT strftime('%s', Matches.timestamp) as epoch, MatchPlayers.mmr
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?1 AND MatchPlayers.mmr > 0
          AND COALESCE(MatchPlayers.mmr_estimated, 0) = 0
        ORDER BY Matches.timestamp ASC;
    )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            if (filtered) {
                stmt.Bind(2, playlist);
            }
            while (stmt.Step()) {
                int64_t epoch = stmt.Get<int64_t>(0);
                float mmr = static_cast<float>(stmt.Get<int>(1));
                outX.push_back(epoch);
                outY.push_back(mmr);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetLifetimeMmrHistory failed: " << e.what() << "\n";
        }
    }

    void HistoryRepository::GetRecentMatchHistory(sqlite::Connection& db, const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit) {
        outMatches.clear();
        if (!db.IsOpen() || primaryId.empty()) return;

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            stmt.Bind(2, limit);

            while (stmt.Step()) {
                SessionMatchSummary summary;
                summary.matchId = stmt.Get<int64_t>(10);
                summary.mode = DescribeMatchPlaylist(stmt, 3, 4, 8, summary.ranked);
                summary.matchGuid = stmt.Get<std::string>(7);
                summary.ourScore = stmt.Get<int>(0);
                summary.theirScore = stmt.Get<int>(1);
                summary.mmr = stmt.Get<int>(6);
                summary.mmrEstimated = stmt.Get<int>(9) != 0;
                summary.win = stmt.Get<int>(2) != 0;
                summary.endedAtUnix = stmt.Get<int64_t>(5);
                outMatches.push_back(std::move(summary));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetRecentMatchHistory failed: " << e.what() << "\n";
        }
    }

    std::optional<std::vector<PersonRecord>> HistoryRepository::QueryEncounterRecords(sqlite::Connection& db, const std::string& account,
                                                                                      const std::string& onlyPlayer,
                                                                                      int64_t excludeMatchId) {
        if (!db.IsOpen() || account.empty()) return std::nullopt;

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, account);
            if (!onlyPlayer.empty()) stmt.Bind(2, onlyPlayer);
            if (excludeMatchId > 0) stmt.Bind(3, excludeMatchId);

            std::vector<PersonRecord> people;
            while (stmt.Step()) {
                PersonRecord person;
                person.primaryId = stmt.Get<std::string>(0);
                person.name = stmt.Get<std::string>(1);
                person.winsWith = stmt.Get<int>(2);
                person.lossesWith = stmt.Get<int>(3);
                person.winsAgainst = stmt.Get<int>(4);
                person.lossesAgainst = stmt.Get<int>(5);
                person.lastSeenUnix = stmt.Get<int64_t>(6);
                people.push_back(std::move(person));
            }
            return people;
        } catch (const std::exception& e) {
            std::cerr << "[Database] Encounter query failed: " << e.what() << "\n";
            return std::nullopt;
        }
    }

    std::optional<PersonRecord> HistoryRepository::GetPlayerEncounterRecord(sqlite::Connection& db, const std::string& account, const std::string& primaryId) {
        if (primaryId.empty()) return std::nullopt;
        auto records = QueryEncounterRecords(db, account, primaryId);
        if (!records) return std::nullopt;
        if (!records->empty()) return std::move(records->front());
        PersonRecord none;
        none.primaryId = primaryId;
        return none;
    }

    void HistoryRepository::GetPeopleRecords(sqlite::Connection& db, const std::string& primaryId, std::vector<PersonRecord>& outPeople) {
        outPeople.clear();
        if (auto records = QueryEncounterRecords(db, primaryId)) {
            outPeople = std::move(*records);
        }
    }

    void HistoryRepository::GetMatchOutcomes(sqlite::Connection& db, const std::string& primaryId, std::vector<MatchOutcome>& outMatches) {
        outMatches.clear();
        if (!db.IsOpen() || primaryId.empty()) return;

        const char* sql = R"(
        SELECT strftime('%s', Matches.timestamp),
               CAST(strftime('%H', Matches.timestamp, 'localtime') AS INTEGER),
               Matches.win, Matches.gamemode, Matches.player_count, Matches.playlist_id
        FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id AND MatchPlayers.primary_id = ?
        WHERE COALESCE(Matches.result_pending, 0) = 0
        ORDER BY Matches.timestamp ASC, Matches.id ASC;
    )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            while (stmt.Step()) {
                MatchOutcome outcome;
                outcome.endedAtUnix = stmt.Get<int64_t>(0);
                outcome.localHour = stmt.Get<int>(1);
                outcome.win = stmt.Get<int>(2) != 0;
                bool ranked = false;
                outcome.playlist = DescribeMatchPlaylist(stmt, 3, 4, 5, ranked);
                outMatches.push_back(std::move(outcome));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetMatchOutcomes failed: " << e.what() << "\n";
        }
    }

    void HistoryRepository::GetMatchMmrContext(sqlite::Connection& db, const std::string& primaryId, std::vector<MatchMmrContext>& out) {
        out.clear();
        if (!db.IsOpen() || primaryId.empty()) return;

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            while (stmt.Step()) {
                MatchMmrContext ctx;
                bool ranked = false;
                std::string playlist = DescribeMatchPlaylist(stmt, 2, 3, 1, ranked);
                ctx.playlist = ranked ? std::move(playlist) : "Casual";
                ctx.win = stmt.Get<int>(4) != 0;
                ctx.endedAtUnix = stmt.Get<int64_t>(5);
                ctx.myMmr = stmt.Get<int>(6);
                ctx.mmrEstimated = stmt.Get<int>(7) != 0;
                ctx.teamAvg = stmt.Get<double>(8);
                ctx.oppAvg = stmt.Get<double>(9);
                ctx.teamCount = stmt.Get<int>(10);
                ctx.oppCount = stmt.Get<int>(11);
                out.push_back(std::move(ctx));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetMatchMmrContext failed: " << e.what() << "\n";
        }
    }

    void HistoryRepository::GetGamemodeStats(sqlite::Connection& db, const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed) {
        wins = 0;
        losses = 0;
        gamesPlayed = 0;
        if (!db.IsOpen() || primaryId.empty()) return;

        const char* sql = R"(
        SELECT
            SUM(CASE WHEN Matches.win = 1 THEN 1 ELSE 0 END),
            SUM(CASE WHEN Matches.win = 0 THEN 1 ELSE 0 END),
            COUNT(*)
        FROM MatchPlayers
        JOIN Matches ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?1 AND Matches.gamemode = ?2
          AND COALESCE(Matches.result_pending, 0) = 0;
    )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            stmt.Bind(2, gamemode);
            if (stmt.Step()) {
                wins = stmt.IsNull(0) ? 0 : stmt.Get<int>(0);
                losses = stmt.IsNull(1) ? 0 : stmt.Get<int>(1);
                gamesPlayed = stmt.IsNull(2) ? 0 : stmt.Get<int>(2);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetGamemodeStats failed: " << e.what() << "\n";
        }
    }

    void HistoryRepository::GetStreakStats(sqlite::Connection& db, const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss) {
        outCurWin = 0;
        outCurLoss = 0;
        outLongestWin = 0;
        outLongestLoss = 0;
        if (!db.IsOpen() || primaryId.empty()) return;

        const char* sql = R"(
        SELECT Matches.win FROM Matches
        JOIN MatchPlayers ON MatchPlayers.match_id = Matches.id
        WHERE MatchPlayers.primary_id = ?1 AND COALESCE(Matches.result_pending, 0) = 0
        ORDER BY Matches.timestamp DESC, Matches.id DESC;
    )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);

            int previous = -1;
            int runLength = 0;
            bool initialRun = true;
            while (stmt.Step()) {
                const int win = stmt.Get<int>(0);
                if (win != previous) {
                    if (previous != -1) initialRun = false;
                    previous = win;
                    runLength = 0;
                }
                ++runLength;
                if (win == 1) {
                    if (initialRun) outCurWin = runLength;
                    outLongestWin = (std::max)(outLongestWin, runLength);
                } else {
                    if (initialRun) outCurLoss = runLength;
                    outLongestLoss = (std::max)(outLongestLoss, runLength);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] Failed to read streak history: " << e.what() << "\n";
            outCurWin = outCurLoss = outLongestWin = outLongestLoss = 0;
        }
    }

    std::vector<PlayerStatRow> HistoryRepository::GetPlayerStatSeries(sqlite::Connection& db, const std::string& primaryId,
                                                                      const std::string& playlistFilter,
                                                                      int limit) {
        std::vector<PlayerStatRow> rows;
        if (!db.IsOpen() || primaryId.empty() || !HasStatsTables(db)) return rows;

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            if (filtered) {
                stmt.Bind(2, normalizedFilter);
                stmt.Bind(3, limit);
            } else {
                stmt.Bind(2, limit);
            }

            while (stmt.Step()) {
                PlayerStatRow row;
                row.matchId = stmt.Get<int64_t>(0);
                row.matchGuid = stmt.Get<std::string>(1);
                row.timestampUnix = stmt.Get<int64_t>(2);
                row.playlist = stmt.Get<std::string>(3);
                row.playlistId = stmt.Get<int>(4);
                row.win = stmt.Get<int>(5) != 0;
                row.ourScore = stmt.Get<int>(6);
                row.theirScore = stmt.Get<int>(7);
                row.mmr = stmt.Get<int>(8);
                if (!stmt.IsNull(9)) row.score = stmt.Get<int>(9);
                row.goals = stmt.Get<int>(10);
                row.assists = stmt.Get<int>(11);
                row.saves = stmt.Get<int>(12);
                row.shots = stmt.Get<int>(13);
                row.demos = stmt.Get<int>(14);
                if (!stmt.IsNull(15)) row.touches = stmt.Get<int>(15);
                if (!stmt.IsNull(16)) row.carTouches = stmt.Get<int>(16);
                if (!stmt.IsNull(17)) row.maxGoalSpeed = static_cast<float>(stmt.Get<double>(17));
                if (!stmt.IsNull(18)) row.fastestGoalTime = static_cast<float>(stmt.Get<double>(18));

                row.hasLocalStats = !stmt.IsNull(19);
                row.boostCollected = stmt.Get<int>(20);
                row.demoed = stmt.Get<int>(21);
                row.crossbars = stmt.Get<int>(22);
                if (!stmt.IsNull(23)) row.hardestCrossbar = static_cast<float>(stmt.Get<double>(23));
                if (!stmt.IsNull(24)) row.maxBallSpeed = static_cast<float>(stmt.Get<double>(24));
                row.ownGoals = stmt.Get<int>(25);
                if (!stmt.IsNull(26)) row.durationSeconds = static_cast<float>(stmt.Get<double>(26));
                if (!stmt.IsNull(27)) row.overtimeSeconds = static_cast<float>(stmt.Get<double>(27));
                row.statsVersion = stmt.Get<int>(28);

                rows.push_back(std::move(row));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetPlayerStatSeries failed: " << e.what() << "\n";
        }
        return rows;
    }

    void HistoryRepository::GetDetailedStatsSummary(sqlite::Connection& db, int& outMatchCount, std::string& outSinceDate) {
        outMatchCount = 0;
        outSinceDate.clear();
        if (!db.IsOpen() || !HasStatsTables(db)) return;

        const char* sql = R"(
        SELECT COUNT(*), COALESCE(MIN(substr(Matches.timestamp, 1, 10)), '')
        FROM Matches
        WHERE EXISTS (SELECT 1 FROM MatchLocalStats s WHERE s.match_id = Matches.id)
           OR EXISTS (SELECT 1 FROM MatchPlayerStats p WHERE p.match_id = Matches.id);
    )";

        try {
            sqlite::Statement stmt(db, sql);
            if (stmt.Step()) {
                outMatchCount = stmt.Get<int>(0);
                if (outMatchCount > 0) {
                    outSinceDate = stmt.Get<std::string>(1);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetDetailedStatsSummary failed: " << e.what() << "\n";
        }
    }

    void HistoryRepository::QueryMatches(sqlite::Connection& db, const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount) {
        outRows.clear();
        outTotalCount = 0;
        if (query.account.empty() || !db.IsOpen()) return;

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
        const int offset = (std::max)(0, query.offset);
        const int pLimit = addInt(limit);
        const int pOffset = addInt(offset);

        const bool hasStats = HasStatsTables(db);
        const bool hasReplays = db.TableExists("ReplayUploads");
        const bool hasAnalytics = db.TableExists("MatchAnalytics");

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
            "Paged AS ("
            "    SELECT id, timestamp, arena, our_score, their_score, win, match_guid,"
            "           playlist_id, gamemode, player_count, my_team, my_mmr, mmr_estimated,"
            "           mmr_delta, total_count"
            "    FROM Filtered " +
            orderSql +
            "    LIMIT ?" + std::to_string(pLimit) + " OFFSET ?" + std::to_string(pOffset) +
            ") "
            "SELECT p.id, CAST(strftime('%s', p.timestamp) AS INTEGER), p.arena,"
            "       p.our_score, p.their_score, p.win, p.match_guid, p.playlist_id,"
            "       p.gamemode, p.player_count, p.my_mmr, p.mmr_estimated, p.mmr_delta,"
            "       p.total_count, p.my_team, ";
        if (hasReplays) {
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
        sql += "FROM Paged p ";
        if (hasReplays) {
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

        try {
            sqlite::Statement stmt(db, sql);
            BindParams(stmt, params);

            std::vector<int64_t> pageMatchIds;
            while (stmt.Step()) {
                MatchRow row;
                row.matchId = stmt.Get<int64_t>(0);
                row.timestampUnix = stmt.Get<int64_t>(1);
                row.arena = stmt.Get<std::string>(2);
                row.ourScore = stmt.Get<int>(3);
                row.theirScore = stmt.Get<int>(4);
                row.win = stmt.Get<int>(5) != 0;
                row.matchGuid = stmt.Get<std::string>(6);
                row.playlistId = stmt.IsNull(7) ? -1 : stmt.Get<int>(7);
                row.gamemode = stmt.Get<std::string>(8);
                row.playlist = DescribeMatchPlaylist(stmt, 8, 9, 7, row.ranked);
                row.myMmr = stmt.Get<int>(10);
                row.mmrEstimated = stmt.Get<int>(11) != 0;
                if (!stmt.IsNull(12)) {
                    row.mmrDelta = stmt.Get<int>(12);
                }
                if (outTotalCount == 0 && !stmt.IsNull(13)) {
                    outTotalCount = stmt.Get<int>(13);
                }
                const int myTeam = stmt.Get<int>(14);
                if (!stmt.IsNull(15)) {
                    row.ballchasingUrl = stmt.Get<std::string>(15);
                    row.hasBallchasing = !row.ballchasingUrl.empty();
                }
                if (!stmt.IsNull(16)) row.myScore = stmt.Get<int>(16);
                if (!stmt.IsNull(17)) row.myGoals = stmt.Get<int>(17);
                if (!stmt.IsNull(18)) row.myAssists = stmt.Get<int>(18);
                if (!stmt.IsNull(19)) row.mySaves = stmt.Get<int>(19);
                if (!stmt.IsNull(20)) row.myShots = stmt.Get<int>(20);
                row.mvp = stmt.Get<int>(21) != 0;
                row.overtime = stmt.Get<int>(22) != 0;

                if (!stmt.IsNull(23)) {
                    std::string analyticsData = stmt.Get<std::string>(23);
                    auto parsed = nlohmann::json::parse(analyticsData, nullptr, false);
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

                pageMatchIds.push_back(row.matchId);
                outRows.push_back(std::move(row));
            }

            if (outRows.empty() && offset > 0) {
                const std::string countSql =
                    "SELECT COUNT(*) FROM ("
                    "    SELECT m.id FROM MatchPlayers me"
                    "    JOIN Matches m ON m.id = me.match_id"
                    "    WHERE me.primary_id = ?1 AND COALESCE(m.result_pending, 0) = 0" +
                    whereSql +
                    ");";
                try {
                    sqlite::Statement countStmt(db, countSql);
                    BindParams(countStmt, filterParams);
                    if (countStmt.Step()) {
                        outTotalCount = countStmt.Get<int>(0);
                    }
                } catch (...) {
                }
            }

            if (!pageMatchIds.empty()) {
                std::string inList;
                for (size_t i = 0; i < pageMatchIds.size(); ++i) {
                    if (i > 0) inList += ",";
                    inList += std::to_string(pageMatchIds[i]);
                }
                const std::string tmSql =
                    "SELECT match_id, name FROM MatchPlayers "
                    "WHERE match_id IN (" +
                    inList + ") AND COALESCE(is_opponent, 0) = 0 AND primary_id != ?1 "
                             "ORDER BY match_id, id;";
                try {
                    sqlite::Statement tmStmt(db, tmSql);
                    tmStmt.Bind(1, query.account);
                    std::unordered_map<int64_t, std::vector<std::string>> teammatesByMatch;
                    while (tmStmt.Step()) {
                        std::string name = tmStmt.Get<std::string>(1);
                        if (!name.empty()) teammatesByMatch[tmStmt.Get<int64_t>(0)].push_back(std::move(name));
                    }
                    for (auto& row : outRows) {
                        if (auto it = teammatesByMatch.find(row.matchId); it != teammatesByMatch.end()) {
                            row.teammates = std::move(it->second);
                        }
                    }
                } catch (...) {
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] QueryMatches failed: " << e.what() << "\n";
        }
    }

    std::vector<AnalyticsMatchRow> HistoryRepository::GetAnalyticsMatches(sqlite::Connection& db, const std::string& primaryId, int limit) {
        std::vector<AnalyticsMatchRow> rows;
        if (primaryId.empty() || !db.IsOpen()) return rows;
        if (limit <= 0) limit = 100;

        const bool hasStats = HasStatsTables(db);
        const bool hasAnalytics = db.TableExists("MatchAnalytics");

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

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            stmt.Bind(2, limit);

            while (stmt.Step()) {
                AnalyticsMatchRow row;
                row.matchId = stmt.Get<int64_t>(0);
                row.timestampUnix = stmt.Get<int64_t>(1);
                row.arena = stmt.Get<std::string>(2);
                row.ourScore = stmt.Get<int>(3);
                row.theirScore = stmt.Get<int>(4);
                row.win = stmt.Get<int>(5) != 0;
                row.playlistId = stmt.IsNull(6) ? -1 : stmt.Get<int>(6);
                row.playlist = DescribeMatchPlaylist(stmt, 7, 8, 6, row.ranked);
                row.myTeam = stmt.Get<int>(9);

                if (!stmt.IsNull(10)) {
                    if (!stmt.IsNull(11)) row.score = stmt.Get<int>(11);
                    if (!stmt.IsNull(12)) row.goals = stmt.Get<int>(12);
                    if (!stmt.IsNull(13)) row.assists = stmt.Get<int>(13);
                    if (!stmt.IsNull(14)) row.saves = stmt.Get<int>(14);
                    if (!stmt.IsNull(15)) row.shots = stmt.Get<int>(15);
                    if (!stmt.IsNull(16)) row.demos = stmt.Get<int>(16);
                    if (!stmt.IsNull(17)) row.touches = stmt.Get<int>(17);
                }

                if (!stmt.IsNull(18)) {
                    MatchLocalStatsRow lstats;
                    lstats.boostCollected = stmt.Get<int>(19);
                    lstats.demoed = stmt.Get<int>(20);
                    lstats.crossbars = stmt.Get<int>(21);
                    if (!stmt.IsNull(22)) {
                        lstats.hardestCrossbar = static_cast<float>(stmt.Get<double>(22));
                    }
                    if (!stmt.IsNull(23)) {
                        lstats.maxBallSpeed = static_cast<float>(stmt.Get<double>(23));
                    }
                    lstats.ownGoals = stmt.Get<int>(24);
                    if (!stmt.IsNull(25)) {
                        row.durationSeconds = static_cast<float>(stmt.Get<double>(25));
                    }
                    if (!stmt.IsNull(26)) {
                        row.overtimeSeconds = static_cast<float>(stmt.Get<double>(26));
                    }
                    row.localStats = lstats;
                }

                if (!stmt.IsNull(27)) {
                    std::string analyticsData = stmt.Get<std::string>(27);
                    auto parsed = nlohmann::json::parse(analyticsData, nullptr, false);
                    if (!parsed.is_discarded()) {
                        row.analytics = MatchAnalyticsFromJson(parsed);
                    }
                }

                rows.push_back(std::move(row));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetAnalyticsMatches failed: " << e.what() << "\n";
        }
        return rows;
    }

    bool HistoryRepository::GetMatchDetail(sqlite::Connection& db, int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId, const std::string& liveAccount) {
        outDetail = {};
        if (matchId <= 0 || !db.IsOpen()) return false;

        const char* matchSql = R"(
        SELECT id, timestamp, CAST(strftime('%s', timestamp) AS INTEGER), arena,
               our_score, their_score, win, match_guid, playlist_id, gamemode, player_count
        FROM Matches
        WHERE id = ?1;
    )";

        std::string rawTimestamp;
        try {
            sqlite::Statement matchStmt(db, matchSql);
            matchStmt.Bind(1, matchId);

            if (!matchStmt.Step()) {
                return false;
            }

            outDetail.found = true;
            outDetail.matchId = matchStmt.Get<int64_t>(0);
            rawTimestamp = matchStmt.Get<std::string>(1);
            outDetail.timestampUnix = matchStmt.Get<int64_t>(2);
            outDetail.arena = matchStmt.Get<std::string>(3);
            outDetail.ourScore = matchStmt.Get<int>(4);
            outDetail.theirScore = matchStmt.Get<int>(5);
            outDetail.win = matchStmt.Get<int>(6) != 0;
            outDetail.matchGuid = matchStmt.Get<std::string>(7);
            outDetail.playlistId = matchStmt.IsNull(8) ? -1 : matchStmt.Get<int>(8);
            outDetail.gamemode = matchStmt.Get<std::string>(9);
            outDetail.playlist = DescribeMatchPlaylist(matchStmt, 9, 10, 8, outDetail.ranked);
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetMatchDetail (match query) failed: " << e.what() << "\n";
            return false;
        }

        if (!outDetail.matchGuid.empty() && db.TableExists("ReplayUploads")) {
            const char* bcSql =
                "SELECT ballchasing_id, ballchasing_url FROM ReplayUploads WHERE match_guid = ?1 AND ballchasing_url IS NOT NULL AND ballchasing_url != '' ORDER BY id DESC LIMIT 1;";
            try {
                sqlite::Statement bcStmt(db, bcSql);
                bcStmt.Bind(1, outDetail.matchGuid);
                if (bcStmt.Step()) {
                    outDetail.ballchasingId = bcStmt.Get<std::string>(0);
                    outDetail.ballchasingUrl = bcStmt.Get<std::string>(1);
                }
            } catch (...) {
            }
        }

        if (db.TableExists("MatchEvents")) {
            const char* eventsSql = R"(
            SELECT event_type, observed_at_ms, match_clock_seconds, actor_id, actor_name,
                   secondary_id, secondary_name, team, value, ball_speed, source, event_key
            FROM MatchEvents WHERE match_id = ?1 ORDER BY observed_at_ms, id LIMIT 512;
        )";
            try {
                sqlite::Statement eventsStmt(db, eventsSql);
                eventsStmt.Bind(1, matchId);
                while (eventsStmt.Step()) {
                    MatchJournalEvent e;
                    e.type = eventsStmt.Get<std::string>(0);
                    e.observedAtUnixMs = eventsStmt.Get<int64_t>(1);
                    if (!eventsStmt.IsNull(2)) e.matchClockSeconds = eventsStmt.Get<double>(2);
                    e.actorId = eventsStmt.Get<std::string>(3);
                    e.actorName = eventsStmt.Get<std::string>(4);
                    e.secondaryId = eventsStmt.Get<std::string>(5);
                    e.secondaryName = eventsStmt.Get<std::string>(6);
                    if (!eventsStmt.IsNull(7)) e.team = eventsStmt.Get<int>(7);
                    if (!eventsStmt.IsNull(8)) e.value = eventsStmt.Get<double>(8);
                    if (!eventsStmt.IsNull(9)) e.ballSpeed = eventsStmt.Get<double>(9);
                    e.source = eventsStmt.Get<std::string>(10);
                    e.eventKey = eventsStmt.Get<std::string>(11);
                    outDetail.events.push_back(std::move(e));
                }
            } catch (...) {
            }
        }

        const bool hasStats = HasStatsTables(db);
        if (hasStats) {
            const char* localSql =
                "SELECT duration_seconds, overtime_seconds, boost_collected, demoed, crossbars, "
                "hardest_crossbar, max_ball_speed, own_goals FROM MatchLocalStats WHERE match_id = ?1;";
            try {
                sqlite::Statement localStmt(db, localSql);
                localStmt.Bind(1, matchId);
                if (localStmt.Step()) {
                    outDetail.hasLocalStats = true;
                    if (!localStmt.IsNull(0)) {
                        outDetail.durationSeconds = static_cast<float>(localStmt.Get<double>(0));
                    }
                    if (!localStmt.IsNull(1)) {
                        outDetail.overtimeSeconds = static_cast<float>(localStmt.Get<double>(1));
                    }
                    MatchLocalStatsRow lstats;
                    lstats.boostCollected = localStmt.Get<int>(2);
                    lstats.demoed = localStmt.Get<int>(3);
                    lstats.crossbars = localStmt.Get<int>(4);
                    if (!localStmt.IsNull(5)) {
                        lstats.hardestCrossbar = static_cast<float>(localStmt.Get<double>(5));
                    }
                    if (!localStmt.IsNull(6)) {
                        lstats.maxBallSpeed = static_cast<float>(localStmt.Get<double>(6));
                    }
                    lstats.ownGoals = localStmt.Get<int>(7);
                    outDetail.localStats = lstats;
                }
            } catch (...) {
            }
        }

        if (db.TableExists("MatchAnalytics")) {
            const char* analyticsSql = "SELECT data FROM MatchAnalytics WHERE match_id = ?1;";
            try {
                sqlite::Statement analyticsStmt(db, analyticsSql);
                analyticsStmt.Bind(1, matchId);
                if (analyticsStmt.Step()) {
                    std::string text = analyticsStmt.Get<std::string>(0);
                    auto parsed = nlohmann::json::parse(text, nullptr, false);
                    if (!parsed.is_discarded()) {
                        outDetail.analytics = MatchAnalyticsFromJson(parsed);
                    }
                }
            } catch (...) {
            }
        }

        const std::string tierKey = TierPlaylistKey(outDetail.playlistId, outDetail.gamemode);
        std::string playersSql;
        if (hasStats) {
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

        std::vector<MatchDetailPlayer> allPlayers;
        try {
            sqlite::Statement playerStmt(db, playersSql);
            playerStmt.Bind(1, matchId);

            while (playerStmt.Step()) {
                MatchDetailPlayer p;
                p.primaryId = playerStmt.Get<std::string>(0);
                p.name = playerStmt.Get<std::string>(1);
                p.platform = PlatformFromPrimaryId(p.primaryId);
                p.team = playerStmt.Get<int>(2);
                p.mmr = playerStmt.Get<int>(3);
                p.mmrEstimated = playerStmt.Get<int>(4) != 0;
                p.isOpponent = playerStmt.Get<int>(5) != 0;
                p.tier = MmrService::GetRankTierForPlaylistMmr(tierKey, p.mmr);
                if (hasStats && !playerStmt.IsNull(6)) {
                    p.hasStats = true;
                    outDetail.hasPlayerStats = true;
                    if (!playerStmt.IsNull(7)) p.score = playerStmt.Get<int>(7);
                    if (!playerStmt.IsNull(8)) p.goals = playerStmt.Get<int>(8);
                    if (!playerStmt.IsNull(9)) p.assists = playerStmt.Get<int>(9);
                    if (!playerStmt.IsNull(10)) p.saves = playerStmt.Get<int>(10);
                    if (!playerStmt.IsNull(11)) p.shots = playerStmt.Get<int>(11);
                    if (!playerStmt.IsNull(12)) p.demos = playerStmt.Get<int>(12);
                    if (!playerStmt.IsNull(13)) p.touches = playerStmt.Get<int>(13);
                    if (!playerStmt.IsNull(14)) p.carTouches = playerStmt.Get<int>(14);
                    if (!playerStmt.IsNull(15))
                        p.maxGoalSpeed = static_cast<float>(playerStmt.Get<double>(15));
                    if (!playerStmt.IsNull(16))
                        p.fastestGoalTime = static_cast<float>(playerStmt.Get<double>(16));
                }
                allPlayers.push_back(std::move(p));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetMatchDetail (players query) failed: " << e.what() << "\n";
            return true;
        }

        auto hasPlayerInMatch = [&](const std::string& pid) {
            if (pid.empty()) return false;
            return std::any_of(allPlayers.begin(), allPlayers.end(), [&](const MatchDetailPlayer& p) {
                return p.primaryId == pid;
            });
        };

        std::string resolvedAccount;
        if (hasPlayerInMatch(accountPrimaryId)) {
            resolvedAccount = accountPrimaryId;
        } else if (hasPlayerInMatch(liveAccount)) {
            resolvedAccount = liveAccount;
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
            try {
                sqlite::Statement inferStmt(db, inferSql);
                inferStmt.Bind(1, matchId);
                if (inferStmt.Step()) {
                    resolvedAccount = inferStmt.Get<std::string>(0);
                }
            } catch (...) {
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
                if (auto records = QueryEncounterRecords(db, resolvedAccount, p.primaryId, matchId)) {
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
            try {
                sqlite::Statement prevStmt(db, prevSql);
                prevStmt.Bind(1, resolvedAccount);
                prevStmt.Bind(2, partitionKey);
                prevStmt.Bind(3, rawTimestamp);
                prevStmt.Bind(4, matchId);
                if (prevStmt.Step()) {
                    const int prevMmr = prevStmt.Get<int>(0);
                    if (prevMmr > 0) {
                        outDetail.mmrBefore = prevMmr;
                        if (outDetail.mmrAfter.has_value()) {
                            outDetail.mmrDelta = *outDetail.mmrAfter - prevMmr;
                        }
                    }
                }
            } catch (...) {
            }
        }

        return true;
    }

    bool HistoryRepository::GetMatchDetailByGuid(sqlite::Connection& db, const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId, const std::string& liveAccount) {
        outDetail = {};
        if (matchGuid.empty() || !db.IsOpen()) return false;

        int64_t matchId = 0;
        try {
            const char* sql = "SELECT id FROM Matches WHERE match_guid = ?1 ORDER BY id DESC LIMIT 1;";
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, matchGuid);
            if (stmt.Step()) {
                matchId = stmt.Get<int64_t>(0);
            }
        } catch (...) {
            return false;
        }

        if (matchId <= 0) return false;
        return GetMatchDetail(db, matchId, outDetail, accountPrimaryId, liveAccount);
    }

} // namespace database::repository
