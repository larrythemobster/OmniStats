#include "DatabaseManager.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "DatabaseDetail.hpp"
#include "core/config/Config.hpp"
#include "database/repository/HistoryRepository.hpp"
#include "database/repository/MatchRepository.hpp"
#include "database/repository/Migrations.hpp"
#include "database/repository/DejaVuImporter.hpp"
#include "database/repository/PlayerNotesRepository.hpp"
#include "database/repository/ReplayRepository.hpp"
#include "database/repository/SeasonRepository.hpp"
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
                // Release the key before running so a request made during this run queues a fresh job.
                if (job.priority == DbJobPriority::Coalescable && !job.coalesceKey.empty()) {
                    m_pendingCoalescedJobs.erase(job.coalesceKey);
                }
            }

            try {
                job.run();
            } catch (const std::exception& e) {
                std::cerr << "[DatabaseManager] Job Exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[DatabaseManager] Job Exception: Unknown\n";
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

    auto lock = LockWriter();
    m_db.Close();
}

bool DatabaseManager::Initialize(const std::string& dbPath) {
    auto lock = LockWriter();
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
    m_schemaUpgradeIncomplete = m_db.UserVersion() < repository::Migrations::LatestVersion();
    m_hasStatsTables = repository::Migrations::HasStatsTables(m_db);
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
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return;
    repository::MatchRepository::SaveMatch(m_db, snapshot, m_hasStatsTables, m_telemetry.get());
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
                RefreshLifetimeMmrSync(primaryId);
            }
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue match save during shutdown.\n";
    }
}

bool DatabaseManager::UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated) {
    auto lock = LockWriter();
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
            RefreshLifetimeMmrSync(primaryId);
        }
    },
                       DbJobPriority::Critical);
}

int64_t DatabaseManager::SaveSession(const SessionRecap& recap) {
    auto lock = LockWriter();
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
    auto lock = LockWriter();
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
            auto lock = LockWriter();
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
        auto read = ReadConnection(false);
        if (read.IsValid()) {
            shouldBackfillAccount = repository::SessionRepository::HasUnlinkedMatches(read.Get(), account);
        }
    }
    if (shouldBackfillAccount) {
        BackfillSessions(500);
    }

    auto read = ReadConnection();
    return repository::SessionRepository::ListSessions(read.Get(), account, offset, limit);
}

SessionRecap DatabaseManager::GetSession(int64_t id) {
    if (m_backfillPending.load(std::memory_order_acquire)) {
        BackfillSessions(500);
    }
    if (id <= 0) return {};

    auto read = ReadConnection();
    return repository::SessionRepository::GetSession(read.Get(), id);
}

void DatabaseManager::GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY) {
    auto read = ReadConnection();
    repository::HistoryRepository::GetLifetimeMmrHistory(read.Get(), primaryId, playlist, outX, outY);
}

void DatabaseManager::RefreshLifetimeMmrSync(const std::string& primaryId) {
    if (!m_state || primaryId.empty()) return;
    const auto live = m_state->liveMatch ? m_state->liveMatch->GetSnapshot() : nullptr;
    if (live && !live->myPrimaryId.empty()) {
        if (primaryId != live->myPrimaryId) return;
    } else if (primaryId != Config::Read().last_primary_id) {
        return;
    }

    std::vector<int64_t> x;
    std::vector<float> y;
    GetLifetimeMmrHistory(primaryId, MmrCategoryToString(m_state->ui.graphMmrCategory.load()), x, y);
    m_state->history.SetLifetimeMmr(std::move(x), std::move(y));
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
    auto read = ReadConnection();
    repository::HistoryRepository::GetRecentMatchHistory(read.Get(), primaryId, outMatches, limit);
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

Tournaments::BracketResult DatabaseManager::GetTournamentBracketResult(const std::string& primaryId, int64_t startUnix,
                                                                       int64_t secondChanceUnix, std::string_view observedMatchGuid) {
    auto read = ReadConnection();
    return repository::HistoryRepository::GetTournamentBracketResult(read.Get(), primaryId, startUnix, secondChanceUnix, observedMatchGuid);
}

std::optional<PersonRecord> DatabaseManager::GetPlayerEncounterRecord(const std::string& account, const std::string& primaryId) {
    auto read = ReadConnection();
    return repository::HistoryRepository::GetPlayerEncounterRecord(read.Get(), account, primaryId);
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
    auto read = ReadConnection();
    repository::HistoryRepository::GetPeopleRecords(read.Get(), primaryId, outPeople);
}

void DatabaseManager::GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches) {
    auto read = ReadConnection();
    repository::HistoryRepository::GetMatchOutcomes(read.Get(), primaryId, outMatches);
}

void DatabaseManager::GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out) {
    auto read = ReadConnection();
    repository::HistoryRepository::GetMatchMmrContext(read.Get(), primaryId, out);
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
    auto read = ReadConnection();
    repository::HistoryRepository::GetGamemodeStats(read.Get(), primaryId, gamemode, wins, losses, gamesPlayed);
}

void DatabaseManager::GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss) {
    auto read = ReadConnection();
    repository::HistoryRepository::GetStreakStats(read.Get(), primaryId, outCurWin, outCurLoss, outLongestWin, outLongestLoss);
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
    auto lock = LockWriter();
    if (!m_db.IsOpen()) {
        error = "Database is not open.";
        return false;
    }
    return repository::TransferService::ExportLocalData(m_db, exportPath, error);
}

bool DatabaseManager::DeleteLocalMatchHistory(std::string& error) {
    auto lock = LockWriter();
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
    auto lock = LockWriter();
    if (!m_db.IsOpen()) {
        return DbMergeResult{.success = false, .error = "Database is not open."};
    }
    return repository::TransferService::MergeDatabase(m_db, sourceDbPath, m_dbPath);
}

int DatabaseManager::CountDejaVuOffer(const std::filesystem::path& playerCounterPath) {
    std::error_code ec;
    if (playerCounterPath.empty() || !std::filesystem::is_regular_file(playerCounterPath, ec)) return 0;
    {
        auto read = ReadConnection();
        if (!read.Get().IsOpen() || repository::DejaVuImporter::Answered(read.Get())) return 0;
    }
    std::string error;
    const auto rows = repository::DejaVuImporter::LoadFile(playerCounterPath, error);
    if (!rows) {
        std::cerr << "[Database] Ignoring DejaVu history: " << error << "\n";
        return 0;
    }
    return repository::DejaVuImporter::CountPlayers(*rows);
}

void DatabaseManager::AsyncOfferDejaVuImport(std::filesystem::path playerCounterPath, std::vector<std::string> localAccounts) {
    if (!m_state) return;
    (void)EnqueueDbJob([this, path = std::move(playerCounterPath), ids = std::move(localAccounts)]() {
        const int players = CountDejaVuOffer(path);
        if (players <= 0 || !m_state) return;
        std::vector<std::pair<std::string, std::string>> accounts;
        for (const auto& id : ids) {
            if (id.empty() || id.rfind("Unknown|", 0) == 0) continue;
            if (std::any_of(accounts.begin(), accounts.end(), [&](const auto& account) { return account.first == id; })) continue;
            if (auto name = FindLatestPlayerName(id)) accounts.emplace_back(id, std::move(*name));
        }
        std::lock_guard lock(m_state->ui.dejaVuMutex);
        m_state->ui.dejaVuPath = path.string();
        m_state->ui.dejaVuAccounts = std::move(accounts);
        m_state->ui.dejaVuPlayers.store(players);
    });
}

DejaVuImportResult DatabaseManager::ImportDejaVuHistory(const std::filesystem::path& playerCounterPath, const std::string& account) {
    DejaVuImportResult result;
    const auto rows = repository::DejaVuImporter::LoadFile(playerCounterPath, result.error);
    if (!rows) return result;

    auto lock = LockWriter();
    if (repository::DejaVuImporter::Answered(m_db)) {
        result.error = "DejaVu history was already imported or declined.";
        return result;
    }
    if (!repository::DejaVuImporter::Import(m_db, account, *rows, result.error)) return result;
    result.imported = true;
    result.players = repository::DejaVuImporter::CountPlayers(*rows);
    return result;
}

bool DatabaseManager::DeclineDejaVuImport(std::string& error) {
    auto lock = LockWriter();
    return repository::DejaVuImporter::Decline(m_db, error);
}

void DatabaseManager::AsyncDeclineDejaVuImport() {
    if (!EnqueueDbJob([this]() {
            std::string error;
            if (!DeclineDejaVuImport(error)) std::cerr << "[Database] Failed to save DejaVu answer: " << error << "\n";
        },
                      DbJobPriority::Critical)) {
        std::cerr << "[Database] Failed to enqueue DejaVu answer.\n";
    }
}

std::vector<PlayerStatRow> DatabaseManager::GetPlayerStatSeries(const std::string& primaryId,
                                                                const std::string& playlistFilter,
                                                                int limit) {
    auto read = ReadConnection();
    return repository::HistoryRepository::GetPlayerStatSeries(read.Get(), primaryId, playlistFilter, limit);
}

std::vector<AnalyticsMatchRow> DatabaseManager::GetAnalyticsMatches(const std::string& primaryId, int limit) {
    auto read = ReadConnection();
    return repository::HistoryRepository::GetAnalyticsMatches(read.Get(), primaryId, limit);
}

void DatabaseManager::GetDetailedStatsSummary(int& outMatchCount, std::string& outSinceDate) {
    auto read = ReadConnection();
    repository::HistoryRepository::GetDetailedStatsSummary(read.Get(), outMatchCount, outSinceDate);
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
    auto read = ReadConnection();
    return repository::HistoryRepository::GetSchemaVersion(read.Get());
}

void DatabaseManager::QueryMatches(const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount) {
    auto read = ReadConnection();
    repository::HistoryRepository::QueryMatches(read.Get(), query, outRows, outTotalCount);
}

void DatabaseManager::AsyncQueryMatches(MatchQuery query, uint64_t requestId, bool append) {
    if (m_state) {
        m_state->historyView.RequestQuery(requestId);
    }

    (void)EnqueueDbJob([this, q = std::move(query), requestId, append]() {
        std::vector<MatchRow> rows;
        int total = 0;
        QueryMatches(q, rows, total);
        if (m_state) {
            m_state->historyView.PublishQueryResult(requestId, std::move(rows), total, append);
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
    auto read = ReadConnection();
    return repository::HistoryRepository::GetMatchDetail(read.Get(), matchId, outDetail, accountPrimaryId, liveAccount);
}

bool DatabaseManager::GetMatchDetailByGuid(const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId) {
    std::string liveAccount;
    if (m_state && m_state->liveMatch) {
        auto liveSnap = m_state->liveMatch->GetSnapshot();
        if (liveSnap) liveAccount = liveSnap->myPrimaryId;
    }
    auto read = ReadConnection();
    return repository::HistoryRepository::GetMatchDetailByGuid(read.Get(), matchGuid, outDetail, accountPrimaryId, liveAccount);
}

void DatabaseManager::AsyncGetMatchDetail(int64_t matchId, uint64_t requestId, std::string accountPrimaryId) {
    if (m_state && requestId != 0) {
        m_state->historyView.RequestDetail(requestId);
    }

    (void)EnqueueDbJob([this, matchId, requestId, account = std::move(accountPrimaryId)]() {
        MatchDetail detail;
        const bool ok = GetMatchDetail(matchId, detail, account);
        if (m_state) {
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
            m_state->historyView.PublishDetailResult(requestId, (ok && detail.found) ? std::optional{std::move(detail)} : std::nullopt);
        }
    },
                       DbJobPriority::Normal);
}

bool DatabaseManager::RecordDiscoveredReplay(const std::string& filePath, const std::string& fileName, const std::string& matchGuid,
                                             const std::string& initialState, const std::string& error) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RecordDiscoveredReplay(m_db, filePath, fileName, matchGuid, initialState, error);
}

bool DatabaseManager::UpdateReplayUploadState(int64_t id, const std::string& state, int attempts, int64_t nextAttemptAt, int lastHttpStatus,
                                              const std::string& lastError, const std::string& ballchasingId, const std::string& ballchasingUrl) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::UpdateReplayUploadState(m_db, id, state, attempts, nextAttemptAt, lastHttpStatus, lastError, ballchasingId, ballchasingUrl);
}

bool DatabaseManager::SetReplayUploading(int64_t id) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::SetReplayUploading(m_db, id);
}

bool DatabaseManager::RecoverStuckUploadingReplays() {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RecoverStuckUploadingReplays(m_db);
}

std::vector<ReplayUploadRecord> DatabaseManager::GetPendingReplayUploads(int limit) {
    auto read = ReadConnection();
    return repository::ReplayRepository::GetPendingReplayUploads(read.Get(), limit);
}

std::optional<ReplayUploadRecord> DatabaseManager::GetReplayUploadByPath(const std::string& filePath) {
    auto read = ReadConnection();
    return repository::ReplayRepository::GetReplayUploadByPath(read.Get(), filePath);
}

ReplayUploadStatus DatabaseManager::GetReplayUploadStatus() {
    auto read = ReadConnection();
    return repository::ReplayRepository::GetReplayUploadStatus(read.Get());
}

bool DatabaseManager::RetryFailedReplayUploads() {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::ReplayRepository::RetryFailedReplayUploads(m_db);
}

bool DatabaseManager::QueryMatchReplayInfo(const std::string& matchGuid, bool& outFound, bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId) {
    auto read = ReadConnection();
    return repository::ReplayRepository::QueryMatchReplayInfo(read.Get(), matchGuid, outFound, outRanked, outWin, outResultPending, outPlaylistId);
}

TrackedAccountInsertResult DatabaseManager::RegisterTrackedAccount(const TrackedAccounts::AccountRecord& record) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return TrackedAccountInsertResult::Failed;
    return repository::TrackedAccountsRepository::RegisterTrackedAccount(m_db, record);
}

std::vector<TrackedAccounts::AccountRecord> DatabaseManager::ListTrackedAccounts() {
    auto read = ReadConnection();
    return repository::TrackedAccountsRepository::ListTrackedAccounts(read.Get());
}

std::optional<TrackedAccounts::AccountRecord> DatabaseManager::GetTrackedAccount(const std::string& primaryId) {
    auto read = ReadConnection();
    return repository::TrackedAccountsRepository::GetTrackedAccount(read.Get(), primaryId);
}

bool DatabaseManager::UpdateTrackedAccountNickname(const std::string& primaryId, const std::string& nickname) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountNickname(m_db, primaryId, nickname);
}

bool DatabaseManager::UpdateTrackedAccountLastSeen(const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountLastSeen(m_db, primaryId, lastKnownName, seenAtUnix);
}

bool DatabaseManager::UpdateTrackedAccountRefreshAttempt(const std::string& primaryId, int64_t attemptedAtUnix) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountRefreshAttempt(m_db, primaryId, attemptedAtUnix);
}

bool DatabaseManager::UpdateTrackedAccountLastKnownName(const std::string& primaryId, const std::string& lastKnownName) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::UpdateTrackedAccountLastKnownName(m_db, primaryId, lastKnownName);
}

std::optional<std::string> DatabaseManager::FindLatestPlayerName(const std::string& primaryId) {
    auto read = ReadConnection();
    return repository::TrackedAccountsRepository::FindLatestPlayerName(read.Get(), primaryId);
}

bool DatabaseManager::SaveTrackedAccountRanks(const std::string& primaryId,
                                              const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                              int64_t fetchedAtUnix) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::SaveTrackedAccountRanks(m_db, primaryId, ranks, fetchedAtUnix);
}

std::map<std::string, TrackedAccounts::PlaylistRank> DatabaseManager::GetTrackedAccountRanks(const std::string& primaryId) {
    auto read = ReadConnection();
    return repository::TrackedAccountsRepository::GetTrackedAccountRanks(read.Get(), primaryId);
}

std::vector<TrackedAccounts::RankHistorySeries> DatabaseManager::GetTrackedAccountRankHistory(
    const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix) {
    auto read = ReadConnection();
    return repository::TrackedAccountsRepository::GetTrackedAccountRankHistory(read.Get(), primaryIds, playlistKey, sinceUnix);
}

bool DatabaseManager::DeleteTrackedAccount(const std::string& primaryId) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return false;
    return repository::TrackedAccountsRepository::DeleteTrackedAccount(m_db, primaryId);
}

int DatabaseManager::ImportTrackedAccounts(const std::vector<TrackedAccounts::AccountRecord>& records) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) return 0;
    return repository::TrackedAccountsRepository::ImportTrackedAccounts(m_db, records);
}

std::optional<std::vector<PlayerNotes::Note>> DatabaseManager::ListPlayerNotes() {
    auto read = ReadConnection();
    return repository::PlayerNotesRepository::ListPlayerNotes(read.Get());
}

std::optional<std::vector<PlayerNotes::Tag>> DatabaseManager::ListPlayerTags() {
    auto read = ReadConnection();
    return repository::PlayerNotesRepository::ListPlayerTags(read.Get());
}

std::optional<std::vector<std::string>> DatabaseManager::GetPlayerTags(const PlayerNotes::Identity& identity) {
    auto read = ReadConnection();
    return repository::PlayerNotesRepository::GetPlayerTags(read.Get(), identity);
}

bool DatabaseManager::SetPlayerTag(const PlayerNotes::Identity& identity, std::string_view tag, bool add,
                                   int64_t nowUnix, std::string* error) {
    auto lock = LockWriter();
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
    auto read = ReadConnection();
    return repository::PlayerNotesRepository::GetPlayerScoutProfile(read.Get(), account, playerId);
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
    auto read = ReadConnection();
    return repository::PlayerNotesRepository::GetPlayerNote(read.Get(), identity);
}

bool DatabaseManager::SavePlayerNote(const PlayerNotes::Identity& identity, std::string_view text, std::string_view lastKnownName,
                                     int64_t nowUnix, std::string* error) {
    auto lock = LockWriter();
    if (!m_db.IsOpen()) {
        if (error) *error = "The local database is unavailable.";
        return false;
    }
    return repository::PlayerNotesRepository::SavePlayerNote(m_db, identity, text, lastKnownName, nowUnix, error);
}

bool DatabaseManager::DeletePlayerNote(const PlayerNotes::Identity& identity, std::string* error) {
    auto lock = LockWriter();
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
    auto lock = LockWriter();
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
    auto read = ReadConnection();
    if (!read.Get().IsOpen()) return defaultValue;
    try {
        database::sqlite::Statement stmt(read.Get(), "SELECT value FROM Settings WHERE key = ?;");
        stmt.Bind(1, key);
        if (stmt.Step() && !stmt.IsNull(0)) {
            return stmt.Get<std::string>(0);
        }
    } catch (...) {
    }
    return defaultValue;
}

bool DatabaseManager::UpsertSeasons(const std::vector<SeasonRecord>& seasons) {
    auto lock = LockWriter();
    return repository::SeasonRepository::UpsertSeasons(m_db, seasons);
}

std::vector<SeasonRecord> DatabaseManager::GetSeasons() {
    auto read = ReadConnection();
    return repository::SeasonRepository::GetSeasons(read.Get());
}

bool DatabaseManager::ObserveRankedSeason(const std::string& primaryId, const std::string& playlistKey,
                                          int matchesPlayed, int64_t observedAt) {
    auto lock = LockWriter();
    return repository::SeasonRepository::ObserveRankedSeason(m_db, primaryId, playlistKey, matchesPlayed, observedAt);
}

bool DatabaseManager::SaveTrackedAccountRewards(const std::string& primaryId, int season, int level, int wins, int64_t observedAt) {
    auto lock = LockWriter();
    return repository::SeasonRepository::SaveTrackedAccountRewards(m_db, primaryId, season, level, wins, observedAt);
}

std::optional<TrackedAccountReward> DatabaseManager::GetTrackedAccountRewards(const std::string& primaryId, int season) {
    auto read = ReadConnection();
    return repository::SeasonRepository::GetTrackedAccountRewards(read.Get(), primaryId, season);
}

std::vector<SeasonSummary> DatabaseManager::GetSeasonSummaries(const std::string& primaryId) {
    auto read = ReadConnection();
    return repository::SeasonRepository::GetSeasonSummaries(read.Get(), primaryId);
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
