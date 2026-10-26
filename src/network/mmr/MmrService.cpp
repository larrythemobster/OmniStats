#include "network/mmr/MmrService.hpp"
#include "network/mmr/OmniNetRankProvider.hpp"
#include "network/mmr/TrnRankProvider.hpp"
#include "network/api/AccountClient.hpp"
#include "database/DatabaseManager.hpp"
#include "core/config/Config.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "utils/PrivacyLog.hpp"

#include <iostream>
#include <numeric>
#include <algorithm>
#include <chrono>
#include <unordered_set>

using namespace MmrServiceDetail;
#include "network/mmr/PlaylistRankThresholds.inl"

namespace {

    std::string LookupTierFromThresholds(std::span<const RankThreshold> table, int mmr) {
        if (mmr <= 0) return "Unranked";
        const RankThreshold* threshold = nullptr;
        for (const auto& entry : table) {
            if (mmr < entry.minMmr) break;
            threshold = &entry;
        }
        if (!threshold) return "Unranked";
        if (threshold->division[0] == '\0') return threshold->tier;
        return std::string(threshold->tier) + " " + threshold->division;
    }

} // namespace

MmrService::MmrService(std::shared_ptr<SessionState> state,
                       std::shared_ptr<DatabaseManager> dbManager,
                       std::shared_ptr<AccountClient> accountClient,
                       std::shared_ptr<IRankProvider> customProvider,
                       std::shared_ptr<IRankProvider> trackerProvider,
                       MmrTimings timings)
    : m_state(std::move(state)),
      m_dbManager(std::move(dbManager)),
      m_accountClient(std::move(accountClient)),
      m_customProvider(std::move(customProvider)),
      m_trackerProvider(std::move(trackerProvider)),
      m_timings(timings),
      m_queue(timings) {
    if (!m_customProvider && m_accountClient) {
        m_customProvider = std::make_shared<OmniNetRankProvider>(m_accountClient, [this]() { return !m_isRunning.load(); }, m_timings);
    }
    if (!m_trackerProvider) {
        m_trackerProvider = std::make_shared<TrnRankProvider>(nullptr, [this]() { return !m_isRunning.load(); }, m_timings);
    }
    m_queue.SetRunningCheck([this]() { return m_isRunning.load(); });
    m_queue.SetTrackedRetryCallback([this](const MMRRequest& req, std::chrono::milliseconds delay, bool rateLimited) {
        TrackedAccountFetchEvent event;
        event.primaryId = req.primaryId;
        event.status = rateLimited ? TrackedAccountFetchStatus::RateLimited : TrackedAccountFetchStatus::Retrying;
        event.retryAt = std::chrono::system_clock::now() +
                        std::chrono::duration_cast<std::chrono::system_clock::duration>(delay);
        NotifyTracked(event);
    });
}

MmrService::~MmrService() {
    Stop();
}

void MmrService::SetAccountClient(std::shared_ptr<AccountClient> accountClient) {
    m_accountClient = std::move(accountClient);
    if (!m_customProvider && m_accountClient) {
        m_customProvider = std::make_shared<OmniNetRankProvider>(m_accountClient, [this]() { return !m_isRunning.load(); }, m_timings);
    }
}

void MmrService::SetCustomProvider(std::shared_ptr<IRankProvider> provider) {
    m_customProvider = std::move(provider);
}

void MmrService::SetTrackerProvider(std::shared_ptr<IRankProvider> provider) {
    m_trackerProvider = std::move(provider);
}

RankProvider MmrService::ActiveRankProvider() {
    const ConfigData conf = Config::Read();
    return SelectActiveRankProvider(conf, !conf.account_refresh_token.empty());
}

RankProvider MmrService::CurrentRankProvider() const {
    const ConfigData conf = Config::Read();
    const bool signedIn = m_accountClient ? m_accountClient->IsSignedIn(conf) : !conf.account_refresh_token.empty();
    return SelectActiveRankProvider(conf, signedIn);
}

void MmrService::Start() {
    if (m_isRunning) return;
    if (!m_customProvider && m_accountClient) {
        m_customProvider = std::make_shared<OmniNetRankProvider>(m_accountClient, [this]() { return !m_isRunning.load(); }, m_timings);
    }
    if (!m_trackerProvider) {
        m_trackerProvider = std::make_shared<TrnRankProvider>(nullptr, [this]() { return !m_isRunning.load(); }, m_timings);
    }

    m_isRunning = true;
    m_workerThread = std::jthread(&MmrService::WorkerLoop, this);
    std::cout << "[MmrService] Background worker started.\n";
}

void MmrService::Stop() {
    m_isRunning = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MmrService::Enqueue(const std::string& primaryId, const std::string& name) {
    if (CurrentRankProvider() == RankProvider::Unavailable || primaryId.empty()) return;
    if (TrySatisfyLocalRosterRequest(primaryId)) return;

    const auto snapshot = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot() : nullptr;
    const uint64_t sessionGen = snapshot ? snapshot->sessionGeneration : 0;
    if (m_queue.EnqueueRoster(primaryId, name, sessionGen)) {
        m_cv.notify_one();
    }
}

void MmrService::EnqueuePostMatch(const std::string& primaryId,
                                  const std::string& name,
                                  const std::string& matchGuid,
                                  const std::string& playlist,
                                  int previousMmr,
                                  int previousMatches,
                                  bool previousMmrIsPlaylistSpecific,
                                  bool won,
                                  bool provisionalImmediately) {
    std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
    if (primaryId.empty() || matchGuid.empty() || playlist.empty()) return;
    if (m_reconciler.GetRecord(matchGuid) || m_reconciler.IsCompletedPostMatch(matchGuid)) return;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_localProfileCache.primaryId == primaryId) m_localProfileCache.valid = false;
    }

    const auto snapshot = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot() : nullptr;
    const uint64_t sessionGen = snapshot ? snapshot->sessionGeneration : 0;
    m_reconciler.EnqueuePostMatch(primaryId, matchGuid, playlist, previousMmr, previousMatches,
                                  previousMmrIsPlaylistSpecific, won, sessionGen);

    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.matchGuid = matchGuid;
    req.playlist = playlist;
    req.previousMmr = previousMmr;
    req.previousMatches = previousMatches;
    req.previousMmrIsPlaylistSpecific = previousMmrIsPlaylistSpecific;
    req.won = won;
    req.reason = MMRRequestReason::PostMatch;
    req.notBefore = std::chrono::steady_clock::now() + m_timings.postMatchInitialDelay;
    req.sessionGeneration = sessionGen;

    if (provisionalImmediately && m_state) {
        std::optional<MmrReconciler::DbUpdate> dbUpdate;
        m_state->history.Mutate([&](database::store::HistoryData& data) {
            std::vector<float> historyY;
            int sessionInitialMmr = 0;
            const auto copyHistory = [&](const auto& histories, const auto& initialMmrs) {
                const auto it = histories.find(playlist);
                if (it != histories.end()) historyY = it->second;
                const auto initIt = initialMmrs.find(playlist);
                if (initIt != initialMmrs.end()) sessionInitialMmr = initIt->second;
            };
            if (data.mmrOwnerPrimaryId.empty() || data.mmrOwnerPrimaryId == primaryId) {
                copyHistory(data.playlistHistoryY, data.playlistInitialMmr);
            } else {
                const auto ownerIt = data.inactiveMmrHistories.find(primaryId);
                if (ownerIt != data.inactiveMmrHistories.end()) {
                    copyHistory(ownerIt->second.playlistHistoryY, ownerIt->second.playlistInitialMmr);
                }
            }
            if (m_state->liveMatch && sessionGen == m_state->liveMatch->GetSnapshot()->sessionGeneration) {
                auto provResult = m_reconciler.EnsureProvisionalPoint(
                    req, previousMmr, previousMmr, previousMatches, historyY, sessionInitialMmr);
                if (provResult.applied) {
                    MmrPublisher::PublishProvisionalPointLocked(*m_state, data, provResult.publication);
                    dbUpdate = provResult.dbUpdate;
                }
            }
        });
        if (dbUpdate) {
            if (auto db = m_dbManager.lock()) {
                db->AsyncUpdateMatchPlayerMmr(dbUpdate->matchGuid, dbUpdate->primaryId, dbUpdate->mmr, true);
            }
        }
    }

    if (m_queue.EnqueuePostMatch(std::move(req))) {
        m_cv.notify_one();
    }
}

void MmrService::EnqueuePendingDestroyedMatch(const PendingDestroyedMatchMmrRefresh& pending) {
    std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
    if (pending.primaryId.empty() || pending.matchGuid.empty() || pending.playlist.empty() ||
        !pending.validCompetitiveMatch || m_reconciler.GetRecord(pending.matchGuid) ||
        m_reconciler.IsCompletedPostMatch(pending.matchGuid)) return;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_localProfileCache.primaryId == pending.primaryId) m_localProfileCache.valid = false;
    }
    const auto snapshot = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot() : nullptr;
    const uint64_t sessionGen = snapshot ? snapshot->sessionGeneration : 0;
    m_reconciler.EnqueuePendingDestroyedMatch(pending, sessionGen);

    MMRRequest req;
    req.primaryId = pending.primaryId;
    req.name = pending.name;
    req.matchGuid = pending.matchGuid;
    req.playlist = pending.playlist;
    req.previousMmr = pending.previousMmr;
    req.previousMatches = pending.previousMatches;
    req.previousMmrIsPlaylistSpecific = pending.previousMmrIsPlaylistSpecific;
    req.won = false;
    req.resultKnown = false;
    req.reason = MMRRequestReason::PostMatch;
    req.notBefore = std::chrono::steady_clock::now() + m_timings.postMatchInitialDelay;
    req.sessionGeneration = sessionGen;

    if (m_state) {
        std::optional<MmrReconciler::DbUpdate> dbUpdate;
        m_state->history.Mutate([&](database::store::HistoryData& data) {
            std::vector<float> historyY;
            int sessionInitialMmr = 0;
            const auto copyHistory = [&](const auto& histories, const auto& initialMmrs) {
                const auto it = histories.find(req.playlist);
                if (it != histories.end()) historyY = it->second;
                const auto initIt = initialMmrs.find(req.playlist);
                if (initIt != initialMmrs.end()) sessionInitialMmr = initIt->second;
            };
            if (data.mmrOwnerPrimaryId.empty() || data.mmrOwnerPrimaryId == req.primaryId) {
                copyHistory(data.playlistHistoryY, data.playlistInitialMmr);
            } else {
                const auto ownerIt = data.inactiveMmrHistories.find(req.primaryId);
                if (ownerIt != data.inactiveMmrHistories.end()) {
                    copyHistory(ownerIt->second.playlistHistoryY, ownerIt->second.playlistInitialMmr);
                }
            }
            if (m_state->liveMatch && sessionGen == m_state->liveMatch->GetSnapshot()->sessionGeneration) {
                auto provResult = m_reconciler.EnsureProvisionalPoint(
                    req, req.previousMmr, req.previousMmr, req.previousMatches, historyY, sessionInitialMmr);
                if (provResult.applied) {
                    MmrPublisher::PublishProvisionalPointLocked(*m_state, data, provResult.publication);
                    dbUpdate = provResult.dbUpdate;
                }
            }
        });
        if (dbUpdate) {
            if (auto db = m_dbManager.lock()) {
                db->AsyncUpdateMatchPlayerMmr(dbUpdate->matchGuid, dbUpdate->primaryId, dbUpdate->mmr, true);
            }
        }
    }

    if (m_queue.EnqueuePostMatch(std::move(req))) {
        m_cv.notify_one();
    }
}

void MmrService::ResolvePendingDestroyedMatch(const std::string& matchGuid, bool won) {
    std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
    m_reconciler.ResolvePendingDestroyedMatch(matchGuid, won);
}

void MmrService::SetDestroyedMatchConfirmationCallback(DestroyedMatchConfirmationCallback callback) {
    m_reconciler.SetDestroyedMatchConfirmationCallback(std::move(callback));
}

TrackedEnqueueResult MmrService::EnqueueTrackedAccount(const std::string& primaryId, const std::string& name) {
    if (CurrentRankProvider() == RankProvider::Unavailable) return TrackedEnqueueResult::ProviderUnavailable;
    auto res = m_queue.EnqueueTrackedAccount(primaryId, name);
    if (res == TrackedEnqueueResult::Queued) {
        m_cv.notify_one();
    }
    return res;
}

void MmrService::CancelTrackedAccount(const std::string& primaryId) {
    m_queue.CancelTrackedAccount(primaryId);
}

void MmrService::SetTrackedAccountSink(TrackedAccountSink sink) {
    std::lock_guard<std::mutex> lock(m_sinkMutex);
    m_trackedSink = std::move(sink);
}

void MmrService::NotifyTracked(const TrackedAccountFetchEvent& event) {
    TrackedAccountSink sink;
    {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        sink = m_trackedSink;
    }
    if (sink) sink(event);
}

bool MmrService::TrySatisfyLocalRosterRequest(const std::string& primaryId) {
    std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
    if (!m_state || !m_state->liveMatch) return false;
    CachedLocalProfile cached;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (m_localProfileCache.valid &&
            m_localProfileCache.primaryId == primaryId &&
            m_localProfileCache.sessionGeneration == m_state->liveMatch->GetSnapshot()->sessionGeneration) {
            cached = m_localProfileCache;
        } else {
            const auto pending = m_reconciler.PendingGuidsForPlayer(primaryId);
            return std::any_of(pending.begin(), pending.end(), [this](const std::string& guid) {
                return m_queue.HasPendingPostMatch(guid);
            });
        }
    }

    return MmrPublisher::PublishCachedLocalRoster(
        *m_state, cached.primaryId, cached.playlists, cached.playlistTiers,
        cached.playlistMatches, cached.bestMmr, cached.bestTier, cached.totalWins);
}

void MmrService::StoreLocalProfileCache(const MMRRequest& req,
                                        int bestMmr,
                                        const std::string& bestTier,
                                        const std::map<std::string, int>& playlists,
                                        const std::map<std::string, std::string>& playlistTiers,
                                        const std::map<std::string, int>& playlistMatches,
                                        int totalWins,
                                        bool postMatchConfirmed) {
    std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
    if (!m_state || !m_state->liveMatch) return;
    const auto snapshot = m_state->liveMatch->GetSnapshot();
    if (snapshot->myPrimaryId != req.primaryId || req.sessionGeneration != snapshot->sessionGeneration) return;
    if ((req.reason == MMRRequestReason::PostMatch && !postMatchConfirmed) ||
        !m_reconciler.PendingRequestsForPlayer(req.primaryId, ~uint64_t{0}).empty()) return;
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    m_localProfileCache.valid = true;
    m_localProfileCache.primaryId = req.primaryId;
    m_localProfileCache.bestMmr = bestMmr;
    m_localProfileCache.bestTier = bestTier;
    m_localProfileCache.playlists = playlists;
    m_localProfileCache.playlistTiers = playlistTiers;
    m_localProfileCache.playlistMatches = playlistMatches;
    if (totalWins >= 0) m_localProfileCache.totalWins = totalWins;
    m_localProfileCache.sessionGeneration = req.sessionGeneration;
}

bool MmrService::ProcessPostMatchResponse(const std::string& matchGuid, int fetchedMmr, int fetchedMatches,
                                          bool allowProvisional) {
    std::unique_lock<std::mutex> reconciliationLock(m_reconciliationMutex);
    const auto record = m_reconciler.GetRecord(matchGuid);
    if (!record) return m_reconciler.IsCompletedPostMatch(matchGuid);

    MMRRequest req;
    req.primaryId = record->primaryId;
    req.matchGuid = matchGuid;
    req.playlist = record->playlist;
    req.previousMmr = record->preMatchMmr;
    req.previousMatches = record->preMatchMatchesPlayed;
    req.previousMmrIsPlaylistSpecific = record->preMatchMmrIsPlaylistSpecific;
    req.won = record->won;
    req.resultKnown = record->resultKnown;
    req.sessionGeneration = record->sessionGeneration;

    std::vector<float> historyY;
    std::vector<SessionMmrPoint> points;
    uint64_t currentGen = 0;
    std::string myPrimaryId;
    bool inMatch = false;
    bool finalized = false;
    int sessionInitialMmr = 0;
    int rosterMmr = 0;
    MmrReconciler::ReconcileResult recResult;
    std::optional<SessionRecap> updatedRecap;
    std::optional<MmrReconciler::DbUpdate> provisionalDbUpdate;
    const auto reconcile = [&](database::store::HistoryData* history) {
        if (history && m_state->liveMatch) {
            const auto snapshot = m_state->liveMatch->GetSnapshot();
            const auto copyHistory = [&](const auto& playlistHistory, const auto& matchPoints, const auto& initialMmrs) {
                const auto it = playlistHistory.find(req.playlist);
                if (it != playlistHistory.end()) historyY = it->second;
                const auto pointsIt = matchPoints.find(req.playlist);
                if (pointsIt != matchPoints.end()) points = pointsIt->second;
                const auto initIt = initialMmrs.find(req.playlist);
                if (initIt != initialMmrs.end()) sessionInitialMmr = initIt->second;
            };
            if (history->mmrOwnerPrimaryId.empty() || history->mmrOwnerPrimaryId == req.primaryId) {
                copyHistory(history->playlistHistoryY, history->playlistMatchPoints,
                            history->playlistInitialMmr);
            } else {
                const auto ownerIt = history->inactiveMmrHistories.find(req.primaryId);
                if (ownerIt != history->inactiveMmrHistories.end()) {
                    copyHistory(ownerIt->second.playlistHistoryY, ownerIt->second.playlistMatchPoints,
                                ownerIt->second.playlistInitialMmr);
                }
            }
            const auto playerIt = snapshot->roster.find(req.primaryId);
            if (playerIt != snapshot->roster.end()) {
                const auto playlistIt = playerIt->second.playlists.find(req.playlist);
                if (playlistIt != playerIt->second.playlists.end()) rosterMmr = playlistIt->second;
            }
            currentGen = snapshot->sessionGeneration;
            myPrimaryId = snapshot->myPrimaryId;
            inMatch = snapshot->inMatch;
            finalized = snapshot->matchFinalized;
        }
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            const auto mmrIt = m_localProfileCache.playlists.find(req.playlist);
            const auto matchesIt = m_localProfileCache.playlistMatches.find(req.playlist);
            if (m_localProfileCache.primaryId == req.primaryId &&
                mmrIt != m_localProfileCache.playlists.end() &&
                matchesIt != m_localProfileCache.playlistMatches.end()) {
                m_reconciler.AdoptCachedPreMatchBaseline(
                    matchGuid, mmrIt->second, matchesIt->second, fetchedMatches);
            }
        }

        recResult = m_reconciler.ReconcileTrackerResponse(req, fetchedMmr, fetchedMatches, historyY,
                                                          currentGen, myPrimaryId, inMatch, finalized,
                                                          &points, sessionInitialMmr);
        if (recResult.applied && history) {
            updatedRecap = MmrPublisher::PublishReconciledPointsLocked(*m_state, *history, recResult.publication);
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            if (m_localProfileCache.primaryId == req.primaryId) {
                if (fetchedMatches >= 0) {
                    m_localProfileCache.playlists[req.playlist] = fetchedMmr;
                    m_localProfileCache.playlistMatches[req.playlist] = fetchedMatches;
                } else {
                    m_localProfileCache.playlistMatches.erase(req.playlist);
                }
            }
        }
        if (!recResult.requestConfirmed && allowProvisional && history && req.sessionGeneration == currentGen) {
            const int baselineMmr = req.previousMmr > 0 ? req.previousMmr : rosterMmr;
            auto provResult = m_reconciler.EnsureProvisionalPoint(req, baselineMmr, fetchedMmr, fetchedMatches, historyY, sessionInitialMmr);
            if (provResult.applied) {
                MmrPublisher::PublishProvisionalPointLocked(*m_state, *history, provResult.publication);
                provisionalDbUpdate = provResult.dbUpdate;
            }
        }
    };
    if (m_state && m_state->liveMatch) {
        m_state->history.Mutate([&](database::store::HistoryData& data) { reconcile(&data); });
    } else {
        reconcile(nullptr);
    }
    if (auto db = m_dbManager.lock()) {
        for (const auto& u : recResult.dbUpdates) {
            db->AsyncUpdateMatchPlayerMmr(u.matchGuid, u.primaryId, u.mmr, u.estimated);
        }
        if (updatedRecap) db->AsyncUpdateSession(*updatedRecap);
        if (provisionalDbUpdate) {
            db->AsyncUpdateMatchPlayerMmr(provisionalDbUpdate->matchGuid, provisionalDbUpdate->primaryId,
                                          provisionalDbUpdate->mmr, true);
        }
    }
    reconciliationLock.unlock();
    if (recResult.confirmationCallback) {
        for (const auto& [guid, won] : recResult.confirmations) {
            recResult.confirmationCallback(guid, won);
        }
    }
    return recResult.requestConfirmed;
}

bool MmrService::PublishProfileResult(const MMRRequest& req, const NormalizedProfileResult& profile) {
    if (req.reason == MMRRequestReason::TrackedAccount) {
        const bool hasPlaylist = std::any_of(profile.playlistMMRs.begin(), profile.playlistMMRs.end(),
                                             [](const auto& entry) { return entry.first != "best" && entry.second > 0; });
        TrackedAccountFetchEvent event;
        event.primaryId = req.primaryId;
        event.status = hasPlaylist ? TrackedAccountFetchStatus::Success : TrackedAccountFetchStatus::NoRankedData;
        event.profile = profile;
        NotifyTracked(event);
        return false;
    }

    if (!m_state) return false;

    const auto snapshot = m_state->liveMatch ? m_state->liveMatch->GetSnapshot() : nullptr;
    const std::string myPrimaryId = snapshot ? snapshot->myPrimaryId : std::string{};
    bool postMatchConfirmed = false;

    if (req.reason == MMRRequestReason::PostMatch) {
        const auto mmrIt = profile.playlistMMRs.find(req.playlist);
        const auto matchesIt = profile.playlistMatches.find(req.playlist);
        const int fetchedMmr = mmrIt != profile.playlistMMRs.end() ? mmrIt->second : 0;
        const int fetchedMatches = matchesIt != profile.playlistMatches.end() ? matchesIt->second : -1;
        postMatchConfirmed = ProcessPostMatchResponse(req.matchGuid, fetchedMmr, fetchedMatches, false);
        if (!postMatchConfirmed) {
            if (req.retriesRemaining > 0 &&
                m_queue.ScheduleRetry(req, m_timings.stalePostMatchRetryDelay,
                                      "post-match match count not advanced")) return true;
            ProcessPostMatchResponse(req.matchGuid, fetchedMmr, fetchedMatches);
        }
    } else if (req.primaryId == myPrimaryId) {
        const auto pendingList = m_reconciler.PendingRequestsForPlayer(req.primaryId, req.sessionGeneration);
        for (const auto& pendingReq : pendingList) {
            const auto mmrIt = profile.playlistMMRs.find(pendingReq.playlist);
            if (mmrIt == profile.playlistMMRs.end()) continue;
            const auto matchesIt = profile.playlistMatches.find(pendingReq.playlist);
            const int fetchedMatches = matchesIt != profile.playlistMatches.end() ? matchesIt->second : -1;
            ProcessPostMatchResponse(pendingReq.matchGuid, mmrIt->second, fetchedMatches);
        }
    }

    {
        std::lock_guard<std::mutex> reconciliationLock(m_reconciliationMutex);
        std::unordered_set<std::string> playlistsAwaitingPostMatch;
        for (const auto& pendingReq : m_reconciler.PendingRequestsForPlayer(req.primaryId, ~uint64_t{0})) {
            playlistsAwaitingPostMatch.insert(pendingReq.playlist);
        }
        MmrPublisher::PublishProfileResult(*m_state, req, profile, &playlistsAwaitingPostMatch);
    }
    StoreLocalProfileCache(req, profile.bestMmr, profile.bestTier, profile.playlistMMRs,
                           profile.playlistTiers, profile.playlistMatches, profile.totalWins, postMatchConfirmed);
    return false;
}

CustomApiFetchResult MmrService::FetchProfileFromCustomApi(const MMRRequest& req) {
    CustomApiPlayerTarget target;
    if (!TryParseCustomApiTarget(req.primaryId, target)) return CustomApiFetchResult::UnusableData;
    if (!m_customProvider) return CustomApiFetchResult::DisabledOrNotReady;
    MmrServiceDetail::AssembledBatch batch;
    batch.playlist = req.playlist;
    batch.playlistId = MmrServiceDetail::CustomApiPlaylistIdForName(req.playlist);
    batch.requests.push_back(req);
    auto res = m_customProvider->FetchBatch(batch);
    if (res.httpResult == CustomApiFetchResult::SuccessFinished) {
        for (const auto& pr : res.playerResults) {
            if (pr.request.primaryId == req.primaryId) {
                if (pr.status == BatchPlayerStatus::Success) {
                    return PublishProfileResult(req, pr.profile) ? CustomApiFetchResult::SuccessRequeued
                                                                 : CustomApiFetchResult::SuccessFinished;
                }
                return CustomApiFetchResult::UnusableData;
            }
        }
        return CustomApiFetchResult::UnusableData;
    }
    return res.httpResult;
}

void MmrService::ProcessCustomApiBatch(const MmrServiceDetail::AssembledBatch& batch) {
    if (batch.requests.empty()) return;
    if (!m_customProvider) {
        for (const auto& req : batch.requests) {
            m_queue.Finish(req);
        }
        return;
    }

    auto res = m_customProvider->FetchBatch(batch);
    const auto now = std::chrono::steady_clock::now();
    const auto config = Config::Read();

    if (res.updateRequired) {
        for (const auto& req : batch.requests) {
            if (config.enable_mmr_tracking && !m_queue.IsRateLimited(now)) {
                if (!FetchProfileTracker(req)) FinishRequest(req);
            } else {
                if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = TrackedAccountFetchStatus::ProviderUnavailable;
                    event.detail = "Update OmniStats to keep live ranks.";
                    NotifyTracked(event);
                }
                m_queue.Finish(req);
            }
        }
        return;
    }

    if (res.httpResult == CustomApiFetchResult::AuthFailure) {
        for (const auto& req : batch.requests) {
            if (config.enable_mmr_tracking && !m_queue.IsRateLimited(now)) {
                if (!FetchProfileTracker(req)) FinishRequest(req);
            } else {
                if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = TrackedAccountFetchStatus::AuthRequired;
                    event.detail = "The rank API rejected the request. Sign in again in Settings > Accounts & services.";
                    NotifyTracked(event);
                }
                m_queue.Finish(req);
            }
        }
        return;
    }

    if (res.httpResult == CustomApiFetchResult::TransientError) {
        const auto delay = (res.httpCode == 429 && res.rateLimitLockout > std::chrono::steady_clock::duration::zero())
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(res.rateLimitLockout)
                               : m_timings.transientRetryDelay;
        const char* reason = (res.httpCode == 429) ? "custom API rate limited" : "transient custom API failure";
        if (res.httpCode == 429) {
            m_queue.SetRateLimitedUntil(now + (res.rateLimitLockout > std::chrono::steady_clock::duration::zero() ? res.rateLimitLockout : m_timings.rateLimitLockoutMinimum));
        }
        for (const auto& req : batch.requests) {
            if (!m_queue.ScheduleRetry(req, delay, reason)) {
                if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = (res.httpCode == 429) ? TrackedAccountFetchStatus::RateLimited : TrackedAccountFetchStatus::Failed;
                    event.detail = (res.httpCode == 429) ? "The rank API is rate limiting requests." : "The rank API is temporarily unavailable.";
                    NotifyTracked(event);
                }
                m_queue.Finish(req);
            }
        }
        return;
    }

    if (res.httpResult == CustomApiFetchResult::UnusableData || res.httpResult == CustomApiFetchResult::DisabledOrNotReady) {
        for (const auto& req : batch.requests) {
            if (config.enable_mmr_tracking && !m_queue.IsRateLimited(now)) {
                if (!FetchProfileTracker(req)) FinishRequest(req);
            } else {
                if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = res.httpCode == 404                                          ? TrackedAccountFetchStatus::NotFound
                                   : res.httpResult == CustomApiFetchResult::DisabledOrNotReady ? TrackedAccountFetchStatus::ProviderUnavailable
                                                                                                : TrackedAccountFetchStatus::Failed;
                    NotifyTracked(event);
                }
                m_queue.Finish(req);
            }
        }
        return;
    }

    for (const auto& pRes : res.playerResults) {
        auto reqIt = std::find_if(batch.requests.begin(), batch.requests.end(), [&](const MMRRequest& r) {
            return r.primaryId == pRes.request.primaryId;
        });
        if (reqIt != batch.requests.end()) {
            if (pRes.status == BatchPlayerStatus::Success) {
                PublishProfileResult(*reqIt, pRes.profile);
            } else {
                if (config.enable_mmr_tracking && !m_queue.IsRateLimited(now)) {
                    if (FetchProfileTracker(*reqIt)) continue;
                } else {
                    if (reqIt->reason == MMRRequestReason::Roster && m_state) {
                        MmrPublisher::PublishRosterFetchFailure(*m_state, reqIt->primaryId);
                    }
                    if (reqIt->reason == MMRRequestReason::TrackedAccount) {
                        TrackedAccountFetchStatus status = TrackedAccountFetchStatus::Failed;
                        if (pRes.errorCode == "update_required") {
                            status = TrackedAccountFetchStatus::ProviderUnavailable;
                        } else if (pRes.status == BatchPlayerStatus::NotFound) {
                            status = TrackedAccountFetchStatus::NotFound;
                        } else if (pRes.errorCode == "no_ranked_skills") {
                            status = TrackedAccountFetchStatus::NoRankedData;
                        }
                        const std::string msg = (pRes.errorCode == "update_required" && pRes.errorMessage.empty())
                                                    ? "Update OmniStats to keep live ranks."
                                                    : pRes.errorMessage;
                        TrackedAccountFetchEvent event;
                        event.primaryId = reqIt->primaryId;
                        event.status = status;
                        event.detail = msg;
                        NotifyTracked(event);
                    }
                }
            }
            m_queue.Finish(*reqIt);
        }
    }
    return;
}

bool MmrService::FetchProfile(MMRRequest req) {
    if (req.reason == MMRRequestReason::PostMatch) {
        if (const auto record = m_reconciler.GetRecord(req.matchGuid)) {
            if (record->reconciliationState == PostMatchReconciliationState::Confirmed) {
                return false;
            }
        }
    }

    const ConfigData config = Config::Read();
    const bool signedIn = m_accountClient ? m_accountClient->IsSignedIn(config) : !config.account_refresh_token.empty();
    const bool signedInPrimary = config.custom_api_enabled && signedIn;

    if (signedInPrimary) {
        const auto customResult = FetchProfileFromCustomApi(req);
        if (customResult == CustomApiFetchResult::SuccessFinished) {
            return false;
        }
        if (customResult == CustomApiFetchResult::SuccessRequeued) {
            return true;
        }
        if (customResult == CustomApiFetchResult::AuthFailure) {
            if (req.reason == MMRRequestReason::TrackedAccount && !config.enable_mmr_tracking) {
                TrackedAccountFetchEvent event;
                event.primaryId = req.primaryId;
                event.status = TrackedAccountFetchStatus::AuthRequired;
                event.detail = "The rank API rejected the request. Sign in again in Settings > Accounts & services.";
                NotifyTracked(event);
                return false;
            }
        } else if (!config.enable_mmr_tracking) {
            if (customResult == CustomApiFetchResult::TransientError) {
                if (m_queue.ScheduleRetry(req, m_timings.transientRetryDelay, "transient custom API failure")) {
                    return true;
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = TrackedAccountFetchStatus::Failed;
                    event.detail = "The rank API is temporarily unavailable.";
                    NotifyTracked(event);
                } else if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                return false;
            } else if (customResult == CustomApiFetchResult::UnusableData) {
                const auto now = std::chrono::steady_clock::now();
                if (m_queue.IsRateLimited(now)) {
                    if (req.reason == MMRRequestReason::TrackedAccount) {
                        TrackedAccountFetchEvent event;
                        event.primaryId = req.primaryId;
                        event.status = TrackedAccountFetchStatus::Failed;
                        event.detail = "The rank API returned no usable data.";
                        NotifyTracked(event);
                    } else if (req.reason == MMRRequestReason::Roster && m_state) {
                        MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                    }
                    return false;
                }
            }
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (m_queue.IsRateLimited(now)) {
                if (customResult == CustomApiFetchResult::TransientError &&
                    m_queue.ScheduleRetry(req, m_timings.transientRetryDelay, "transient custom API failure")) {
                    return true;
                }
                if (req.reason == MMRRequestReason::TrackedAccount) {
                    TrackedAccountFetchEvent event;
                    event.primaryId = req.primaryId;
                    event.status = TrackedAccountFetchStatus::RateLimited;
                    event.detail = "Rank lookups are paused by a rate limit.";
                    NotifyTracked(event);
                } else if (req.reason == MMRRequestReason::Roster && m_state) {
                    MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
                }
                return false;
            }
        }
    }
    return FetchProfileTracker(std::move(req));
}

bool MmrService::FetchProfileTracker(MMRRequest req) {
    const ConfigData config = Config::Read();

    if (!config.enable_mmr_tracking) {
        if (req.reason == MMRRequestReason::PostMatch) {
            ProcessPostMatchResponse(req.matchGuid, 0, -1);
        } else if (req.reason == MMRRequestReason::TrackedAccount) {
            TrackedAccountFetchEvent event;
            event.primaryId = req.primaryId;
            event.status = TrackedAccountFetchStatus::ProviderUnavailable;
            event.detail = "Rank lookups are disabled.";
            NotifyTracked(event);
        } else if (req.reason == MMRRequestReason::Roster && m_state) {
            MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
        }
        return false;
    }

    NormalizedProfileResult profile;
    std::string detail;
    bool ok = false;
    if (m_trackerProvider) {
        ok = m_trackerProvider->FetchProfile(req, profile, detail);
    } else {
        detail = "The rank lookup component is not available.";
    }
    const auto trn = std::dynamic_pointer_cast<TrnRankProvider>(m_trackerProvider);
    const auto result = trn ? trn->LastResult() : TrnRankProvider::Result{};
    if ((ok || (trn && result.transportError == 0 && result.statusCode == 200)) &&
        m_queue.ResetForbidden()) {
        std::cout << "[MmrService] Tracker.gg requests recovered; circuit breaker reset.\n";
    }

    if (ok) return PublishProfileResult(req, profile);

    if (trn && result.failure == TrnRankProvider::Failure::Http) {
        if (result.statusCode == 403) {
            const auto breaker = m_queue.RequeueForbidden(req);
            if (!breaker) return false;
            std::cout << "[MmrService] Tracker.gg blocked requests (HTTP 403). Circuit breaker strike "
                      << breaker->first << "; pausing all Tracker requests for "
                      << std::chrono::duration_cast<std::chrono::seconds>(breaker->second).count()
                      << " seconds.\n";
            m_cv.notify_one();
            return true;
        }
        if (result.statusCode == 429) {
            const auto retryDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(result.retryAfterSeconds));
            const auto lockout = (std::max)(retryDuration, m_timings.rateLimitLockoutMinimum);
            m_queue.SetRateLimitedUntil(std::chrono::steady_clock::now() + lockout);
            std::cout << "[MmrService] Rate limited by Tracker.gg (HTTP 429). Pausing requests for "
                      << std::chrono::duration_cast<std::chrono::seconds>(lockout).count() << " seconds.\n";
            if (m_queue.ScheduleRetry(req, lockout, "rate limited")) return true;
        } else if (result.transportError != 0 || result.statusCode == 408 ||
                   (result.statusCode >= 500 && result.statusCode <= 599)) {
            if (m_queue.ScheduleRetry(req, m_timings.transientRetryDelay, "transient Tracker failure")) return true;
        }
        detail = "Tracker returned HTTP " + std::to_string(result.statusCode) + ".";
    } else if (trn && result.failure == TrnRankProvider::Failure::Incomplete) {
        if (m_queue.ScheduleRetry(req, m_timings.transientRetryDelay, "incomplete Tracker response")) return true;
        return false;
    } else if (trn && result.failure == TrnRankProvider::Failure::Invalid) {
        if (m_queue.ScheduleRetry(req, m_timings.transientRetryDelay, "invalid Tracker response")) return true;
        detail = "Tracker returned an invalid response.";
    }

    {
        if (req.reason == MMRRequestReason::PostMatch) {
            ProcessPostMatchResponse(req.matchGuid, 0, -1);
        } else if (req.reason == MMRRequestReason::TrackedAccount) {
            TrackedAccountFetchEvent event;
            event.primaryId = req.primaryId;
            if (trn && result.failure == TrnRankProvider::Failure::Unsupported) {
                event.status = TrackedAccountFetchStatus::Unsupported;
            } else if (!m_trackerProvider || (trn && result.failure == TrnRankProvider::Failure::Unavailable)) {
                event.status = TrackedAccountFetchStatus::ProviderUnavailable;
            } else if (trn && result.failure == TrnRankProvider::Failure::Http && result.statusCode == 404) {
                event.status = TrackedAccountFetchStatus::NotFound;
            } else if (trn && result.failure == TrnRankProvider::Failure::Http && result.statusCode == 429) {
                event.status = TrackedAccountFetchStatus::RateLimited;
            } else {
                event.status = TrackedAccountFetchStatus::Failed;
            }
            event.detail = detail;
            NotifyTracked(event);
        } else if (req.reason == MMRRequestReason::Roster && m_state) {
            MmrPublisher::PublishRosterFetchFailure(*m_state, req.primaryId);
        }
        return false;
    }
}

void MmrService::FetchRosterProfile(const std::string& primaryId, const std::string& name) {
    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.reason = MMRRequestReason::Roster;
    req.sessionGeneration = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot()->sessionGeneration : 0;
    (void)FetchProfile(std::move(req));
}

void MmrService::FetchTrackedAccount(const std::string& primaryId, const std::string& name) {
    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.reason = MMRRequestReason::TrackedAccount;
    req.sessionGeneration = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot()->sessionGeneration : 0;
    (void)FetchProfile(std::move(req));
}

bool MmrService::IsCustomApiActiveSource() const {
    const RankProvider p = CurrentRankProvider();
    return p == RankProvider::OmniStatsApi || p == RankProvider::OmniStatsApiWithTrackerFallback;
}

bool MmrService::IsRateLimited() const {
    return m_queue.IsRateLimited(std::chrono::steady_clock::now());
}

bool MmrService::HasPendingDestroyedMatch(const std::string& matchGuid) const {
    return m_reconciler.HasPendingDestroyedMatch(matchGuid);
}

std::vector<SessionMmrPoint> MmrService::PlaylistMatchPoints(const std::string& playlist) const {
    if (!m_state) return {};
    const auto snapshot = m_state->history.GetSnapshot();
    const auto it = snapshot->playlistMatchPoints.find(playlist);
    return it == snapshot->playlistMatchPoints.end() ? std::vector<SessionMmrPoint>{} : it->second;
}

MmrServiceDetail::AssembledBatch MmrService::TakeNextCustomApiBatch() {
    const auto now = std::chrono::steady_clock::now();
    const auto reason = m_queue.NextBatchReason(now);
    if (!reason) return {};
    const uint64_t sessionGen = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot()->sessionGeneration : 0;
    return m_queue.DrainBatchForReason(*reason, now, sessionGen, 8);
}

std::optional<MMRRequest> MmrService::TakeNextTrackerRequest() {
    return m_queue.PopNext(std::chrono::steady_clock::now());
}

void MmrService::FinishRequest(const MMRRequest& req) {
    m_queue.Finish(req);
    if (req.reason == MMRRequestReason::PostMatch) {
        ProcessPostMatchResponse(req.matchGuid, 0, -1);
    }
}

void MmrService::WorkerLoop() {
    while (m_isRunning) {
        const RankProvider provider = CurrentRankProvider();
        if (provider == RankProvider::Unavailable) {
            std::unique_lock<std::mutex> lock(m_cvMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return !m_isRunning || CurrentRankProvider() != RankProvider::Unavailable;
            });
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (m_queue.HasReady(MMRRequestReason::PostMatch, now)) {
            DispatchSingle(provider);
            continue;
        }
        if (m_queue.IsRateLimited(now)) {
            std::unique_lock<std::mutex> lock(m_cvMutex);
            m_cv.wait_until(lock, now + std::chrono::milliseconds(500));
            continue;
        }

        if (provider == RankProvider::OmniStatsApi || provider == RankProvider::OmniStatsApiWithTrackerFallback) {
            DispatchBatch(provider);
        } else {
            DispatchSingle(provider);
        }
    }
}

void MmrService::DispatchBatch(RankProvider provider) {
    const auto now = std::chrono::steady_clock::now();
    const auto reason = m_queue.NextBatchReason(now);
    if (!reason) {
        DispatchSingle(provider);
        return;
    }
    const auto playlist = m_queue.FirstReadyPlaylist(*reason, now);
    if (!playlist) return;

    const auto higherPriorityReady = [&] {
        const auto current = std::chrono::steady_clock::now();
        return m_queue.HasReady(MMRRequestReason::PostMatch, current) ||
               (*reason == MMRRequestReason::TrackedAccount &&
                m_queue.HasReady(MMRRequestReason::Roster, current));
    };
    const auto deadline = now + m_timings.batchCoalesceWindow;
    while (m_isRunning && std::chrono::steady_clock::now() < deadline &&
           m_queue.ReadyCount(*reason, *playlist, std::chrono::steady_clock::now()) < kMaxCustomApiBatchSize) {
        if (higherPriorityReady()) return;
        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_until(lock, (std::min)(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(10)));
    }
    if (!m_isRunning || higherPriorityReady()) return;
    const uint64_t sessionGen = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot()->sessionGeneration : 0;
    auto batch = m_queue.DrainBatch(*reason, *playlist, std::chrono::steady_clock::now(), sessionGen, kMaxCustomApiBatchSize);
    for (const auto& req : batch.requests) {
        if (req.reason != MMRRequestReason::TrackedAccount) continue;
        TrackedAccountFetchEvent event;
        event.primaryId = req.primaryId;
        event.status = TrackedAccountFetchStatus::Fetching;
        NotifyTracked(event);
    }
    if (!batch.requests.empty()) ProcessCustomApiBatch(batch);
}

void MmrService::DispatchSingle(RankProvider /*provider*/) {
    const auto now = std::chrono::steady_clock::now();
    auto nextReq = m_queue.PopNextPostMatch(now);
    if (!nextReq) {
        nextReq = m_queue.PopNext(now);
    }
    if (!nextReq) {
        std::unique_lock<std::mutex> lock(m_cvMutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(50));
        return;
    }

    MMRRequest req = std::move(*nextReq);
    req.sessionGeneration = m_state && m_state->liveMatch ? m_state->liveMatch->GetSnapshot()->sessionGeneration : 0;
    if (req.reason == MMRRequestReason::TrackedAccount) {
        TrackedAccountFetchEvent event;
        event.primaryId = req.primaryId;
        event.status = TrackedAccountFetchStatus::Fetching;
        NotifyTracked(event);
    }
    const bool requeued = FetchProfile(req);
    if (!requeued) {
        m_queue.Finish(req);
    }
    std::unique_lock<std::mutex> lock(m_cvMutex);
    m_cv.wait_for(lock, m_timings.queueSpacing, [this] { return !m_isRunning; });
}

std::string MmrService::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";
    return LookupTierFromThresholds(TournamentRankThresholds, mmr);
}

std::span<const RankProgress::Threshold> MmrService::RankThresholdsForPlaylist(std::string_view playlist) {
    if (playlist == "1v1") return SoloDuelRankThresholds;
    if (playlist == "2v2") return DoublesRankThresholds;
    if (playlist == "3v3") return StandardRankThresholds;
    if (playlist == "hoops") return HoopsRankThresholds;
    if (playlist == "rumble") return RumbleRankThresholds;
    if (playlist == "dropshot") return DropshotRankThresholds;
    if (playlist == "snowday") return SnowdayRankThresholds;
    if (playlist == "heatseeker") return HeatseekerRankThresholds;
    if (playlist == "t") return TournamentRankThresholds;
    return {};
}

std::string MmrService::GetRankTierForPlaylistMmr(const std::string& playlist, int mmr) {
    if (mmr <= 0 || playlist == "casual") return "Unranked";
    const auto table = RankThresholdsForPlaylist(playlist);
    return table.empty() ? GetTournamentTierForMmr(mmr) : LookupTierFromThresholds(table, mmr);
}

std::string MmrService::PlaylistNameForTrackerId(int playlistId) {
    return PlaylistMetadata::TrackerKey(playlistId);
}

std::string MmrService::RankTierName(int tier, int division) {
    static constexpr std::array<const char*, 23> kTiers = {
        "Unranked", "Bronze I", "Bronze II", "Bronze III",
        "Silver I", "Silver II", "Silver III",
        "Gold I", "Gold II", "Gold III",
        "Platinum I", "Platinum II", "Platinum III",
        "Diamond I", "Diamond II", "Diamond III",
        "Champion I", "Champion II", "Champion III",
        "Grand Champion I", "Grand Champion II", "Grand Champion III",
        "Supersonic Legend"};
    if (tier < 0 || static_cast<size_t>(tier) >= kTiers.size()) return "Unranked";
    std::string result = kTiers[static_cast<size_t>(tier)];
    if (tier > 0 && tier < 22 && division >= 0 && division < 4) {
        static constexpr std::array<const char*, 4> kDivisions = {"I", "II", "III", "IV"};
        result += " Div ";
        result += kDivisions[static_cast<size_t>(division)];
    }
    return result;
}

MMRProfileTotals MmrService::ExtractProfileTotals(const nlohmann::json& jsonResp) {
    MMRProfileTotals totals;
    if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
        !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
        return totals;
    }
    for (const auto& seg : jsonResp["data"]["segments"]) {
        if (!seg.is_object()) continue;
        if (seg.contains("type") && seg["type"].is_string() && seg["type"] == "playlist") continue;
        if (!seg.contains("stats") || !seg["stats"].is_object()) continue;

        const auto& stats = seg["stats"];
        if (totals.totalWins < 0) {
            (void)TryReadStatValue(stats, {"wins", "Wins"}, totals.totalWins);
        }
    }
    return totals;
}

bool MmrService::IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches, int fetchedMatches) {
    return MmrReconciler::IsPostMatchMmrStale(previousMmr, fetchedMmr, previousMatches, fetchedMatches);
}

size_t MmrService::CoveredPendingMatchCount(int oldestPreviousMatches, int fetchedMatches, size_t pendingCount) {
    return MmrReconciler::CoveredPendingMatchCount(oldestPreviousMatches, fetchedMatches, pendingCount);
}

int MmrService::ResolvePostMatchBaseline(int requestedPreviousMmr, bool previousMmrIsPlaylistSpecific, const std::vector<float>& recentHistory) {
    return MmrReconciler::ResolvePostMatchBaseline(requestedPreviousMmr, previousMmrIsPlaylistSpecific, recentHistory);
}

int MmrService::EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory) {
    return MmrReconciler::EstimatePostMatchMmr(previousMmr, won, recentHistory);
}
