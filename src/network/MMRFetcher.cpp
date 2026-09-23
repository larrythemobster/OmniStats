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

MMRFetcher::MMRFetcher(std::shared_ptr<SessionState> state,
                       std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)) {}

MMRFetcher::~MMRFetcher() {
    Stop();
}

#ifdef OMNISTATS_TEST_ENVIRONMENT
size_t MMRFetcher::PendingRequestCountForTests() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}

void MMRFetcher::ProcessPostMatchResponseForTests(const std::string& matchGuid,
                                                  int fetchedMmr,
                                                  int fetchedMatches) {
    MMRRequest req;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto it = m_postMatchRecordsByGuid.find(matchGuid);
        if (it == m_postMatchRecordsByGuid.end()) return;
        req.primaryId = it->second.primaryId;
        req.matchGuid = it->second.matchGuid;
        req.playlist = it->second.playlist;
        req.previousMmr = it->second.preMatchMmr;
        req.previousMatches = it->second.preMatchMatchesPlayed;
        req.previousMmrIsPlaylistSpecific = it->second.preMatchMmrIsPlaylistSpecific;
        req.won = it->second.won;
        req.resultKnown = it->second.resultKnown;
    }
    if (!ReconcileTrackerResponse(req, fetchedMmr, fetchedMatches)) {
        EnsureProvisionalPoint(req, req.previousMmr, fetchedMmr, fetchedMatches);
    }
}

void MMRFetcher::FetchRosterProfileForTests(const std::string& primaryId,
                                            const std::string& name) {
    MMRRequest req;
    req.primaryId = primaryId;
    req.name = name;
    req.reason = MMRRequestReason::Roster;
    (void)FetchProfile(std::move(req));
}

std::vector<SessionMmrPoint> MMRFetcher::PlaylistMatchPointsForTests(const std::string& playlist) {
    std::shared_lock<std::shared_mutex> lock(m_state->history.mutex);
    const auto it = m_state->history.playlistMatchPoints.find(playlist);
    return it == m_state->history.playlistMatchPoints.end() ? std::vector<SessionMmrPoint>{} : it->second;
}

bool MMRFetcher::HasPendingDestroyedMatchForTests(
    const std::string& matchGuid) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    const auto recordIt = m_postMatchRecordsByGuid.find(matchGuid);
    return recordIt != m_postMatchRecordsByGuid.end() &&
           recordIt->second.destroyedMatch &&
           !recordIt->second.databaseMatchFinalized;
}

bool MMRFetcher::IsRateLimitedForTests() const {
    return m_rateLimitedUntil > std::chrono::steady_clock::now();
}

CustomApiFetchResult MMRFetcher::FetchProfileFromCustomApiForTests(const MMRRequest& req) {
    return FetchProfileFromCustomApi(req);
}

bool MMRFetcher::PublishProfileResultForTests(const MMRRequest& req, const NormalizedProfileResult& profile) {
    return PublishProfileResult(req, profile);
}

#endif
