#pragma once

#include "network/mmr/MmrRequestQueue.hpp"
#include "network/mmr/MmrReconciler.hpp"
#include "network/mmr/IRankProvider.hpp"
#include "network/mmr/MmrStatePublisher.hpp"
#include "core/state/SessionState.hpp"
#include "core/game/RankProgress.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
class DatabaseManager;
class AccountClient;
class CurlImpersonate;
class MmrService {
  public:
    using DestroyedMatchConfirmationCallback = MmrReconciler::DestroyedMatchConfirmationCallback;
    using TrackedAccountSink = std::function<void(const TrackedAccountFetchEvent&)>;

    explicit MmrService(std::shared_ptr<SessionState> state,
                        std::shared_ptr<DatabaseManager> dbManager = nullptr,
                        std::shared_ptr<AccountClient> accountClient = nullptr,
                        std::shared_ptr<IRankProvider> customProvider = nullptr,
                        std::shared_ptr<IRankProvider> trackerProvider = nullptr,
                        MmrTimings timings = {},
                        std::shared_ptr<CurlImpersonate> curl = nullptr);
    virtual ~MmrService();

    MmrService(const MmrService&) = delete;
    MmrService& operator=(const MmrService&) = delete;

    void SetAccountClient(std::shared_ptr<AccountClient> accountClient);
    void SetCustomProvider(std::shared_ptr<IRankProvider> provider);
    void SetTrackerProvider(std::shared_ptr<IRankProvider> provider);
    std::shared_ptr<CurlImpersonate> GetCurlImpersonate() const;
    void Start();
    void Stop();

    void Enqueue(const std::string& primaryId, const std::string& name);

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

    TrackedEnqueueResult EnqueueTrackedAccount(const std::string& primaryId, const std::string& name);
    void CancelTrackedAccount(const std::string& primaryId);
    void SetTrackedAccountSink(TrackedAccountSink sink);

    static RankProvider ActiveRankProvider();
    RankProvider CurrentRankProvider() const;

    static std::string GetTournamentTierForMmr(int mmr);
    static std::string GetRankTierForPlaylistMmr(const std::string& playlist, int mmr);
    static std::span<const RankProgress::Threshold> RankThresholdsForPlaylist(std::string_view playlist);
    static std::string PlaylistNameForTrackerId(int playlistId);
    static std::string RankTierName(int tier, int division);
    static MMRProfileTotals ExtractProfileTotals(const nlohmann::json& jsonResp);

    static bool IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches = -1, int fetchedMatches = -1);
    static int ResolvePostMatchBaseline(int requestedPreviousMmr, bool previousMmrIsPlaylistSpecific, const std::vector<float>& recentHistory);
    static int EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory);
    static size_t CoveredPendingMatchCount(int oldestPreviousMatches, int fetchedMatches, size_t pendingCount);

    bool ProcessPostMatchResponse(const std::string& matchGuid, int fetchedMmr, int fetchedMatches,
                                  bool allowProvisional = true);
    bool PublishProfileResult(const MMRRequest& req, const NormalizedProfileResult& profile);

    bool FetchProfile(MMRRequest req);
    void FetchRosterProfile(const std::string& primaryId, const std::string& name);
    void FetchTrackedAccount(const std::string& primaryId, const std::string& name);
    CustomApiFetchResult FetchProfileFromCustomApi(const MMRRequest& req);
    void ProcessCustomApiBatch(const MmrServiceDetail::AssembledBatch& batch);

    bool IsCustomApiActiveSource() const;
    bool IsRateLimited() const;
    bool HasPendingDestroyedMatch(const std::string& matchGuid) const;
    std::vector<SessionMmrPoint> PlaylistMatchPoints(const std::string& playlist) const;

    MmrServiceDetail::AssembledBatch TakeNextCustomApiBatch();
    std::optional<MMRRequest> TakeNextTrackerRequest();
    void FinishRequest(const MMRRequest& req);

    std::shared_ptr<IRankProvider> CustomProvider() const {
        return m_customProvider;
    }
    std::shared_ptr<IRankProvider> TrackerProvider() const {
        return m_trackerProvider;
    }
    MmrRequestQueue& Queue() {
        return m_queue;
    }
    MmrReconciler& Reconciler() {
        return m_reconciler;
    }
    size_t PendingRequestCount() const {
        return m_queue.PendingCount();
    }

  private:
    struct CachedLocalProfile {
        bool valid = false;
        std::string primaryId;
        int bestMmr = 0;
        std::string bestTier;
        std::map<std::string, int> playlists;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;
        int totalWins = -1;
        uint64_t sessionGeneration = 0;
    };

    void WorkerLoop();
    void DispatchBatch(RankProvider provider);
    void DispatchSingle(RankProvider provider);
    void NotifyTracked(const TrackedAccountFetchEvent& event);
    bool FetchProfileTracker(MMRRequest req);
    bool TrySatisfyLocalRosterRequest(const std::string& primaryId);
    void StoreLocalProfileCache(const MMRRequest& req,
                                int bestMmr,
                                const std::string& bestTier,
                                const std::map<std::string, int>& playlists,
                                const std::map<std::string, std::string>& playlistTiers,
                                const std::map<std::string, int>& playlistMatches,
                                int totalWins,
                                bool postMatchConfirmed);

    std::shared_ptr<SessionState> m_state;
    std::weak_ptr<DatabaseManager> m_dbManager;
    std::shared_ptr<AccountClient> m_accountClient;
    std::shared_ptr<IRankProvider> m_customProvider;
    std::shared_ptr<IRankProvider> m_trackerProvider;
    std::shared_ptr<CurlImpersonate> m_curlImpersonate;
    MmrTimings m_timings;
    MmrRequestQueue m_queue;
    MmrReconciler m_reconciler;
    mutable std::mutex m_reconciliationMutex;

    mutable std::mutex m_cacheMutex;
    CachedLocalProfile m_localProfileCache;

    mutable std::mutex m_sinkMutex;
    TrackedAccountSink m_trackedSink;

    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
};
