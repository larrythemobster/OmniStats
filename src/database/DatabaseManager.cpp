#include "DatabaseManager.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"

#include <algorithm>
#include <exception>
#include <iostream>
#include <shared_mutex>
#include <utility>

using namespace DatabaseDetail;

DatabaseManager::ReadLease::ReadLease(sqlite3* connection, std::mutex& mutex)
    : lock(mutex), db(connection) {
    if (db && sqlite3_get_autocommit(db)) {
        if (sqlite3_exec(db, "BEGIN;", nullptr, nullptr, nullptr) == SQLITE_OK) {
            ownsTransaction = true;
        } else {
            std::cerr << "[Database] Cannot begin read snapshot: " << sqlite3_errmsg(db) << '\n';
            db = nullptr;
        }
    }
}

DatabaseManager::ReadLease::~ReadLease() {
    if (ownsTransaction) sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}

DatabaseManager::ReadLease DatabaseManager::AcquireRead() {
    return ReadLease(m_readDb ? m_readDb : m_db, m_readDb ? m_readDbMutex : m_dbMutex);
}

DatabaseManager::DatabaseManager(std::shared_ptr<SessionState> state)
    : m_state(state) {
    s_test_async_get_lifetime_calls.store(0);
    s_test_async_refresh_calls.store(0);
    m_worker = std::jthread([this]() {
        while (true) {
            DbJob job;

            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait(lock, [this] {
                    return m_stopWorker || !m_jobs.empty();
                });

                if (m_stopWorker && m_jobs.empty()) {
                    break;
                }

                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }

            try {
                job.run();
            } catch (const std::exception& e) {
                std::cerr << "[DatabaseManager] Job Exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[DatabaseManager] Job Exception: Unknown\n";
            }

            if (job.priority == DbJobPriority::Coalescable && !job.coalesceKey.empty()) {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_pendingCoalescedJobs.erase(job.coalesceKey);
            }
        }
    });
}

DatabaseManager::~DatabaseManager() {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stopWorker = true;
    }

    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    std::scoped_lock lock(m_dbMutex, m_readDbMutex);
    if (m_readDb) {
        sqlite3_close(m_readDb);
        m_readDb = nullptr;
    }
    if (m_db) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool DatabaseManager::Initialize(const std::string& dbPath) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    m_dbPath = dbPath;
    int rc = sqlite3_open(dbPath.c_str(), &m_db);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to open SQLite database: " << sqlite3_errmsg(m_db) << "\n";
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    sqlite3_busy_timeout(m_db, 3000);

    sqlite3_stmt* integrity = nullptr;
    rc = sqlite3_prepare_v2(m_db, "PRAGMA foreign_key_check;", -1, &integrity, nullptr);
    if (rc == SQLITE_OK) rc = sqlite3_step(integrity);
    if (rc != SQLITE_DONE) {
        if (rc == SQLITE_ROW) {
            std::cerr << "[Database] Existing foreign-key violations in "
                      << sqlite3_column_text(integrity, 0) << "; database left unchanged.\n";
        } else {
            std::cerr << "[Database] Foreign-key check failed: " << sqlite3_errmsg(m_db) << '\n';
        }
        sqlite3_finalize(integrity);
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }
    sqlite3_finalize(integrity);
    int foreignKeysEnabled = 0;
    rc = sqlite3_db_config(m_db, SQLITE_DBCONFIG_ENABLE_FKEY, 1, &foreignKeysEnabled);
    if (rc != SQLITE_OK || foreignKeysEnabled != 1) {
        std::cerr << "[Database] Cannot enable foreign-key enforcement: " << sqlite3_errmsg(m_db) << '\n';
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    // Enable WAL (Write-Ahead Logging) and synchronous NORMAL mode
    char* errMsg = nullptr;
    rc = sqlite3_exec(m_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::cerr << "Warning: Failed to enable WAL mode: " << (errMsg ? errMsg : "unknown") << "\n";
        if (errMsg) sqlite3_free(errMsg);
    }

    if (!CreateTables()) {
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    if (!IsInMemoryDbPath(dbPath)) {
        rc = sqlite3_open_v2(dbPath.c_str(), &m_readDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "[Database] Read connection unavailable: "
                      << (m_readDb ? sqlite3_errmsg(m_readDb) : "unknown") << '\n';
            sqlite3_close(m_readDb);
            m_readDb = nullptr;
        } else {
            sqlite3_busy_timeout(m_readDb, 3000);
        }
    }
    return true;
}

bool DatabaseManager::HasStatsTablesLocked() const {
    return m_hasStatsTables &&
           SqliteTableExists(m_db, "MatchPlayerStats") &&
           SqliteTableExists(m_db, "MatchLocalStats");
}

bool DatabaseManager::HasSessionsTableLocked() const {
    return m_hasSessionsTable &&
           SqliteTableExists(m_db, "Sessions") &&
           SqliteTableHasColumn(m_db, "Matches", "session_id");
}

void DatabaseManager::AsyncRefreshDetailedStatsSummary() {
    (void)EnqueueDbJob([this]() {
        int count = 0;
        std::string sinceDate;
        GetDetailedStatsSummary(count, sinceDate);
        if (m_state) {
            std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
            m_state->ui.cachedDbStats.detailedStatsMatchCount = count;
            m_state->ui.cachedDbStats.detailedStatsSinceDate = std::move(sinceDate);
            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
        }
    },
                       DbJobPriority::Coalescable, "detailed_stats_summary");
}

bool DatabaseManager::SetSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(m_dbMutex);
    if (!m_db) return false;

    const char* sql = "INSERT OR REPLACE INTO Settings (key, value) VALUES (?, ?);";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(m_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE);
}

void DatabaseManager::AsyncSetSetting(std::string key, std::string value) {
    if (!EnqueueDbJob([this, key = std::move(key), value = std::move(value)]() {
            (void)SetSetting(key, value);
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue setting write for " << key << ".\n";
    }
}

bool DatabaseManager::EnqueueDbJob(std::function<void()> job, DbJobPriority priority, std::string coalesceKey) {
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stopWorker) {
            std::cerr << "[Database] Rejecting job during shutdown.\n";
            return false;
        }

        if (priority == DbJobPriority::Coalescable && !coalesceKey.empty()) {
            if (m_pendingCoalescedJobs.count(coalesceKey)) {
                return true;
            }
            m_pendingCoalescedJobs.insert(coalesceKey);
        }

        if (m_jobs.size() > 100) {
            if (priority != DbJobPriority::Critical) {
                if (priority == DbJobPriority::Coalescable && !coalesceKey.empty()) {
                    m_pendingCoalescedJobs.erase(coalesceKey);
                }
                std::cerr << "[Database] Queue full! Dropping non-critical job.\n";
                return false;
            }
            std::cerr << "[Database] Queue full, preserving critical job.\n";
        }

        m_jobs.push_back({std::move(job), priority, std::move(coalesceKey)});
    }

    m_cv.notify_one();
    return true;
}

void DatabaseManager::AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    std::string pl = playlist;
    {
        std::lock_guard<std::mutex> lk(s_test_mutex);
        s_test_last_get_lifetime_primary = pid;
        s_test_last_get_lifetime_playlist = pl;
    }
    s_test_async_get_lifetime_calls.fetch_add(1);
    (void)EnqueueDbJob([this, pid, pl]() {
        std::vector<int64_t> tempX;
        std::vector<float> tempY;
        GetLifetimeMmrHistory(pid, pl, tempX, tempY);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
            m_state->history.lifetimeMmrX = std::move(tempX);
            m_state->history.lifetimeMmrY = std::move(tempY);
            m_state->history.version++;
        }
    },
                       DbJobPriority::Coalescable, "lifetime:" + pid + "|" + pl);
}

void DatabaseManager::AsyncGetRecentMatchHistory(const std::string& primaryId, int limit) {
    std::string pid = primaryId;
    int rowLimit = std::clamp(limit, 10, kPreviousGamesMaxLimit);
    (void)EnqueueDbJob([this, pid, rowLimit]() {
        std::vector<SessionMatchSummary> matches;
        GetRecentMatchHistory(pid, matches, rowLimit);
        if (m_state) {
            std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
            m_state->history.recentSavedMatches = std::move(matches);
            m_state->history.recentSavedMatchesLoaded = true;
            m_state->history.version++;
        }
    },
                       DbJobPriority::Coalescable, "recent_matches:" + pid + "|" + std::to_string(rowLimit));
}

// Testing instrumentation defaults
std::atomic<int> DatabaseManager::s_test_async_get_lifetime_calls{0};
std::atomic<int> DatabaseManager::s_test_async_refresh_calls{0};
std::mutex DatabaseManager::s_test_mutex;
std::string DatabaseManager::s_test_last_get_lifetime_primary = "";
std::string DatabaseManager::s_test_last_get_lifetime_playlist = "";
std::string DatabaseManager::s_test_last_refresh_primary = "";

void DatabaseManager::RefreshDbStatsSync(const std::string& primaryId) {
    if (primaryId.empty()) return;
    CachedDbStats newStats;
    GetStreakStats(primaryId, newStats.currentWins, newStats.currentLosses, newStats.longestWins, newStats.longestLosses);
    for (const auto& gm : {"1v1", "2v2", "3v3", "hoops", "rumble", "dropshot", "snowday", "heatseeker", "casual", "t"}) {
        GetGamemodeStats(primaryId, gm, newStats.gamemodes[gm].wins, newStats.gamemodes[gm].losses, newStats.gamemodes[gm].total);
    }
    GetDetailedStatsSummary(newStats.detailedStatsMatchCount, newStats.detailedStatsSinceDate);
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
        m_state->ui.cachedDbStats = newStats;
        m_state->ui.dbStatsDirty.store(false);
        m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatabaseManager::AsyncRefreshDbStats(const std::string& primaryId) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    {
        std::lock_guard<std::mutex> lk(s_test_mutex);
        s_test_last_refresh_primary = pid;
    }
    s_test_async_refresh_calls.fetch_add(1);
    (void)EnqueueDbJob([this, pid]() {
        RefreshDbStatsSync(pid);
    },
                       DbJobPriority::Coalescable, "refresh:" + pid);
}

void DatabaseManager::AsyncGetPlayerEncounterRecord(const std::string& primaryId) {
    if (primaryId.empty() || !m_state) return;
    std::string pid = primaryId;
    // Not coalesced: a lookup queued after a match save must not be absorbed by
    // an older pending lookup that would run before the save.
    (void)EnqueueDbJob([this, pid]() {
        std::string account;
        {
            std::shared_lock<std::shared_mutex> lk(m_state->game.mutex);
            account = m_state->game.myPrimaryId;
        }
        if (account.empty() || account == pid) return;
        const auto record = GetPlayerEncounterRecord(account, pid);
        if (!record) return;
        std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
        if (m_state->game.myPrimaryId != account) return;
        auto it = m_state->game.roster.find(pid);
        if (it == m_state->game.roster.end()) return;
        auto& player = it->second;
        player.lifetimeWinsWith = record->winsWith;
        player.lifetimeLossesWith = record->lossesWith;
        player.lifetimeWinsAgainst = record->winsAgainst;
        player.lifetimeLossesAgainst = record->lossesAgainst;
        player.hasLifetimeData = true;
        m_state->game.version++;
    });
}

void DatabaseManager::AsyncLoadInsights(const std::string& primaryId) {
    std::string pid = primaryId;
    (void)EnqueueDbJob([this, pid]() {
        std::vector<PersonRecord> people;
        std::vector<MatchOutcome> outcomes;
        std::vector<MatchMmrContext> mmrContext;
        std::vector<SessionRecap> sessions;
        std::vector<AnalyticsMatchRow> analyticsMatches;
        GetPeopleRecords(pid, people);
        GetMatchOutcomes(pid, outcomes);
        GetMatchMmrContext(pid, mmrContext);
        sessions = ListSessions(pid, 0, 200);
        analyticsMatches = GetAnalyticsMatches(pid, 300);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->insights.mutex);
        m_state->insights.primaryId = pid;
        m_state->insights.people = std::move(people);
        m_state->insights.outcomes = std::move(outcomes);
        m_state->insights.mmrContext = std::move(mmrContext);
        m_state->insights.sessions = std::move(sessions);
        m_state->insights.analyticsMatches = std::move(analyticsMatches);
        m_state->insights.loaded = true;
        m_state->insights.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Coalescable, "insights:" + pid);
}

void DatabaseManager::AsyncQueryMatches(MatchQuery query, uint64_t requestId, bool append) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId >= m_state->historyView.requestId) {
            m_state->historyView.requestId = requestId;
            m_state->historyView.loading = true;
            m_state->historyView.appendMode = append;
            m_state->historyView.query = query;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, q = std::move(query), requestId, append]() {
        std::vector<MatchRow> rows;
        int total = 0;
        QueryMatches(q, rows, total);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId < m_state->historyView.requestId) {
            return;
        }
        m_state->historyView.requestId = requestId;
        m_state->historyView.query = q;
        m_state->historyView.total = total;
        m_state->historyView.loading = false;
        m_state->historyView.appendMode = append;
        if (append && q.offset > 0) {
            for (auto& row : rows) {
                m_state->historyView.rows.push_back(std::move(row));
            }
        } else {
            m_state->historyView.rows = std::move(rows);
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncGetMatchDetail(int64_t matchId, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId == 0 || requestId >= m_state->historyView.detailRequestId) {
            if (requestId != 0) m_state->historyView.detailRequestId = requestId;
            m_state->historyView.detailLoading = true;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, matchId, requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetail(matchId, detail, account);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId != 0 && requestId < m_state->historyView.detailRequestId) {
            return;
        }
        if (requestId != 0) m_state->historyView.detailRequestId = requestId;
        m_state->historyView.detailLoading = false;
        if (ok && detail.found) {
            m_state->historyView.detail = std::move(detail);
        } else {
            m_state->historyView.detail.reset();
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncGetMatchDetailByGuid(std::string matchGuid, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state) {
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId == 0 || requestId >= m_state->historyView.detailRequestId) {
            if (requestId != 0) m_state->historyView.detailRequestId = requestId;
            m_state->historyView.detailLoading = true;
            m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
        }
    }

    (void)EnqueueDbJob([this, guid = std::move(matchGuid), requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetailByGuid(guid, detail, account);
        if (!m_state) return;
        std::lock_guard<std::mutex> lock(m_state->historyView.mutex);
        if (requestId != 0 && requestId < m_state->historyView.detailRequestId) {
            return;
        }
        if (requestId != 0) m_state->historyView.detailRequestId = requestId;
        m_state->historyView.detailLoading = false;
        if (ok && detail.found) {
            m_state->historyView.detail = std::move(detail);
        } else {
            m_state->historyView.detail.reset();
        }
        m_state->historyView.version.fetch_add(1, std::memory_order_relaxed);
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncSaveMatch(MatchSaveSnapshot snapshot) {
    if (!EnqueueDbJob([this, snap = std::move(snapshot)]() {
            SaveMatch(snap);
            RefreshDbStatsSync(snap.myPrimaryId);
            if (snap.myPrimaryId.empty()) {
                int detailedCount = 0;
                std::string detailedSince;
                GetDetailedStatsSummary(detailedCount, detailedSince);
                if (m_state) {
                    std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
                    m_state->ui.cachedDbStats.detailedStatsMatchCount = detailedCount;
                    m_state->ui.cachedDbStats.detailedStatsSinceDate = std::move(detailedSince);
                    m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
                }
            }
            ConfigData conf = Config::Read();
            std::string primaryId = snap.myPrimaryId.empty() ? conf.last_primary_id : snap.myPrimaryId;
            std::vector<SessionMatchSummary> matches;
            GetRecentMatchHistory(primaryId, matches, conf.previous_games_limit);
            if (m_state) {
                std::unique_lock<std::shared_mutex> lock(m_state->history.mutex);
                m_state->history.recentSavedMatches = std::move(matches);
                m_state->history.recentSavedMatchesLoaded = true;
                m_state->history.version++;
            }
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue match save during shutdown.\n";
    }
}
