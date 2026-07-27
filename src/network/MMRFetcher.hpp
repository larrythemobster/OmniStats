#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/SessionState.hpp"

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
    int previousMatches = 0;
    bool previousMmrIsPlaylistSpecific = false;
    bool won = false;
    int retriesRemaining = 2;
    std::chrono::steady_clock::time_point notBefore{};
};

struct MMRProfileTotals {
    int totalWins = -1;
};

class MMRFetcher {
  public:
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
                          bool won);

    static std::string GetTournamentTierForMmr(int mmr);
    static std::string PlaylistNameForTrackerId(int playlistId);
    static MMRProfileTotals ExtractProfileTotals(const nlohmann::json& jsonResp);
    static bool IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches = 0, int fetchedMatches = 0);
    static int ResolvePostMatchBaseline(int requestedPreviousMmr, bool previousMmrIsPlaylistSpecific, const std::vector<float>& recentHistory);
    static int EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory);
    static bool ShouldPreserveEstimatedMmr(int lastConfirmedMmr, int estimatedMmr, int fetchedMmr);

#ifdef OMNISTATS_TEST_ENVIRONMENT
    size_t PendingRequestCountForTests();
#endif

  private:
    void WorkerLoop();
    bool FetchProfile(MMRRequest req);
    bool ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason);
    std::string GetTRNPlatform(const std::string& primaryId);
    void FinishRequest(const MMRRequest& req);

    std::shared_ptr<SessionState> m_state;
    std::weak_ptr<DatabaseManager> m_dbManager;

    std::deque<MMRRequest> m_queue;
    std::unordered_set<std::string> m_rosterQueuedOrInFlight;
    std::unordered_set<std::string> m_pendingPostMatchGuids;
    std::unordered_map<std::string, size_t> m_pendingPostMatchCountByPlaylist;
    struct PendingMmrEstimate {
        int lastConfirmedMmr = 0;
        int estimatedMmr = 0;
    };

    std::unordered_set<std::string> m_completedPostMatchGuids;
    std::unordered_map<std::string, PendingMmrEstimate> m_pendingEstimatedMmrByPlaylist;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
};
