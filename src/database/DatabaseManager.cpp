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
#include "core/game/PlaylistMetadata.hpp"
#include "network/mmr/MmrService.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
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
    m_initializationError.clear();

    try {
        m_db = database::sqlite::Connection::Open(dbPath);
    } catch (const std::exception& e) {
        m_initializationError = std::string("The database file could not be opened: ") + e.what();
        std::cerr << "[Database] Failed to open SQLite database: " << e.what() << "\n";
        return false;
    }

    m_db.BusyTimeout(5000);

    try {
        database::sqlite::Statement probe(m_db, "SELECT COUNT(*) FROM sqlite_master;");
        (void)probe.Step();
    } catch (const std::exception& e) {
        m_initializationError = "The database file is damaged or is not an OmniStats database.";
        std::cerr << "[Database] Database file is unreadable: " << e.what() << "\n";
        m_db.Close();
        return false;
    }

    std::string foreignKeyFailure;
    if (repository::Migrations::RepairForeignKeyViolations(m_db, dbPath, foreignKeyFailure)) {
        try {
            m_db.Execute("PRAGMA foreign_keys = ON;");
            database::sqlite::Statement enabled(m_db, "PRAGMA foreign_keys;");
            if (!enabled.Step() || enabled.Get<int>(0) != 1) foreignKeyFailure = "Cannot enable foreign-key enforcement.";
        } catch (const std::exception& e) {
            foreignKeyFailure = std::string("Cannot enable foreign-key enforcement: ") + e.what();
        }
    }
    if (!foreignKeyFailure.empty()) {
        m_initializationError = foreignKeyFailure;
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
        m_initializationError = "The database could not be upgraded to this version of OmniStats.";
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

bool DatabaseManager::UpsertSeasons(const std::vector<SeasonRecord>& seasons) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    try {
        sqlite::Transaction tx(m_db, sqlite::TransactionType::Immediate);
        sqlite::Statement stmt(m_db, R"(
            INSERT INTO Seasons(season, starts_at, ends_at, source) VALUES(?1, ?2, ?3, ?4)
            ON CONFLICT(season) DO UPDATE SET starts_at=excluded.starts_at,
                ends_at=excluded.ends_at, source=excluded.source, updated_at=unixepoch()
            WHERE excluded.source != 'observed' OR Seasons.source = 'observed';
        )");
        for (const auto& season : seasons) {
            if (season.season <= 0 || season.startsAt <= 0 || season.endsAt < season.startsAt) return false;
            stmt.Reset();
            stmt.Bind(1, season.season);
            stmt.Bind(2, season.startsAt);
            stmt.Bind(3, season.endsAt);
            stmt.Bind(4, season.source);
            stmt.Execute();
        }
        tx.Commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Database] UpsertSeasons failed: " << e.what() << "\n";
        return false;
    }
}

std::vector<SeasonRecord> DatabaseManager::GetSeasons() {
    std::lock_guard lock(m_writeMutex);
    std::vector<SeasonRecord> result;
    if (!m_db.IsOpen()) return result;
    try {
        sqlite::Statement stmt(m_db, "SELECT season, starts_at, ends_at, source FROM Seasons ORDER BY starts_at DESC, season DESC;");
        while (stmt.Step())
            result.push_back({stmt.Get<int>(0), stmt.Get<int64_t>(1), stmt.Get<int64_t>(2), stmt.Get<std::string>(3)});
    } catch (const std::exception& e) {
        std::cerr << "[Database] GetSeasons failed: " << e.what() << "\n";
    }
    return result;
}

bool DatabaseManager::ObserveRankedSeason(const std::string& primaryId, const std::string& playlistKey,
                                          int matchesPlayed, int64_t observedAt) {
    if (primaryId.empty() || !TrackedAccounts::IsSupportedPlaylistKey(playlistKey) ||
        playlistKey == "casual" || playlistKey == "t" || matchesPlayed < 0 || observedAt <= 0) return false;
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    try {
        sqlite::Transaction tx(m_db, sqlite::TransactionType::Immediate);
        sqlite::Statement previous(m_db, "SELECT matches_played, observed_at FROM RankedSeasonObservation WHERE primary_id=?1 AND playlist_key=?2;");
        previous.Bind(1, primaryId);
        previous.Bind(2, playlistKey);
        if (previous.Step()) {
            const int oldMatches = previous.Get<int>(0);
            const int64_t oldAt = previous.Get<int64_t>(1);
            if (observedAt <= oldAt) return true;
            if (matchesPlayed < oldMatches) {
                sqlite::Statement latest(m_db, "SELECT season, starts_at, ends_at FROM Seasons WHERE starts_at<=?1 ORDER BY starts_at DESC, season DESC LIMIT 1;");
                latest.Bind(1, observedAt);
                if (latest.Step() && oldAt >= latest.Get<int64_t>(1)) {
                    sqlite::Statement insert(m_db, "INSERT OR IGNORE INTO Seasons(season, starts_at, ends_at, source) VALUES(?1, ?2, ?2, 'observed');");
                    insert.Bind(1, latest.Get<int>(0) + 1);
                    insert.Bind(2, observedAt);
                    insert.Execute();
                }
            }
        }
        sqlite::Statement save(m_db, R"(
            INSERT INTO RankedSeasonObservation(primary_id, playlist_key, matches_played, observed_at)
            VALUES(?1, ?2, ?3, ?4) ON CONFLICT(primary_id, playlist_key) DO UPDATE SET
            matches_played=excluded.matches_played, observed_at=excluded.observed_at;
        )");
        save.Bind(1, primaryId);
        save.Bind(2, playlistKey);
        save.Bind(3, matchesPlayed);
        save.Bind(4, observedAt);
        save.Execute();
        tx.Commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Database] ObserveRankedSeason failed: " << e.what() << "\n";
        return false;
    }
}

bool DatabaseManager::SaveTrackedAccountRewards(const std::string& primaryId, int season, int level, int wins, int64_t observedAt) {
    if (primaryId.empty() || season <= 0 || level < 0 || wins < 0 || observedAt <= 0) return false;
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return false;
    try {
        sqlite::Statement stmt(m_db, R"(
            INSERT INTO TrackedAccountRewards(primary_id, season, level, wins, observed_at) VALUES(?1, ?2, ?3, ?4, ?5)
            ON CONFLICT(primary_id, season) DO UPDATE SET level=excluded.level, wins=excluded.wins, observed_at=excluded.observed_at
            WHERE excluded.observed_at >= TrackedAccountRewards.observed_at;
        )");
        stmt.Bind(1, primaryId);
        stmt.Bind(2, season);
        stmt.Bind(3, level);
        stmt.Bind(4, wins);
        stmt.Bind(5, observedAt);
        stmt.Execute();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[Database] SaveTrackedAccountRewards failed: " << e.what() << "\n";
        return false;
    }
}

std::optional<TrackedAccountReward> DatabaseManager::GetTrackedAccountRewards(const std::string& primaryId, int season) {
    std::lock_guard lock(m_writeMutex);
    if (!m_db.IsOpen()) return std::nullopt;
    try {
        sqlite::Statement stmt(m_db, "SELECT season, level, wins, observed_at FROM TrackedAccountRewards WHERE primary_id=?1 AND season=?2;");
        stmt.Bind(1, primaryId);
        stmt.Bind(2, season);
        if (stmt.Step()) return TrackedAccountReward{stmt.Get<int>(0), stmt.Get<int>(1), stmt.Get<int>(2), stmt.Get<int64_t>(3)};
    } catch (const std::exception& e) {
        std::cerr << "[Database] GetTrackedAccountRewards failed: " << e.what() << "\n";
    }
    return std::nullopt;
}

std::vector<SeasonSummary> DatabaseManager::GetSeasonSummaries(const std::string& primaryId) {
    std::lock_guard lock(m_writeMutex);
    std::vector<SeasonSummary> result;
    if (!m_db.IsOpen()) return result;
    try {
        sqlite::Statement seasons(m_db, "SELECT season, starts_at, ends_at, source FROM Seasons ORDER BY starts_at DESC, season DESC;");
        while (seasons.Step()) {
            SeasonSummary summary;
            summary.season = {seasons.Get<int>(0), seasons.Get<int64_t>(1), seasons.Get<int64_t>(2), seasons.Get<std::string>(3)};
            result.push_back(std::move(summary));
        }
        for (size_t i = 0; i < result.size(); ++i) {
            auto& summary = result[i];
            const int64_t end = i > 0 ? result[i - 1].season.startsAt
                                      : (summary.season.endsAt > summary.season.startsAt
                                             ? summary.season.endsAt + 1
                                             : std::numeric_limits<int64_t>::max());
            struct Rating {
                int64_t time;
                int mmr;
            };
            std::map<std::string, std::vector<Rating>> ratings;
            sqlite::Statement matches(m_db, R"(
                SELECT m.playlist_id, m.gamemode, m.win, CAST(strftime('%s',m.timestamp) AS INTEGER),
                       p.mmr, p.mmr_estimated, p.is_opponent
                FROM Matches m JOIN MatchPlayers p ON p.id = (
                    SELECT MIN(id) FROM MatchPlayers WHERE match_id=m.id AND primary_id=?1
                )
                WHERE COALESCE(m.result_pending,0)=0 AND m.win IS NOT NULL
                  AND CAST(strftime('%s',m.timestamp) AS INTEGER)>=?2
                  AND CAST(strftime('%s',m.timestamp) AS INTEGER)<?3
                ORDER BY m.timestamp, m.id;
            )");
            matches.Bind(1, primaryId);
            matches.Bind(2, summary.season.startsAt);
            matches.Bind(3, end);
            while (matches.Step()) {
                std::string key;
                if (!matches.IsNull(0)) {
                    const auto* info = PlaylistMetadata::Find(matches.Get<int>(0));
                    if (!info || info->playlistClass != PlaylistMetadata::PlaylistClass::Ranked) continue;
                    key = info->mmrKey;
                } else {
                    key = matches.Get<std::string>(1);
                    if (!TrackedAccounts::IsSupportedPlaylistKey(key) || key == "casual" || key == "t") continue;
                }
                if (key.empty()) continue;
                const bool won = (matches.Get<int>(2) != 0) != (matches.Get<int>(6) != 0);
                auto& playlist = summary.playlists[key];
                if (won) {
                    ++summary.wins;
                    ++playlist.wins;
                } else {
                    ++summary.losses;
                    ++playlist.losses;
                }
                const int mmr = matches.Get<int>(4);
                if (mmr > 0 && matches.Get<int>(5) == 0) {
                    ratings[key].push_back({matches.Get<int64_t>(3), mmr});
                    if (mmr > summary.peakMmr) {
                        summary.peakMmr = mmr;
                        summary.peakRank = MmrService::GetRankTierForPlaylistMmr(key, mmr);
                        summary.peakPlaylist = key;
                    }
                }
            }
            sqlite::Statement observed(m_db, R"(
                SELECT playlist_key, mmr, COALESCE(rank_tier,''), observed_at
                FROM TrackedAccountRankObservations WHERE primary_id=?1 AND observed_at>=?2 AND observed_at<?3
                ORDER BY observed_at, id;
            )");
            observed.Bind(1, primaryId);
            observed.Bind(2, summary.season.startsAt);
            observed.Bind(3, end);
            while (observed.Step()) {
                const std::string key = observed.Get<std::string>(0);
                if (!TrackedAccounts::IsSupportedPlaylistKey(key) || key == "casual" || key == "t") continue;
                const int mmr = observed.Get<int>(1);
                ratings[key].push_back({observed.Get<int64_t>(3), mmr});
                if (mmr > summary.peakMmr || (mmr == summary.peakMmr && summary.peakRank.empty())) {
                    summary.peakMmr = mmr;
                    summary.peakRank = observed.Get<std::string>(2);
                    summary.peakPlaylist = key;
                }
            }
            if (summary.peakMmr > 0 && summary.peakRank.empty()) {
                summary.peakRank = MmrService::GetRankTierForPlaylistMmr(summary.peakPlaylist, summary.peakMmr);
            }
            for (auto& [key, points] : ratings) {
                std::stable_sort(points.begin(), points.end(), [](const Rating& a, const Rating& b) { return a.time < b.time; });
                if (points.size() < 2) continue;
                const int change = points.back().mmr - points.front().mmr;
                summary.mmrChangeByPlaylist[key] = change;
                summary.playlists[key].mmrChange = change;
                summary.totalMmrChange += change;
            }
            const int games = summary.wins + summary.losses;
            if (games) summary.winRate = static_cast<double>(summary.wins) / games;
        }
    } catch (const std::exception& e) {
        std::cerr << "[Database] GetSeasonSummaries failed: " << e.what() << "\n";
        return {};
    }
    return result;
}

void DatabaseManager::AsyncGetSeasonSummaries(std::string primaryId, uint64_t requestId) {
    if (!m_state) return;
    auto pending = std::make_shared<SeasonSummarySnapshot>();
    pending->primaryId = primaryId;
    pending->requestId = requestId;
    std::shared_ptr<const SeasonSummarySnapshot> desired = pending;
    auto current = m_state->seasonSummaries.load();
    do {
        if (current && current->requestId > requestId) return;
    } while (!m_state->seasonSummaries.compare_exchange_weak(current, desired));
    (void)EnqueueDbJob([this, primaryId = std::move(primaryId), requestId, pending = std::move(desired)]() mutable {
        auto completed = std::make_shared<SeasonSummarySnapshot>();
        completed->loaded = true;
        completed->primaryId = primaryId;
        completed->requestId = requestId;
        completed->seasons = GetSeasonSummaries(primaryId);
        m_state->seasonSummaries.compare_exchange_strong(pending, std::move(completed));
    });
}
