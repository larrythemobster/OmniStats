#pragma once

#include "network/mmr/MmrServiceDetail.hpp"
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class MmrRequestQueue {
  public:
    explicit MmrRequestQueue(MmrTimings timings = {}) : m_timings(timings) {}
    ~MmrRequestQueue() = default;

    MmrRequestQueue(const MmrRequestQueue&) = delete;
    MmrRequestQueue& operator=(const MmrRequestQueue&) = delete;

    using RunningCheck = std::function<bool()>;
    using TrackedRetryCallback = std::function<void(const MMRRequest&, std::chrono::milliseconds, bool)>;

    void SetRunningCheck(RunningCheck check) {
        m_runningCheck = std::move(check);
    }
    void SetTrackedRetryCallback(TrackedRetryCallback cb) {
        m_trackedRetryCallback = std::move(cb);
    }

    bool EnqueueRoster(const std::string& primaryId, const std::string& name, uint64_t sessionGen);
    bool EnqueuePostMatch(MMRRequest req);
    TrackedEnqueueResult EnqueueTrackedAccount(const std::string& primaryId, const std::string& name);
    void CancelTrackedAccount(const std::string& primaryId);

    void SetRateLimitedUntil(std::chrono::steady_clock::time_point until);
    bool IsRateLimited(std::chrono::steady_clock::time_point now) const;

    size_t PendingCount() const;
    bool Empty() const;
    bool HasPendingPostMatch(const std::string& matchGuid) const;

    std::optional<MMRRequest> PopNextPostMatch(std::chrono::steady_clock::time_point now);
    std::optional<MMRRequest> PopNext(std::chrono::steady_clock::time_point now);
    MmrServiceDetail::AssembledBatch DrainBatch(MMRRequestReason reason,
                                                const std::string& playlist,
                                                std::chrono::steady_clock::time_point now,
                                                uint64_t sessionGen,
                                                size_t maxBatch = 8);
    MmrServiceDetail::AssembledBatch DrainBatchForReason(MMRRequestReason reason,
                                                         std::chrono::steady_clock::time_point now,
                                                         uint64_t sessionGen,
                                                         size_t maxBatch = 8);

    std::optional<MMRRequestReason> NextBatchReason(std::chrono::steady_clock::time_point now) const;
    std::optional<std::string> FirstReadyPlaylist(MMRRequestReason reason, std::chrono::steady_clock::time_point now) const;
    size_t ReadyCount(MMRRequestReason reason, const std::string& playlist, std::chrono::steady_clock::time_point now) const;
    bool HasReady(MMRRequestReason reason, std::chrono::steady_clock::time_point now) const;

    bool ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason);
    std::optional<std::pair<size_t, std::chrono::steady_clock::duration>> RequeueForbidden(MMRRequest req);
    bool ResetForbidden();
    void Finish(const MMRRequest& req);

    void Clear();

  private:
    MmrTimings m_timings;
    mutable std::mutex m_mutex;
    std::deque<MMRRequest> m_queue;
    std::unordered_set<std::string> m_rosterQueuedOrInFlight;
    std::unordered_set<std::string> m_trackedQueuedOrInFlight;
    std::unordered_set<std::string> m_pendingPostMatchGuids;
    size_t m_forbiddenStrikeCount = 0;
    std::chrono::steady_clock::time_point m_rateLimitedUntil{};
    RunningCheck m_runningCheck;
    TrackedRetryCallback m_trackedRetryCallback;
};
