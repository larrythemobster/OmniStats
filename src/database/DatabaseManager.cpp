#include "DatabaseManager.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"
#include "database/repository/HistoryRepository.hpp"
#include "database/repository/MatchRepository.hpp"
#include "database/repository/Migrations.hpp"
#include "database/repository/PlayerNotesRepository.hpp"
#include "database/repository/ReplayRepository.hpp"
#include "database/repository/SessionRepository.hpp"
#include "database/repository/TrackedAccountsRepository.hpp"
#include "database/repository/TransferService.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <utility>
namespace repository = database::repository;
namespace sqlite = database::sqlite;

DatabaseManager::DatabaseManager(std::shared_ptr<SessionState> state, DatabaseHooks hooks)
    : m_state(std::move(state)), m_hooks(std::move(hooks)) {
    m_worker = std::jthread([this]() {
        while (true) {
            DbJob job;
            {
                std::unique_lock lock(m_queueMutex);
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
                std::lock_guard lock(m_queueMutex);
                m_pendingCoalescedJobs.erase(job.coalesceKey);
            }
        }
    });
}

DatabaseManager::~DatabaseManager() {
    {
        std::lock_guard lock(m_queueMutex);
        m_stopWorker = true;
    }
    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    m_readerPool.Shutdown();

    std::lock_guard lock(m_writeMutex);
    m_db.Close();
}

bool DatabaseManager::Initialize(const std::string& dbPath) {
    std::lock_guard lock(m_writeMutex);
    m_dbPath = dbPath;

    try {
        m_db = database::sqlite::Connection::Open(dbPath);
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to open SQLite database: " << e.what() << "\n";
        return false;
    }

    m_db.BusyTimeout(5000);

    std::string foreignKeyFailure;
    try {
        std::string violatingTable;
        {
            database::sqlite::Statement integrity(m_db, "PRAGMA foreign_key_check;");
            if (integrity.Step()) violatingTable = integrity.Get<std::string>(0);
        }
        if (!violatingTable.empty()) {
            foreignKeyFailure = "Existing foreign-key violations in " + violatingTable + "; database left unchanged.";
        } else {
            m_db.Execute("PRAGMA foreign_keys = ON;");
            bool enforced = false;
            {
                database::sqlite::Statement enabled(m_db, "PRAGMA foreign_keys;");
                enforced = enabled.Step() && enabled.Get<int>(0) == 1;
            }
            if (!enforced) foreignKeyFailure = "Cannot enable foreign-key enforcement.";
        }
    } catch (const std::exception& e) {
        foreignKeyFailure = std::string("Foreign-key check failed: ") + e.what();
    }
    if (!foreignKeyFailure.empty()) {
        std::cerr << "[Database] " << foreignKeyFailure << "\n";
        m_db.Close();
        return false;
    }

    try {
        m_db.Execute("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    } catch (const std::exception& e) {
        std::cerr << "[Database] Failed to enable WAL mode: " << e.what() << "\n";
    }

    if (!repository::Migrations::Migrate(m_db, dbPath)) {
        m_db.Close();
        return false;
    }
    repository::Migrations::RepairArenaNames(m_db);

    if (repository::SessionRepository::HasUnlinkedMatches(m_db)) {
        m_backfillPending.store(true, std::memory_order_release);
        (void)EnqueueDbJob([this]() {
            BackfillSessions(500);
        },
                           DbJobPriority::Coalescable, "sessions_backfill");
    }

    if (!DatabaseDetail::IsInMemoryDbPath(dbPath)) {
        m_readerPool.Initialize(dbPath, 3);
    }

    return true;
}

database::sqlite::Connection& DatabaseManager::GetReadConnection() {
    return m_db;
}

bool DatabaseManager::EnqueueDbJob(std::function<void()> job, DbJobPriority priority, std::string coalesceKey) {
    {
        std::lock_guard lock(m_queueMutex);
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

        // Critical jobs are inserted in FIFO order before any non-critical jobs
        if (priority == DbJobPriority::Critical) {
            auto it = m_jobs.begin();
            while (it != m_jobs.end() && it->priority == DbJobPriority::Critical) {
                ++it;
            }
            m_jobs.insert(it, DbJob{std::move(job), priority, std::move(coalesceKey)});
        } else {
            m_jobs.push_back(DbJob{std::move(job), priority, std::move(coalesceKey)});
        }
    }

    m_cv.notify_one();
    return true;
}

void DatabaseManager::SaveMatch(const MatchSaveSnapshot& snapshot) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return;
    repository::MatchRepository::SaveMatch(m_db, snapshot, repository::Migrations::HasStatsTables(m_db), m_telemetry.get());
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
                    std::lock_guard lock(m_state->ui.dbStatsMutex);
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
                m_state->history.SetRecentSavedMatches(std::move(matches));
            }
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue match save during shutdown.\n";
    }
}

bool DatabaseManager::UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::MatchRepository::UpdateMatchPlayerMmr(m_db, matchGuid, primaryId, mmr, estimated);
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
        GetRecentMatchHistory(primaryId, matches, conf.previous_games_limit);
        if (m_state) {
            m_state->history.SetRecentSavedMatches(std::move(matches));
        }
    },
                       DbJobPriority::Critical);
}

int64_t DatabaseManager::SaveSession(const SessionRecap& recap) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return 0;
    std::string liveAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) liveAccount = liveSnap->myPrimaryId;
    }
    return repository::SessionRepository::SaveSession(m_db, recap, &m_sessionIdByGeneration, liveAccount);
}

void DatabaseManager::AsyncSaveSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        const int64_t id = SaveSession(r);
        if (id > 0 && m_state) {
            auto snap = m_state->insights.GetSnapshot();
            std::string pid = (snap && snap->loaded) ? snap->primaryId : std::string{};
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                m_state->insights.UpdateSessions(pid, std::move(sessions));
            }
        }
    },
                       DbJobPriority::Critical);
}

bool DatabaseManager::UpdateSession(const SessionRecap& recap) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    std::string liveAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) liveAccount = liveSnap->myPrimaryId;
    }
    return repository::SessionRepository::UpdateSession(m_db, recap, &m_sessionIdByGeneration, liveAccount);
}

void DatabaseManager::AsyncUpdateSession(SessionRecap recap) {
    (void)EnqueueDbJob([this, r = std::move(recap)]() {
        if (UpdateSession(r) && m_state) {
            auto snap = m_state->insights.GetSnapshot();
            std::string pid = (snap && snap->loaded) ? snap->primaryId : std::string{};
            if (!pid.empty()) {
                auto sessions = ListSessions(pid, 0, 200);
                m_state->insights.UpdateSessions(pid, std::move(sessions));
            }
        }
    },
                       DbJobPriority::Critical);
}

int DatabaseManager::BackfillSessions(int chunkSize) {
    int totalCreated = 0;
    while (true) {
        bool moreRemaining = false;
        int linked = 0;
        std::string preferredAccount;
        std::vector<std::string> activeSessionGuids;
        if (m_state && m_state->liveMatch) {
            auto liveSnap = m_state->liveMatch->GetSnapshot();
            if (liveSnap) {
                preferredAccount = liveSnap->myPrimaryId;
                if (liveSnap->sessionTotals.wins + liveSnap->sessionTotals.losses > 0) {
                    activeSessionGuids = liveSnap->sessionMatchGuids;
                }
            }
        }
        if (preferredAccount.empty()) {
            preferredAccount = Config::Read().last_primary_id;
        }

        {
            std::lock_guard lock(m_writeMutex);
            totalCreated += repository::SessionRepository::BackfillSessionsChunk(
                m_db, chunkSize, moreRemaining, linked, preferredAccount, activeSessionGuids);
        }
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
    if (account.empty()) return {};

    bool shouldBackfillAccount = false;
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
        if (m_readerPool.HasPool()) {
            auto lease = m_readerPool.Acquire();
            if (lease.IsValid()) {
                shouldBackfillAccount = repository::SessionRepository::HasUnlinkedMatches(lease.Get(), account);
            }
        } else {
            std::lock_guard lock(m_writeMutex);
            shouldBackfillAccount = repository::SessionRepository::HasUnlinkedMatches(m_db, account);
        }
    }
    if (shouldBackfillAccount) {
        BackfillSessions(500);
    }

    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::SessionRepository::ListSessions(lease.Get(), account, offset, limit);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::SessionRepository::ListSessions(m_db, account, offset, limit);
}

SessionRecap DatabaseManager::GetSession(int64_t id) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    if (id <= 0) return {};

    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::SessionRepository::GetSession(lease.Get(), id);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::SessionRepository::GetSession(m_db, id);
}

void DatabaseManager::GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetLifetimeMmrHistory(lease.Get(), primaryId, playlist, outX, outY);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetLifetimeMmrHistory(m_db, primaryId, playlist, outX, outY);
}

void DatabaseManager::AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;
    std::string pl = playlist;

    if (m_hooks.onAsyncGetLifetimeMmr) {
        m_hooks.onAsyncGetLifetimeMmr(pid, pl);
    }

    (void)EnqueueDbJob([this, pid, pl]() {
        std::vector<int64_t> tempX;
        std::vector<float> tempY;
        GetLifetimeMmrHistory(pid, pl, tempX, tempY);
        if (m_state) {
            m_state->history.SetLifetimeMmr(std::move(tempX), std::move(tempY));
        }
    },
                       DbJobPriority::Coalescable, "lifetime:" + pid + "|" + pl);
}

void DatabaseManager::GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetRecentMatchHistory(lease.Get(), primaryId, outMatches, limit);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetRecentMatchHistory(m_db, primaryId, outMatches, limit);
}

void DatabaseManager::AsyncGetRecentMatchHistory(const std::string& primaryId, int limit) {
    std::string pid = primaryId;
    int rowLimit = std::clamp(limit, 10, kPreviousGamesMaxLimit);
    (void)EnqueueDbJob([this, pid, rowLimit]() {
        std::vector<SessionMatchSummary> matches;
        GetRecentMatchHistory(pid, matches, rowLimit);
        if (m_state) {
            m_state->history.SetRecentSavedMatches(std::move(matches));
        }
    },
                       DbJobPriority::Coalescable, "recent_matches:" + pid + "|" + std::to_string(rowLimit));
}

std::optional<PersonRecord> DatabaseManager::GetPlayerEncounterRecord(const std::string& account, const std::string& primaryId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetPlayerEncounterRecord(lease.Get(), account, primaryId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetPlayerEncounterRecord(m_db, account, primaryId);
}

void DatabaseManager::AsyncGetPlayerEncounterRecord(const std::string& primaryId) {
    if (primaryId.empty() || !m_state) return;
    std::string pid = primaryId;
    (void)EnqueueDbJob([this, pid]() {
        if (!m_state || !m_state->liveMatch) return;
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        const std::string account = liveSnap ? liveSnap->myPrimaryId : std::string{};
        if (account.empty() || account == pid) return;
        const auto record = GetPlayerEncounterRecord(account, pid);
        if (!record) return;
        m_state->liveMatch->UpdatePlayerLifetimeData(account, pid, record->winsWith, record->lossesWith, record->winsAgainst, record->lossesAgainst);
    });
}

void DatabaseManager::GetPeopleRecords(const std::string& primaryId, std::vector<PersonRecord>& outPeople) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetPeopleRecords(lease.Get(), primaryId, outPeople);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetPeopleRecords(m_db, primaryId, outPeople);
}

void DatabaseManager::GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetMatchOutcomes(lease.Get(), primaryId, outMatches);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetMatchOutcomes(m_db, primaryId, outMatches);
}

void DatabaseManager::GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetMatchMmrContext(lease.Get(), primaryId, out);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetMatchMmrContext(m_db, primaryId, out);
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
        if (m_state) {
            m_state->insights.PublishInsights(
                pid, std::move(people), std::move(outcomes), std::move(mmrContext), std::move(sessions), std::move(analyticsMatches));
        }
    },
                       DbJobPriority::Coalescable, "insights:" + pid);
}

void DatabaseManager::GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetGamemodeStats(lease.Get(), primaryId, gamemode, wins, losses, gamesPlayed);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetGamemodeStats(m_db, primaryId, gamemode, wins, losses, gamesPlayed);
}

void DatabaseManager::GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetStreakStats(lease.Get(), primaryId, outCurWin, outCurLoss, outLongestWin, outLongestLoss);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetStreakStats(m_db, primaryId, outCurWin, outCurLoss, outLongestWin, outLongestLoss);
}

void DatabaseManager::RefreshDbStatsSync(const std::string& primaryId) {
    if (primaryId.empty()) return;
    CachedDbStats newStats;
    GetStreakStats(primaryId, newStats.currentWins, newStats.currentLosses, newStats.longestWins, newStats.longestLosses);
    for (const auto& gm : {"1v1", "2v2", "3v3", "hoops", "rumble", "dropshot", "snowday", "heatseeker", "casual", "t"}) {
        GetGamemodeStats(primaryId, gm, newStats.gamemodes[gm].wins, newStats.gamemodes[gm].losses, newStats.gamemodes[gm].total);
    }
    GetDetailedStatsSummary(newStats.detailedStatsMatchCount, newStats.detailedStatsSinceDate);
    if (m_state) {
        std::lock_guard lock(m_state->ui.dbStatsMutex);
        m_state->ui.cachedDbStats = newStats;
        m_state->ui.dbStatsDirty.store(false);
        m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
    }
}

void DatabaseManager::AsyncRefreshDbStats(const std::string& primaryId) {
    if (primaryId.empty()) return;
    std::string pid = primaryId;

    if (m_hooks.onAsyncRefreshDbStats) {
        m_hooks.onAsyncRefreshDbStats(pid);
    }

    (void)EnqueueDbJob([this, pid]() {
        RefreshDbStatsSync(pid);
    },
                       DbJobPriority::Coalescable, "refresh:" + pid);
}

bool DatabaseManager::ExportLocalData(std::string& exportPath, std::string& error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        error = "Database is not open.";
        return false;
    }
    return repository::TransferService::ExportLocalData(m_db, exportPath, error);
}

bool DatabaseManager::DeleteLocalMatchHistory(std::string& error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        error = "Database is not open.";
        return false;
    }
    bool ok = repository::TransferService::DeleteLocalMatchHistory(m_db, error);
    if (ok) {
        m_sessionIdByGeneration.clear();
        m_backfillPending.store(false, std::memory_order_release);
    }
    return ok;
}

DbMergeResult DatabaseManager::MergeDatabase(const std::string& sourceDbPath) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        return DbMergeResult{.success = false, .error = "Database is not open."};
    }
    return repository::TransferService::MergeDatabase(m_db, sourceDbPath, m_dbPath);
}

std::vector<PlayerStatRow> DatabaseManager::GetPlayerStatSeries(const std::string& primaryId,
                                                                const std::string& playlistFilter,
                                                                int limit) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetPlayerStatSeries(lease.Get(), primaryId, playlistFilter, limit);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetPlayerStatSeries(m_db, primaryId, playlistFilter, limit);
}

std::vector<AnalyticsMatchRow> DatabaseManager::GetAnalyticsMatches(const std::string& primaryId, int limit) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetAnalyticsMatches(lease.Get(), primaryId, limit);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetAnalyticsMatches(m_db, primaryId, limit);
}

void DatabaseManager::GetDetailedStatsSummary(int& outMatchCount, std::string& outSinceDate) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::GetDetailedStatsSummary(lease.Get(), outMatchCount, outSinceDate);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::GetDetailedStatsSummary(m_db, outMatchCount, outSinceDate);
}

void DatabaseManager::AsyncRefreshDetailedStatsSummary() {
    (void)EnqueueDbJob([this]() {
        int count = 0;
        std::string sinceDate;
        GetDetailedStatsSummary(count, sinceDate);
        if (m_state) {
            std::lock_guard lock(m_state->ui.dbStatsMutex);
            m_state->ui.cachedDbStats.detailedStatsMatchCount = count;
            m_state->ui.cachedDbStats.detailedStatsSinceDate = std::move(sinceDate);
            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
        }
    },
                       DbJobPriority::Coalescable, "detailed_stats_summary");
}

int DatabaseManager::GetSchemaVersion() {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetSchemaVersion(lease.Get());
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetSchemaVersion(m_db);
}

void DatabaseManager::QueryMatches(const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            repository::HistoryRepository::QueryMatches(lease.Get(), query, outRows, outTotalCount);
            return;
        }
    }
    std::lock_guard lock(m_writeMutex);
    repository::HistoryRepository::QueryMatches(m_db, query, outRows, outTotalCount);
}

void DatabaseManager::AsyncQueryMatches(MatchQuery query, uint64_t requestId, bool append) {
    if (m_state) {
        m_state->historyView.RequestQuery(requestId, append, query);
    }

    (void)EnqueueDbJob([this, q = std::move(query), requestId, append]() {
        std::vector<MatchRow> rows;
        int total = 0;
        QueryMatches(q, rows, total);
        if (m_state) {
            auto snap = m_state->historyView.GetSnapshot();
            if (snap && requestId < snap->requestId) {
                return;
            }
            m_state->historyView.PublishQueryResult(requestId, q, std::move(rows), total, append);
        }
    },
                       DbJobPriority::Normal);
}

bool DatabaseManager::GetMatchDetail(int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    std::string liveAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) liveAccount = liveSnap->myPrimaryId;
    }
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetMatchDetail(lease.Get(), matchId, outDetail, accountPrimaryId, liveAccount);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetMatchDetail(m_db, matchId, outDetail, accountPrimaryId, liveAccount);
}

bool DatabaseManager::GetMatchDetailByGuid(const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    std::string liveAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) liveAccount = liveSnap->myPrimaryId;
    }
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::HistoryRepository::GetMatchDetailByGuid(lease.Get(), matchGuid, outDetail, accountPrimaryId, liveAccount);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::HistoryRepository::GetMatchDetailByGuid(m_db, matchGuid, outDetail, accountPrimaryId, liveAccount);
}

void DatabaseManager::AsyncGetMatchDetail(int64_t matchId, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state && requestId != 0) {
        m_state->historyView.RequestDetail(requestId);
    }

    (void)EnqueueDbJob([this, matchId, requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetail(matchId, detail, account);
        if (m_state) {
            auto snap = m_state->historyView.GetSnapshot();
            if (snap && requestId < snap->detailRequestId) {
                return;
            }
            m_state->historyView.PublishDetailResult(requestId, (ok && detail.found) ? std::optional{std::move(detail)} : std::nullopt);
        }
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncGetMatchDetailByGuid(std::string matchGuid, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state && requestId != 0) {
        m_state->historyView.RequestDetail(requestId);
    }

    (void)EnqueueDbJob([this, guid = std::move(matchGuid), requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetailByGuid(guid, detail, account);
        if (m_state) {
            auto snap = m_state->historyView.GetSnapshot();
            if (snap && requestId < snap->detailRequestId) {
                return;
            }
            m_state->historyView.PublishDetailResult(requestId, (ok && detail.found) ? std::optional{std::move(detail)} : std::nullopt);
        }
    },
                       DbJobPriority::Normal);
}

bool DatabaseManager::RecordDiscoveredReplay(const std::string& filePath, const std::string& fileName, const std::string& matchGuid,
                                             const std::string& initialState, const std::string& error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RecordDiscoveredReplay(m_db, filePath, fileName, matchGuid, initialState, error);
}

bool DatabaseManager::UpdateReplayUploadState(int64_t id, const std::string& state, int attempts, int64_t nextAttemptAt, int lastHttpStatus,
                                              const std::string& lastError, const std::string& ballchasingId, const std::string& ballchasingUrl) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::UpdateReplayUploadState(m_db, id, state, attempts, nextAttemptAt, lastHttpStatus, lastError, ballchasingId, ballchasingUrl);
}

bool DatabaseManager::SetReplayUploading(int64_t id) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::SetReplayUploading(m_db, id);
}

bool DatabaseManager::RecoverStuckUploadingReplays() {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RecoverStuckUploadingReplays(m_db);
}

std::vector<ReplayUploadRecord> DatabaseManager::GetPendingReplayUploads(int limit) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::ReplayRepository::GetPendingReplayUploads(lease.Get(), limit);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::ReplayRepository::GetPendingReplayUploads(m_db, limit);
}

std::optional<ReplayUploadRecord> DatabaseManager::GetReplayUploadByPath(const std::string& filePath) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::ReplayRepository::GetReplayUploadByPath(lease.Get(), filePath);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::ReplayRepository::GetReplayUploadByPath(m_db, filePath);
}

ReplayUploadStatus DatabaseManager::GetReplayUploadStatus() {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::ReplayRepository::GetReplayUploadStatus(lease.Get());
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::ReplayRepository::GetReplayUploadStatus(m_db);
}

bool DatabaseManager::RetryFailedReplayUploads() {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RetryFailedReplayUploads(m_db);
}

bool DatabaseManager::QueryMatchReplayInfo(const std::string& matchGuid, bool& outFound, bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::ReplayRepository::QueryMatchReplayInfo(lease.Get(), matchGuid, outFound, outRanked, outWin, outResultPending, outPlaylistId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::ReplayRepository::QueryMatchReplayInfo(m_db, matchGuid, outFound, outRanked, outWin, outResultPending, outPlaylistId);
}

TrackedAccountInsertResult DatabaseManager::RegisterTrackedAccount(const TrackedAccounts::AccountRecord& record) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return TrackedAccountInsertResult::Failed;
    return repository::TrackedAccountsRepository::RegisterTrackedAccount(m_db, record);
}

std::vector<TrackedAccounts::AccountRecord> DatabaseManager::ListTrackedAccounts() {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::TrackedAccountsRepository::ListTrackedAccounts(lease.Get());
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::TrackedAccountsRepository::ListTrackedAccounts(m_db);
}

std::optional<TrackedAccounts::AccountRecord> DatabaseManager::GetTrackedAccount(const std::string& primaryId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::TrackedAccountsRepository::GetTrackedAccount(lease.Get(), primaryId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::TrackedAccountsRepository::GetTrackedAccount(m_db, primaryId);
}

bool DatabaseManager::UpdateTrackedAccountNickname(const std::string& primaryId, const std::string& nickname) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountNickname(m_db, primaryId, nickname);
}

bool DatabaseManager::UpdateTrackedAccountLastSeen(const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountLastSeen(m_db, primaryId, lastKnownName, seenAtUnix);
}

bool DatabaseManager::UpdateTrackedAccountRefreshAttempt(const std::string& primaryId, int64_t attemptedAtUnix) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountRefreshAttempt(m_db, primaryId, attemptedAtUnix);
}

bool DatabaseManager::UpdateTrackedAccountLastKnownName(const std::string& primaryId, const std::string& lastKnownName) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountLastKnownName(m_db, primaryId, lastKnownName);
}

std::optional<std::string> DatabaseManager::FindLatestPlayerName(const std::string& primaryId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::TrackedAccountsRepository::FindLatestPlayerName(lease.Get(), primaryId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::TrackedAccountsRepository::FindLatestPlayerName(m_db, primaryId);
}

bool DatabaseManager::SaveTrackedAccountRanks(const std::string& primaryId,
                                              const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                              int64_t fetchedAtUnix) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::SaveTrackedAccountRanks(m_db, primaryId, ranks, fetchedAtUnix);
}

std::map<std::string, TrackedAccounts::PlaylistRank> DatabaseManager::GetTrackedAccountRanks(const std::string& primaryId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::TrackedAccountsRepository::GetTrackedAccountRanks(lease.Get(), primaryId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::TrackedAccountsRepository::GetTrackedAccountRanks(m_db, primaryId);
}

std::vector<TrackedAccounts::RankHistorySeries> DatabaseManager::GetTrackedAccountRankHistory(
    const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::TrackedAccountsRepository::GetTrackedAccountRankHistory(lease.Get(), primaryIds, playlistKey, sinceUnix);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::TrackedAccountsRepository::GetTrackedAccountRankHistory(m_db, primaryIds, playlistKey, sinceUnix);
}

bool DatabaseManager::DeleteTrackedAccount(const std::string& primaryId) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::DeleteTrackedAccount(m_db, primaryId);
}

int DatabaseManager::ImportTrackedAccounts(const std::vector<TrackedAccounts::AccountRecord>& records) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return 0;
    return repository::TrackedAccountsRepository::ImportTrackedAccounts(m_db, records);
}

std::optional<std::vector<PlayerNotes::Note>> DatabaseManager::ListPlayerNotes() {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::PlayerNotesRepository::ListPlayerNotes(lease.Get());
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::PlayerNotesRepository::ListPlayerNotes(m_db);
}

std::optional<std::vector<PlayerNotes::Tag>> DatabaseManager::ListPlayerTags() {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::PlayerNotesRepository::ListPlayerTags(lease.Get());
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::PlayerNotesRepository::ListPlayerTags(m_db);
}

std::optional<std::vector<std::string>> DatabaseManager::GetPlayerTags(const PlayerNotes::Identity& identity) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::PlayerNotesRepository::GetPlayerTags(lease.Get(), identity);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::PlayerNotesRepository::GetPlayerTags(m_db, identity);
}

bool DatabaseManager::SetPlayerTag(const PlayerNotes::Identity& identity, std::string_view tag, bool add,
                                   int64_t nowUnix, std::string* error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        if (error) *error = "The local database is unavailable.";
        return false;
    }
    return repository::PlayerNotesRepository::SetPlayerTag(m_db, identity, tag, add, nowUnix, error);
}

void DatabaseManager::AsyncSetPlayerTag(PlayerNotes::Identity identity, std::string tag, bool add, uint64_t requestId) {
    (void)EnqueueDbJob([this, id = std::move(identity), t = std::move(tag), add, requestId]() {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        std::string error;
        const bool ok = SetPlayerTag(id, t, add, now, &error);
        auto tags = ok ? GetPlayerTags(id) : std::nullopt;
        if (m_state) {
            m_state->notes.PublishTagUpdated(requestId, ok, error, id, std::move(tags));
        }
    },
                       DbJobPriority::Critical);
}

std::optional<PlayerScoutProfile> DatabaseManager::GetPlayerScoutProfile(const std::string& account, const std::string& playerId) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::PlayerNotesRepository::GetPlayerScoutProfile(lease.Get(), account, playerId);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::PlayerNotesRepository::GetPlayerScoutProfile(m_db, account, playerId);
}

void DatabaseManager::AsyncGetPlayerScout(std::string account, std::string playerId, uint64_t requestId) {
    (void)EnqueueDbJob([this, acct = std::move(account), pid = std::move(playerId), requestId]() {
        auto profile = GetPlayerScoutProfile(acct, pid);
        if (m_state) {
            m_state->scout.PublishScout(requestId, pid, profile.has_value(), std::move(profile));
        }
    },
                       DbJobPriority::Normal);
}

std::optional<PlayerNotes::Note> DatabaseManager::GetPlayerNote(const PlayerNotes::Identity& identity) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            return repository::PlayerNotesRepository::GetPlayerNote(lease.Get(), identity);
        }
    }
    std::lock_guard lock(m_writeMutex);
    return repository::PlayerNotesRepository::GetPlayerNote(m_db, identity);
}

bool DatabaseManager::SavePlayerNote(const PlayerNotes::Identity& identity, std::string_view text, std::string_view lastKnownName,
                                     int64_t nowUnix, std::string* error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        if (error) *error = "The local database is unavailable.";
        return false;
    }
    return repository::PlayerNotesRepository::SavePlayerNote(m_db, identity, text, lastKnownName, nowUnix, error);
}

bool DatabaseManager::DeletePlayerNote(const PlayerNotes::Identity& identity, std::string* error) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) {
        if (error) *error = "The local database is unavailable.";
        return false;
    }
    return repository::PlayerNotesRepository::DeletePlayerNote(m_db, identity, error);
}

void DatabaseManager::AsyncLoadPlayerNotes() {
    (void)EnqueueDbJob([this]() {
        auto notes = ListPlayerNotes();
        auto tags = ListPlayerTags();
        if (!m_state) return;

        std::unordered_map<std::string, PlayerNotes::Note> notesMap;
        std::unordered_map<std::string, std::vector<std::string>> tagsMap;
        if (notes.has_value()) {
            for (auto& n : *notes)
                notesMap.emplace(n.identity.Key(), std::move(n));
        }
        if (tags.has_value()) {
            for (auto& t : *tags)
                tagsMap[t.identity.Key()].push_back(std::move(t.text));
        }

        m_state->notes.PublishLoaded(!notes.has_value(), std::move(notesMap), std::move(tagsMap));
    },
                       DbJobPriority::Normal);
}

void DatabaseManager::AsyncSavePlayerNote(PlayerNotes::Identity identity, std::string text, std::string lastKnownName, uint64_t requestId) {
    (void)EnqueueDbJob([this, id = std::move(identity), text = std::move(text), name = std::move(lastKnownName), requestId]() {
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        std::string error;
        const bool ok = PlayerNotes::NormalizeNote(text).empty()
                            ? DeletePlayerNote(id, &error)
                            : SavePlayerNote(id, text, name, now, &error);
        if (m_state) {
            m_state->notes.PublishNoteSaved(requestId, ok, error, id, text, name, now);
        }
    },
                       DbJobPriority::Critical);
}

void DatabaseManager::AsyncDeletePlayerNote(PlayerNotes::Identity identity, uint64_t requestId) {
    (void)EnqueueDbJob([this, id = std::move(identity), requestId]() {
        std::string error;
        const bool ok = DeletePlayerNote(id, &error);
        if (m_state) {
            m_state->notes.PublishNoteDeleted(requestId, ok, error, id);
        }
    },
                       DbJobPriority::Critical);
}

// Settings
bool DatabaseManager::SetSetting(const std::string& key, const std::string& value) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;

    try {
        const char* sql = "INSERT OR REPLACE INTO Settings (key, value) VALUES (?, ?);";
        database::sqlite::Statement stmt(m_db, sql);
        stmt.Bind(1, key);
        stmt.Bind(2, value);
        stmt.Execute();
        return true;
    } catch (...) {
        return false;
    }
}

void DatabaseManager::AsyncSetSetting(std::string key, std::string value) {
    if (!EnqueueDbJob([this, k = std::move(key), v = std::move(value)]() {
            (void)SetSetting(k, v);
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue setting write for " << key << ".\n";
    }
}

std::string DatabaseManager::GetSetting(const std::string& key, const std::string& defaultValue) {
    if (m_readerPool.HasPool()) {
        auto lease = m_readerPool.Acquire();
        if (lease.IsValid()) {
            try {
                database::sqlite::Statement stmt(lease.Get(), "SELECT value FROM Settings WHERE key = ?;");
                stmt.Bind(1, key);
                if (stmt.Step() && !stmt.IsNull(0)) {
                    return stmt.Get<std::string>(0);
                }
            } catch (...) {
            }
            return defaultValue;
        }
    }

    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return defaultValue;
    try {
        database::sqlite::Statement stmt(m_db, "SELECT value FROM Settings WHERE key = ?;");
        stmt.Bind(1, key);
        if (stmt.Step() && !stmt.IsNull(0)) {
            return stmt.Get<std::string>(0);
        }
    } catch (...) {
    }
    return defaultValue;
}
