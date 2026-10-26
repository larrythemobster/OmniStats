#pragma once

#include "network/mmr/MmrServiceDetail.hpp"
#include "network/mmr/MmrStatePublisher.hpp"
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class MmrReconciler {
  public:
    using DestroyedMatchConfirmationCallback =
        std::function<void(const std::string& matchGuid, bool won)>;

    struct DbUpdate {
        std::string matchGuid;
        std::string primaryId;
        int mmr = 0;
        bool estimated = false;
    };

    struct ReconcileResult {
        bool applied = false;
        MmrPublisher::ReconcilePublication publication;
        std::vector<DbUpdate> dbUpdates;
        std::vector<std::pair<std::string, bool>> confirmations;
        DestroyedMatchConfirmationCallback confirmationCallback;
        bool allPendingCovered = false;
        bool requestConfirmed = false;
    };

    struct ProvisionalResult {
        bool applied = false;
        MmrPublisher::ProvisionalPublication publication;
        std::optional<DbUpdate> dbUpdate;
    };

    MmrReconciler() = default;
    ~MmrReconciler() = default;

    MmrReconciler(const MmrReconciler&) = delete;
    MmrReconciler& operator=(const MmrReconciler&) = delete;

    void SetDestroyedMatchConfirmationCallback(DestroyedMatchConfirmationCallback cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_destroyedMatchConfirmationCallback = std::move(cb);
    }

    void EnqueuePostMatch(const std::string& primaryId,
                          const std::string& matchGuid,
                          const std::string& playlist,
                          int previousMmr,
                          int previousMatches,
                          bool previousMmrIsPlaylistSpecific,
                          bool won,
                          uint64_t sessionGeneration);

    void EnqueuePendingDestroyedMatch(const PendingDestroyedMatchMmrRefresh& pending,
                                      uint64_t sessionGeneration);

    void ResolvePendingDestroyedMatch(const std::string& matchGuid, bool won);

    ReconcileResult ReconcileTrackerResponse(const MMRRequest& req,
                                             int fetchedMmr,
                                             int fetchedMatches,
                                             const std::vector<float>& historyY,
                                             uint64_t currentSessionGeneration,
                                             const std::string& myPrimaryId,
                                             bool inMatch,
                                             bool matchFinalized,
                                             const std::vector<SessionMmrPoint>* points = nullptr,
                                             int sessionInitialMmr = 0);

    ProvisionalResult EnsureProvisionalPoint(const MMRRequest& req,
                                             int baselineMmr,
                                             int fetchedMmr,
                                             int fetchedMatches,
                                             const std::vector<float>& historyY,
                                             int sessionInitialMmr);

    bool HasPendingDestroyedMatch(const std::string& matchGuid) const;
    size_t PendingPlaylistCount(const std::string& primaryId, const std::string& playlist) const;
    std::optional<PendingPostMatchRecord> GetRecord(const std::string& matchGuid) const;
    std::vector<MMRRequest> PendingRequestsForPlayer(const std::string& primaryId, uint64_t sessionGeneration) const;
    std::vector<std::string> PendingGuidsForPlayer(const std::string& primaryId) const;
    bool IsCompletedPostMatch(const std::string& matchGuid) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_completedPostMatchGuids.count(matchGuid) > 0;
    }

    void AdoptCachedPreMatchBaseline(const std::string& matchGuid,
                                     int cachedMmr,
                                     int cachedMatches,
                                     int fetchedMatches);

    static bool IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches = -1, int fetchedMatches = -1);
    static size_t CoveredPendingMatchCount(int oldestPreviousMatches, int fetchedMatches, size_t pendingCount);
    static int ResolvePostMatchBaseline(int requestedPreviousMmr, bool previousMmrIsPlaylistSpecific, const std::vector<float>& recentHistory);
    static int EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory);

  private:
    static std::string PendingPostMatchKey(const std::string& primaryId, const std::string& playlist) {
        return primaryId + "|" + playlist;
    }

    void ResetPublicationBaselineForCounterRollback(const std::string& primaryId,
                                                    const std::string& playlist,
                                                    int previousMatches);

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, PendingPostMatchRecord> m_postMatchRecordsByGuid;
    std::unordered_map<std::string, std::deque<std::string>> m_pendingPostMatchesByPlaylist;
    std::unordered_set<std::string> m_completedPostMatchGuids;
    std::unordered_map<std::string, int> m_trackerPublicationBaselineByPlaylist;
    DestroyedMatchConfirmationCallback m_destroyedMatchConfirmationCallback;
};
