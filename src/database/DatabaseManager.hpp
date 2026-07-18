#pragma once
#include <string>
#include <memory>
#include <mutex>
#include "core/SessionState.hpp"
#include <sqlite3.h>

#include <deque>
#include <functional>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_set>

#include "core/MatchSaveSnapshot.hpp"

class DatabaseManager {
  public:
    DatabaseManager(std::shared_ptr<SessionState> state);
    ~DatabaseManager();

    [[nodiscard]] bool Initialize(const std::string& dbPath);
    void SaveMatch(const MatchSaveSnapshot& snapshot);
    void AsyncSaveMatch(MatchSaveSnapshot snapshot);

    void GetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist, std::vector<float>& outX, std::vector<float>& outY);
    void AsyncGetLifetimeMmrHistory(const std::string& primaryId, const std::string& playlist);
    void GetRecentMatchHistory(const std::string& primaryId, std::vector<SessionMatchSummary>& outMatches, int limit = kPreviousGamesDefaultLimit);
    void AsyncGetRecentMatchHistory(const std::string& primaryId, int limit = kPreviousGamesDefaultLimit);
    void GetPlayerEncounterRecord(const std::string& primaryId, int& winsWith, int& lossesWith, int& winsAgainst, int& lossesAgainst);
    void AsyncGetPlayerEncounterRecord(const std::string& primaryId);
    void GetOpponentRecord(const std::string& primaryId, const std::string& opponentId, int& wins, int& losses);
    void GetGamemodeStats(const std::string& primaryId, const std::string& gamemode, int& wins, int& losses, int& gamesPlayed);
    void GetStreakStats(const std::string& primaryId, int& outCurWin, int& outCurLoss, int& outLongestWin, int& outLongestLoss);
    void RefreshDbStatsSync(const std::string& primaryId);
    void AsyncRefreshDbStats(const std::string& primaryId);
    bool ExportLocalData(std::string& exportPath, std::string& error);
    bool DeleteLocalMatchHistory(std::string& error);

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

    [[nodiscard]] bool EnqueueDbJob(std::function<void()> job, DbJobPriority priority = DbJobPriority::Normal, std::string coalesceKey = "");
    bool CreateTables();
    std::string InferGamemode(int playerCount);

    std::shared_ptr<SessionState> m_state;
    sqlite3* m_db = nullptr;
    std::string m_dbPath;
    std::mutex m_dbMutex;

    std::deque<DbJob> m_jobs;
    std::unordered_set<std::string> m_pendingCoalescedJobs;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_stopWorker = false;

  public:
    // Testing instrumentation (unit tests read these to assert Async calls)
    static std::atomic<int> s_test_async_get_lifetime_calls;
    static std::atomic<int> s_test_async_refresh_calls;
    static std::mutex s_test_mutex;
    static std::string s_test_last_get_lifetime_primary;
    static std::string s_test_last_get_lifetime_playlist;
    static std::string s_test_last_refresh_primary;
};
