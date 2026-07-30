#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/SessionState.hpp"
#include "core/SideEffects.hpp"

class DatabaseManager;

enum class MMRRequestReason {
    Roster,
    PostMatch
};

struct MMRRequest {
    std::string primaryId;
    std::string name;
    MMRRequestReason reason = MMRRequestReason::Roster;
    std::string matchGuid;
    std::string playlist;
    int previousMmr = 0;
    int previousMatches = -1;
    bool previousMmrIsPlaylistSpecific = false;
    bool won = false;
    bool resultKnown = true;
    int retriesRemaining = 2;
    std::chrono::steady_clock::time_point notBefore{};
};

struct MMRProfileTotals {
    int totalWins = -1;
};

enum class PostMatchReconciliationState {
    AwaitingTracker,
    Provisional,
    Confirmed
};

struct PendingPostMatchRecord {
    std::string matchGuid;
    std::string primaryId;
    std::string playlist;
    int preMatchMmr = 0;
    int preMatchMatchesPlayed = -1;
    bool preMatchMmrIsPlaylistSpecific = false;
    int firstObservedMmr = 0;
    int firstObservedMatchesPlayed = -1;
    bool won = false;
    bool resultKnown = true;
    bool destroyedMatch = false;
    bool localPlayerDisappeared = false;
    bool explicitLocalForfeit = false;
    int localTeam = -1;
    std::array<int, 2> score{};
    int64_t destroyedAtUnixMs = 0;
    bool validCompetitiveMatch = false;
    bool databaseMatchFinalized = false;
    int provisionalMmr = 0;
    bool graphPointAppended = false;
    bool databaseRowUpdated = false;
    bool trackerCovered = false;
    bool valueEstimated = true;
    PostMatchReconciliationState reconciliationState = PostMatchReconciliationState::AwaitingTracker;
};

class MMRFetcher {
  public:
    using DestroyedMatchConfirmationCallback =
        std::function<void(const std::string& matchGuid, bool won)>;
    explicit MMRFetcher(std::shared_ptr<SessionState> state,
                        std::shared_ptr<DatabaseManager> dbManager = nullptr);
    ~MMRFetcher();

    void Start();
    void Stop();

    // Pushes a normal roster refresh into the queue. Duplicate roster requests
    // for the same player are coalesced while one is queued or in flight.
    void Enqueue(const std::string& primaryId, const std::string& name);

    // Schedules a guaranteed local-player refresh for a completed match. These
    // requests are keyed by match GUID, so they are never lost behind a normal
    // roster refresh and each completed match can produce one graph point.
    void EnqueuePostMatch(const std::string& primaryId,
                          const std::string& name,
                          const std::string& matchGuid,
                          const std::string& playlist,
                          int previousMmr,
                          int previousMatches,
                          bool previousMmrIsPlaylistSpecific,
                          bool won,
                          bool provisionalImmediately = false);
    void EnqueuePendingDestroyedMatch(const PendingDestroyedMatchMmrRefresh& pending);
    void ResolvePendingDestroyedMatch(const std::string& matchGuid, bool won);
    void SetDestroyedMatchConfirmationCallback(DestroyedMatchConfirmationCallback callback);

    static std::string GetTournamentTierForMmr(int mmr);
    static std::string PlaylistNameForTrackerId(int playlistId);
    static MMRProfileTotals ExtractProfileTotals(const nlohmann::json& jsonResp);
    static bool IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches = -1, int fetchedMatches = -1);
    static int ResolvePostMatchBaseline(int requestedPreviousMmr, bool previousMmrIsPlaylistSpecific, const std::vector<float>& recentHistory);
    static int EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory);
    static size_t CoveredPendingMatchCount(int oldestPreviousMatches,
                                           int fetchedMatches,
                                           size_t pendingCount);

#ifdef OMNISTATS_TEST_ENVIRONMENT
    size_t PendingRequestCountForTests();
    void ProcessPostMatchResponseForTests(const std::string& matchGuid, int fetchedMmr, int fetchedMatches);
    std::vector<SessionMmrPoint> PlaylistMatchPointsForTests(const std::string& playlist);
    bool HasPendingDestroyedMatchForTests(const std::string& matchGuid);
    void FetchRosterProfileForTests(const std::string& primaryId, const std::string& name);

#endif

  private:
    void WorkerLoop();
    bool FetchProfile(MMRRequest req);
    bool ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason);
    std::string GetTRNPlatform(const std::string& primaryId);
    void FinishRequest(const MMRRequest& req);
    bool ReconcileTrackerResponse(const MMRRequest& req, int fetchedMmr, int fetchedMatches);
    void EnsureProvisionalPoint(const MMRRequest& req,
                                int baselineMmr,
                                int fetchedMmr = 0,
                                int fetchedMatches = -1);
    void UpdateSessionAggregateLocked();
    size_t PendingPlaylistCountLocked(const std::string& playlist) const;

    std::shared_ptr<SessionState> m_state;
    std::weak_ptr<DatabaseManager> m_dbManager;

    std::deque<MMRRequest> m_queue;
    std::unordered_set<std::string> m_rosterQueuedOrInFlight;
    std::unordered_set<std::string> m_pendingPostMatchGuids;
    std::unordered_set<std::string> m_completedPostMatchGuids;
    std::unordered_map<std::string, PendingPostMatchRecord> m_postMatchRecordsByGuid;
    std::unordered_map<std::string, std::deque<std::string>> m_pendingPostMatchesByPlaylist;
    std::unordered_map<std::string, int> m_trackerPublicationBaselineByPlaylist;
    DestroyedMatchConfirmationCallback m_destroyedMatchConfirmationCallback;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
};
