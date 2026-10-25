#pragma once
#include <string>
#include <memory>
#include <mutex>
#include "core/state/SessionState.hpp"
#include <sqlite3.h>
class TelemetryManager;

#include <deque>
#include <functional>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <optional>
#include <vector>

#include "core/state/MatchSaveSnapshot.hpp"
#include "core/accounts/TrackedAccounts.hpp"

struct DbMergeResult {
    int matchesImported = 0;
    int matchesSkipped = 0;
    int playersImported = 0;
    bool success = false;
    std::string error;
};

enum class TrackedAccountInsertResult {
    Inserted,
    AlreadyExists,
    Failed
};

struct ReplayUploadRecord {
    int64_t id = 0;
    std::string filePath;
    std::string fileName;
    std::string matchGuid;
    std::string state; // pending, uploading, done, duplicate, failed, skipped
    int attempts = 0;
    int64_t nextAttemptAt = 0;
    int lastHttpStatus = 0;
    std::string lastError;
    std::string ballchasingId;
    std::string ballchasingUrl;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

struct PlayerStatRow {
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string playlist;
    int playlistId = -1;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int mmr = 0;
    std::optional<int> score;
    int goals = 0;
    int assists = 0;
    int saves = 0;
    int shots = 0;
    int demos = 0;
    std::optional<int> touches;
    std::optional<int> carTouches;
    std::optional<float> maxGoalSpeed;
    std::optional<float> fastestGoalTime;
    bool hasLocalStats = false;
    int boostCollected = 0;
    int demoed = 0;
    int crossbars = 0;
    std::optional<float> hardestCrossbar;
    std::optional<float> maxBallSpeed;
    int ownGoals = 0;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    int statsVersion = 1;
};

class DatabaseManager {
  public:
    DatabaseManager(std::shared_ptr<SessionState> state);
    ~DatabaseManager();

    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm) {
        m_telemetry = std::move(tm);
    }
    [[nodiscard]] bool Initialize(const std::string& dbPath);
    void SaveMatch(const MatchSaveSnapshot& snapshot);
    void AsyncSaveMatch(MatchSaveSnapshot snapshot);
    bool UpdateMatchPlayerMmr(const std::string& matchGuid, const std::string& primaryId, int mmr, bool estimated = false);
    void AsyncUpdateMatchPlayerMmr(std::string matchGuid, std::string primaryId, int mmr, bool estimated = false);

    int64_t SaveSession(const SessionRecap& recap);
    void AsyncSaveSession(SessionRecap recap);
    bool UpdateSession(const SessionRecap& recap);
    void AsyncUpdateSession(SessionRecap recap);
    int BackfillSessions(int chunkSize = 500);
    std::vector<SessionRecap> ListSessions(const std::string& account, int offset = 0, int limit = 50);
    SessionRecap GetSession(int64_t id);
    void GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<int64_t>& outX, std::vector<float>& outY);
    void AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist);
    void GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit = kPreviousGamesDefaultLimit);
    void AsyncGetRecentMatchHistory(const std::string& primaryId, int limit = kPreviousGamesDefaultLimit);
    // With/against record between the local `account` and `primaryId`; nullopt
    // when the database cannot answer, which is distinct from zero encounters.
    std::optional<PersonRecord> GetPlayerEncounterRecord(const std::string& account, const std::string& primaryId);
    // Resolves the local account from session state when the job runs.
    void AsyncGetPlayerEncounterRecord(const std::string& primaryId);
    void GetPeopleRecords(const std::string& primaryId, std::vector<PersonRecord>& outPeople);
    void GetMatchOutcomes(const std::string& primaryId, std::vector<MatchOutcome>& outMatches);
    void GetMatchMmrContext(const std::string& primaryId, std::vector<MatchMmrContext>& out);
    // Loads people and match outcomes for the Insights window into state->insights.
    void AsyncLoadInsights(const std::string& primaryId);
    void GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed);
    void GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss);
    void RefreshDbStatsSync(const std::string& primaryId);
    void AsyncRefreshDbStats(const std::string& primaryId);
    bool ExportLocalData(std::string& exportPath, std::string& error);
    bool DeleteLocalMatchHistory(std::string& error);
    DbMergeResult MergeDatabase(const std::string& sourceDbPath);
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

    bool RecordDiscoveredReplay(const std::string& filePath, const std::string& fileName, const std::string& matchGuid = "", const std::string& initialState = "pending", const std::string& error = "");
    bool UpdateReplayUploadState(int64_t id, const std::string& state, int attempts, int64_t nextAttemptAt, int lastHttpStatus, const std::string& lastError, const std::string& ballchasingId = "", const std::string& ballchasingUrl = "");
    bool SetReplayUploading(int64_t id);
    bool RecoverStuckUploadingReplays();
    std::vector<ReplayUploadRecord> GetPendingReplayUploads(int limit = 10);
    std::optional<ReplayUploadRecord> GetReplayUploadByPath(const std::string& filePath);
    ReplayUploadStatus GetReplayUploadStatus();
    bool RetryFailedReplayUploads();
    bool QueryMatchReplayInfo(const std::string& matchGuid, bool& outFound, bool& outRanked, bool& outWin, bool& outResultPending, int& outPlaylistId);

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
    // Indexed, time-scoped, downsampled observation reads; call off the render thread.
    std::vector<TrackedAccounts::RankHistorySeries> GetTrackedAccountRankHistory(
        const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix = 0);
    bool DeleteTrackedAccount(const std::string& primaryId);
    int ImportTrackedAccounts(const std::vector<TrackedAccounts::AccountRecord>& records);

    // Player notes. Text is normalized before storage; saving an empty note
    // fails rather than storing a blank row. Load/save/delete publish results to
    // SessionState::notes.
    std::optional<std::vector<PlayerNotes::Note>> ListPlayerNotes();
    std::optional<std::vector<PlayerNotes::Tag>> ListPlayerTags();
    std::optional<std::vector<std::string>> GetPlayerTags(const PlayerNotes::Identity& identity);
    bool SetPlayerTag(const PlayerNotes::Identity& identity, std::string_view tag, bool add,
                      int64_t nowUnix, std::string* error = nullptr);
    void AsyncSetPlayerTag(PlayerNotes::Identity identity, std::string tag, bool add, uint64_t requestId);
    // Call through AsyncGetPlayerScout off the render thread.
    std::optional<PlayerScoutProfile> GetPlayerScoutProfile(const std::string& account, const std::string& playerId);
    void AsyncGetPlayerScout(std::string account, std::string playerId, uint64_t requestId);
    std::optional<PlayerNotes::Note> GetPlayerNote(const PlayerNotes::Identity& identity);
    bool SavePlayerNote(const PlayerNotes::Identity& identity, std::string_view text, std::string_view lastKnownName,
                        int64_t nowUnix, std::string* error = nullptr);
    bool DeletePlayerNote(const PlayerNotes::Identity& identity, std::string* error = nullptr);
    void AsyncLoadPlayerNotes();
    // An empty normalized note deletes the stored one.
    void AsyncSavePlayerNote(PlayerNotes::Identity identity, std::string text, std::string lastKnownName, uint64_t requestId);
    void AsyncDeletePlayerNote(PlayerNotes::Identity identity, uint64_t requestId);

    [[nodiscard]] bool SetSetting(const std::string& key, const std::string& value);
    void AsyncSetSetting(std::string key, std::string value);
    std::string GetSetting(const std::string& key, const std::string& defaultValue = "");
    sqlite3* GetRawDb() {
        return m_db;
    }
    std::string GetDatabasePath() const {
        return m_dbPath;
    }

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

    struct ReadLease {
        ReadLease(sqlite3* connection, std::mutex& mutex);
        ~ReadLease();
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        std::unique_lock<std::mutex> lock;
        sqlite3* db = nullptr;
        bool ownsTransaction = false;
    };
    ReadLease AcquireRead();

    [[nodiscard]] bool EnqueueDbJob(std::function<void()> job, DbJobPriority priority = DbJobPriority::Normal, std::string coalesceKey = "");
    bool CreateTables();
    void ResolvePendingMatchLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot);
    bool UpsertMatchStatsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot);
    bool InsertMatchEventsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot);
    bool UpsertMatchAnalyticsLocked(sqlite3_int64 matchId, const MatchSaveSnapshot& snapshot);
    // Shared by the roster, People, and match detail so the screens cannot
    // disagree. Empty `onlyPlayer` returns everyone; `excludeMatchId` > 0 leaves
    // that match out. nullopt means the query failed.
    std::optional<std::vector<PersonRecord>> QueryEncounterRecordsLocked(sqlite3* db, const std::string& account,
                                                                         const std::string& onlyPlayer = {},
                                                                         int64_t excludeMatchId = 0);
    bool HasStatsTablesLocked() const;
    bool m_hasStatsTables = false;
    bool HasSessionsTableLocked() const;
    int64_t SaveSessionLocked(const SessionRecap& recap);
    bool UpdateSessionLocked(const SessionRecap& recap);
    void LinkSessionMatchesLocked(sqlite3_int64 sessionId,
                                  const std::string& account,
                                  int64_t startedAt,
                                  int64_t endedAt,
                                  const std::vector<std::string>& matchGuids,
                                  int expectedGames,
                                  int64_t& outEarliestMatchTs,
                                  int64_t& outLatestMatchTs);
    bool RecomputeBackfillSessionLocked(sqlite3_int64 sessionId, const std::string& account);
    int BackfillSessionsChunkLocked(int chunkSize, bool& outMoreRemaining, int& outLinked);
    bool m_hasSessionsTable = false;
    std::atomic<bool> m_backfillPending{false};
    struct LiveSessionRecord {
        sqlite3_int64 sessionId = 0;
        int64_t endedAtUnix = 0;
    };
    std::map<std::pair<std::string, uint64_t>, LiveSessionRecord> m_sessionIdByGeneration;

    std::shared_ptr<SessionState> m_state;
    sqlite3* m_db = nullptr;
    sqlite3* m_readDb = nullptr;
    std::string m_dbPath;
    std::mutex m_dbMutex;
    std::mutex m_readDbMutex;

    std::deque<DbJob> m_jobs;
    std::unordered_set<std::string> m_pendingCoalescedJobs;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_stopWorker = false;
    std::shared_ptr<TelemetryManager> m_telemetry;

  public:
    // Testing instrumentation (unit tests read these to assert Async calls)
    static std::atomic<int> s_test_async_get_lifetime_calls;
    static std::atomic<int> s_test_async_refresh_calls;
    static std::mutex s_test_mutex;
    static std::string s_test_last_get_lifetime_primary;
    static std::string s_test_last_get_lifetime_playlist;
    static std::string s_test_last_refresh_primary;
};
