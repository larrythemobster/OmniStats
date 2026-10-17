#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include "utils/Storage.hpp"
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;
using namespace DatabaseDetail;

static std::string CsvEscape(const std::string& value) {
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

static std::string TimestampForFilename() {
    std::time_t now = std::time(nullptr);
    std::tm localTime{};
    localtime_s(&localTime, &now);

    std::ostringstream ss;
    ss << std::put_time(&localTime, "%Y%m%d_%H%M%S");
    return ss.str();
}

bool DatabaseManager::ExportLocalData(std::string& exportPath, std::string& error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
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
    if (SqliteTableExists(m_db, "MatchEvents")) {
        sqlite3_stmt* events = nullptr;
        const char* sql = "SELECT match_id,event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key FROM MatchEvents ORDER BY match_id,observed_at_ms,id;";
        if (sqlite3_prepare_v2(m_db, sql, -1, &events, nullptr) != SQLITE_OK) {
            error = "Failed to query match events for export.";
            return false;
        }
        while (sqlite3_step(events) == SQLITE_ROW) {
            for (int c = 0; c < 13; ++c) {
                if (c) eventsCsv << ',';
                if (sqlite3_column_type(events, c) != SQLITE_NULL)
                    eventsCsv << CsvEscape(SqlColumnText(events, c));
            }
            eventsCsv << '\n';
        }
        sqlite3_finalize(events);
    }

    const bool hasStatsTables = HasStatsTablesLocked();
    sqlite3_stmt* localStatsStmt = nullptr;
    sqlite3_stmt* playerStatsStmt = nullptr;
    if (hasStatsTables) {
        const char* localStatsSql =
            "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
            "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;";
        sqlite3_prepare_v2(m_db, localStatsSql, -1, &localStatsStmt, nullptr);
        const char* playerStatsSql =
            "SELECT score, goals, assists, saves, shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time "
            "FROM MatchPlayerStats WHERE match_id = ? AND primary_id = ?;";
        sqlite3_prepare_v2(m_db, playerStatsSql, -1, &playerStatsStmt, nullptr);
    }

    const bool hasSessionsTable = HasSessionsTableLocked();
    const char* matchesSql = hasSessionsTable
                                 ? "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, session_id FROM Matches ORDER BY timestamp ASC;"
                                 : "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, NULL FROM Matches ORDER BY timestamp ASC;";
    sqlite3_stmt* matchStmt = nullptr;
    if (sqlite3_prepare_v2(m_db, matchesSql, -1, &matchStmt, nullptr) != SQLITE_OK) {
        error = "Failed to read matches.";
        return false;
    }

    while (sqlite3_step(matchStmt) == SQLITE_ROW) {
        nlohmann::json match;
        int matchId = sqlite3_column_int(matchStmt, 0);
        match["id"] = matchId;
        match["timestamp"] = SqlColumnText(matchStmt, 1);
        match["arena"] = SqlColumnText(matchStmt, 2);
        match["our_score"] = sqlite3_column_int(matchStmt, 3);
        match["their_score"] = sqlite3_column_int(matchStmt, 4);
        match["win"] = sqlite3_column_int(matchStmt, 5) != 0;
        match["match_guid"] = SqlColumnText(matchStmt, 6);
        if (sqlite3_column_type(matchStmt, 7) == SQLITE_NULL)
            match["playlist_id"] = nullptr;
        else
            match["playlist_id"] = sqlite3_column_int(matchStmt, 7);
        match["gamemode"] = SqlColumnText(matchStmt, 8);
        match["player_count"] = sqlite3_column_int(matchStmt, 9);
        if (sqlite3_column_type(matchStmt, 10) == SQLITE_NULL)
            match["session_id"] = nullptr;
        else
            match["session_id"] = sqlite3_column_int64(matchStmt, 10);
        match["players"] = nlohmann::json::array();
        match["local_stats"] = nullptr;
        if (localStatsStmt) {
            sqlite3_reset(localStatsStmt);
            sqlite3_clear_bindings(localStatsStmt);
            sqlite3_bind_int(localStatsStmt, 1, matchId);
            if (sqlite3_step(localStatsStmt) == SQLITE_ROW) {
                auto optInt = [&](int col) -> nlohmann::json {
                    return sqlite3_column_type(localStatsStmt, col) == SQLITE_NULL
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(sqlite3_column_int(localStatsStmt, col));
                };
                auto optDouble = [&](int col) -> nlohmann::json {
                    return sqlite3_column_type(localStatsStmt, col) == SQLITE_NULL
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(sqlite3_column_double(localStatsStmt, col));
                };
                match["local_stats"] = {
                    {"boost_collected", optInt(0)},
                    {"demoed", optInt(1)},
                    {"crossbars", optInt(2)},
                    {"hardest_crossbar", optDouble(3)},
                    {"max_ball_speed", optDouble(4)},
                    {"own_goals", optInt(5)},
                    {"duration_seconds", optDouble(6)},
                    {"overtime_seconds", optDouble(7)},
                    {"stats_version", sqlite3_column_int(localStatsStmt, 8)},
                };
            }
        }

        matchesCsv << matchId << ","
                   << CsvEscape(match["timestamp"].get<std::string>()) << ","
                   << CsvEscape(match["arena"].get<std::string>()) << ","
                   << match["our_score"].get<int>() << ","
                   << match["their_score"].get<int>() << ","
                   << (match["win"].get<bool>() ? 1 : 0) << ","
                   << CsvEscape(match["match_guid"].get<std::string>()) << ","
                   << (match["playlist_id"].is_null()
                           ? ""
                           : std::to_string(match["playlist_id"].get<int>()))
                   << ","
                   << CsvEscape(match["gamemode"].get<std::string>()) << ","
                   << match["player_count"].get<int>() << "\n";

        const char* playersSql = "SELECT primary_id, name, team, mmr, mmr_estimated, is_opponent FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";
        sqlite3_stmt* playerStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, playersSql, -1, &playerStmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(playerStmt, 1, matchId);
            while (sqlite3_step(playerStmt) == SQLITE_ROW) {
                nlohmann::json player;
                player["primary_id"] = SqlColumnText(playerStmt, 0);
                player["name"] = SqlColumnText(playerStmt, 1);
                player["team"] = sqlite3_column_int(playerStmt, 2);
                player["mmr"] = sqlite3_column_int(playerStmt, 3);
                player["mmr_estimated"] = sqlite3_column_int(playerStmt, 4) != 0;
                player["is_opponent"] = sqlite3_column_int(playerStmt, 5) != 0;
                player["stats"] = nullptr;
                if (playerStatsStmt) {
                    sqlite3_reset(playerStatsStmt);
                    sqlite3_clear_bindings(playerStatsStmt);
                    sqlite3_bind_int(playerStatsStmt, 1, matchId);
                    const std::string pid = player["primary_id"].get<std::string>();
                    sqlite3_bind_text(playerStatsStmt, 2, pid.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(playerStatsStmt) == SQLITE_ROW) {
                        auto optInt = [&](int col) -> nlohmann::json {
                            return sqlite3_column_type(playerStatsStmt, col) == SQLITE_NULL
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(sqlite3_column_int(playerStatsStmt, col));
                        };
                        auto optDouble = [&](int col) -> nlohmann::json {
                            return sqlite3_column_type(playerStatsStmt, col) == SQLITE_NULL
                                       ? nlohmann::json(nullptr)
                                       : nlohmann::json(sqlite3_column_double(playerStatsStmt, col));
                        };
                        player["stats"] = {
                            {"score", optInt(0)},
                            {"goals", optInt(1)},
                            {"assists", optInt(2)},
                            {"saves", optInt(3)},
                            {"shots", optInt(4)},
                            {"demos", optInt(5)},
                            {"touches", optInt(6)},
                            {"car_touches", optInt(7)},
                            {"max_goal_speed", optDouble(8)},
                            {"fastest_goal_time", optDouble(9)},
                        };
                    }
                }
                match["players"].push_back(player);

                playersCsv << matchId << ","
                           << CsvEscape(player["primary_id"].get<std::string>()) << ","
                           << CsvEscape(player["name"].get<std::string>()) << ","
                           << player["team"].get<int>() << ","
                           << player["mmr"].get<int>() << ","
                           << (player["mmr_estimated"].get<bool>() ? 1 : 0) << ","
                           << (player["is_opponent"].get<bool>() ? 1 : 0) << "\n";
            }
        }
        if (playerStmt) sqlite3_finalize(playerStmt);

        matches.push_back(match);
    }
    sqlite3_finalize(matchStmt);
    if (playerStatsStmt) sqlite3_finalize(playerStatsStmt);
    if (localStatsStmt) sqlite3_finalize(localStatsStmt);

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
        sqlite3_stmt* sessStmt = nullptr;
        const char* sessSql =
            "SELECT id, account, COALESCE(started_at, 0), COALESCE(ended_at, 0), COALESCE(wins, 0), COALESCE(losses, 0), "
            "COALESCE(mmr_change_json, '{}'), COALESCE(totals_json, '{}'), COALESCE(source, 'live') "
            "FROM Sessions ORDER BY started_at ASC, id ASC;";
        if (sqlite3_prepare_v2(m_db, sessSql, -1, &sessStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(sessStmt) == SQLITE_ROW) {
                const int64_t sid = sqlite3_column_int64(sessStmt, 0);
                const std::string account = SqlColumnText(sessStmt, 1);
                const int64_t startedAt = sqlite3_column_int64(sessStmt, 2);
                const int64_t endedAt = sqlite3_column_int64(sessStmt, 3);
                const int wins = sqlite3_column_int(sessStmt, 4);
                const int losses = sqlite3_column_int(sessStmt, 5);
                const std::string mmrJsonStr = SqlColumnText(sessStmt, 6);
                const std::string totalsJsonStr = SqlColumnText(sessStmt, 7);
                const std::string source = SqlColumnText(sessStmt, 8);

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
            sqlite3_finalize(sessStmt);
        }
        std::ofstream sessJsonFile(exportPath + "sessions.json", std::ios::trunc);
        if (sessJsonFile.is_open()) {
            sessJsonFile << sessionsJson.dump(2);
        }
    }
    if (SqliteTableExists(m_db, "ReplayUploads")) {
        nlohmann::json replaysJson = nlohmann::json::array();
        std::ofstream replaysCsv(exportPath + "replay_uploads.csv", std::ios::trunc);
        if (replaysCsv.is_open()) {
            replaysCsv << "id,file_path,file_name,match_guid,state,attempts,next_attempt_at,last_http_status,last_error,ballchasing_id,ballchasing_url,created_at,updated_at\n";
        }
        sqlite3_stmt* ruStmt = nullptr;
        const char* ruSql =
            "SELECT id, file_path, file_name, COALESCE(match_guid, ''), state, attempts, next_attempt_at, last_http_status, COALESCE(last_error, ''), COALESCE(ballchasing_id, ''), COALESCE(ballchasing_url, ''), created_at, updated_at "
            "FROM ReplayUploads ORDER BY id ASC;";
        if (sqlite3_prepare_v2(m_db, ruSql, -1, &ruStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(ruStmt) == SQLITE_ROW) {
                int64_t rId = sqlite3_column_int64(ruStmt, 0);
                std::string filePath = SqlColumnText(ruStmt, 1);
                std::string fileName = SqlColumnText(ruStmt, 2);
                std::string matchGuid = SqlColumnText(ruStmt, 3);
                std::string state = SqlColumnText(ruStmt, 4);
                int attempts = sqlite3_column_int(ruStmt, 5);
                int64_t nextAttemptAt = sqlite3_column_int64(ruStmt, 6);
                int lastHttpStatus = sqlite3_column_int(ruStmt, 7);
                std::string lastError = SqlColumnText(ruStmt, 8);
                std::string ballchasingId = SqlColumnText(ruStmt, 9);
                std::string ballchasingUrl = SqlColumnText(ruStmt, 10);
                int64_t createdAt = sqlite3_column_int64(ruStmt, 11);
                int64_t updatedAt = sqlite3_column_int64(ruStmt, 12);

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
            sqlite3_finalize(ruStmt);
        }
        std::ofstream ruJsonFile(exportPath + "replay_uploads.json", std::ios::trunc);
        if (ruJsonFile.is_open()) {
            ruJsonFile << replaysJson.dump(2);
        }
    }
    if (SqliteTableExists(m_db, "TrackedAccounts")) {
        nlohmann::json accountsJson = nlohmann::json::array();
        std::map<std::string, nlohmann::json> ranksByPrimary;
        if (SqliteTableExists(m_db, "TrackedAccountRanks")) {
            const char* ranksSql =
                "SELECT primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at "
                "FROM TrackedAccountRanks ORDER BY primary_id ASC, playlist_key ASC;";
            sqlite3_stmt* rStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, ranksSql, -1, &rStmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(rStmt) == SQLITE_ROW) {
                    std::string pid = SqlColumnText(rStmt, 0);
                    nlohmann::json rObj;
                    rObj["playlist_key"] = SqlColumnText(rStmt, 1);
                    if (sqlite3_column_type(rStmt, 2) == SQLITE_NULL) {
                        rObj["mmr"] = nullptr;
                    } else {
                        rObj["mmr"] = sqlite3_column_int(rStmt, 2);
                    }
                    if (sqlite3_column_type(rStmt, 3) == SQLITE_NULL) {
                        rObj["rank_tier"] = nullptr;
                    } else {
                        rObj["rank_tier"] = SqlColumnText(rStmt, 3);
                    }
                    if (sqlite3_column_type(rStmt, 4) == SQLITE_NULL) {
                        rObj["matches_played"] = nullptr;
                    } else {
                        rObj["matches_played"] = sqlite3_column_int(rStmt, 4);
                    }
                    if (sqlite3_column_type(rStmt, 5) == SQLITE_NULL) {
                        rObj["source"] = nullptr;
                    } else {
                        rObj["source"] = SqlColumnText(rStmt, 5);
                    }
                    rObj["fetched_at"] = sqlite3_column_int64(rStmt, 6);
                    ranksByPrimary[pid].push_back(std::move(rObj));
                }
                sqlite3_finalize(rStmt);
            }
        }

        const char* accSql =
            "SELECT primary_id, platform, platform_account_id, nickname, last_known_name, "
            "created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, "
            "sort_order, discovery_source FROM TrackedAccounts "
            "ORDER BY sort_order ASC, created_at ASC, primary_id ASC;";
        sqlite3_stmt* accStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, accSql, -1, &accStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(accStmt) == SQLITE_ROW) {
                std::string pid = SqlColumnText(accStmt, 0);
                nlohmann::json aObj;
                aObj["nickname"] = SqlColumnText(accStmt, 3);
                aObj["platform"] = SqlColumnText(accStmt, 1);
                aObj["platform_account_id"] = SqlColumnText(accStmt, 2);
                aObj["primary_id"] = pid;
                if (sqlite3_column_type(accStmt, 4) == SQLITE_NULL) {
                    aObj["last_known_name"] = nullptr;
                } else {
                    aObj["last_known_name"] = SqlColumnText(accStmt, 4);
                }
                aObj["created_at"] = sqlite3_column_int64(accStmt, 5);
                if (sqlite3_column_type(accStmt, 6) == SQLITE_NULL) {
                    aObj["last_seen_at"] = nullptr;
                } else {
                    aObj["last_seen_at"] = sqlite3_column_int64(accStmt, 6);
                }
                if (sqlite3_column_type(accStmt, 7) == SQLITE_NULL) {
                    aObj["last_refresh_attempt_at"] = nullptr;
                } else {
                    aObj["last_refresh_attempt_at"] = sqlite3_column_int64(accStmt, 7);
                }
                if (sqlite3_column_type(accStmt, 8) == SQLITE_NULL) {
                    aObj["last_successful_refresh_at"] = nullptr;
                } else {
                    aObj["last_successful_refresh_at"] = sqlite3_column_int64(accStmt, 8);
                }
                aObj["sort_order"] = sqlite3_column_int64(accStmt, 9);
                aObj["discovery_source"] = SqlColumnText(accStmt, 10);
                auto rIt = ranksByPrimary.find(pid);
                if (rIt != ranksByPrimary.end()) {
                    aObj["ranks"] = rIt->second;
                } else {
                    aObj["ranks"] = nlohmann::json::array();
                }
                accountsJson.push_back(std::move(aObj));
            }
            sqlite3_finalize(accStmt);
        }

        std::ofstream taJsonFile(exportPath + "tracked_accounts.json", std::ios::trunc);
        if (taJsonFile.is_open()) {
            taJsonFile << accountsJson.dump(2);
        }
    }
    if (SqliteTableExists(m_db, "TrackedAccountRankObservations")) {
        std::ofstream historyCsv(exportPath + "tracked_account_rank_observations.csv", std::ios::trunc);
        if (!historyCsv.is_open()) {
            error = "Failed to open account rank history export.";
            return false;
        }
        historyCsv << "primary_id,playlist_key,mmr,rank_tier,matches_played,source,observed_at\n";
        const char* sql = "SELECT primary_id,playlist_key,mmr,rank_tier,matches_played,source,observed_at "
                          "FROM TrackedAccountRankObservations ORDER BY primary_id,playlist_key,observed_at,id;";
        sqlite3_stmt* historyStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, sql, -1, &historyStmt, nullptr) != SQLITE_OK) {
            error = "Failed to query account rank observations for export.";
            return false;
        }
        bool queryOk = true;
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(historyStmt)) == SQLITE_ROW) {
            for (int col = 0; col < 7; ++col) {
                if (col) historyCsv << ',';
                if (sqlite3_column_type(historyStmt, col) != SQLITE_NULL)
                    historyCsv << CsvEscape(SqlColumnText(historyStmt, col));
            }
            historyCsv << '\n';
        }
        if (rc != SQLITE_DONE || !historyCsv.good()) queryOk = false;
        sqlite3_finalize(historyStmt);
        if (!queryOk) {
            error = "Failed to export all account rank observations.";
            return false;
        }
    }
    if (SqliteTableExists(m_db, "PlayerNotes")) {
        nlohmann::json notesJson = nlohmann::json::array();
        const char* notesSql =
            "SELECT platform, account_id, splitscreen, note, last_known_name, created_at, updated_at "
            "FROM PlayerNotes ORDER BY updated_at DESC;";
        sqlite3_stmt* nStmt = nullptr;
        if (sqlite3_prepare_v2(m_db, notesSql, -1, &nStmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(nStmt) == SQLITE_ROW) {
                notesJson.push_back({{"platform", SqlColumnText(nStmt, 0)},
                                     {"account_id", SqlColumnText(nStmt, 1)},
                                     {"splitscreen", sqlite3_column_int(nStmt, 2)},
                                     {"note", SqlColumnText(nStmt, 3)},
                                     {"last_known_name", SqlColumnText(nStmt, 4)},
                                     {"created_at", sqlite3_column_int64(nStmt, 5)},
                                     {"updated_at", sqlite3_column_int64(nStmt, 6)}});
            }
            sqlite3_finalize(nStmt);
        }
        std::ofstream notesFile(exportPath + "player_notes.json", std::ios::trunc);
        if (notesFile.is_open()) notesFile << notesJson.dump(2);
    }
    if (SqliteTableExists(m_db, "PlayerTags")) {
        std::ofstream tagsCsv(exportPath + "player_tags.csv", std::ios::trunc);
        if (!tagsCsv.is_open()) {
            error = "Couldn't open the local player tags export.";
            return false;
        }
        tagsCsv << "platform,account_id,splitscreen,tag,created_at\n";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(m_db, "SELECT platform,account_id,splitscreen,tag,created_at FROM PlayerTags;", -1, &stmt, nullptr) != SQLITE_OK) {
            error = "Couldn't query local player tags.";
            return false;
        }
        int rc = SQLITE_ROW;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            for (int col = 0; col < 5; ++col) {
                if (col) tagsCsv << ',';
                tagsCsv << CsvEscape(SqlColumnText(stmt, col));
            }
            tagsCsv << '\n';
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE || !tagsCsv.good()) {
            error = "Couldn't export all local player tags.";
            return false;
        }
    }
    error.clear();
    return true;
}

bool DatabaseManager::DeleteLocalMatchHistory(std::string& error) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
        error = "Database is not open.";
        return false;
    }

    char* errMsg = nullptr;
    std::string sql = "BEGIN IMMEDIATE;";
    if (SqliteTableExists(m_db, "MatchEvents")) sql += "DELETE FROM MatchEvents;";
    if (SqliteTableExists(m_db, "MatchPlayerStats")) sql += "DELETE FROM MatchPlayerStats;";
    if (SqliteTableExists(m_db, "MatchLocalStats")) sql += "DELETE FROM MatchLocalStats;";
    sql += "DELETE FROM MatchPlayers; DELETE FROM Matches; ";
    if (SqliteTableExists(m_db, "Sessions")) sql += "DELETE FROM Sessions; ";
    if (SqliteTableExists(m_db, "ReplayUploads")) sql += "UPDATE ReplayUploads SET match_guid = NULL; ";
    sql += "DELETE FROM sqlite_sequence WHERE name IN ('Matches', 'MatchPlayers', 'Sessions'); COMMIT;";
    if (sqlite3_exec(m_db, sql.c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
        error = errMsg ? errMsg : "Failed to delete local history.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    m_sessionIdByGeneration.clear();
    m_backfillPending.store(false, std::memory_order_release);
    error.clear();
    return true;
}

DbMergeResult DatabaseManager::MergeDatabase(const std::string& sourceDbPath) {
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

    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) {
        result.error = "Target database is not open.";
        return result;
    }

    if (!m_dbPath.empty()) {
        fs::path targetPath(m_dbPath);
        if (fs::equivalent(srcPath, targetPath, ec)) {
            result.error = "Cannot merge the active database into itself.";
            return result;
        }
    }

    sqlite3* srcDb = nullptr;
    int rc = sqlite3_open_v2(srcPath.string().c_str(), &srcDb, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK || !srcDb) {
        result.error = "Failed to open source database: " + std::string(srcDb ? sqlite3_errmsg(srcDb) : "unknown error");
        if (srcDb) sqlite3_close(srcDb);
        return result;
    }

    auto tableExists = [](sqlite3* db, const char* name) -> bool {
        const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    };

    if (!tableExists(srcDb, "Matches")) {
        result.error = "Selected file is not an OmniStats database (missing Matches table).";
        sqlite3_close(srcDb);
        return result;
    }

    if (!tableExists(srcDb, "MatchPlayers")) {
        result.error = "Selected file is not an OmniStats database (missing MatchPlayers table).";
        sqlite3_close(srcDb);
        return result;
    }

    const bool srcHasPlaylistId = SqliteTableHasColumn(srcDb, "Matches", "playlist_id");
    const bool srcHasResultPending = SqliteTableHasColumn(srcDb, "Matches", "result_pending");
    const bool srcHasMmrEstimated = SqliteTableHasColumn(srcDb, "MatchPlayers", "mmr_estimated");
    const bool srcHasIsOpponent = SqliteTableHasColumn(srcDb, "MatchPlayers", "is_opponent");

    const bool copyPlayerStats =
        tableExists(srcDb, "MatchPlayerStats") && SqliteTableExists(m_db, "MatchPlayerStats");
    const bool copyLocalStats =
        tableExists(srcDb, "MatchLocalStats") && SqliteTableExists(m_db, "MatchLocalStats");
    const bool copyEvents = tableExists(srcDb, "MatchEvents") && SqliteTableExists(m_db, "MatchEvents");
    char* errMsg = nullptr;
    if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        result.error = errMsg ? errMsg : "Failed to begin transaction on target database.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_close(srcDb);
        return result;
    }

    const char* checkGuidSql = "SELECT id FROM Matches WHERE match_guid = ? AND match_guid != '' LIMIT 1;";
    sqlite3_stmt* checkGuidStmt = nullptr;
    const char* checkFallbackSql = "SELECT id FROM Matches WHERE timestamp = ? AND arena = ? AND our_score = ? AND their_score = ? AND win = ? LIMIT 1;";
    sqlite3_stmt* checkFallbackStmt = nullptr;

    const char* insertMatchSql =
        "INSERT INTO Matches (timestamp, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, result_pending) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertMatchStmt = nullptr;

    const char* insertPlayerSql =
        "INSERT INTO MatchPlayers (match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent) "
        "VALUES (?, ?, ?, ?, ?, ?, ?);";
    sqlite3_stmt* insertPlayerStmt = nullptr;

    if (sqlite3_prepare_v2(m_db, checkGuidSql, -1, &checkGuidStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, checkFallbackSql, -1, &checkFallbackStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, insertMatchSql, -1, &insertMatchStmt, nullptr) != SQLITE_OK ||
        sqlite3_prepare_v2(m_db, insertPlayerSql, -1, &insertPlayerStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to prepare target database statements.";
        if (checkGuidStmt) sqlite3_finalize(checkGuidStmt);
        if (checkFallbackStmt) sqlite3_finalize(checkFallbackStmt);
        if (insertMatchStmt) sqlite3_finalize(insertMatchStmt);
        if (insertPlayerStmt) sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    std::string selectMatchesSql = "SELECT id, timestamp, arena, our_score, their_score, win, match_guid, ";
    selectMatchesSql += srcHasPlaylistId ? "playlist_id, " : "NULL, ";
    selectMatchesSql += "gamemode, player_count, ";
    selectMatchesSql += srcHasResultPending ? "COALESCE(result_pending, 0) " : "0 ";
    selectMatchesSql += "FROM Matches ORDER BY id ASC;";

    sqlite3_stmt* selectMatchesStmt = nullptr;
    if (sqlite3_prepare_v2(srcDb, selectMatchesSql.c_str(), -1, &selectMatchesStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to read matches from source database: " + std::string(sqlite3_errmsg(srcDb));
        sqlite3_finalize(checkGuidStmt);
        sqlite3_finalize(checkFallbackStmt);
        sqlite3_finalize(insertMatchStmt);
        sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    std::string selectPlayersSql = "SELECT primary_id, name, team, mmr, ";
    selectPlayersSql += srcHasMmrEstimated ? "mmr_estimated, " : "0, ";
    selectPlayersSql += srcHasIsOpponent ? "is_opponent " : "0 ";
    selectPlayersSql += "FROM MatchPlayers WHERE match_id = ? ORDER BY id ASC;";

    sqlite3_stmt* selectPlayersStmt = nullptr;
    if (sqlite3_prepare_v2(srcDb, selectPlayersSql.c_str(), -1, &selectPlayersStmt, nullptr) != SQLITE_OK) {
        result.error = "Failed to prepare player read statement from source database: " + std::string(sqlite3_errmsg(srcDb));
        sqlite3_finalize(selectMatchesStmt);
        sqlite3_finalize(checkGuidStmt);
        sqlite3_finalize(checkFallbackStmt);
        sqlite3_finalize(insertMatchStmt);
        sqlite3_finalize(insertPlayerStmt);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        sqlite3_close(srcDb);
        return result;
    }

    sqlite3_stmt* selectPlayerStatsStmt = nullptr;
    sqlite3_stmt* insertPlayerStatsStmt = nullptr;
    if (copyPlayerStats) {
        const char* selectPlayerStatsSql =
            "SELECT primary_id, score, goals, assists, saves, shots, demos, touches, car_touches, "
            "max_goal_speed, fastest_goal_time FROM MatchPlayerStats WHERE match_id = ?;";
        const char* insertPlayerStatsSql =
            "INSERT OR REPLACE INTO MatchPlayerStats (match_id, primary_id, score, goals, assists, saves, "
            "shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(srcDb, selectPlayerStatsSql, -1, &selectPlayerStatsStmt, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(m_db, insertPlayerStatsSql, -1, &insertPlayerStatsStmt, nullptr) != SQLITE_OK) {
            result.error = "Failed to prepare MatchPlayerStats merge statements.";
            if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
            if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
            sqlite3_finalize(selectPlayersStmt);
            sqlite3_finalize(selectMatchesStmt);
            sqlite3_finalize(checkGuidStmt);
            sqlite3_finalize(checkFallbackStmt);
            sqlite3_finalize(insertMatchStmt);
            sqlite3_finalize(insertPlayerStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(srcDb);
            return result;
        }
    }

    sqlite3_stmt* selectLocalStatsStmt = nullptr;
    sqlite3_stmt* insertLocalStatsStmt = nullptr;
    if (copyLocalStats) {
        const char* selectLocalStatsSql =
            "SELECT boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, "
            "duration_seconds, overtime_seconds, stats_version FROM MatchLocalStats WHERE match_id = ?;";
        const char* insertLocalStatsSql =
            "INSERT OR REPLACE INTO MatchLocalStats (match_id, boost_collected, demoed, crossbars, "
            "hardest_crossbar, max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
        if (sqlite3_prepare_v2(srcDb, selectLocalStatsSql, -1, &selectLocalStatsStmt, nullptr) != SQLITE_OK ||
            sqlite3_prepare_v2(m_db, insertLocalStatsSql, -1, &insertLocalStatsStmt, nullptr) != SQLITE_OK) {
            result.error = "Failed to prepare MatchLocalStats merge statements.";
            if (selectLocalStatsStmt) sqlite3_finalize(selectLocalStatsStmt);
            if (insertLocalStatsStmt) sqlite3_finalize(insertLocalStatsStmt);
            if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
            if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
            sqlite3_finalize(selectPlayersStmt);
            sqlite3_finalize(selectMatchesStmt);
            sqlite3_finalize(checkGuidStmt);
            sqlite3_finalize(checkFallbackStmt);
            sqlite3_finalize(insertMatchStmt);
            sqlite3_finalize(insertPlayerStmt);
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            sqlite3_close(srcDb);
            return result;
        }
    }
    bool loopOk = true;
    while (sqlite3_step(selectMatchesStmt) == SQLITE_ROW) {
        int64_t oldMatchId = sqlite3_column_int64(selectMatchesStmt, 0);
        std::string timestamp = SqlColumnText(selectMatchesStmt, 1);
        std::string arena = SqlColumnText(selectMatchesStmt, 2);
        int ourScore = sqlite3_column_int(selectMatchesStmt, 3);
        int theirScore = sqlite3_column_int(selectMatchesStmt, 4);
        int win = sqlite3_column_int(selectMatchesStmt, 5);
        std::string matchGuid = SqlColumnText(selectMatchesStmt, 6);
        bool playlistNull = (sqlite3_column_type(selectMatchesStmt, 7) == SQLITE_NULL);
        int playlistId = playlistNull ? 0 : sqlite3_column_int(selectMatchesStmt, 7);
        std::string gamemode = SqlColumnText(selectMatchesStmt, 8);
        int playerCount = sqlite3_column_int(selectMatchesStmt, 9);
        const int resultPending = sqlite3_column_int(selectMatchesStmt, 10);

        if (arena.empty()) {
            result.matchesSkipped++;
            continue;
        }

        bool isDuplicate = false;
        if (!matchGuid.empty()) {
            sqlite3_reset(checkGuidStmt);
            sqlite3_clear_bindings(checkGuidStmt);
            sqlite3_bind_text(checkGuidStmt, 1, matchGuid.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(checkGuidStmt) == SQLITE_ROW) {
                isDuplicate = true;
            }
        }

        if (!isDuplicate && !timestamp.empty()) {
            sqlite3_reset(checkFallbackStmt);
            sqlite3_clear_bindings(checkFallbackStmt);
            sqlite3_bind_text(checkFallbackStmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(checkFallbackStmt, 2, arena.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(checkFallbackStmt, 3, ourScore);
            sqlite3_bind_int(checkFallbackStmt, 4, theirScore);
            sqlite3_bind_int(checkFallbackStmt, 5, win);
            if (sqlite3_step(checkFallbackStmt) == SQLITE_ROW) {
                isDuplicate = true;
            }
        }

        if (isDuplicate) {
            result.matchesSkipped++;
            continue;
        }

        sqlite3_reset(insertMatchStmt);
        sqlite3_clear_bindings(insertMatchStmt);
        sqlite3_bind_text(insertMatchStmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(insertMatchStmt, 2, arena.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(insertMatchStmt, 3, ourScore);
        sqlite3_bind_int(insertMatchStmt, 4, theirScore);
        sqlite3_bind_int(insertMatchStmt, 5, win);
        sqlite3_bind_text(insertMatchStmt, 6, matchGuid.c_str(), -1, SQLITE_STATIC);
        if (playlistNull) {
            sqlite3_bind_null(insertMatchStmt, 7);
        } else {
            sqlite3_bind_int(insertMatchStmt, 7, playlistId);
        }
        sqlite3_bind_text(insertMatchStmt, 8, gamemode.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(insertMatchStmt, 9, playerCount);
        sqlite3_bind_int(insertMatchStmt, 10, resultPending);

        if (sqlite3_step(insertMatchStmt) != SQLITE_DONE) {
            result.error = "Failed to insert match into target database: " + std::string(sqlite3_errmsg(m_db));
            loopOk = false;
            break;
        }

        sqlite3_int64 newMatchId = sqlite3_last_insert_rowid(m_db);
        result.matchesImported++;

        sqlite3_reset(selectPlayersStmt);
        sqlite3_clear_bindings(selectPlayersStmt);
        sqlite3_bind_int64(selectPlayersStmt, 1, oldMatchId);

        while (sqlite3_step(selectPlayersStmt) == SQLITE_ROW) {
            std::string primaryId = SqlColumnText(selectPlayersStmt, 0);
            std::string name = SqlColumnText(selectPlayersStmt, 1);
            int team = sqlite3_column_int(selectPlayersStmt, 2);
            int mmr = sqlite3_column_int(selectPlayersStmt, 3);
            int mmrEstimated = sqlite3_column_int(selectPlayersStmt, 4);
            int isOpponent = sqlite3_column_int(selectPlayersStmt, 5);

            sqlite3_reset(insertPlayerStmt);
            sqlite3_clear_bindings(insertPlayerStmt);
            sqlite3_bind_int64(insertPlayerStmt, 1, newMatchId);
            sqlite3_bind_text(insertPlayerStmt, 2, primaryId.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(insertPlayerStmt, 3, name.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(insertPlayerStmt, 4, team);
            sqlite3_bind_int(insertPlayerStmt, 5, mmr);
            sqlite3_bind_int(insertPlayerStmt, 6, mmrEstimated);
            sqlite3_bind_int(insertPlayerStmt, 7, isOpponent);

            if (sqlite3_step(insertPlayerStmt) != SQLITE_DONE) {
                result.error = "Failed to insert player into target database: " + std::string(sqlite3_errmsg(m_db));
                loopOk = false;
                break;
            }
            result.playersImported++;
        }

        if (!loopOk) break;
        if (copyEvents) {
            const char* fromSql = "SELECT event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key,format_version FROM MatchEvents WHERE match_id=? ORDER BY id LIMIT 512;";
            const char* toSql = "INSERT OR IGNORE INTO MatchEvents (match_id,event_type,observed_at_ms,match_clock_seconds,actor_id,actor_name,secondary_id,secondary_name,team,value,ball_speed,source,event_key,format_version,match_guid) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
            sqlite3_stmt* from = nullptr;
            sqlite3_stmt* to = nullptr;
            if (sqlite3_prepare_v2(srcDb, fromSql, -1, &from, nullptr) != SQLITE_OK ||
                sqlite3_prepare_v2(m_db, toSql, -1, &to, nullptr) != SQLITE_OK) {
                result.error = "Failed to prepare match journal import.";
                loopOk = false;
            } else {
                sqlite3_bind_int64(from, 1, oldMatchId);
                while (sqlite3_step(from) == SQLITE_ROW) {
                    sqlite3_reset(to);
                    sqlite3_clear_bindings(to);
                    sqlite3_bind_int64(to, 1, newMatchId);
                    for (int col = 0; col < 13; ++col) {
                        const int dst = col + 2;
                        const int type = sqlite3_column_type(from, col);
                        if (type == SQLITE_NULL)
                            sqlite3_bind_null(to, dst);
                        else if (type == SQLITE_INTEGER)
                            sqlite3_bind_int64(to, dst, sqlite3_column_int64(from, col));
                        else if (type == SQLITE_FLOAT)
                            sqlite3_bind_double(to, dst, sqlite3_column_double(from, col));
                        else
                            sqlite3_bind_text(to, dst, SqlColumnText(from, col).c_str(), -1, SQLITE_TRANSIENT);
                    }
                    sqlite3_bind_text(to, 15, matchGuid.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(to) != SQLITE_DONE) {
                        result.error = "Failed to import match journal.";
                        loopOk = false;
                        break;
                    }
                }
            }
            sqlite3_finalize(from);
            sqlite3_finalize(to);
            if (!loopOk) break;
        }
        if (copyPlayerStats) {
            sqlite3_reset(selectPlayerStatsStmt);
            sqlite3_clear_bindings(selectPlayerStatsStmt);
            sqlite3_bind_int64(selectPlayerStatsStmt, 1, oldMatchId);
            while (sqlite3_step(selectPlayerStatsStmt) == SQLITE_ROW) {
                std::string primaryId = SqlColumnText(selectPlayerStatsStmt, 0);
                sqlite3_reset(insertPlayerStatsStmt);
                sqlite3_clear_bindings(insertPlayerStatsStmt);
                sqlite3_bind_int64(insertPlayerStatsStmt, 1, newMatchId);
                sqlite3_bind_text(insertPlayerStatsStmt, 2, primaryId.c_str(), -1, SQLITE_TRANSIENT);
                for (int col = 1; col <= 8; ++col) {
                    if (sqlite3_column_type(selectPlayerStatsStmt, col) == SQLITE_NULL) {
                        sqlite3_bind_null(insertPlayerStatsStmt, col + 2);
                    } else {
                        sqlite3_bind_int(insertPlayerStatsStmt, col + 2, sqlite3_column_int(selectPlayerStatsStmt, col));
                    }
                }
                for (int col = 9; col <= 10; ++col) {
                    if (sqlite3_column_type(selectPlayerStatsStmt, col) == SQLITE_NULL) {
                        sqlite3_bind_null(insertPlayerStatsStmt, col + 2);
                    } else {
                        sqlite3_bind_double(insertPlayerStatsStmt, col + 2, sqlite3_column_double(selectPlayerStatsStmt, col));
                    }
                }
                if (sqlite3_step(insertPlayerStatsStmt) != SQLITE_DONE) {
                    result.error = "Failed to insert MatchPlayerStats into target database: " + std::string(sqlite3_errmsg(m_db));
                    loopOk = false;
                    break;
                }
            }
            if (!loopOk) break;
        }

        if (copyLocalStats) {
            sqlite3_reset(selectLocalStatsStmt);
            sqlite3_clear_bindings(selectLocalStatsStmt);
            sqlite3_bind_int64(selectLocalStatsStmt, 1, oldMatchId);
            if (sqlite3_step(selectLocalStatsStmt) == SQLITE_ROW) {
                sqlite3_reset(insertLocalStatsStmt);
                sqlite3_clear_bindings(insertLocalStatsStmt);
                sqlite3_bind_int64(insertLocalStatsStmt, 1, newMatchId);
                auto bindOptInt = [&](int srcCol, int dstParam) {
                    if (sqlite3_column_type(selectLocalStatsStmt, srcCol) == SQLITE_NULL) {
                        sqlite3_bind_null(insertLocalStatsStmt, dstParam);
                    } else {
                        sqlite3_bind_int(insertLocalStatsStmt, dstParam, sqlite3_column_int(selectLocalStatsStmt, srcCol));
                    }
                };
                auto bindOptDouble = [&](int srcCol, int dstParam) {
                    if (sqlite3_column_type(selectLocalStatsStmt, srcCol) == SQLITE_NULL) {
                        sqlite3_bind_null(insertLocalStatsStmt, dstParam);
                    } else {
                        sqlite3_bind_double(insertLocalStatsStmt, dstParam, sqlite3_column_double(selectLocalStatsStmt, srcCol));
                    }
                };
                bindOptInt(0, 2);
                bindOptInt(1, 3);
                bindOptInt(2, 4);
                bindOptDouble(3, 5);
                bindOptDouble(4, 6);
                bindOptInt(5, 7);
                bindOptDouble(6, 8);
                bindOptDouble(7, 9);
                sqlite3_bind_int(insertLocalStatsStmt, 10, sqlite3_column_int(selectLocalStatsStmt, 8));
                if (sqlite3_step(insertLocalStatsStmt) != SQLITE_DONE) {
                    result.error = "Failed to insert MatchLocalStats into target database: " + std::string(sqlite3_errmsg(m_db));
                    loopOk = false;
                    break;
                }
            }
            if (!loopOk) break;
        }
    }

    sqlite3_finalize(selectPlayersStmt);
    if (selectLocalStatsStmt) sqlite3_finalize(selectLocalStatsStmt);
    if (insertLocalStatsStmt) sqlite3_finalize(insertLocalStatsStmt);
    if (selectPlayerStatsStmt) sqlite3_finalize(selectPlayerStatsStmt);
    if (insertPlayerStatsStmt) sqlite3_finalize(insertPlayerStatsStmt);
    sqlite3_finalize(selectMatchesStmt);
    sqlite3_finalize(checkGuidStmt);
    sqlite3_finalize(checkFallbackStmt);
    sqlite3_finalize(insertMatchStmt);
    sqlite3_finalize(insertPlayerStmt);
    if (loopOk && tableExists(srcDb, "ReplayUploads") && SqliteTableExists(m_db, "ReplayUploads")) {
        const char* selReplaySql =
            "SELECT file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, ballchasing_id, ballchasing_url, created_at, updated_at FROM ReplayUploads;";
        sqlite3_stmt* selReplayStmt = nullptr;
        if (sqlite3_prepare_v2(srcDb, selReplaySql, -1, &selReplayStmt, nullptr) == SQLITE_OK) {
            const char* insReplaySql =
                "INSERT OR IGNORE INTO ReplayUploads (file_path, file_name, match_guid, state, attempts, next_attempt_at, last_http_status, last_error, ballchasing_id, ballchasing_url, created_at, updated_at) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12);";
            sqlite3_stmt* insReplayStmt = nullptr;
            if (sqlite3_prepare_v2(m_db, insReplaySql, -1, &insReplayStmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(selReplayStmt) == SQLITE_ROW) {
                    for (int i = 0; i < 12; ++i) {
                        if (sqlite3_column_type(selReplayStmt, i) == SQLITE_NULL) {
                            sqlite3_bind_null(insReplayStmt, i + 1);
                        } else if (i == 4 || i == 6) {
                            sqlite3_bind_int(insReplayStmt, i + 1, sqlite3_column_int(selReplayStmt, i));
                        } else if (i == 5 || i == 10 || i == 11) {
                            sqlite3_bind_int64(insReplayStmt, i + 1, sqlite3_column_int64(selReplayStmt, i));
                        } else {
                            std::string txt = SqlColumnText(selReplayStmt, i);
                            sqlite3_bind_text(insReplayStmt, i + 1, txt.c_str(), -1, SQLITE_TRANSIENT);
                        }
                    }
                    sqlite3_step(insReplayStmt);
                    sqlite3_reset(insReplayStmt);
                    sqlite3_clear_bindings(insReplayStmt);
                }
                sqlite3_finalize(insReplayStmt);
            }
            sqlite3_finalize(selReplayStmt);
        }
    }
    sqlite3_close(srcDb);

    if (!loopOk) {
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return result;
    }

    if (sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        result.error = errMsg ? errMsg : "Failed to commit merge transaction.";
        if (errMsg) sqlite3_free(errMsg);
        sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return result;
    }
    while (result.matchesImported > 0) {
        bool moreRemaining = false;
        int linked = 0;
        BackfillSessionsChunkLocked(500, moreRemaining, linked);
        if (!moreRemaining || linked == 0) break;
    }
    result.success = true;
    return result;
}
