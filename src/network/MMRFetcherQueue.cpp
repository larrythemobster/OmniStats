#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <cmath>
#include <cctype>
#include <string_view>
#include "network/MMRFetcherDetail.hpp"

using namespace MMRFetcherDetail;

void MMRFetcher::Start() {
    if (m_isRunning) return;
    m_isRunning = true;
    m_workerThread = std::jthread(&MMRFetcher::WorkerLoop, this);
    std::cout << "[MMRFetcher] Background thread started.\n";
}

void MMRFetcher::Stop() {
    m_isRunning = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool MMRFetcher::TrySatisfyLocalRosterRequest(const std::string& primaryId) {
    CachedLocalProfile cached;
    bool hasCachedProfile = false;
    bool postMatchRefreshPending = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_localProfileCache.valid &&
            m_localProfileCache.primaryId == primaryId) {
            cached = m_localProfileCache;
            hasCachedProfile = true;
        } else {
            // A completed match already owns a dedicated local-player refresh.
            // Do not create a second roster request while that refresh is queued
            // or in flight; its response updates the current roster as well.
            for (const auto& guid : m_pendingPostMatchGuids) {
                const auto recordIt = m_postMatchRecordsByGuid.find(guid);
                if (recordIt != m_postMatchRecordsByGuid.end() &&
                    recordIt->second.primaryId == primaryId) {
                    postMatchRefreshPending = true;
                    break;
                }
            }
        }
    }

    if (!hasCachedProfile) return postMatchRefreshPending;

    std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
    if (m_state->game.myPrimaryId != primaryId) return false;

    const auto playerIt = m_state->game.roster.find(primaryId);
    if (playerIt == m_state->game.roster.end()) return false;

    auto& player = playerIt->second;
    player.playlists = cached.playlists;
    player.playlistTiers = cached.playlistTiers;
    player.playlistMatches = cached.playlistMatches;
    if (cached.totalWins >= 0) {
        player.totalWins = cached.totalWins;
    }
    player.mmr = cached.bestMmr;
    player.rankTier = cached.bestTier;
    player.fetched = true;
    player.fetchFailed = false;

    // If the cached value is still valid, it is also the correct pre-match
    // baseline for a newly joined game. This preserves post-match MMR tracking
    // without fetching the local profile again at match start.
    if (m_state->game.inMatch &&
        !m_state->game.matchGuid.empty() &&
        m_state->game.preMatchMmrByGuid.count(m_state->game.matchGuid) == 0) {
        const bool hasPlaylistMmr = std::any_of(
            cached.playlists.begin(), cached.playlists.end(),
            [](const auto& entry) {
                return entry.first != "best" && entry.second > 0;
            });
        if (hasPlaylistMmr) {
            m_state->game.preMatchMmrByGuid.emplace(
                m_state->game.matchGuid,
                LocalPreMatchMmrSnapshot{
                    .playlistMmrs = cached.playlists,
                    .playlistMatches = cached.playlistMatches});
        }
    }

    m_state->game.version++;
    return true;
}

void MMRFetcher::StoreLocalProfileCache(
    const MMRRequest& req,
    int bestMmr,
    const std::string& bestTier,
    const std::map<std::string, int>& playlists,
    const std::map<std::string, std::string>& playlistTiers,
    const std::map<std::string, int>& playlistMatches,
    int totalWins,
    bool postMatchConfirmed) {
    {
        std::shared_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.myPrimaryId != req.primaryId) return;
    }

    std::lock_guard<std::mutex> lock(m_queueMutex);

    // Once a match completes, the old cache is deliberately invalid. Do not
    // make it valid again from a stale roster response or an unconfirmed
    // post-match response. Wait until every pending completed match has been
    // reconciled against Tracker.
    if (req.reason == MMRRequestReason::PostMatch && !postMatchConfirmed) return;
    for (const auto& [playlist, guids] : m_pendingPostMatchesByPlaylist) {
        if (!guids.empty()) return;
    }

    m_localProfileCache.valid = true;
    m_localProfileCache.primaryId = req.primaryId;
    m_localProfileCache.bestMmr = bestMmr;
    m_localProfileCache.bestTier = bestTier;
    m_localProfileCache.playlists = playlists;
    m_localProfileCache.playlistTiers = playlistTiers;
    m_localProfileCache.playlistMatches = playlistMatches;
    if (totalWins >= 0) {
        m_localProfileCache.totalWins = totalWins;
    }
}

void MMRFetcher::Enqueue(const std::string& primaryId, const std::string& name) {
    if (!Config::Read().enable_mmr_tracking || primaryId.empty()) return;

    // Only the local player's successful profile is cached. It stays valid
    // until an actual completed match can have changed that player's rank.
    // Opponents never hit this path because their primaryId cannot match the
    // local cache. On first launch there is no cache, so training or a real
    // lobby performs one normal fetch for the local player. While a dedicated
    // post-match refresh is pending, do not duplicate that request either.
    if (TrySatisfyLocalRosterRequest(primaryId)) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_rosterQueuedOrInFlight.count(primaryId)) return;
        m_rosterQueuedOrInFlight.insert(primaryId);

        MMRRequest request;
        request.primaryId = primaryId;
        request.name = name;
        request.reason = MMRRequestReason::Roster;
        request.retriesRemaining = 2;
        request.notBefore = std::chrono::steady_clock::now();
        m_queue.push_back(std::move(request));
    }
    m_cv.notify_one();
}

void MMRFetcher::EnqueuePostMatch(const std::string& primaryId,
                                  const std::string& name,
                                  const std::string& matchGuid,
                                  const std::string& playlist,
                                  int previousMmr,
                                  int previousMatches,
                                  bool previousMmrIsPlaylistSpecific,
                                  bool won,
                                  bool provisionalImmediately) {
    if (!Config::Read().enable_mmr_tracking || primaryId.empty() || matchGuid.empty() || playlist.empty()) return;

    std::optional<MMRRequest> provisionalRequest;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_localProfileCache.primaryId == primaryId) {
            m_localProfileCache.valid = false;
        }

        if (m_postMatchRecordsByGuid.count(matchGuid) ||
            m_pendingPostMatchGuids.count(matchGuid) ||
            m_completedPostMatchGuids.count(matchGuid)) {
            return;
        }

        ResetPublicationBaselineForCounterRollbackLocked(
            playlist, previousMatches);
        m_pendingPostMatchGuids.insert(matchGuid);
        m_postMatchRecordsByGuid.emplace(
            matchGuid,
            PendingPostMatchRecord{
                .matchGuid = matchGuid,
                .primaryId = primaryId,
                .playlist = playlist,
                .preMatchMmr = previousMmr,
                .preMatchMatchesPlayed = previousMatches,
                .preMatchMmrIsPlaylistSpecific = previousMmrIsPlaylistSpecific,
                .won = won});
        m_pendingPostMatchesByPlaylist[playlist].push_back(matchGuid);

        MMRRequest request;
        request.primaryId = primaryId;
        request.name = name;
        request.reason = MMRRequestReason::PostMatch;
        request.matchGuid = matchGuid;
        request.playlist = playlist;
        request.previousMmr = previousMmr;
        request.previousMatches = previousMatches;
        request.previousMmrIsPlaylistSpecific = previousMmrIsPlaylistSpecific;
        request.won = won;
        request.retriesRemaining = 2;
        request.notBefore = std::chrono::steady_clock::now() + kPostMatchInitialDelay;
        if (provisionalImmediately) {
            provisionalRequest = request;
        }
        m_queue.push_back(std::move(request));
    }
    if (provisionalRequest) {
        EnsureProvisionalPoint(
            *provisionalRequest,
            previousMmr,
            previousMmr,
            previousMatches);
    }
    m_cv.notify_one();
}

void MMRFetcher::EnqueuePendingDestroyedMatch(
    const PendingDestroyedMatchMmrRefresh& pending) {
    if (!Config::Read().enable_mmr_tracking ||
        pending.primaryId.empty() || pending.matchGuid.empty() ||
        pending.playlist.empty() || !pending.validCompetitiveMatch) {
        return;
    }

    MMRRequest request;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_localProfileCache.primaryId == pending.primaryId) {
            m_localProfileCache.valid = false;
        }

        if (m_postMatchRecordsByGuid.count(pending.matchGuid) ||
            m_pendingPostMatchGuids.count(pending.matchGuid) ||
            m_completedPostMatchGuids.count(pending.matchGuid)) {
            return;
        }

        ResetPublicationBaselineForCounterRollbackLocked(
            pending.playlist, pending.previousMatches);
        m_pendingPostMatchGuids.insert(pending.matchGuid);
        m_postMatchRecordsByGuid.emplace(
            pending.matchGuid,
            PendingPostMatchRecord{
                .matchGuid = pending.matchGuid,
                .primaryId = pending.primaryId,
                .playlist = pending.playlist,
                .preMatchMmr = pending.previousMmr,
                .preMatchMatchesPlayed = pending.previousMatches,
                .preMatchMmrIsPlaylistSpecific =
                    pending.previousMmrIsPlaylistSpecific,
                .won = false,
                .resultKnown = false,
                .destroyedMatch = true,
                .localPlayerDisappeared =
                    pending.localPlayerDisappeared,
                .explicitLocalForfeit =
                    pending.explicitLocalForfeit,
                .localTeam = pending.localTeam,
                .score = pending.score,
                .destroyedAtUnixMs = pending.destroyedAtUnixMs,
                .validCompetitiveMatch =
                    pending.validCompetitiveMatch,
                .databaseMatchFinalized = false,
                .graphPointAppended = false});
        m_pendingPostMatchesByPlaylist[pending.playlist].push_back(
            pending.matchGuid);

        request.primaryId = pending.primaryId;
        request.name = pending.name;
        request.reason = MMRRequestReason::PostMatch;
        request.matchGuid = pending.matchGuid;
        request.playlist = pending.playlist;
        request.previousMmr = pending.previousMmr;
        request.previousMatches = pending.previousMatches;
        request.previousMmrIsPlaylistSpecific =
            pending.previousMmrIsPlaylistSpecific;
        request.won = false;
        request.resultKnown = false;
        request.retriesRemaining = 2;
        request.notBefore =
            std::chrono::steady_clock::now() +
            kPostMatchInitialDelay;
        m_queue.push_back(request);
    }

    EnsureProvisionalPoint(
        request, request.previousMmr, request.previousMmr,
        request.previousMatches);
    m_cv.notify_one();
}

void MMRFetcher::ResolvePendingDestroyedMatch(
    const std::string& matchGuid,
    bool won) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    const auto recordIt = m_postMatchRecordsByGuid.find(matchGuid);
    if (recordIt == m_postMatchRecordsByGuid.end() ||
        !recordIt->second.destroyedMatch) {
        return;
    }
    recordIt->second.resultKnown = true;
    recordIt->second.won = won;
    recordIt->second.databaseMatchFinalized = true;
    for (auto& request : m_queue) {
        if (request.matchGuid == matchGuid) {
            request.resultKnown = true;
            request.won = won;
        }
    }
}

void MMRFetcher::SetDestroyedMatchConfirmationCallback(
    DestroyedMatchConfirmationCallback callback) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_destroyedMatchConfirmationCallback = std::move(callback);
}

void MMRFetcher::WorkerLoop() {
    auto& ci = CurlImpersonate::Instance();
    if (!ci.EnsureAvailable()) {
        std::cout << "[MMRFetcher] WARNING: curl-impersonate not available. "
                  << "MMR fetching will be disabled.\n";
    }

    while (m_isRunning) {
        MMRRequest req;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            while (m_isRunning && m_queue.empty()) {
                m_cv.wait(lock);
            }
            if (!m_isRunning && m_queue.empty()) break;

            const auto now = std::chrono::steady_clock::now();
            if (m_rateLimitedUntil > now &&
                !m_useCustomApiFallback.load()) {
                const auto wakeAt = m_rateLimitedUntil;
                m_cv.wait_until(lock, wakeAt);
                continue;
            }

            auto nextIt = std::min_element(
                m_queue.begin(), m_queue.end(),
                [](const MMRRequest& lhs, const MMRRequest& rhs) {
                    return lhs.notBefore < rhs.notBefore;
                });

            if (nextIt->notBefore > now) {
                const auto wakeAt = nextIt->notBefore;
                m_cv.wait_until(lock, wakeAt);
                continue;
            }

            req = std::move(*nextIt);
            m_queue.erase(nextIt);
        }

        const bool requeued = FetchProfile(req);
        if (!requeued) FinishRequest(req);

        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_cv.wait_for(lock, kQueueSpacing, [this] { return !m_isRunning; });
    }
}

bool MMRFetcher::ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason) {
    if (!m_isRunning || req.retriesRemaining <= 0) return false;

    --req.retriesRemaining;
    req.notBefore = std::chrono::steady_clock::now() + delay;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(std::move(req));
    }
    std::cout << "[MMRFetcher] Retrying MMR request after " << delay.count()
              << " ms (" << reason << ").\n";
    m_cv.notify_one();
    return true;
}

void MMRFetcher::FinishRequest(const MMRRequest& req) {
    bool needsProvisionalPoint = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (req.reason == MMRRequestReason::PostMatch) {
            m_pendingPostMatchGuids.erase(req.matchGuid);
            const auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
            needsProvisionalPoint =
                recordIt != m_postMatchRecordsByGuid.end() &&
                !recordIt->second.graphPointAppended;
        } else {
            m_rosterQueuedOrInFlight.erase(req.primaryId);
        }
    }
    if (needsProvisionalPoint) {
        EnsureProvisionalPoint(req, req.previousMmr);
    }
}
