#include "ReplayDriver.hpp"
#include "network/http/CurlImpersonate.hpp"
#include "network/omninet/OmniNet.hpp"
#include "core/config/Config.hpp"
#include "utils/Storage.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <thread>
#include <unordered_map>

namespace omnistats::replay {

    namespace {

        struct ScopedOfflineCurlMock {
            pfn_curl_easy_perform orig_perform = nullptr;
            pfn_curl_easy_getinfo orig_getinfo = nullptr;
            pfn_curl_easy_setopt orig_setopt = nullptr;
            pfn_curl_easy_cleanup orig_cleanup = nullptr;
            pfn_curl_slist_free_all orig_slist_free = nullptr;
            pfn_curl_slist_append orig_slist_append = nullptr;
            pfn_curl_easy_init orig_init = nullptr;
            pfn_curl_easy_escape orig_escape = nullptr;
            pfn_curl_free orig_free = nullptr;
            pfn_curl_easy_impersonate orig_impersonate = nullptr;
            bool orig_ready = false;

            ScopedOfflineCurlMock() {
                auto& curl = CurlImpersonate::Instance();
                orig_perform = curl.easy_perform;
                orig_getinfo = curl.easy_getinfo;
                orig_setopt = curl.easy_setopt;
                orig_cleanup = curl.easy_cleanup;
                orig_slist_free = curl.slist_free_all;
                orig_slist_append = curl.slist_append;
                orig_init = curl.easy_init;
                orig_escape = curl.easy_escape;
                orig_free = curl.free_ptr;
                orig_impersonate = curl.easy_impersonate;
                orig_ready = curl.IsReady();

                curl.easy_init = []() -> void* { return (void*)1; };
                curl.easy_cleanup = [](void*) {};
                curl.easy_setopt = [](void*, int, ...) -> int { return 0; };
                curl.easy_perform = [](void*) -> int { return 0; };
                curl.easy_getinfo = [](void*, int info, ...) -> int {
                    if (info == CI_CURLINFO_RESPONSE_CODE) {
                        va_list args;
                        va_start(args, info);
                        long* code = va_arg(args, long*);
                        if (code) *code = 200;
                        va_end(args);
                    }
                    return 0;
                };
                curl.easy_escape = [](void*, const char* val, int len) -> char* {
                    static std::string buf;
                    buf.assign(val, static_cast<size_t>(len));
                    return buf.data();
                };
                curl.free_ptr = [](void*) {};
                curl.slist_append = [](void*, const char*) -> void* { return (void*)1; };
                curl.slist_free_all = [](void*) {};
                curl.easy_impersonate = [](void*, const char*, int) -> int { return 0; };
                curl.SetReadyForTests(true);
                OmniNet::ResetUpdateRequiredForTesting();
                OmniNet::SetTransportForTesting([](const OmniNet::Request&) {
                    OmniNet::Response res;
                    res.status = 200;
                    res.body = "{}";
                    return res;
                });
            }

            ~ScopedOfflineCurlMock() {
                auto& curl = CurlImpersonate::Instance();
                curl.easy_perform = orig_perform;
                curl.easy_getinfo = orig_getinfo;
                curl.easy_setopt = orig_setopt;
                curl.easy_cleanup = orig_cleanup;
                curl.slist_free_all = orig_slist_free;
                curl.slist_append = orig_slist_append;
                curl.easy_init = orig_init;
                curl.easy_escape = orig_escape;
                curl.free_ptr = orig_free;
                curl.easy_impersonate = orig_impersonate;
                curl.SetReadyForTests(orig_ready);
                OmniNet::SetTransportForTesting({});
                OmniNet::ResetUpdateRequiredForTesting();
            }
        };

        static float Round1(float v) {
            return std::round(v * 10.0f) / 10.0f;
        }

    } // namespace

    ReplayDriver::ReplayDriver(Options options) : m_options(std::move(options)) {}
    ReplayDriver::~ReplayDriver() = default;

    bool ReplayDriver::DrainDatabase(const std::shared_ptr<DatabaseManager>& dbManager, int timeoutMs) {
        if (!dbManager) return true;
        static std::atomic<uint64_t> s_barrierIndex{0};
        const std::string barrierKey = "__replay_drain_barrier_" + std::to_string(++s_barrierIndex);
        dbManager->AsyncSetSetting(barrierKey, "complete");

        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start)
                   .count() < timeoutMs) {
            if (dbManager->GetSetting(barrierKey, "") == "complete") {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return dbManager->GetSetting(barrierKey, "") == "complete";
    }

    nlohmann::json ReplayDriver::BuildCanonicalGolden(
        sqlite3* db,
        const std::shared_ptr<SessionState>& state,
        const std::vector<StepSideEffectsSummary>& emittedEffects) {

        nlohmann::json root;
        std::unordered_map<int64_t, int64_t> matchIdMap;
        std::unordered_map<int64_t, int64_t> sessionIdMap;

        // 1. Matches
        nlohmann::json jMatches = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT id, arena, our_score, their_score, win, match_guid, playlist_id, gamemode, player_count, result_pending, session_id "
                "FROM Matches ORDER BY id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                int64_t nextMatchIdx = 1;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawId = sqlite3_column_int64(stmt, 0);
                    matchIdMap[rawId] = nextMatchIdx++;

                    nlohmann::json row;
                    row["id"] = matchIdMap[rawId];
                    row["timestamp"] = "<normalized_timestamp>";
                    row["arena"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    row["our_score"] = sqlite3_column_int(stmt, 2);
                    row["their_score"] = sqlite3_column_int(stmt, 3);
                    row["win"] = sqlite3_column_int(stmt, 4);
                    row["match_guid"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
                    if (sqlite3_column_type(stmt, 6) == SQLITE_NULL) {
                        row["playlist_id"] = nullptr;
                    } else {
                        row["playlist_id"] = sqlite3_column_int(stmt, 6);
                    }
                    row["gamemode"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                    row["player_count"] = sqlite3_column_int(stmt, 8);
                    row["result_pending"] = sqlite3_column_int(stmt, 9);
                    if (sqlite3_column_type(stmt, 10) == SQLITE_NULL) {
                        row["session_id"] = nullptr;
                    } else {
                        const int64_t rawSessId = sqlite3_column_int64(stmt, 10);
                        if (sessionIdMap.find(rawSessId) == sessionIdMap.end()) {
                            sessionIdMap[rawSessId] = sessionIdMap.size() + 1;
                        }
                        row["session_id"] = sessionIdMap[rawSessId];
                    }
                    jMatches.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 2. MatchPlayers
        nlohmann::json jMatchPlayers = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT id, match_id, primary_id, name, team, mmr, mmr_estimated, is_opponent "
                "FROM MatchPlayers ORDER BY match_id ASC, team ASC, primary_id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                int64_t playerRowIdx = 1;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawMatchId = sqlite3_column_int64(stmt, 1);
                    nlohmann::json row;
                    row["id"] = playerRowIdx++;
                    row["match_id"] = matchIdMap.count(rawMatchId) ? matchIdMap[rawMatchId] : rawMatchId;
                    row["primary_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    row["name"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    row["team"] = sqlite3_column_int(stmt, 4);
                    row["mmr"] = sqlite3_column_int(stmt, 5);
                    row["mmr_estimated"] = sqlite3_column_int(stmt, 6);
                    row["is_opponent"] = sqlite3_column_int(stmt, 7);
                    jMatchPlayers.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 3. MatchPlayerStats
        nlohmann::json jMatchPlayerStats = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT match_id, primary_id, score, goals, assists, saves, shots, demos, touches, car_touches, max_goal_speed, fastest_goal_time "
                "FROM MatchPlayerStats ORDER BY match_id ASC, primary_id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawMatchId = sqlite3_column_int64(stmt, 0);
                    nlohmann::json row;
                    row["match_id"] = matchIdMap.count(rawMatchId) ? matchIdMap[rawMatchId] : rawMatchId;
                    row["primary_id"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    if (sqlite3_column_type(stmt, 2) == SQLITE_NULL) {
                        row["score"] = nullptr;
                    } else {
                        row["score"] = sqlite3_column_int(stmt, 2);
                    }
                    row["goals"] = sqlite3_column_int(stmt, 3);
                    row["assists"] = sqlite3_column_int(stmt, 4);
                    row["saves"] = sqlite3_column_int(stmt, 5);
                    row["shots"] = sqlite3_column_int(stmt, 6);
                    row["demos"] = sqlite3_column_int(stmt, 7);
                    if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
                        row["touches"] = nullptr;
                    } else {
                        row["touches"] = sqlite3_column_int(stmt, 8);
                    }
                    if (sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
                        row["car_touches"] = nullptr;
                    } else {
                        row["car_touches"] = sqlite3_column_int(stmt, 9);
                    }
                    if (sqlite3_column_type(stmt, 10) == SQLITE_NULL) {
                        row["max_goal_speed"] = nullptr;
                    } else {
                        row["max_goal_speed"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 10)));
                    }
                    if (sqlite3_column_type(stmt, 11) == SQLITE_NULL) {
                        row["fastest_goal_time"] = nullptr;
                    } else {
                        row["fastest_goal_time"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 11)));
                    }
                    jMatchPlayerStats.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 4. MatchLocalStats
        nlohmann::json jMatchLocalStats = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT match_id, boost_collected, demoed, crossbars, hardest_crossbar, max_ball_speed, own_goals, duration_seconds, overtime_seconds, stats_version "
                "FROM MatchLocalStats ORDER BY match_id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawMatchId = sqlite3_column_int64(stmt, 0);
                    nlohmann::json row;
                    row["match_id"] = matchIdMap.count(rawMatchId) ? matchIdMap[rawMatchId] : rawMatchId;
                    row["boost_collected"] = sqlite3_column_int(stmt, 1);
                    row["demoed"] = sqlite3_column_int(stmt, 2);
                    row["crossbars"] = sqlite3_column_int(stmt, 3);
                    if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
                        row["hardest_crossbar"] = nullptr;
                    } else {
                        row["hardest_crossbar"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 4)));
                    }
                    if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
                        row["max_ball_speed"] = nullptr;
                    } else {
                        row["max_ball_speed"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 5)));
                    }
                    row["own_goals"] = sqlite3_column_int(stmt, 6);
                    row["duration_seconds"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 7)));
                    row["overtime_seconds"] = Round1(static_cast<float>(sqlite3_column_double(stmt, 8)));
                    row["stats_version"] = sqlite3_column_int(stmt, 9);
                    jMatchLocalStats.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 5. MatchAnalytics
        nlohmann::json jMatchAnalytics = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT match_id, format_version, data FROM MatchAnalytics ORDER BY match_id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawMatchId = sqlite3_column_int64(stmt, 0);
                    nlohmann::json row;
                    row["match_id"] = matchIdMap.count(rawMatchId) ? matchIdMap[rawMatchId] : rawMatchId;
                    row["format_version"] = sqlite3_column_int(stmt, 1);
                    const char* dataStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    if (dataStr) {
                        try {
                            row["data"] = nlohmann::json::parse(dataStr);
                        } catch (...) {
                            row["data"] = dataStr;
                        }
                    } else {
                        row["data"] = nullptr;
                    }
                    jMatchAnalytics.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 6. Sessions
        nlohmann::json jSessions = nlohmann::json::array();
        if (db) {
            sqlite3_stmt* stmt = nullptr;
            const char* sql =
                "SELECT id, account, started_at, ended_at, wins, losses, mmr_change_json, totals_json, source "
                "FROM Sessions ORDER BY id ASC;";
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const int64_t rawId = sqlite3_column_int64(stmt, 0);
                    if (sessionIdMap.find(rawId) == sessionIdMap.end()) {
                        sessionIdMap[rawId] = sessionIdMap.size() + 1;
                    }
                    nlohmann::json row;
                    row["id"] = sessionIdMap[rawId];
                    row["account"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    row["started_at"] = "<normalized_timestamp>";
                    row["ended_at"] = "<normalized_timestamp>";
                    row["wins"] = sqlite3_column_int(stmt, 4);
                    row["losses"] = sqlite3_column_int(stmt, 5);
                    const char* mmrStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
                    if (mmrStr) {
                        try {
                            row["mmr_change_json"] = nlohmann::json::parse(mmrStr);
                        } catch (...) {
                            row["mmr_change_json"] = mmrStr;
                        }
                    } else {
                        row["mmr_change_json"] = nullptr;
                    }
                    const char* totalsStr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 7));
                    if (totalsStr) {
                        try {
                            row["totals_json"] = nlohmann::json::parse(totalsStr);
                        } catch (...) {
                            row["totals_json"] = totalsStr;
                        }
                    } else {
                        row["totals_json"] = nullptr;
                    }
                    row["source"] = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
                    jSessions.push_back(std::move(row));
                }
                sqlite3_finalize(stmt);
            }
        }

        // 7. Emitted Side Effects
        nlohmann::json jEmitted = nlohmann::json::array();
        for (const auto& step : emittedEffects) {
            if (step.hasEffects) {
                jEmitted.push_back(step.details);
            }
        }

        // 8. Final Session Totals
        nlohmann::json jTotals;
        if (state) {
            const auto snapshot = state->liveMatch->GetSnapshot();
            const auto& totals = snapshot->sessionTotals;
            jTotals["wins"] = totals.wins;
            jTotals["losses"] = totals.losses;
            jTotals["goals"] = totals.goals;
            jTotals["saves"] = totals.saves;
            jTotals["savesTotal"] = totals.savesTotal;
            jTotals["shots"] = totals.shots;
            jTotals["shotsTotal"] = totals.shotsTotal;
            jTotals["demos"] = totals.demos;
            jTotals["demosTotal"] = totals.demosTotal;
            jTotals["demoed"] = totals.demoed;
            jTotals["assists"] = totals.assists;
            jTotals["assistsTotal"] = totals.assistsTotal;
            jTotals["crossbars"] = totals.crossbars;
            jTotals["crossbarsTotal"] = totals.crossbarsTotal;
            jTotals["maxGoalSpeed"] = Round1(totals.maxGoalSpeed);
            jTotals["maxGoalSpeedSelf"] = Round1(totals.maxGoalSpeedSelf);
            jTotals["maxBallSpeed"] = Round1(totals.maxBallSpeed);
            jTotals["maxBallSpeedSelf"] = Round1(totals.maxBallSpeedSelf);
            jTotals["fastestGoalTime"] = Round1(totals.fastestGoalTime);
            jTotals["fastestGoalTimeSelf"] = Round1(totals.fastestGoalTimeSelf);
            jTotals["ownGoals"] = totals.ownGoals;
            jTotals["ownGoalsSelf"] = totals.ownGoalsSelf;
            jTotals["boostPickedUp"] = totals.boostPickedUp;
            jTotals["teamGoals"] = totals.teamGoals;
            jTotals["goalParticipations"] = totals.goalParticipations;
            jTotals["totalMmrChange"] = Round1(totals.totalMmrChange);
            jTotals["mmrChangeByPlaylist"] = totals.mmrChangeByPlaylist;
        }

        root["database"] = {
            {"Matches", jMatches},
            {"MatchPlayers", jMatchPlayers},
            {"MatchPlayerStats", jMatchPlayerStats},
            {"MatchLocalStats", jMatchLocalStats},
            {"MatchAnalytics", jMatchAnalytics},
            {"Sessions", jSessions}};
        root["emitted_side_effects"] = jEmitted;
        root["final_session_totals"] = jTotals;

        return root;
    }

    ReplayResult ReplayDriver::ReplayFile(const std::filesystem::path& fixturePath) {
        if (!std::filesystem::exists(fixturePath)) {
            ReplayResult res;
            res.errorMessage = "Fixture file does not exist: " + fixturePath.string();
            return res;
        }
        const auto records = TelemetryRecorder::ReadRecording(fixturePath);
        return ReplayRecords(records);
    }

    ReplayResult ReplayDriver::ReplayLines(const std::vector<std::string>& rawLines) {
        std::vector<TelemetryRecord> records;
        records.reserve(rawLines.size());
        int64_t stepTs = 1000;
        for (const auto& line : rawLines) {
            auto rec = TelemetryRecorder::ParseLine(line);
            if (rec.has_value()) {
                if (rec->timestampMs == 0) rec->timestampMs = stepTs;
                records.push_back(std::move(*rec));
                stepTs += 100;
            }
        }
        return ReplayRecords(records);
    }

    ReplayResult ReplayDriver::ReplayRecords(const std::vector<TelemetryRecord>& records) {
        ReplayResult result;
        ScopedOfflineCurlMock curlMock;
        ConfigData origConfig = Config::Read();
        Config::Update([&](ConfigData& c) {
            c = ConfigData{};
            c.reset_session_on_close = m_options.resetSessionOnClose;
            c.enable_mmr_tracking = m_options.enableMmrTracking;
        },
                       false);
        struct ConfigGuard {
            ConfigData orig;
            ~ConfigGuard() {
                Config::Update([this](ConfigData& c) { c = orig; }, false);
            }
        } configGuard{origConfig};

        static std::atomic<uint64_t> s_runCounter{0};
        const uint64_t runId = ++s_runCounter;

        std::filesystem::path tempDir;
        if (!m_options.tempDbDirectory.empty()) {
            tempDir = m_options.tempDbDirectory / ("replay_run_" + std::to_string(runId));
        } else {
            tempDir = std::filesystem::temp_directory_path() / ("omnistats_replay_run_" + std::to_string(runId));
        }

        std::error_code ec;
        std::filesystem::create_directories(tempDir, ec);
        const auto dbPath = tempDir / "replay.db";

        auto state = std::make_shared<SessionState>();
        state->liveMatch->UpdateMatchStateForTests([this](MatchState& match) {
            match.myPrimaryId = m_options.initialPrimaryId;
            match.roster[m_options.initialPrimaryId] = PlayerData{
                .primaryId = m_options.initialPrimaryId,
                .name = m_options.initialHeroName,
                .team = 0,
                .mmr = m_options.initialMmr,
                .playlists = {{m_options.initialPlaylist, m_options.initialMmr}},
                .playlistMatches = {{m_options.initialPlaylist, m_options.initialMatchesPlayed}}};
        });
        state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
        state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);

        auto dbManager = std::make_shared<DatabaseManager>(state);
        if (!dbManager->Initialize(dbPath.string())) {
            result.errorMessage = "Failed to initialize DatabaseManager at " + dbPath.string();
            std::filesystem::remove_all(tempDir, ec);
            return result;
        }

        auto mmrFetcher = std::make_shared<MMRFetcher>(state, dbManager);

        ReplayPipelineContext ctx{
            .state = state,
            .dbManager = dbManager,
            .mmrFetcher = mmrFetcher};

        std::vector<StepSideEffectsSummary> emittedEffects;

        int64_t currentClockMs = 0;
        for (const auto& rec : records) {
            if (rec.timestampMs > 0) {
                currentClockMs = rec.timestampMs;
            } else {
                currentClockMs += 100;
            }
            ctx.currentClockMs = currentClockMs;

            auto step = DispatchWireLine(ctx, rec.line);
            if (step.hasEffects) {
                emittedEffects.push_back(std::move(step));
            }
        }

        // Wait for DB jobs to drain completely:
        if (!DrainDatabase(dbManager, 5000)) {
            std::cerr << "[ReplayDriver] Warning: Database queue drain timed out.\n";
        }

        // Produce canonical golden:
        result.goldenObj = BuildCanonicalGolden(dbManager->GetRawDb(), state, emittedEffects);
        result.goldenJson = result.goldenObj.dump(2);
        result.success = true;

        // Teardown:
        mmrFetcher.reset();
        dbManager.reset();

        std::filesystem::remove_all(tempDir, ec);
        return result;
    }

} // namespace omnistats::replay
