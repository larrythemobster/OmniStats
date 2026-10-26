#include "network/mmr/MmrReconciler.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>

bool MmrReconciler::IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches, int fetchedMatches) {
    if (fetchedMmr <= 0) return true;

    if (previousMatches >= 0 && fetchedMatches >= 0) {
        return fetchedMatches <= previousMatches;
    }

    if (previousMmr <= 0) return true;
    return fetchedMmr == previousMmr;
}

size_t MmrReconciler::CoveredPendingMatchCount(int oldestPreviousMatches,
                                               int fetchedMatches,
                                               size_t pendingCount) {
    if (oldestPreviousMatches < 0 || fetchedMatches <= oldestPreviousMatches || pendingCount == 0) return 0;
    const size_t matchDelta = static_cast<size_t>(fetchedMatches - oldestPreviousMatches);
    return (std::min)(matchDelta, pendingCount);
}

int MmrReconciler::ResolvePostMatchBaseline(int requestedPreviousMmr,
                                            bool previousMmrIsPlaylistSpecific,
                                            const std::vector<float>& recentHistory) {
    if (previousMmrIsPlaylistSpecific && requestedPreviousMmr > 0) {
        return requestedPreviousMmr;
    }
    if (!recentHistory.empty()) {
        const int latestHistoryMmr = static_cast<int>(std::lround(recentHistory.back()));
        if (latestHistoryMmr > 0) return latestHistoryMmr;
    }
    return requestedPreviousMmr;
}

int MmrReconciler::EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory) {
    const int baselineMmr = ResolvePostMatchBaseline(previousMmr, false, recentHistory);
    if (baselineMmr <= 0) return 0;
    std::vector<int> deltas;
    constexpr size_t kMaximumTransitions = 12;
    const size_t firstIndex = recentHistory.size() > kMaximumTransitions + 1
                                  ? recentHistory.size() - kMaximumTransitions
                                  : 1;
    for (size_t i = firstIndex; i < recentHistory.size(); ++i) {
        const int current = static_cast<int>(std::lround(recentHistory[i]));
        const int previous = static_cast<int>(std::lround(recentHistory[i - 1]));
        const int delta = std::abs(current - previous);
        if (delta >= 3 && delta <= 15) deltas.push_back(delta);
    }

    int estimatedDelta = 9;
    if (!deltas.empty()) {
        std::sort(deltas.begin(), deltas.end());
        const size_t middle = deltas.size() / 2;
        estimatedDelta = deltas.size() % 2 == 0
                             ? (deltas[middle - 1] + deltas[middle]) / 2
                             : deltas[middle];
    }

    return (std::max)(1, baselineMmr + (won ? estimatedDelta : -estimatedDelta));
}

void MmrReconciler::EnqueuePostMatch(const std::string& primaryId,
                                     const std::string& matchGuid,
                                     const std::string& playlist,
                                     int previousMmr,
                                     int previousMatches,
                                     bool previousMmrIsPlaylistSpecific,
                                     bool won,
                                     uint64_t sessionGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_postMatchRecordsByGuid.count(matchGuid) || m_completedPostMatchGuids.count(matchGuid)) return;
    ResetPublicationBaselineForCounterRollback(primaryId, playlist, previousMatches);
    auto& pendingGuids = m_pendingPostMatchesByPlaylist[PendingPostMatchKey(primaryId, playlist)];
    const bool followsPreviousSessionMatch = std::any_of(
        pendingGuids.begin(), pendingGuids.end(), [&](const std::string& guid) {
            const auto it = m_postMatchRecordsByGuid.find(guid);
            return it != m_postMatchRecordsByGuid.end() && it->second.sessionGeneration != sessionGeneration;
        });

    m_postMatchRecordsByGuid.emplace(
        matchGuid,
        PendingPostMatchRecord{
            .matchGuid = matchGuid,
            .primaryId = primaryId,
            .playlist = playlist,
            .preMatchMmr = previousMmr,
            .preMatchMatchesPlayed = previousMatches,
            .preMatchMmrIsPlaylistSpecific = previousMmrIsPlaylistSpecific,
            .won = won,
            .resultKnown = true,
            .sessionGeneration = sessionGeneration,
            .followsPreviousSessionMatch = followsPreviousSessionMatch});

    pendingGuids.push_back(matchGuid);
}

void MmrReconciler::EnqueuePendingDestroyedMatch(const PendingDestroyedMatchMmrRefresh& pending,
                                                 uint64_t sessionGeneration) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_postMatchRecordsByGuid.count(pending.matchGuid) ||
        m_completedPostMatchGuids.count(pending.matchGuid)) return;
    ResetPublicationBaselineForCounterRollback(pending.primaryId, pending.playlist, pending.previousMatches);
    auto& pendingGuids = m_pendingPostMatchesByPlaylist[PendingPostMatchKey(pending.primaryId, pending.playlist)];
    const bool followsPreviousSessionMatch = std::any_of(
        pendingGuids.begin(), pendingGuids.end(), [&](const std::string& guid) {
            const auto it = m_postMatchRecordsByGuid.find(guid);
            return it != m_postMatchRecordsByGuid.end() && it->second.sessionGeneration != sessionGeneration;
        });

    m_postMatchRecordsByGuid.emplace(
        pending.matchGuid,
        PendingPostMatchRecord{
            .matchGuid = pending.matchGuid,
            .primaryId = pending.primaryId,
            .playlist = pending.playlist,
            .preMatchMmr = pending.previousMmr,
            .preMatchMatchesPlayed = pending.previousMatches,
            .preMatchMmrIsPlaylistSpecific = pending.previousMmrIsPlaylistSpecific,
            .won = false,
            .resultKnown = false,
            .destroyedMatch = true,
            .localPlayerDisappeared = pending.localPlayerDisappeared,
            .explicitLocalForfeit = pending.explicitLocalForfeit,
            .localTeam = pending.localTeam,
            .score = pending.score,
            .destroyedAtUnixMs = pending.destroyedAtUnixMs,
            .validCompetitiveMatch = pending.validCompetitiveMatch,
            .databaseMatchFinalized = false,
            .sessionGeneration = sessionGeneration,
            .followsPreviousSessionMatch = followsPreviousSessionMatch});

    pendingGuids.push_back(pending.matchGuid);
}

void MmrReconciler::ResolvePendingDestroyedMatch(const std::string& matchGuid, bool won) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_postMatchRecordsByGuid.find(matchGuid);
    if (it != m_postMatchRecordsByGuid.end() && it->second.destroyedMatch) {
        it->second.won = won;
        it->second.resultKnown = true;
        it->second.databaseMatchFinalized = true;
    }
}

bool MmrReconciler::HasPendingDestroyedMatch(const std::string& matchGuid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_postMatchRecordsByGuid.find(matchGuid);
    return it != m_postMatchRecordsByGuid.end() && it->second.destroyedMatch &&
           !it->second.databaseMatchFinalized;
}

size_t MmrReconciler::PendingPlaylistCount(const std::string& primaryId, const std::string& playlist) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_pendingPostMatchesByPlaylist.find(PendingPostMatchKey(primaryId, playlist));
    return it == m_pendingPostMatchesByPlaylist.end() ? 0 : it->second.size();
}

std::optional<PendingPostMatchRecord> MmrReconciler::GetRecord(const std::string& matchGuid) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_postMatchRecordsByGuid.find(matchGuid);
    if (it == m_postMatchRecordsByGuid.end()) return std::nullopt;
    return it->second;
}

std::vector<MMRRequest> MmrReconciler::PendingRequestsForPlayer(const std::string& primaryId, uint64_t sessionGeneration) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<MMRRequest> pendingPlaylists;
    for (const auto& [pendingKey, guids] : m_pendingPostMatchesByPlaylist) {
        if (guids.empty()) continue;
        const auto recordIt = m_postMatchRecordsByGuid.find(guids.front());
        if (recordIt == m_postMatchRecordsByGuid.end()) continue;
        const auto& record = recordIt->second;
        if (record.primaryId != primaryId || record.sessionGeneration > sessionGeneration) {
            continue;
        }
        MMRRequest pendingReq;
        pendingReq.primaryId = record.primaryId;
        pendingReq.matchGuid = record.matchGuid;
        pendingReq.playlist = record.playlist;
        pendingReq.previousMmr = record.preMatchMmr;
        pendingReq.previousMatches = record.preMatchMatchesPlayed;
        pendingReq.previousMmrIsPlaylistSpecific = record.preMatchMmrIsPlaylistSpecific;
        pendingReq.won = record.won;
        pendingReq.resultKnown = record.resultKnown;
        pendingReq.sessionGeneration = sessionGeneration;
        pendingPlaylists.push_back(std::move(pendingReq));
    }
    return pendingPlaylists;
}

std::vector<std::string> MmrReconciler::PendingGuidsForPlayer(const std::string& primaryId) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> guids;
    for (const auto& [guid, record] : m_postMatchRecordsByGuid) {
        if (record.primaryId == primaryId) guids.push_back(guid);
    }
    return guids;
}

void MmrReconciler::AdoptCachedPreMatchBaseline(const std::string& matchGuid,
                                                int cachedMmr,
                                                int cachedMatches,
                                                int fetchedMatches) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (cachedMmr <= 0 || cachedMatches < 0 || fetchedMatches < 0) return;
    const auto requestIt = m_postMatchRecordsByGuid.find(matchGuid);
    if (requestIt == m_postMatchRecordsByGuid.end()) return;
    const auto pendingIt = m_pendingPostMatchesByPlaylist.find(
        PendingPostMatchKey(requestIt->second.primaryId, requestIt->second.playlist));
    if (pendingIt == m_pendingPostMatchesByPlaylist.end() || pendingIt->second.empty()) return;
    const auto recordIt = m_postMatchRecordsByGuid.find(pendingIt->second.front());
    if (recordIt == m_postMatchRecordsByGuid.end()) return;
    auto& record = recordIt->second;
    if (record.preMatchMatchesPlayed >= 0 || record.firstObservedMatchesPlayed >= 0 ||
        (record.preMatchMmrIsPlaylistSpecific && record.preMatchMmr > 0)) return;
    const int advanced = fetchedMatches - cachedMatches;
    if (advanced < 0 || static_cast<size_t>(advanced) > pendingIt->second.size()) return;
    record.preMatchMmr = cachedMmr;
    record.preMatchMatchesPlayed = cachedMatches;
    record.preMatchMmrIsPlaylistSpecific = true;
}

void MmrReconciler::ResetPublicationBaselineForCounterRollback(const std::string& primaryId,
                                                               const std::string& playlist,
                                                               int previousMatches) {
    if (playlist.empty() || previousMatches < 0) return;
    const std::string key = PendingPostMatchKey(primaryId, playlist);
    const auto pendingIt = m_pendingPostMatchesByPlaylist.find(key);
    if (pendingIt != m_pendingPostMatchesByPlaylist.end() && !pendingIt->second.empty()) return;
    const auto baselineIt = m_trackerPublicationBaselineByPlaylist.find(key);
    if (baselineIt != m_trackerPublicationBaselineByPlaylist.end() &&
        previousMatches < baselineIt->second - 1) {
        baselineIt->second = previousMatches;
    }
}

MmrReconciler::ReconcileResult MmrReconciler::ReconcileTrackerResponse(const MMRRequest& req,
                                                                       int fetchedMmr,
                                                                       int fetchedMatches,
                                                                       const std::vector<float>& historyY,
                                                                       uint64_t currentSessionGeneration,
                                                                       const std::string& myPrimaryId,
                                                                       bool inMatch,
                                                                       bool matchFinalized,
                                                                       const std::vector<SessionMmrPoint>* points,
                                                                       int sessionInitialMmr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ReconcileResult result;
    if (fetchedMmr <= 0 || req.playlist.empty()) return result;

    const std::string pendingKey = PendingPostMatchKey(req.primaryId, req.playlist);
    auto pendingIt = m_pendingPostMatchesByPlaylist.find(pendingKey);
    if (pendingIt == m_pendingPostMatchesByPlaylist.end() || pendingIt->second.empty()) {
        result.requestConfirmed = m_completedPostMatchGuids.count(req.matchGuid) > 0;
        return result;
    }

    auto& pendingGuids = pendingIt->second;
    const auto oldestRecordIt = m_postMatchRecordsByGuid.find(pendingGuids.front());
    if (oldestRecordIt == m_postMatchRecordsByGuid.end()) return result;
    auto& oldestRecord = oldestRecordIt->second;

    int publicationBaseline = -1;
    if (oldestRecord.preMatchMatchesPlayed >= 0) {
        publicationBaseline = oldestRecord.preMatchMatchesPlayed;
    }
    if (oldestRecord.firstObservedMatchesPlayed >= 0) {
        publicationBaseline = (std::max)(publicationBaseline, oldestRecord.firstObservedMatchesPlayed);
    }
    const auto baselineIt = m_trackerPublicationBaselineByPlaylist.find(pendingKey);
    if (baselineIt != m_trackerPublicationBaselineByPlaylist.end()) {
        publicationBaseline = (std::max)(publicationBaseline, baselineIt->second);
    }

    const bool hasPublicationBaseline = publicationBaseline >= 0;
    const bool matchCountsAvailable = hasPublicationBaseline && fetchedMatches >= 0;
    size_t coveredCount = matchCountsAvailable
                              ? CoveredPendingMatchCount(publicationBaseline, fetchedMatches, pendingGuids.size())
                              : 0;

    const int ratingBaseline = oldestRecord.firstObservedMmr > 0
                                   ? oldestRecord.firstObservedMmr
                                   : (oldestRecord.preMatchMmrIsPlaylistSpecific && oldestRecord.preMatchMmr > 0
                                          ? oldestRecord.preMatchMmr
                                          : 0);

    if (!matchCountsAvailable && !IsPostMatchMmrStale(ratingBaseline, fetchedMmr, -1, -1)) {
        coveredCount = 1;
    }

    if (coveredCount == 0) {
        if (fetchedMmr > 0) oldestRecord.firstObservedMmr = fetchedMmr;
        if (fetchedMatches >= 0) {
            oldestRecord.firstObservedMatchesPlayed = (std::max)(oldestRecord.firstObservedMatchesPlayed, fetchedMatches);
            auto [it, inserted] = m_trackerPublicationBaselineByPlaylist.emplace(pendingKey, fetchedMatches);
            if (!inserted) it->second = (std::max)(it->second, fetchedMatches);
        }
        return result;
    }

    if (matchCountsAvailable && coveredCount < pendingGuids.size()) {
        const auto lastCoveredIt = m_postMatchRecordsByGuid.find(pendingGuids[coveredCount - 1]);
        const auto firstUncoveredIt = m_postMatchRecordsByGuid.find(pendingGuids[coveredCount]);
        if (lastCoveredIt != m_postMatchRecordsByGuid.end() &&
            firstUncoveredIt != m_postMatchRecordsByGuid.end() &&
            lastCoveredIt->second.preMatchMatchesPlayed >= 0 &&
            lastCoveredIt->second.preMatchMatchesPlayed == firstUncoveredIt->second.preMatchMatchesPlayed) {
            return result;
        }
    }

    std::vector<size_t> unknownResultIndexes;
    for (size_t index = 0; index < coveredCount; ++index) {
        auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[index]);
        if (recordIt == m_postMatchRecordsByGuid.end()) continue;
        auto& record = recordIt->second;
        if (!record.destroyedMatch || record.resultKnown) continue;
        if (record.explicitLocalForfeit) {
            record.won = false;
            record.resultKnown = true;
        } else {
            unknownResultIndexes.push_back(index);
        }
    }

    if (!unknownResultIndexes.empty() &&
        m_destroyedMatchConfirmationCallback &&
        oldestRecord.preMatchMmrIsPlaylistSpecific &&
        oldestRecord.preMatchMmr > 0 &&
        fetchedMmr > 0 &&
        unknownResultIndexes.size() < 16) {
        std::vector<int> unanimousResults(unknownResultIndexes.size(), -1);
        size_t feasibleAssignments = 0;
        const size_t assignmentCount = size_t{1} << unknownResultIndexes.size();
        for (size_t assignment = 0; assignment < assignmentCount; ++assignment) {
            std::vector<bool> candidateResults;
            candidateResults.reserve(coveredCount);
            size_t unknownOffset = 0;
            for (size_t index = 0; index < coveredCount; ++index) {
                const auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[index]);
                if (recordIt == m_postMatchRecordsByGuid.end()) {
                    candidateResults.push_back(false);
                    continue;
                }
                if (recordIt->second.resultKnown) {
                    candidateResults.push_back(recordIt->second.won);
                } else {
                    candidateResults.push_back((assignment & (size_t{1} << unknownOffset)) != 0);
                    ++unknownOffset;
                }
            }
            if (MmrServiceDetail::BuildDirectionalMmrPath(oldestRecord.preMatchMmr, fetchedMmr, candidateResults).size() != coveredCount) {
                continue;
            }

            ++feasibleAssignments;
            for (size_t offset = 0; offset < unknownResultIndexes.size(); ++offset) {
                const int candidate = (assignment & (size_t{1} << offset)) != 0 ? 1 : 0;
                if (unanimousResults[offset] == -1) {
                    unanimousResults[offset] = candidate;
                } else if (unanimousResults[offset] != candidate) {
                    unanimousResults[offset] = 2;
                }
            }
        }

        if (feasibleAssignments > 0) {
            for (size_t offset = 0; offset < unknownResultIndexes.size(); ++offset) {
                if (unanimousResults[offset] != 0 && unanimousResults[offset] != 1) {
                    continue;
                }
                auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[unknownResultIndexes[offset]]);
                if (recordIt != m_postMatchRecordsByGuid.end()) {
                    recordIt->second.won = (unanimousResults[offset] == 1);
                    recordIt->second.resultKnown = true;
                }
            }
        }
    }

    for (const size_t index : unknownResultIndexes) {
        auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[index]);
        if (recordIt == m_postMatchRecordsByGuid.end() || recordIt->second.resultKnown ||
            !m_destroyedMatchConfirmationCallback) {
            continue;
        }

        auto& record = recordIt->second;
        if (record.localPlayerDisappeared) {
            record.won = false;
            record.resultKnown = true;
            continue;
        }
        if ((record.localTeam != 0 && record.localTeam != 1) || record.score[0] == record.score[1]) {
            continue;
        }

        record.won = record.score[record.localTeam] > record.score[1 - record.localTeam];
        record.resultKnown = true;
    }

    const bool allCoveredResultsKnown = std::all_of(
        pendingGuids.begin(), pendingGuids.begin() + static_cast<std::ptrdiff_t>(coveredCount),
        [&](const std::string& guid) {
            const auto recordIt = m_postMatchRecordsByGuid.find(guid);
            return recordIt != m_postMatchRecordsByGuid.end() && recordIt->second.resultKnown;
        });
    if (!allCoveredResultsKnown) {
        return result;
    }

    int pathBaselineMmr = oldestRecord.preMatchMmrIsPlaylistSpecific ? oldestRecord.preMatchMmr : 0;
    if (oldestRecord.sessionGeneration == currentSessionGeneration) {
        size_t earliestPendingHistoryIndex = historyY.size();
        if (points) {
            for (const auto& point : *points) {
                if (std::find(pendingGuids.begin(), pendingGuids.end(), point.matchGuid) != pendingGuids.end()) {
                    earliestPendingHistoryIndex = (std::min)(earliestPendingHistoryIndex, point.historyIndex);
                }
            }
        }
        if (earliestPendingHistoryIndex > 0 && earliestPendingHistoryIndex <= historyY.size()) {
            pathBaselineMmr = static_cast<int>(std::lround(historyY[earliestPendingHistoryIndex - 1]));
        }
        if (pathBaselineMmr <= 0) pathBaselineMmr = sessionInitialMmr;
    }

    std::vector<int> estimatedPath;
    estimatedPath.reserve(coveredCount);
    std::vector<bool> coveredResults;
    coveredResults.reserve(coveredCount);
    int lastEstimatedMmr = pathBaselineMmr;

    for (size_t index = 0; index < coveredCount; ++index) {
        const std::string& guid = pendingGuids[index];
        const auto recordIt = m_postMatchRecordsByGuid.find(guid);
        bool won = recordIt != m_postMatchRecordsByGuid.end() ? recordIt->second.won : (index == 0 ? req.won : true);
        coveredResults.push_back(won);

        const SessionMmrPoint* point = nullptr;
        if (points) {
            const auto pointIt = std::find_if(points->begin(), points->end(), [&](const SessionMmrPoint& candidate) {
                return candidate.matchGuid == guid;
            });
            if (pointIt != points->end()) point = &*pointIt;
        }
        if (point && point->mmr > 0 && point->valueEstimated) {
            estimatedPath.push_back(point->mmr);
            lastEstimatedMmr = point->mmr;
        } else {
            const int step = won ? 9 : -9;
            int synthesizedMmr = lastEstimatedMmr + step;
            if (synthesizedMmr <= 0) synthesizedMmr = (std::max)(1, lastEstimatedMmr);
            estimatedPath.push_back(synthesizedMmr);
            lastEstimatedMmr = synthesizedMmr;
        }
    }

    const std::vector<int> reconciledPath =
        MmrServiceDetail::ReconcileEstimatedPath(pathBaselineMmr, fetchedMmr, estimatedPath, coveredResults);
    const bool reconciledPathAvailable = reconciledPath.size() == coveredCount;
    int chainMmr = pathBaselineMmr;
    result.publication.primaryId = req.primaryId;
    result.publication.playlist = req.playlist;
    result.publication.sessionGeneration = currentSessionGeneration;
    result.publication.fetchedMatches = fetchedMatches;

    int recapLatestMmr = 0;
    for (size_t index = 0; index < coveredCount; ++index) {
        const std::string guid = pendingGuids[index];
        auto rIt = m_postMatchRecordsByGuid.find(guid);
        if (rIt == m_postMatchRecordsByGuid.end()) continue;
        auto& record = rIt->second;
        const bool inSession = record.sessionGeneration == currentSessionGeneration;

        const bool valueEstimated = index < coveredCount - 1;
        int resolvedMmr = 0;
        if (!valueEstimated) {
            resolvedMmr = fetchedMmr;
        } else if (reconciledPathAvailable) {
            resolvedMmr = reconciledPath[index];
        } else if (inSession && points) {
            const auto pointIt = std::find_if(points->begin(), points->end(), [&](const SessionMmrPoint& point) {
                return point.matchGuid == guid;
            });
            resolvedMmr = pointIt != points->end() && pointIt->mmr > 0
                              ? pointIt->mmr
                              : EstimatePostMatchMmr(chainMmr, record.won, historyY);
        } else {
            resolvedMmr = EstimatePostMatchMmr(chainMmr, record.won, historyY);
        }
        if (resolvedMmr <= 0) resolvedMmr = (std::max)(1, chainMmr);

        if (!inSession) {
            recapLatestMmr = resolvedMmr;
        }

        const int matchesPlayed = publicationBaseline >= 0
                                      ? publicationBaseline + static_cast<int>(index) + 1
                                      : fetchedMatches;
        const bool countProvesCurrent =
            record.preMatchMatchesPlayed >= 0 && publicationBaseline >= 0 &&
            record.preMatchMatchesPlayed == publicationBaseline + static_cast<int>(index);
        const int sessionStartMmr =
            record.preMatchMmrIsPlaylistSpecific && record.preMatchMmr > 0 &&
                    (!record.followsPreviousSessionMatch || countProvesCurrent)
                ? record.preMatchMmr
                : 0;

        record.firstObservedMmr = resolvedMmr;
        record.firstObservedMatchesPlayed = matchesPlayed;
        record.provisionalMmr = resolvedMmr;
        record.graphPointAppended = true;
        record.databaseRowUpdated = true;
        record.trackerCovered = true;
        record.valueEstimated = valueEstimated;
        record.reconciliationState = PostMatchReconciliationState::Confirmed;

        MmrPublisher::ReconciledMatchUpdate update;
        update.matchGuid = guid;
        update.mmr = resolvedMmr;
        update.matchesPlayed = matchesPlayed;
        update.sessionStartMmr = sessionStartMmr;
        update.won = record.won;
        update.valueEstimated = valueEstimated;
        update.databaseRowUpdated = true;
        update.inSession = inSession;
        update.sessionGeneration = record.sessionGeneration;
        result.publication.updates.push_back(std::move(update));

        result.dbUpdates.push_back({guid, record.primaryId, resolvedMmr, valueEstimated});
        if (record.destroyedMatch && !record.databaseMatchFinalized) {
            record.databaseMatchFinalized = true;
            result.confirmations.emplace_back(guid, record.won);
        }

        if (guid == req.matchGuid) {
            result.requestConfirmed = true;
        }
        m_completedPostMatchGuids.insert(guid);
        chainMmr = resolvedMmr;
    }

    const bool stillPendingEndedSession = std::any_of(
        pendingGuids.begin() + static_cast<std::ptrdiff_t>(coveredCount),
        pendingGuids.end(),
        [&](const std::string& g) {
            const auto recordIt = m_postMatchRecordsByGuid.find(g);
            return recordIt != m_postMatchRecordsByGuid.end() &&
                   recordIt->second.sessionGeneration != currentSessionGeneration;
        });

    if (recapLatestMmr > 0 && !stillPendingEndedSession &&
        !result.publication.updates.empty() && !result.publication.updates.back().inSession) {
        result.publication.currentSessionSeedMmr = recapLatestMmr;
    }

    const bool allPendingCovered = coveredCount >= pendingGuids.size();
    result.allPendingCovered = allPendingCovered;

    const int carriedMatchesBaseline = fetchedMatches >= 0
                                           ? fetchedMatches
                                           : (publicationBaseline >= 0
                                                  ? publicationBaseline + static_cast<int>(coveredCount)
                                                  : -1);
    if (carriedMatchesBaseline >= 0) {
        auto [it, inserted] = m_trackerPublicationBaselineByPlaylist.emplace(pendingKey, carriedMatchesBaseline);
        if (!inserted) it->second = (std::max)(it->second, carriedMatchesBaseline);
    }
    for (size_t index = coveredCount; index < pendingGuids.size(); ++index) {
        const auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[index]);
        if (recordIt != m_postMatchRecordsByGuid.end()) {
            result.publication.pendingSessionGenerations.insert(recordIt->second.sessionGeneration);
        }
        if (recordIt != m_postMatchRecordsByGuid.end()) {
            recordIt->second.firstObservedMmr = fetchedMmr;
            if (carriedMatchesBaseline >= 0) {
                recordIt->second.firstObservedMatchesPlayed =
                    (std::max)(recordIt->second.firstObservedMatchesPlayed, carriedMatchesBaseline);
            }
        }
    }
    for (size_t index = 0; index < coveredCount; ++index) {
        m_postMatchRecordsByGuid.erase(pendingGuids[index]);
    }
    if (allPendingCovered) {
        m_pendingPostMatchesByPlaylist.erase(pendingIt);
    } else {
        pendingGuids.erase(pendingGuids.begin(), pendingGuids.begin() + static_cast<std::ptrdiff_t>(coveredCount));
    }

    result.publication.shouldUpdatePreMatchSnapshots =
        (allPendingCovered && fetchedMatches >= 0 && inMatch && !matchFinalized && req.primaryId == myPrimaryId);
    if (result.publication.shouldUpdatePreMatchSnapshots) {
        result.publication.protectedSnapshotGuids = m_completedPostMatchGuids;
        for (const auto& [guid, record] : m_postMatchRecordsByGuid) {
            result.publication.protectedSnapshotGuids.insert(guid);
        }
    }
    result.applied = true;
    result.confirmationCallback = m_destroyedMatchConfirmationCallback;
    return result;
}

MmrReconciler::ProvisionalResult MmrReconciler::EnsureProvisionalPoint(const MMRRequest& req,
                                                                       int baselineMmr,
                                                                       int fetchedMmr,
                                                                       int fetchedMatches,
                                                                       const std::vector<float>& historyY,
                                                                       int sessionInitialMmr) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ProvisionalResult result;
    if (req.matchGuid.empty() || req.playlist.empty()) return result;

    const auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
    if (recordIt == m_postMatchRecordsByGuid.end()) return result;
    auto& record = recordIt->second;

    if (record.graphPointAppended || record.reconciliationState == PostMatchReconciliationState::Confirmed) return result;
    if (record.firstObservedMmr <= 0 && fetchedMmr > 0) {
        record.firstObservedMmr = fetchedMmr;
    }
    if (record.firstObservedMatchesPlayed < 0 &&
        record.preMatchMatchesPlayed < 0 &&
        fetchedMatches >= 0) {
        record.firstObservedMatchesPlayed = fetchedMatches;
    }

    if (record.preMatchMmrIsPlaylistSpecific && record.preMatchMmr > 0) {
        baselineMmr = record.preMatchMmr;
    }
    const int sessionStartMmr =
        record.preMatchMmrIsPlaylistSpecific && record.preMatchMmr > 0 &&
                !record.followsPreviousSessionMatch
            ? record.preMatchMmr
            : 0;
    bool canApplyResultDirection =
        record.resultKnown && record.preMatchMmrIsPlaylistSpecific && baselineMmr > 0;
    if (!historyY.empty()) {
        const int historyMmr = static_cast<int>(std::lround(historyY.back()));
        if (historyMmr > 0) {
            baselineMmr = historyMmr;
            canApplyResultDirection = true;
        }
    }
    if (!canApplyResultDirection && sessionInitialMmr > 0) {
        baselineMmr = sessionInitialMmr;
        canApplyResultDirection = true;
    }
    if (!canApplyResultDirection && fetchedMmr > 0) baselineMmr = fetchedMmr;
    const int provisionalMmr = canApplyResultDirection
                                   ? EstimatePostMatchMmr(baselineMmr, record.won, historyY)
                                   : baselineMmr;
    if (provisionalMmr <= 0) return result;
    if (record.preMatchMmr <= 0 && canApplyResultDirection) {
        record.preMatchMmr = baselineMmr;
        record.preMatchMmrIsPlaylistSpecific = true;
    }

    record.provisionalMmr = provisionalMmr;
    record.graphPointAppended = true;
    record.databaseRowUpdated = !record.destroyedMatch || record.databaseMatchFinalized;
    record.trackerCovered = false;
    record.valueEstimated = true;
    record.reconciliationState = PostMatchReconciliationState::Provisional;
    m_completedPostMatchGuids.insert(req.matchGuid);

    result.publication.primaryId = req.primaryId;
    result.publication.playlist = req.playlist;
    result.publication.matchGuid = req.matchGuid;
    result.publication.sessionGeneration = record.sessionGeneration;
    result.publication.provisionalMmr = provisionalMmr;
    result.publication.sessionStartMmr = sessionStartMmr;
    result.publication.matchesPlayed = record.preMatchMatchesPlayed;
    result.publication.won = record.won;
    result.publication.databaseRowUpdated = record.databaseRowUpdated;

    if (record.databaseRowUpdated) {
        result.dbUpdate = DbUpdate{req.matchGuid, req.primaryId, provisionalMmr, true};
    }
    result.applied = true;
    return result;
}
