#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/WorkQueue.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include "network/mmr/MmrService.hpp"

class DatabaseManager;

class AccountManagerService {
  public:
    enum class RefreshState {
        Idle,
        Queued,
        Fetching,
        RateLimited,
        AuthRequired,
        NotFound,
        NoRankedData,
        ProviderUnavailable,
        Unsupported,
        Failed
    };

    struct AccountView {
        TrackedAccounts::AccountRecord record;
        RefreshState refreshState = RefreshState::Idle;
        std::string refreshDetail;
        int64_t retryAtUnix = 0;
    };

    struct HistorySnapshot {
        std::string playlistKey;
        std::string window;
        std::vector<std::string> accountIds;
        std::vector<TrackedAccounts::RankHistorySeries> series;
        bool loading = false;
        bool loaded = false;
    };

    struct Snapshot {
        uint64_t version = 0;
        bool loaded = false;
        RankProvider provider = RankProvider::Unavailable;
        std::vector<AccountView> accounts;
        HistorySnapshot history;
    };

    struct AddResult {
        enum class Status {
            Added,
            AlreadyTracked,
            Invalid
        };
        Status status = Status::Invalid;
        std::string primaryId;
        std::string message;
    };

    enum class RefreshRequest {
        Queued,
        AlreadyQueued,
        CoolingDown,
        ProviderUnavailable,
        Unsupported,
        UnknownAccount
    };

    struct ImportCandidate {
        TrackedAccounts::Identity identity;
        std::string lastKnownName;
    };

    static constexpr int64_t kStaleAfterSeconds = 30 * 60;
    static constexpr int64_t kManualRefreshCooldownSeconds = 60;
    static constexpr int64_t kAutoRefreshRetrySeconds = 5 * 60;

    AccountManagerService(std::shared_ptr<DatabaseManager> dbManager,
                          std::shared_ptr<MmrService> mmrFetcher);
    ~AccountManagerService();
    AccountManagerService(const AccountManagerService&) = delete;
    AccountManagerService& operator=(const AccountManagerService&) = delete;

    void Start();
    void Stop();

    uint64_t Version() const {
        return m_version.load(std::memory_order_acquire);
    }
    Snapshot GetSnapshot() const;

    AddResult AddManual(TrackedAccounts::Platform platform,
                        std::string_view accountInput,
                        std::string_view nickname);
    bool Rename(const std::string& primaryId, std::string_view nickname);
    bool Remove(const std::string& primaryId);

    RefreshRequest Refresh(const std::string& primaryId);
    size_t RefreshAll();
    size_t RefreshStale();
    // Worker-owned, nonblocking historical graph query. Repeated requests for
    // the same selection use cache until a successful rank save invalidates it.
    void RequestHistory(std::vector<std::string> accountIds,
                        std::string playlistKey, std::string window);
    // Runs RefreshStale on the worker after Start's initial load.
    void QueueStartupRefresh();

    std::vector<ImportCandidate> ImportCandidates() const;
    size_t ImportKnown();

    void ObserveLocalAccount(const std::string& primaryId, const std::string& displayName);

    static RankProvider CurrentProvider();

#ifdef OMNISTATS_TEST_ENVIRONMENT
    void SetClockForTests(std::function<int64_t()> nowUnix);
    void FlushForTests();
#endif

  private:
    struct SinkGuard {
        std::mutex mutex;
        AccountManagerService* owner = nullptr;
    };

    int64_t NowUnix() const;
    void Publish();
    void ApplyFetchEvent(const TrackedAccountFetchEvent& event);
    RefreshRequest RefreshLocked(AccountView& account, int64_t now, bool manual);
    AccountView* FindLocked(const std::string& primaryId);
    AddResult RegisterLocked(const TrackedAccounts::Identity& identity,
                             std::string_view nickname,
                             std::string_view lastKnownName,
                             TrackedAccounts::DiscoverySource source,
                             int64_t now);
    void ObserveLocked(const std::string& primaryId, const std::string& displayName, int64_t now);
    void ResolveNamesOnWorker(const std::vector<std::string>& primaryIds, bool refreshAfter);
    void PostResolveNames(std::vector<std::string> primaryIds, bool refreshAfter);
    void PostDb(std::function<void(DatabaseManager&)> job);

    std::shared_ptr<DatabaseManager> m_db;
    std::shared_ptr<MmrService> m_fetcher;
    std::shared_ptr<SinkGuard> m_sinkGuard;

    mutable std::mutex m_mutex;
    bool m_loaded = false;
    bool m_started = false;
    std::vector<AccountView> m_accounts;
    HistorySnapshot m_history;
    bool m_historyDirty = true;
    uint64_t m_historyGeneration = 0;
    int64_t m_historyRequestedAtUnix = 0;
    std::vector<std::pair<std::string, std::string>> m_pendingObservations;
    std::unordered_set<std::string> m_pending;
    std::unordered_set<std::string> m_seenThisRun;
    std::unordered_map<std::string, int64_t> m_lastManualRefresh;
    std::unordered_map<std::string, std::string> m_resolvedNames;
    int64_t m_nextSortOrder = 1;
    std::function<int64_t()> m_clock;
    std::atomic<uint64_t> m_version{1};

    WorkQueue m_worker;
};

const char* RefreshStateKey(AccountManagerService::RefreshState state);
