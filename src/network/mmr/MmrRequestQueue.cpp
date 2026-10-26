#include "network/mmr/MmrRequestQueue.hpp"
#include <algorithm>
#include <iostream>

bool MmrRequestQueue::EnqueueRoster(const std::string& primaryId, const std::string& name, uint64_t sessionGen) {
    if (primaryId.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rosterQueuedOrInFlight.count(primaryId) > 0) {
        return false;
    }
    m_rosterQueuedOrInFlight.insert(primaryId);
    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.reason = MMRRequestReason::Roster;
    req.sessionGeneration = sessionGen;
    req.notBefore = std::chrono::steady_clock::now();
    m_queue.push_back(std::move(req));
    return true;
}

bool MmrRequestQueue::EnqueuePostMatch(MMRRequest req) {
    if (req.matchGuid.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pendingPostMatchGuids.count(req.matchGuid) > 0) {
        return false;
    }
    m_pendingPostMatchGuids.insert(req.matchGuid);
    req.reason = MMRRequestReason::PostMatch;
    m_queue.push_back(std::move(req));
    return true;
}

TrackedEnqueueResult MmrRequestQueue::EnqueueTrackedAccount(const std::string& primaryId, const std::string& name) {
    if (primaryId.empty()) return TrackedEnqueueResult::InvalidIdentity;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_trackedQueuedOrInFlight.count(primaryId) > 0) {
        return TrackedEnqueueResult::AlreadyQueued;
    }
    m_trackedQueuedOrInFlight.insert(primaryId);
    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.reason = MMRRequestReason::TrackedAccount;
    req.notBefore = std::chrono::steady_clock::now();
    m_queue.push_back(std::move(req));
    return TrackedEnqueueResult::Queued;
}

void MmrRequestQueue::CancelTrackedAccount(const std::string& primaryId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::erase_if(m_queue, [&](const MMRRequest& r) {
        return r.reason == MMRRequestReason::TrackedAccount && r.primaryId == primaryId;
    });
    m_trackedQueuedOrInFlight.erase(primaryId);
}

void MmrRequestQueue::SetRateLimitedUntil(std::chrono::steady_clock::time_point until) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_rateLimitedUntil = until;
}

bool MmrRequestQueue::IsRateLimited(std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_rateLimitedUntil > now;
}

size_t MmrRequestQueue::PendingCount() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.size();
}

bool MmrRequestQueue::Empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_queue.empty();
}

bool MmrRequestQueue::HasPendingPostMatch(const std::string& matchGuid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingPostMatchGuids.count(matchGuid) != 0;
}

std::optional<MMRRequest> MmrRequestQueue::PopNextPostMatch(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = std::find_if(m_queue.begin(), m_queue.end(), [&](const MMRRequest& r) {
        return r.reason == MMRRequestReason::PostMatch && r.notBefore <= now;
    });
    if (it == m_queue.end()) return std::nullopt;
    MMRRequest req = std::move(*it);
    m_queue.erase(it);
    return req;
}

std::optional<MMRRequest> MmrRequestQueue::PopNext(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_rateLimitedUntil > now) {
        return std::nullopt;
    }

    for (const auto reason : {MMRRequestReason::Roster, MMRRequestReason::TrackedAccount}) {
        auto best = m_queue.end();
        for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
            if (it->reason != reason || it->notBefore > now) continue;
            if (best == m_queue.end() || it->notBefore < best->notBefore) best = it;
        }
        if (best != m_queue.end()) {
            MMRRequest req = std::move(*best);
            m_queue.erase(best);
            return req;
        }
    }
    return std::nullopt;
}

MmrServiceDetail::AssembledBatch MmrRequestQueue::DrainBatch(MMRRequestReason reason,
                                                             const std::string& playlist,
                                                             std::chrono::steady_clock::time_point now,
                                                             uint64_t sessionGen,
                                                             size_t maxBatch) {
    std::lock_guard<std::mutex> lock(m_mutex);
    MmrServiceDetail::AssembledBatch batch;
    batch.playlist = playlist;
    batch.playlistId = MmrServiceDetail::CustomApiPlaylistIdForName(playlist);

    for (auto it = m_queue.begin(); it != m_queue.end() && batch.requests.size() < maxBatch;) {
        if (it->reason == reason && it->playlist == playlist && it->notBefore <= now) {
            MMRRequest item = std::move(*it);
            item.sessionGeneration = sessionGen;
            batch.requests.push_back(std::move(item));
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
    return batch;
}

MmrServiceDetail::AssembledBatch MmrRequestQueue::DrainBatchForReason(MMRRequestReason reason,
                                                                      std::chrono::steady_clock::time_point now,
                                                                      uint64_t sessionGen,
                                                                      size_t maxBatch) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto first = std::find_if(m_queue.begin(), m_queue.end(), [&](const MMRRequest& r) {
        return r.reason == reason && r.notBefore <= now;
    });
    if (first == m_queue.end()) return {};
    const std::string playlist = first->playlist;

    MmrServiceDetail::AssembledBatch batch;
    batch.playlist = playlist;
    batch.playlistId = MmrServiceDetail::CustomApiPlaylistIdForName(playlist);

    for (auto it = m_queue.begin(); it != m_queue.end() && batch.requests.size() < maxBatch;) {
        if (it->reason == reason && it->playlist == playlist && it->notBefore <= now) {
            MMRRequest item = std::move(*it);
            item.sessionGeneration = sessionGen;
            batch.requests.push_back(std::move(item));
            it = m_queue.erase(it);
        } else {
            ++it;
        }
    }
    return batch;
}

std::optional<MMRRequestReason> MmrRequestQueue::NextBatchReason(std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto reason : {MMRRequestReason::Roster, MMRRequestReason::TrackedAccount}) {
        const bool ready = std::any_of(m_queue.begin(), m_queue.end(), [&](const MMRRequest& r) {
            return r.reason == reason && r.notBefore <= now;
        });
        if (ready) return reason;
    }
    return std::nullopt;
}

std::optional<std::string> MmrRequestQueue::FirstReadyPlaylist(MMRRequestReason reason, std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = std::find_if(m_queue.begin(), m_queue.end(), [&](const MMRRequest& req) {
        return req.reason == reason && req.notBefore <= now;
    });
    if (it == m_queue.end()) return std::nullopt;
    return it->playlist;
}

size_t MmrRequestQueue::ReadyCount(MMRRequestReason reason, const std::string& playlist, std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return static_cast<size_t>(std::count_if(m_queue.begin(), m_queue.end(), [&](const MMRRequest& req) {
        return req.reason == reason && req.playlist == playlist && req.notBefore <= now;
    }));
}

bool MmrRequestQueue::HasReady(MMRRequestReason reason, std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_queue.begin(), m_queue.end(), [&](const MMRRequest& req) {
        return req.reason == reason && req.notBefore <= now;
    });
}

bool MmrRequestQueue::ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason) {
    if (m_runningCheck && !m_runningCheck()) return false;
    if (req.retriesRemaining <= 0) return false;
    --req.retriesRemaining;
    const auto now = std::chrono::steady_clock::now();
    req.notBefore = now + delay;
    bool rateLimited = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        rateLimited = m_rateLimitedUntil > now;
    }
    if (m_trackedRetryCallback && req.reason == MMRRequestReason::TrackedAccount) {
        m_trackedRetryCallback(req, delay, rateLimited);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(req));
    }
    if (reason && reason[0] != '\0') {
        std::cout << "[MmrRequestQueue] Retrying MMR request after " << delay.count()
                  << " ms (" << reason << ").\n";
    }
    return true;
}

std::optional<std::pair<size_t, std::chrono::steady_clock::duration>> MmrRequestQueue::RequeueForbidden(MMRRequest req) {
    if (m_runningCheck && !m_runningCheck()) return std::nullopt;
    size_t strike;
    std::chrono::steady_clock::duration lockout;
    std::optional<MMRRequest> trackedReq;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        strike = ++m_forbiddenStrikeCount;
        lockout = MmrServiceDetail::ForbiddenLockoutForStrike(strike);
        m_rateLimitedUntil = std::chrono::steady_clock::now() + lockout;
        req.notBefore = m_rateLimitedUntil;
        if (req.reason == MMRRequestReason::TrackedAccount) trackedReq = req;
        m_queue.push_front(std::move(req));
    }
    if (trackedReq && m_trackedRetryCallback) {
        m_trackedRetryCallback(*trackedReq, std::chrono::duration_cast<std::chrono::milliseconds>(lockout), true);
    }
    return std::make_pair(strike, lockout);
}

bool MmrRequestQueue::ResetForbidden() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool recovered = m_forbiddenStrikeCount > 0;
    m_forbiddenStrikeCount = 0;
    m_rateLimitedUntil = {};
    return recovered;
}

void MmrRequestQueue::Finish(const MMRRequest& req) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (req.reason == MMRRequestReason::PostMatch) {
        m_pendingPostMatchGuids.erase(req.matchGuid);
    } else if (req.reason == MMRRequestReason::TrackedAccount) {
        m_trackedQueuedOrInFlight.erase(req.primaryId);
    } else {
        m_rosterQueuedOrInFlight.erase(req.primaryId);
    }
}

void MmrRequestQueue::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    m_rosterQueuedOrInFlight.clear();
    m_trackedQueuedOrInFlight.clear();
    m_pendingPostMatchGuids.clear();
}
