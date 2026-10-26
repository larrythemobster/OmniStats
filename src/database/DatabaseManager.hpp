#pragma once
class TelemetryManager;

#include "DatabaseTypes.hpp"
#include "ReaderPool.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include "core/state/MatchSaveSnapshot.hpp"
#include "core/state/SessionState.hpp"
#include "database/sqlite/Connection.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

struct DatabaseHooks {
    std::function<void(const std::string& primaryId, const std::string& playlist)> onAsyncGetLifetimeMmr;
    std::function<void(const std::string& primaryId)> onAsyncRefreshDbStats;
};

class DatabaseManager {
  public:
    explicit DatabaseManager(std::shared_ptr<SessionState> state, DatabaseHooks hooks = {});
    ~DatabaseManager();

    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm) {
        m_telemetry = std::move(tm);
    }
    DatabaseManager(const DatabaseManager&) = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;
    [[nodiscard]] bool Initialize(const std::string& dbPath);

    // Match persistence
    void SaveMatch(const MatchSaveSnapshot& snapshot);
    void AsyncSaveMatch(MatchSaveSnapshot snapshot);
    bool UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated = false);
    void AsyncUpdateMatchPlayerMmr(std::string matchGuid, std::string primaryId, int mmr, bool estimated = false);

    // Sessions
    int64_t SaveSession(const SessionRecap& recap);
    void AsyncSaveSession(SessionRecap recap);
    bool UpdateSession(const SessionRecap& recap);
    void AsyncUpdateSession(SessionRecap recap);
    int BackfillSessions(int chunkSize = 500);
    std::vector<SessionRecap> ListSessions(const std::string& account, int offset = 0, int limit = 50);
    SessionRecap GetSession(int64_t id);

    // History & Stats
    void GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY);
    void AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist);
    void GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit = kPreviousGamesDefaultLimit);
    void AsyncGetRecentMatchHistory(const std::string& primaryId, int limit = kPreviousGamesDefaultLimit);
    std::optional<PersonRecord> GetPlayerEncounterRecord(const std::string& account, const std::string& primaryId);
    void AsyncGetPlayerEncounterRecord(const std::string& primaryId);
    void GetPeopleRecords(const std::string& primaryId, std::vector<PersonRecord>& outPeople);
    void GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches);
    void GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out);
    void AsyncLoadInsights(const std::string& primaryId);
    void GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed);
    void GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss);
    void RefreshDbStatsSync(const std::string& primaryId);
    void AsyncRefreshDbStats(const std::string& primaryId);

    // Transfers & export
    bool ExportLocalData(std::string& exportPath, std::string& error);
    bool DeleteLocalMatchHistory(std::string& error);
    DbMergeResult MergeDatabase(const std::string& sourceDbPath);

    // Queries
    std::vector<PlayerStatRow> GetPlayerStatSeries(const std::string& primaryId,
                                                   const std::string& playlistFilter = "",
                                                   int limit = 100);
    std::vector<AnalyticsMatchRow> GetAnalyticsMatches(const std::string& primaryId, int limit = 100);
    void GetDetailedStatsSummary(int& outMatchCount, std::string& outSinceDate);
    void AsyncRefreshDetailedStatsSummary();
    int GetSchemaVersion();
    void QueryMatches(const MatchQuery& query, std::vector<MatchRow>& outRows, int& outTotalCount);
    void AsyncQueryMatches(MatchQuery query, uint64_t requestId, bool append = false);
    bool GetMatchDetail(int64_t matchId, MatchDetail& outDetail, const std::string& accountPrimaryId = "");
    bool GetMatchDetailByGuid(const std::string& matchGuid, MatchDetail& outDetail, const std::string& accountPrimaryId = "");
    void AsyncGetMatchDetail(int64_t matchId, uint64_t requestId = 0, std::string accountPrimaryId = "");
    void AsyncGetMatchDetailByGuid(std::string matchGuid, uint64_t requestId = 0, std::string accountPrimaryId = "");

    // Replay uploads
    bool RecordDiscoveredReplay(const std::string& filePath, const std::string& fileName, const std::string& matchGuid = "",
                                const std::string& initialState = "pending", const std::string& error = "");
    bool UpdateReplayUploadState(int64_t id, const std::string& state, int attempts, int64_t nextAttemptAt, int lastHttpStatus,
                                 const std::string& lastError, const std::string& ballchasingId = "", const std::string& ballchasingUrl = "");
    bool SetReplayUploading(int64_t id);
    bool RecoverStuckUploadingReplays();
    std::vector<ReplayUploadRecord> GetPendingReplayUploads(int limit = 10);
    std::optional<ReplayUploadRecord> GetReplayUploadByPath(const std::string& filePath);
    ReplayUploadStatus GetReplayUploadStatus();
    bool RetryFailedReplayUploads();
    bool QueryMatchReplayInfo(const std::string& matchGuid, bool& outFound, bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId);

    // Tracked accounts
    TrackedAccountInsertResult RegisterTrackedAccount(const TrackedAccounts::AccountRecord& record);
    std::vector<TrackedAccounts::AccountRecord> ListTrackedAccounts();
    std::optional<TrackedAccounts::AccountRecord> GetTrackedAccount(const std::string& primaryId);
    bool UpdateTrackedAccountNickname(const std::string& primaryId, const std::string& nickname);
    bool UpdateTrackedAccountLastSeen(const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix);
    bool UpdateTrackedAccountRefreshAttempt(const std::string& primaryId, int64_t attemptedAtUnix);
    bool UpdateTrackedAccountLastKnownName(const std::string& primaryId, const std::string& lastKnownName);
    std::optional<std::string> FindLatestPlayerName(const std::string& primaryId);
    bool SaveTrackedAccountRanks(const std::string& primaryId,
                                 const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                 int64_t fetchedAtUnix);
    std::map<std::string, TrackedAccounts::PlaylistRank> GetTrackedAccountRanks(const std::string& primaryId);
    std::vector<TrackedAccounts::RankHistorySeries> GetTrackedAccountRankHistory(
        const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix = 0);
    bool DeleteTrackedAccount(const std::string& primaryId);
    int ImportTrackedAccounts(const std::vector<TrackedAccounts::AccountRecord>& records);

    // Player notes & scout
    std::optional<std::vector<PlayerNotes::Note>> ListPlayerNotes();
    std::optional<std::vector<PlayerNotes::Tag>> ListPlayerTags();
    std::optional<std::vector<std::string>> GetPlayerTags(const PlayerNotes::Identity& identity);
    bool SetPlayerTag(const PlayerNotes::Identity& identity, std::string_view tag, bool add,
                      int64_t nowUnix, std::string* error = nullptr);
    void AsyncSetPlayerTag(PlayerNotes::Identity identity, std::string tag, bool add, uint64_t requestId);
    std::optional<PlayerScoutProfile> GetPlayerScoutProfile(const std::string& account, const std::string& playerId);
    void AsyncGetPlayerScout(std::string account, std::string playerId, uint64_t requestId);
    std::optional<PlayerNotes::Note> GetPlayerNote(const PlayerNotes::Identity& identity);
    bool SavePlayerNote(const PlayerNotes::Identity& identity, std::string_view text, std::string_view lastKnownName,
                        int64_t nowUnix, std::string* error = nullptr);
    bool DeletePlayerNote(const PlayerNotes::Identity& identity, std::string* error = nullptr);
    void AsyncLoadPlayerNotes();
    void AsyncSavePlayerNote(PlayerNotes::Identity identity, std::string text, std::string lastKnownName, uint64_t requestId);
    void AsyncDeletePlayerNote(PlayerNotes::Identity identity, uint64_t requestId);

    // Settings
    [[nodiscard]] bool SetSetting(const std::string& key, const std::string& value);
    void AsyncSetSetting(std::string key, std::string value);
    std::string GetSetting(const std::string& key, const std::string& defaultValue = "");

    sqlite3* GetRawDb() {
        return m_db.Handle();
    }
    database::sqlite::Connection& GetConnection() {
        return m_db;
    }
    std::string GetDatabasePath() const {
        return m_dbPath;
    }
    database::ReaderPool& GetReaderPool() {
        return m_readerPool;
    }

    // Testing bridge (for backward compatibility with tests asserting async calls)
    static std::atomic<int> s_test_async_get_lifetime_calls;
    static std::atomic<int> s_test_async_refresh_calls;
    static std::mutex s_test_mutex;
    static std::string s_test_last_get_lifetime_primary;
    static std::string s_test_last_get_lifetime_playlist;
    static std::string s_test_last_refresh_primary;

  private:
    enum class DbJobPriority {
        Critical,
        Normal,
        Coalescable
    };

    struct DbJob {
        std::function<void()> run;
        DbJobPriority priority = DbJobPriority::Normal;
        std::string coalesceKey;
    };

    [[nodiscard]] bool EnqueueDbJob(std::function<void()> job, DbJobPriority priority = DbJobPriority::Normal, std::string coalesceKey = "");
    database::sqlite::Connection& GetReadConnection();

    std::shared_ptr<SessionState> m_state;
    DatabaseHooks m_hooks;

    database::sqlite::Connection m_db; // writer connection
    database::ReaderPool m_readerPool; // 2-4 read-only connections
    std::string m_dbPath;
    std::mutex m_writeMutex;

    std::atomic<bool> m_backfillPending{false};
    std::map<std::pair<std::string, uint64_t>, std::pair<int64_t, int64_t>> m_sessionIdByGeneration;

    std::deque<DbJob> m_jobs;
    std::unordered_set<std::string> m_pendingCoalescedJobs;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_stopWorker = false;
    std::shared_ptr<TelemetryManager> m_telemetry;
};
