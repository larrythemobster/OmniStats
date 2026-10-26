#include "TransferService.hpp"
#include "SessionRepository.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include "utils/Storage.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace database::repository {

    namespace {

        std::string TimestampForFilename() {
            std::time_t now = std::time(nullptr);
            std::tm localTime{};
            localtime_s(&localTime, &now);

            std::ostringstream ss;
            ss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
            return ss.str();
        }

        bool HasStatsTables(sqlite::Connection& db) {
            return db.TableExists("MatchPlayerStats") && db.TableExists("MatchLocalStats");
        }

    } // namespace

    std::string TransferService::CsvEscape(const std::string& value) {
        const auto first = value.find_first_not_of(" \t\r\n");
        const bool formula = (!value.empty() && value.find_first_of("\t\r\n") == 0) ||
                             (first != std::string::npos && value.find_first_of("=+-@", first) == first);
        const bool needsQuotes = value.find_first_of(",\"\r\n") != std::string::npos;
        if (!needsQuotes) return formula ? "'" + value : value;

        std::string escaped;
        escaped.reserve(value.size() + 2 + (formula ? 1 : 0) +
                        static_cast<size_t>(std::count(value.begin(), value.end(), '\"')));
        escaped += '\"';
        if (formula) escaped += '\'';
        for (char c : value) {
            if (c == '\"')
                escaped += "\"\"";
            else
                escaped += c;
        }
        escaped += '\"';
        return escaped;
    }

    bool TransferService::ExportLocalData(sqlite::Connection& db, std::string& exportPath, std::string& error) {
        if (!db.IsOpen()) {
            error = "Database is not open.";
            return false;
        }

        exportPath = Storage::GetDataDirectory() + "exports\\omnistats_export_" + TimestampForFilename() + "\\";
        std::error_code ec;
        fs::create_directories(exportPath, ec);
        if (ec) {
            error = "Failed to create export folder.";
            return false;
        }

        nlohmann::json matches = nlohmann::json::array();
        std::ofstream matchesCsv(exportPath + "matches.csv", std::ios::trunc);
        std::ofstream playersCsv(exportPath + "match_players.csv", std::ios::trunc);
        if (!matchesCsv.is_open() || !playersCsv.is_open()) {
            error = "Failed to open export files.";
            return false;
        }

        matchesCsv << "id,timestamp,arena,our_score,their_score,win,match_guid,playlist_id,gamemode,player_count\n";
        playersCsv << "match_id,primary_id,name,team,mmr,mmr_estimated,is_opponent\n";
        std::ofstream eventsCsv(exportPath + "match_events.csv", std::ios::trunc);
        if (!eventsCsv.is_open()) {
            error = "Failed to open match event export.";
            return false;
        }
        eventsCsv << "match_id,event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key\n";

        if (db.TableExists("MatchEvents")) {
            try {
                const char* sql = "SELECT match_id,event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key FROM MatchEvents ORDER BY match_id,observed_at_ms,id;";
                sqlite::Statement events(db, sql);
                while (events.Step()) {
                    for (int c = 0; c < 13; ++c) {
                        if (c) eventsCsv << ',';
                        if (!events.IsNull(c)) {
                            eventsCsv << CsvEscape(events.Get<std::string>(c));
                        }
                    }
                    eventsCsv << '\n';
                }
            } catch (...) {
                error = "Failed to query match events for export.";
                return false;
            }
        }

        const bool hasStatsTables = HasStatsTables(db);
        std::unique_ptr<sqlite::Statement> localStatsStmt;
        std::unique_ptr<sqlite::Statement> playerStatsStmt;
        if (hasStatsTables) {
            try {
                const char* localStatsSql =
                    "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
                    "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;";
                localStatsStmt = std::make_unique<sqlite::Statement>(db, localStatsSql);
                const char* playerStatsSql =
                    "SELECT score, goals, assists, saves, shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time "
                    "FROM MatchPlayerStats WHERE match_id = ? AND primary_id = ?;";
                playerStatsStmt = std::make_unique<sqlite::Statement>(db, playerStatsSql);
            } catch (...) {
            }
        }

        std::unique_ptr<sqlite::Statement> analyticsStmt;
        if (db.TableExists("MatchAnalytics")) {
            try {
                const char* analyticsSql = "SELECT format_version, data FROM MatchAnalytics WHERE match_id = ?;";
                analyticsStmt = std::make_unique<sqlite::Statement>(db, analyticsSql);
            } catch (...) {
            }
        }

        const bool hasSessionsTable = SessionRepository::HasSessionsTable(db);
        const char* matchesSql = hasSessionsTable
                                     ? "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, session_id FROM Matches ORDER BY timestamp ASC;"
                                     : "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, NULL FROM Matches ORDER BY timestamp ASC;";

        try {
            sqlite::Statement matchStmt(db, matchesSql);
            while (matchStmt.Step()) {
                nlohmann::json match;
                int64_t matchId = matchStmt.Get<int64_t>(0);
                match["id"] = matchId;
                match["timestamp"] = matchStmt.Get<std::string>(1);
                match["arena"] = matchStmt.Get<std::string>(2);
                match["our_score"] = matchStmt.Get<int>(3);
                match["their_score"] = matchStmt.Get<int>(4);
                match["win"] = matchStmt.Get<int>(5) != 0;
                match["match_guid"] = matchStmt.Get<std::string>(6);
                if (matchStmt.IsNull(7))
                    match["playlist_id"] = nullptr;
                else
                    match["playlist_id"] = matchStmt.Get<int>(7);
                match["gamemode"] = matchStmt.Get<std::string>(8);
                match["player_count"] = matchStmt.Get<int>(9);
                if (matchStmt.IsNull(10))
                    match["session_id"] = nullptr;
                else
                    match["session_id"] = matchStmt.Get<int64_t>(10);
                match["players"] = nlohmann::json::array();
                match["analytics"] = nullptr;

                if (analyticsStmt) {
                    analyticsStmt->Reset();
                    analyticsStmt->Bind(1, matchId);
                    if (analyticsStmt->Step()) {
                        nlohmann::json aObj;
                        aObj["format_version"] = analyticsStmt->Get<int>(0);
                        std::string aData = analyticsStmt->Get<std::string>(1);
                        aObj["data"] = nlohmann::json::parse(aData, nullptr, false);
                        if (aObj["data"].is_discarded()) aObj["data"] = aData;
                        match["analytics"] = std::move(aObj);
                    }
                }

                match["local_stats"] = nullptr;
                if (localStatsStmt) {
                    localStatsStmt->Reset();
                    localStatsStmt->Bind(1, matchId);
                    if (localStatsStmt->Step()) {
                        const auto optionalInt = [&](int column) -> nlohmann::json {
                            return localStatsStmt->IsNull(column)
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(localStatsStmt->Get<int>(column));
                        };
                        const auto optionalDouble = [&](int column) -> nlohmann::json {
                            return localStatsStmt->IsNull(column)
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(localStatsStmt->Get<double>(column));
                        };
                        match["local_stats"] = {
                            {"boost_collected", optionalInt(0)},
                            {"demoed", optionalInt(1)},
                            {"crossbars", optionalInt(2)},
                            {"hardest_crossbar", optionalDouble(3)},
                            {"max_ball_speed", optionalDouble(4)},
                            {"own_goals", optionalInt(5)},
                            {"duration_seconds", optionalDouble(6)},
                            {"overtime_seconds", optionalDouble(7)},
                            {"stats_version", localStatsStmt->Get<int>(8)},
                        };
                    }
                }

                matchesCsv << matchId << ","
                           << CsvEscape(match["timestamp"].get<std::string>()) << ","
                           << CsvEscape(match["arena"].get<std::string>()) << ","
                           << match["our_score"].get<int>() << ","
                           << match["their_score"].get<int>() << ","
                           << (match["win"].get<bool>() ? 1 : 0) << ","
                           << CsvEscape(match["match_guid"].get<std::string>()) << ",";
                if (match["playlist_id"].is_null())
                    matchesCsv << ",";
                else
                    matchesCsv << match["playlist_id"].get<int>() << ",";
                matchesCsv << CsvEscape(match["gamemode"].get<std::string>()) << ","
                           << match["player_count"].get<int>() << "\n";

                const char* playersSql =
                    "SELECT primary_id, name, team, mmr, mmr_estimated, is_opponent "
                    "FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";
                try {
                    sqlite::Statement playerStmt(db, playersSql);
                    playerStmt.Bind(1, matchId);
                    while (playerStmt.Step()) {
                        nlohmann::json player;
                        std::string pid = playerStmt.Get<std::string>(0);
                        player["primary_id"] = pid;
                        player["name"] = playerStmt.Get<std::string>(1);
                        player["team"] = playerStmt.Get<int>(2);
                        player["mmr"] = playerStmt.Get<int>(3);
                        player["mmr_estimated"] = playerStmt.Get<int>(4) != 0;
                        player["is_opponent"] = playerStmt.Get<int>(5) != 0;
                        player["stats"] = nullptr;

                        if (playerStatsStmt) {
                            playerStatsStmt->Reset();
                            playerStatsStmt->Bind(1, matchId);
                            playerStatsStmt->Bind(2, pid);
                            if (playerStatsStmt->Step()) {
                                const auto optionalInt = [&](int column) -> nlohmann::json {
                                    return playerStatsStmt->IsNull(column)
                                               ? nlohmann::json(nullptr)
                                               : nlohmann::json(playerStatsStmt->Get<int>(column));
                                };
                                const auto optionalDouble = [&](int column) -> nlohmann::json {
                                    return playerStatsStmt->IsNull(column)
                                               ? nlohmann::json(nullptr)
                                               : nlohmann::json(playerStatsStmt->Get<double>(column));
                                };
                                player["stats"] = {
                                    {"score", optionalInt(0)},
                                    {"goals", optionalInt(1)},
                                    {"assists", optionalInt(2)},
                                    {"saves", optionalInt(3)},
                                    {"shots", optionalInt(4)},
                                    {"demos", optionalInt(5)},
                                    {"touches", optionalInt(6)},
                                    {"car_touches", optionalInt(7)},
                                    {"max_goal_speed", optionalDouble(8)},
                                    {"fastest_goal_time", optionalDouble(9)},
                                };
                            }
                        }

                        playersCsv << matchId << ","
                                   << CsvEscape(player["primary_id"].get<std::string>()) << ","
                                   << CsvEscape(player["name"].get<std::string>()) << ","
                                   << player["team"].get<int>() << ","
                                   << player["mmr"].get<int>() << ","
                                   << (player["mmr_estimated"].get<bool>() ? 1 : 0) << ","
                                   << (player["is_opponent"].get<bool>() ? 1 : 0) << "\n";

                        match["players"].push_back(std::move(player));
                    }
                } catch (...) {
                }

                matches.push_back(std::move(match));
            }
        } catch (const std::exception& e) {
            error = std::string("Failed to read matches: ") + e.what();
            return false;
        }

        std::ofstream jsonFile(exportPath + "matches.json", std::ios::trunc);
        if (!jsonFile.is_open()) {
            error = "Failed to write JSON export.";
            return false;
        }
        jsonFile << matches.dump(2);

        if (hasSessionsTable) {
            nlohmann::json sessionsJson = nlohmann::json::array();
            std::ofstream sessionsCsv(exportPath + "sessions.csv", std::ios::trunc);
            if (sessionsCsv.is_open()) {
                sessionsCsv << "id,account,started_at,ended_at,wins,losses,source,mmr_change_json,totals_json\n";
            }
            const char* sessSql =
                "SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0), COALESCE(wins, 0), COALESCE(losses, 0), "
                "COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'), COALESCE(source, 'live') "
                "FROM Sessions ORDER BY started_at ASC, id ASC;";
            try {
                sqlite::Statement sessStmt(db, sessSql);
                while (sessStmt.Step()) {
                    const int64_t sid = sessStmt.Get<int64_t>(0);
                    const std::string account = sessStmt.Get<std::string>(1);
                    const int64_t startedAt = sessStmt.Get<int64_t>(2);
                    const int64_t endedAt = sessStmt.Get<int64_t>(3);
                    const int wins = sessStmt.Get<int>(4);
                    const int losses = sessStmt.Get<int>(5);
                    const std::string mmrJsonStr = sessStmt.Get<std::string>(6);
                    const std::string totalsJsonStr = sessStmt.Get<std::string>(7);
                    const std::string source = sessStmt.Get<std::string>(8);

                    nlohmann::json sObj;
                    sObj["id"] = sid;
                    sObj["account"] = account;
                    sObj["started_at"] = startedAt;
                    sObj["ended_at"] = endedAt;
                    sObj["wins"] = wins;
                    sObj["losses"] = losses;
                    sObj["source"] = source;
                    sObj["mmr_change"] = nlohmann::json::parse(mmrJsonStr, nullptr, false);
                    if (sObj["mmr_change"].is_discarded()) sObj["mmr_change"] = nlohmann::json::object();
                    sObj["totals"] = nlohmann::json::parse(totalsJsonStr, nullptr, false);
                    if (sObj["totals"].is_discarded()) sObj["totals"] = nlohmann::json::object();
                    sessionsJson.push_back(std::move(sObj));

                    if (sessionsCsv.is_open()) {
                        sessionsCsv << sid << ","
                                    << CsvEscape(account) << ","
                                    << startedAt << ","
                                    << endedAt << ","
                                    << wins << ","
                                    << losses << ","
                                    << CsvEscape(source) << ","
                                    << CsvEscape(mmrJsonStr) << ","
                                    << CsvEscape(totalsJsonStr) << "\n";
                    }
                }
            } catch (...) {
            }

            std::ofstream sessJsonFile(exportPath + "sessions.json", std::ios::trunc);
            if (sessJsonFile.is_open()) {
                sessJsonFile << sessionsJson.dump(2);
            }
        }

        if (db.TableExists("ReplayUploads")) {
            nlohmann::json replaysJson = nlohmann::json::array();
            std::ofstream replaysCsv(exportPath + "replay_uploads.csv", std::ios::trunc);
            if (replaysCsv.is_open()) {
                replaysCsv << "id,file_path,file_name,match_guid,state,attempts,next_attempt_at,last_http_status,last_error,ballchasing_id,ballchasing_url,created_at,updated_at\n";
            }
            const char* ruSql =
                "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
                "FROM ReplayUploads ORDER BY id ASC;";
            try {
                sqlite::Statement ruStmt(db, ruSql);
                while (ruStmt.Step()) {
                    int64_t rId = ruStmt.Get<int64_t>(0);
                    std::string filePath = ruStmt.Get<std::string>(1);
                    std::string fileName = ruStmt.Get<std::string>(2);
                    std::string matchGuid = ruStmt.Get<std::string>(3);
                    std::string state = ruStmt.Get<std::string>(4);
                    int attempts = ruStmt.Get<int>(5);
                    int64_t nextAttemptAt = ruStmt.Get<int64_t>(6);
                    int lastHttpStatus = ruStmt.Get<int>(7);
                    std::string lastError = ruStmt.Get<std::string>(8);
                    std::string ballchasingId = ruStmt.Get<std::string>(9);
                    std::string ballchasingUrl = ruStmt.Get<std::string>(10);
                    int64_t createdAt = ruStmt.Get<int64_t>(11);
                    int64_t updatedAt = ruStmt.Get<int64_t>(12);

                    nlohmann::json rObj;
                    rObj["id"] = rId;
                    rObj["file_path"] = filePath;
                    rObj["file_name"] = fileName;
                    rObj["match_guid"] = matchGuid;
                    rObj["state"] = state;
                    rObj["attempts"] = attempts;
                    rObj["next_attempt_at"] = nextAttemptAt;
                    rObj["last_http_status"] = lastHttpStatus;
                    rObj["last_error"] = lastError;
                    rObj["ballchasing_id"] = ballchasingId;
                    rObj["ballchasing_url"] = ballchasingUrl;
                    rObj["created_at"] = createdAt;
                    rObj["updated_at"] = updatedAt;
                    replaysJson.push_back(std::move(rObj));

                    if (replaysCsv.is_open()) {
                        replaysCsv << rId << ","
                                   << CsvEscape(filePath) << ","
                                   << CsvEscape(fileName) << ","
                                   << CsvEscape(matchGuid) << ","
                                   << CsvEscape(state) << ","
                                   << attempts << ","
                                   << nextAttemptAt << ","
                                   << lastHttpStatus << ","
                                   << CsvEscape(lastError) << ","
                                   << CsvEscape(ballchasingId) << ","
                                   << CsvEscape(ballchasingUrl) << ","
                                   << createdAt << ","
                                   << updatedAt << "\n";
                    }
                }
            } catch (...) {
            }

            std::ofstream ruJsonFile(exportPath + "replay_uploads.json", std::ios::trunc);
            if (ruJsonFile.is_open()) {
                ruJsonFile << replaysJson.dump(2);
            }
        }

        if (db.TableExists("TrackedAccounts")) {
            nlohmann::json accountsJson = nlohmann::json::array();
            std::map<std::string, nlohmann::json> ranksByPrimary;
            if (db.TableExists("TrackedAccountRanks")) {
                const char* ranksSql =
                    "SELECT primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at "
                    "FROM TrackedAccountRanks ORDER BY primary_id ASC, playlist_key ASC;";
                try {
                    sqlite::Statement rStmt(db, ranksSql);
                    while (rStmt.Step()) {
                        std::string pid = rStmt.Get<std::string>(0);
                        nlohmann::json rObj;
                        rObj["playlist_key"] = rStmt.Get<std::string>(1);
                        if (rStmt.IsNull(2))
                            rObj["mmr"] = nullptr;
                        else
                            rObj["mmr"] = rStmt.Get<int>(2);
                        if (rStmt.IsNull(3))
                            rObj["rank_tier"] = nullptr;
                        else
                            rObj["rank_tier"] = rStmt.Get<std::string>(3);
                        if (rStmt.IsNull(4))
                            rObj["matches_played"] = nullptr;
                        else
                            rObj["matches_played"] = rStmt.Get<int>(4);
                        if (rStmt.IsNull(5))
                            rObj["source"] = nullptr;
                        else
                            rObj["source"] = rStmt.Get<std::string>(5);
                        rObj["fetched_at"] = rStmt.Get<int64_t>(6);
                        ranksByPrimary[pid].push_back(std::move(rObj));
                    }
                } catch (...) {
                }
            }

            const char* accSql =
                "SELECT primary_id, platform, platform_account_id, nickname, last_known_name, "
                "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, "
                "sort_order, discovery_source FROM TrackedAccounts "
                "ORDER BY sort_order ASC, created_at ASC, primary_id ASC;";
            try {
                sqlite::Statement accStmt(db, accSql);
                while (accStmt.Step()) {
                    std::string pid = accStmt.Get<std::string>(0);
                    nlohmann::json aObj;
                    aObj["nickname"] = accStmt.Get<std::string>(3);
                    aObj["platform"] = accStmt.Get<std::string>(1);
                    aObj["platform_account_id"] = accStmt.Get<std::string>(2);
                    aObj["primary_id"] = pid;
                    if (accStmt.IsNull(4))
                        aObj["last_known_name"] = nullptr;
                    else
                        aObj["last_known_name"] = accStmt.Get<std::string>(4);
                    aObj["created_at"] = accStmt.Get<int64_t>(5);
                    if (accStmt.IsNull(6))
                        aObj["last_seen_at"] = nullptr;
                    else
                        aObj["last_seen_at"] = accStmt.Get<int64_t>(6);
                    if (accStmt.IsNull(7))
                        aObj["last_refresh_attempt_at"] = nullptr;
                    else
                        aObj["last_refresh_attempt_at"] = accStmt.Get<int64_t>(7);
                    if (accStmt.IsNull(8))
                        aObj["last_successful_refresh_at"] = nullptr;
                    else
                        aObj["last_successful_refresh_at"] = accStmt.Get<int64_t>(8);
                    aObj["sort_order"] = accStmt.Get<int64_t>(9);
                    aObj["discovery_source"] = accStmt.Get<std::string>(10);
                    auto rIt = ranksByPrimary.find(pid);
                    if (rIt != ranksByPrimary.end())
                        aObj["ranks"] = rIt->second;
                    else
                        aObj["ranks"] = nlohmann::json::array();
                    accountsJson.push_back(std::move(aObj));
                }
            } catch (...) {
            }

            std::ofstream taJsonFile(exportPath + "tracked_accounts.json", std::ios::trunc);
            if (taJsonFile.is_open()) {
                taJsonFile << accountsJson.dump(2);
            }
        }

        if (db.TableExists("TrackedAccountRankObservations")) {
            std::ofstream historyCsv(exportPath + "tracked_account_rank_observations.csv", std::ios::trunc);
            if (!historyCsv.is_open()) {
                error = "Failed to open account rank history export.";
                return false;
            }
            historyCsv << "primary_id,playlist_key,mmr,rank_tier,matches_played,source,observed_at\n";
            const char* sql = "SELECT primary_id,playlist_key,mmr,rank_tier,matches_played,source,observed_at "
                              "FROM TrackedAccountRankObservations ORDER BY primary_id,playlist_key,observed_at,id;";
            try {
                sqlite::Statement historyStmt(db, sql);
                while (historyStmt.Step()) {
                    for (int col = 0; col < 7; ++col) {
                        if (col) historyCsv << ',';
                        if (!historyStmt.IsNull(col)) {
                            historyCsv << CsvEscape(historyStmt.Get<std::string>(col));
                        }
                    }
                    historyCsv << '\n';
                }
            } catch (...) {
                error = "Failed to query account rank observations for export.";
                return false;
            }
        }

        if (db.TableExists("PlayerNotes")) {
            nlohmann::json notesJson = nlohmann::json::array();
            const char* notesSql =
                "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at "
                "FROM PlayerNotes ORDER BY updated_at DESC;";
            try {
                sqlite::Statement nStmt(db, notesSql);
                while (nStmt.Step()) {
                    notesJson.push_back({{"platform", nStmt.Get<std::string>(0)},
                                         {"account_id", nStmt.Get<std::string>(1)},
                                         {"splitscreen", nStmt.Get<int>(2)},
                                         {"note", nStmt.Get<std::string>(3)},
                                         {"last_known_name", nStmt.Get<std::string>(4)},
                                         {"created_at", nStmt.Get<int64_t>(5)},
                                         {"updated_at", nStmt.Get<int64_t>(6)}});
                }
            } catch (...) {
            }

            std::ofstream notesFile(exportPath + "player_notes.json", std::ios::trunc);
            if (notesFile.is_open()) notesFile << notesJson.dump(2);
        }

        if (db.TableExists("PlayerTags")) {
            std::ofstream tagsCsv(exportPath + "player_tags.csv", std::ios::trunc);
            if (!tagsCsv.is_open()) {
                error = "Couldn't open the local player tags export.";
                return false;
            }
            tagsCsv << "platform,account_id,splitscreen,tag,created_at\n";
            try {
                sqlite::Statement stmt(db, "SELECT platform,account_id,splitscreen,tag,created_at FROM PlayerTags;");
                while (stmt.Step()) {
                    for (int col = 0; col < 5; ++col) {
                        if (col) tagsCsv << ',';
                        tagsCsv << CsvEscape(stmt.Get<std::string>(col));
                    }
                    tagsCsv << '\n';
                }
            } catch (...) {
                error = "Couldn't query local player tags.";
                return false;
            }
        }

        error.clear();
        return true;
    }

    bool TransferService::DeleteLocalMatchHistory(sqlite::Connection& db, std::string& error) {
        if (!db.IsOpen()) {
            error = "Database is not open.";
            return false;
        }

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            if (db.TableExists("MatchEvents")) db.Execute("DELETE FROM MatchEvents;");
            if (db.TableExists("MatchPlayerStats")) db.Execute("DELETE FROM MatchPlayerStats;");
            if (db.TableExists("MatchLocalStats")) db.Execute("DELETE FROM MatchLocalStats;");
            if (db.TableExists("MatchAnalytics")) db.Execute("DELETE FROM MatchAnalytics;");
            db.Execute("DELETE FROM MatchPlayers;");
            db.Execute("DELETE FROM Matches;");
            if (db.TableExists("Sessions")) db.Execute("DELETE FROM Sessions;");
            if (db.TableExists("ReplayUploads")) db.Execute("UPDATE ReplayUploads SET match_guid = NULL;");
            db.Execute("DELETE FROM sqlite_sequence WHERE name IN ('Matches', 'MatchPlayers', 'Sessions');");
            tx.Commit();
            error.clear();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    DbMergeResult TransferService::MergeDatabase(sqlite::Connection& db, const std::string& sourceDbPath, const std::string& activeDbPath) {
        DbMergeResult result;
        if (sourceDbPath.empty()) {
            result.error = "No database file selected.";
            return result;
        }

        std::error_code ec;
        fs::path srcPath(sourceDbPath);
        if (!fs::exists(srcPath, ec) || !fs::is_regular_file(srcPath, ec)) {
            result.error = "Selected file does not exist or is not a regular file.";
            return result;
        }

        if (!db.IsOpen()) {
            result.error = "Target database is not open.";
            return result;
        }

        if (!activeDbPath.empty()) {
            fs::path targetPath(activeDbPath);
            if (fs::equivalent(srcPath, targetPath, ec)) {
                result.error = "Cannot merge the active database into itself.";
                return result;
            }
        }

        sqlite::Connection srcDb;
        try {
            srcDb = sqlite::Connection::OpenReadOnly(srcPath.string());
        } catch (const std::exception& e) {
            result.error = "Failed to open source database: " + std::string(e.what());
            return result;
        }

        if (!srcDb.TableExists("Matches") || !srcDb.TableExists("MatchPlayers")) {
            result.error = "Selected file is not an OmniStats database (missing Matches or MatchPlayers table).";
            return result;
        }

        const bool srcHasPlaylistId = srcDb.TableHasColumn("Matches", "playlist_id");
        const bool srcHasResultPending = srcDb.TableHasColumn("Matches", "result_pending");
        const bool srcHasMmrEstimated = srcDb.TableHasColumn("MatchPlayers", "mmr_estimated");
        const bool srcHasIsOpponent = srcDb.TableHasColumn("MatchPlayers", "is_opponent");

        const bool copyPlayerStats = srcDb.TableExists("MatchPlayerStats") && db.TableExists("MatchPlayerStats");
        const bool copyLocalStats = srcDb.TableExists("MatchLocalStats") && db.TableExists("MatchLocalStats");
        const bool copyAnalytics = srcDb.TableExists("MatchAnalytics") && db.TableExists("MatchAnalytics");
        const bool copyEvents = srcDb.TableExists("MatchEvents") && db.TableExists("MatchEvents");

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            const char* checkGuidSql = "SELECT id FROM Matches WHERE match_guid = ? AND match_guid != '' LIMIT 1;";
            sqlite::Statement checkGuidStmt(db, checkGuidSql);

            const char* checkFallbackSql =
                "SELECT id FROM Matches WHERE timestamp = ? AND arena = ? AND our_score = ? AND their_score = ? AND win = ? LIMIT 1;";
            sqlite::Statement checkFallbackStmt(db, checkFallbackSql);

            const char* insertMatchSql =
                "INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, result_pending) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
            sqlite::Statement insertMatchStmt(db, insertMatchSql);

            const char* insertPlayerSql =
                "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) "
                "VALUES (?, ?, ?, ?, ?, ?, ?);";
            sqlite::Statement insertPlayerStmt(db, insertPlayerSql);

            std::string selectMatchesSql = "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, ";
            selectMatchesSql += srcHasPlaylistId ? "playlist_id, " : "NULL, ";
            selectMatchesSql += "gamemode, player_count, ";
            selectMatchesSql += srcHasResultPending ? "COALESCE(result_pending, 0) " : "0 ";
            selectMatchesSql += "FROM Matches ORDER BY id ASC;";
            sqlite::Statement selectMatchesStmt(srcDb, selectMatchesSql);

            std::string selectPlayersSql = "SELECT primary_id, name, team, mmr, ";
            selectPlayersSql += srcHasMmrEstimated ? "mmr_estimated, " : "0, ";
            selectPlayersSql += srcHasIsOpponent ? "is_opponent " : "0 ";
            selectPlayersSql += "FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";
            sqlite::Statement selectPlayersStmt(srcDb, selectPlayersSql);

            std::unique_ptr<sqlite::Statement> selectPlayerStatsStmt;
            std::unique_ptr<sqlite::Statement> insertPlayerStatsStmt;
            if (copyPlayerStats) {
                selectPlayerStatsStmt = std::make_unique<sqlite::Statement>(
                    srcDb,
                    "SELECT primary_id, score, goals, assists, saves, shots, demos, touches, car_touches, "
                    "max_goal_speed, fastest_goal_time FROM MatchPlayerStats WHERE match_id = ?;");
                insertPlayerStatsStmt = std::make_unique<sqlite::Statement>(
                    db,
                    "INSERT OR REPLACE INTO MatchPlayerStats (match_id, primary_id, score, goals, assists, saves, "
                    "shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
            }

            std::unique_ptr<sqlite::Statement> selectLocalStatsStmt;
            std::unique_ptr<sqlite::Statement> insertLocalStatsStmt;
            if (copyLocalStats) {
                selectLocalStatsStmt = std::make_unique<sqlite::Statement>(
                    srcDb,
                    "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
                    "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;");
                insertLocalStatsStmt = std::make_unique<sqlite::Statement>(
                    db,
                    "INSERT OR REPLACE INTO MatchLocalStats (match_id, boost_collected, demoed, crossbars, "
                    "hardest_crossbar, max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);");
            }

            std::unique_ptr<sqlite::Statement> selectAnalyticsStmt;
            std::unique_ptr<sqlite::Statement> insertAnalyticsStmt;
            if (copyAnalytics) {
                selectAnalyticsStmt = std::make_unique<sqlite::Statement>(
                    srcDb, "SELECT format_version, data FROM MatchAnalytics WHERE match_id = ?;");
                insertAnalyticsStmt = std::make_unique<sqlite::Statement>(
                    db, "INSERT OR REPLACE INTO MatchAnalytics (match_id, format_version, data) VALUES (?, ?, ?);");
            }

            while (selectMatchesStmt.Step()) {
                int64_t oldMatchId = selectMatchesStmt.Get<int64_t>(0);
                std::string timestamp = selectMatchesStmt.Get<std::string>(1);
                std::string arena = selectMatchesStmt.Get<std::string>(2);
                int ourScore = selectMatchesStmt.Get<int>(3);
                int theirScore = selectMatchesStmt.Get<int>(4);
                int win = selectMatchesStmt.Get<int>(5);
                std::string matchGuid = selectMatchesStmt.Get<std::string>(6);
                bool playlistNull = selectMatchesStmt.IsNull(7);
                int playlistId = playlistNull ? 0 : selectMatchesStmt.Get<int>(7);
                std::string gamemode = selectMatchesStmt.Get<std::string>(8);
                int playerCount = selectMatchesStmt.Get<int>(9);
                int resultPending = selectMatchesStmt.Get<int>(10);

                if (arena.empty()) {
                    result.matchesSkipped++;
                    continue;
                }

                bool isDuplicate = false;
                if (!matchGuid.empty()) {
                    checkGuidStmt.Reset();
                    checkGuidStmt.Bind(1, matchGuid);
                    if (checkGuidStmt.Step()) {
                        isDuplicate = true;
                    }
                }

                if (!isDuplicate && !timestamp.empty()) {
                    checkFallbackStmt.Reset();
                    checkFallbackStmt.Bind(1, timestamp);
                    checkFallbackStmt.Bind(2, arena);
                    checkFallbackStmt.Bind(3, ourScore);
                    checkFallbackStmt.Bind(4, theirScore);
                    checkFallbackStmt.Bind(5, win);
                    if (checkFallbackStmt.Step()) {
                        isDuplicate = true;
                    }
                }

                if (isDuplicate) {
                    result.matchesSkipped++;
                    continue;
                }

                insertMatchStmt.Reset();
                insertMatchStmt.Bind(1, timestamp);
                insertMatchStmt.Bind(2, arena);
                insertMatchStmt.Bind(3, ourScore);
                insertMatchStmt.Bind(4, theirScore);
                insertMatchStmt.Bind(5, win);
                insertMatchStmt.Bind(6, matchGuid);
                if (playlistNull)
                    insertMatchStmt.BindNull(7);
                else
                    insertMatchStmt.Bind(7, playlistId);
                insertMatchStmt.Bind(8, gamemode);
                insertMatchStmt.Bind(9, playerCount);
                insertMatchStmt.Bind(10, resultPending);
                insertMatchStmt.Execute();

                int64_t newMatchId = db.LastInsertRowId();
                result.matchesImported++;

                selectPlayersStmt.Reset();
                selectPlayersStmt.Bind(1, oldMatchId);
                while (selectPlayersStmt.Step()) {
                    std::string primaryId = selectPlayersStmt.Get<std::string>(0);
                    std::string name = selectPlayersStmt.Get<std::string>(1);
                    int team = selectPlayersStmt.Get<int>(2);
                    int mmr = selectPlayersStmt.Get<int>(3);
                    int mmrEstimated = selectPlayersStmt.Get<int>(4);
                    int isOpponent = selectPlayersStmt.Get<int>(5);

                    insertPlayerStmt.Reset();
                    insertPlayerStmt.Bind(1, newMatchId);
                    insertPlayerStmt.Bind(2, primaryId);
                    insertPlayerStmt.Bind(3, name);
                    insertPlayerStmt.Bind(4, team);
                    insertPlayerStmt.Bind(5, mmr);
                    insertPlayerStmt.Bind(6, mmrEstimated);
                    insertPlayerStmt.Bind(7, isOpponent);
                    insertPlayerStmt.Execute();
                    result.playersImported++;
                }

                if (copyEvents) {
                    try {
                        const char* fromSql = "SELECT event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key,format_version FROM MatchEvents WHERE match_id=? ORDER BY id LIMIT 512;";
                        const char* toSql = "INSERT OR IGNORE INTO MatchEvents (match_id,event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key,format_version,match_guid) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
                        sqlite::Statement from(srcDb, fromSql);
                        sqlite::Statement to(db, toSql);
                        from.Bind(1, oldMatchId);
                        while (from.Step()) {
                            to.Reset();
                            to.Bind(1, newMatchId);
                            for (int col = 0; col < 13; ++col) {
                                int dst = col + 2;
                                if (from.IsNull(col)) {
                                    to.BindNull(dst);
                                } else {
                                    to.Bind(dst, from.Get<std::string>(col));
                                }
                            }
                            to.Bind(15, matchGuid);
                            to.Execute();
                        }
                    } catch (...) {
                    }
                }

                if (copyPlayerStats && selectPlayerStatsStmt && insertPlayerStatsStmt) {
                    selectPlayerStatsStmt->Reset();
                    selectPlayerStatsStmt->Bind(1, oldMatchId);
                    while (selectPlayerStatsStmt->Step()) {
                        std::string primaryId = selectPlayerStatsStmt->Get<std::string>(0);
                        insertPlayerStatsStmt->Reset();
                        insertPlayerStatsStmt->Bind(1, newMatchId);
                        insertPlayerStatsStmt->Bind(2, primaryId);
                        for (int col = 1; col <= 8; ++col) {
                            if (selectPlayerStatsStmt->IsNull(col))
                                insertPlayerStatsStmt->BindNull(col + 2);
                            else
                                insertPlayerStatsStmt->Bind(col + 2, selectPlayerStatsStmt->Get<int>(col));
                        }
                        for (int col = 9; col <= 10; ++col) {
                            if (selectPlayerStatsStmt->IsNull(col))
                                insertPlayerStatsStmt->BindNull(col + 2);
                            else
                                insertPlayerStatsStmt->Bind(col + 2, selectPlayerStatsStmt->Get<double>(col));
                        }
                        insertPlayerStatsStmt->Execute();
                    }
                }

                if (copyLocalStats && selectLocalStatsStmt && insertLocalStatsStmt) {
                    selectLocalStatsStmt->Reset();
                    selectLocalStatsStmt->Bind(1, oldMatchId);
                    if (selectLocalStatsStmt->Step()) {
                        insertLocalStatsStmt->Reset();
                        insertLocalStatsStmt->Bind(1, newMatchId);
                        for (int i = 0; i <= 2; ++i) {
                            if (selectLocalStatsStmt->IsNull(i))
                                insertLocalStatsStmt->BindNull(i + 2);
                            else
                                insertLocalStatsStmt->Bind(i + 2, selectLocalStatsStmt->Get<int>(i));
                        }
                        for (int i = 3; i <= 4; ++i) {
                            if (selectLocalStatsStmt->IsNull(i))
                                insertLocalStatsStmt->BindNull(i + 2);
                            else
                                insertLocalStatsStmt->Bind(i + 2, selectLocalStatsStmt->Get<double>(i));
                        }
                        if (selectLocalStatsStmt->IsNull(5))
                            insertLocalStatsStmt->BindNull(7);
                        else
                            insertLocalStatsStmt->Bind(7, selectLocalStatsStmt->Get<int>(5));
                        for (int i = 6; i <= 7; ++i) {
                            if (selectLocalStatsStmt->IsNull(i))
                                insertLocalStatsStmt->BindNull(i + 2);
                            else
                                insertLocalStatsStmt->Bind(i + 2, selectLocalStatsStmt->Get<double>(i));
                        }
                        insertLocalStatsStmt->Bind(10, selectLocalStatsStmt->Get<int>(8));
                        insertLocalStatsStmt->Execute();
                    }
                }

                if (copyAnalytics && selectAnalyticsStmt && insertAnalyticsStmt) {
                    selectAnalyticsStmt->Reset();
                    selectAnalyticsStmt->Bind(1, oldMatchId);
                    if (selectAnalyticsStmt->Step()) {
                        insertAnalyticsStmt->Reset();
                        insertAnalyticsStmt->Bind(1, newMatchId);
                        insertAnalyticsStmt->Bind(2, selectAnalyticsStmt->Get<int>(0));
                        insertAnalyticsStmt->Bind(3, selectAnalyticsStmt->Get<std::string>(1));
                        insertAnalyticsStmt->Execute();
                    }
                }
            }

            if (srcDb.TableExists("ReplayUploads") && db.TableExists("ReplayUploads")) {
                const char* selReplaySql =
                    "SELECT file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, ballchasing_id, ballchasing_url, created_at, updated_at FROM ReplayUploads;";
                const char* insReplaySql =
                    "INSERT OR IGNORE INTO ReplayUploads (file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, ballchasing_id, ballchasing_url, created_at, updated_at) "
                    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12);";
                try {
                    sqlite::Statement selReplayStmt(srcDb, selReplaySql);
                    sqlite::Statement insReplayStmt(db, insReplaySql);
                    while (selReplayStmt.Step()) {
                        insReplayStmt.Reset();
                        for (int i = 0; i < 12; ++i) {
                            if (selReplayStmt.IsNull(i)) {
                                insReplayStmt.BindNull(i + 1);
                            } else if (i == 4 || i == 6) {
                                insReplayStmt.Bind(i + 1, selReplayStmt.Get<int>(i));
                            } else if (i == 5 || i == 10 || i == 11) {
                                insReplayStmt.Bind(i + 1, selReplayStmt.Get<int64_t>(i));
                            } else {
                                insReplayStmt.Bind(i + 1, selReplayStmt.Get<std::string>(i));
                            }
                        }
                        insReplayStmt.Execute();
                    }
                } catch (...) {
                }
            }

            tx.Commit();

            while (result.matchesImported > 0) {
                bool moreRemaining = false;
                int linked = 0;
                SessionRepository::BackfillSessionsChunk(db, 500, moreRemaining, linked);
                if (!moreRemaining || linked == 0) break;
            }

            result.success = true;
            return result;
        } catch (const std::exception& e) {
            result.error = e.what();
            return result;
        }
    }

} // namespace database::repository
