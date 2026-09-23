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

bool MMRFetcher::IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches, int fetchedMatches) {
    if (fetchedMmr <= 0) return true;

    // Tracker's playlist match count is the publication version. Rating is
    // only a fallback because a completed match can legitimately have no MMR
    // change, and a rating can move for reasons unrelated to this request.
    if (previousMatches >= 0 && fetchedMatches >= 0) {
        return fetchedMatches <= previousMatches;
    }

    if (previousMmr <= 0) return true;
    return fetchedMmr == previousMmr;
}

size_t MMRFetcher::CoveredPendingMatchCount(int oldestPreviousMatches,
                                            int fetchedMatches,
                                            size_t pendingCount) {
    if (oldestPreviousMatches < 0 || fetchedMatches <= oldestPreviousMatches || pendingCount == 0) return 0;
    return (std::min)(pendingCount, static_cast<size_t>(fetchedMatches - oldestPreviousMatches));
}

int MMRFetcher::ResolvePostMatchBaseline(int requestedPreviousMmr,
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

int MMRFetcher::EstimatePostMatchMmr(int previousMmr, bool won, const std::vector<float>& recentHistory) {
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

size_t MMRFetcher::PendingPlaylistCountLocked(const std::string& playlist) const {
    const auto it = m_pendingPostMatchesByPlaylist.find(playlist);
    return it == m_pendingPostMatchesByPlaylist.end() ? 0 : it->second.size();
}

void MMRFetcher::ResetPublicationBaselineForCounterRollbackLocked(
    const std::string& playlist, int previousMatches) {
    if (playlist.empty() || previousMatches < 0 ||
        PendingPlaylistCountLocked(playlist) != 0) {
        return;
    }

    const auto baselineIt =
        m_trackerPublicationBaselineByPlaylist.find(playlist);
    if (baselineIt == m_trackerPublicationBaselineByPlaylist.end() ||
        previousMatches >= baselineIt->second - 1) {
        return;
    }

    std::cout
        << "[MMRFetcher] Tracker matchesPlayed counter rolled back: playlist="
        << playlist << ", previousBaseline=" << baselineIt->second
        << ", newPreMatchCount=" << previousMatches
        << ", action=reset-publication-baseline.\n";
    baselineIt->second = previousMatches;
}

void MMRFetcher::UpdateSessionAggregateLocked() {
    m_state->game.sessionTotals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(m_state->game.sessionTotals.mmrChangeByPlaylist));
}

bool MMRFetcher::ReconcileTrackerResponse(const MMRRequest& req, int fetchedMmr, int fetchedMatches) {
    if (fetchedMmr <= 0 || req.playlist.empty()) return false;

    struct DbUpdate {
        std::string matchGuid;
        std::string primaryId;
        int mmr = 0;
        bool estimated = false;
    };
    std::vector<DbUpdate> dbUpdates;
    bool requestConfirmed = false;
    std::vector<std::pair<std::string, bool>>
        destroyedMatchConfirmations;
    DestroyedMatchConfirmationCallback confirmationCallback;

    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        auto pendingIt = m_pendingPostMatchesByPlaylist.find(req.playlist);
        if (pendingIt == m_pendingPostMatchesByPlaylist.end() || pendingIt->second.empty()) {
            return m_completedPostMatchGuids.count(req.matchGuid) > 0;
        }

        auto& pendingGuids = pendingIt->second;
        const auto oldestRecordIt = m_postMatchRecordsByGuid.find(pendingGuids.front());
        if (oldestRecordIt == m_postMatchRecordsByGuid.end()) return false;
        auto& oldestRecord = oldestRecordIt->second;
        const bool hadFirstObservedMmr = oldestRecord.firstObservedMmr > 0;
        const bool hadFirstObservedMatches = oldestRecord.firstObservedMatchesPlayed >= 0;

        int publicationBaseline = -1;
        if (oldestRecord.preMatchMatchesPlayed >= 0) {
            publicationBaseline = oldestRecord.preMatchMatchesPlayed;
        }
        if (hadFirstObservedMatches) {
            publicationBaseline =
                (std::max)(publicationBaseline, oldestRecord.firstObservedMatchesPlayed);
        }
        const auto baselineIt = m_trackerPublicationBaselineByPlaylist.find(req.playlist);
        if (baselineIt != m_trackerPublicationBaselineByPlaylist.end()) {
            publicationBaseline = (std::max)(publicationBaseline, baselineIt->second);
        }

        const bool hasPublicationBaseline = publicationBaseline >= 0;
        const bool matchCountsAvailable = hasPublicationBaseline && fetchedMatches >= 0;
        size_t coveredCount = matchCountsAvailable
                                  ? CoveredPendingMatchCount(
                                        publicationBaseline, fetchedMatches, pendingGuids.size())
                                  : 0;
        const int ratingBaseline =
            hadFirstObservedMmr
                ? oldestRecord.firstObservedMmr
                : (oldestRecord.preMatchMmrIsPlaylistSpecific && oldestRecord.preMatchMmr > 0
                       ? oldestRecord.preMatchMmr
                       : 0);
        if (!matchCountsAvailable &&
            !IsPostMatchMmrStale(ratingBaseline, fetchedMmr, -1, -1)) {
            // If either count is unavailable, a new playlist rating can cover
            // only the oldest unresolved match.
            coveredCount = 1;
        }

        const auto carryTrackerBaseline =
            [&](PendingPostMatchRecord& record, int matchesBaseline) {
                if (fetchedMmr > 0) {
                    record.firstObservedMmr = fetchedMmr;
                }
                if (matchesBaseline >= 0) {
                    record.firstObservedMatchesPlayed =
                        (std::max)(record.firstObservedMatchesPlayed, matchesBaseline);
                }
            };

        const auto persistPublicationBaseline = [&](int matchesBaseline) {
            if (matchesBaseline < 0) return;
            auto [it, inserted] =
                m_trackerPublicationBaselineByPlaylist.emplace(req.playlist, matchesBaseline);
            if (!inserted) {
                it->second = (std::max)(it->second, matchesBaseline);
            }
        };

        if (coveredCount == 0) {
            // This response becomes the baseline for the next comparison. It
            // must never be compared with itself on this call.
            carryTrackerBaseline(oldestRecord, fetchedMatches);
            persistPublicationBaseline(fetchedMatches);
            return false;
        }

        if (matchCountsAvailable &&
            coveredCount < pendingGuids.size()) {
            const auto lastCoveredIt =
                m_postMatchRecordsByGuid.find(
                    pendingGuids[coveredCount - 1]);
            const auto firstUncoveredIt =
                m_postMatchRecordsByGuid.find(
                    pendingGuids[coveredCount]);
            if (lastCoveredIt !=
                    m_postMatchRecordsByGuid.end() &&
                firstUncoveredIt !=
                    m_postMatchRecordsByGuid.end() &&
                lastCoveredIt->second
                        .preMatchMatchesPlayed >= 0 &&
                lastCoveredIt->second
                        .preMatchMatchesPlayed ==
                    firstUncoveredIt->second
                        .preMatchMatchesPlayed) {
                std::cout
                    << "[MMRFetcher] Post-match reconciliation deferred: "
                    << "matchGuid="
                    << PrivacyLog::Sensitive(
                           pendingGuids.front(), "match GUID")
                    << ", playlist=" << req.playlist
                    << ", previousMatches="
                    << publicationBaseline
                    << ", fetchedMatches=" << fetchedMatches
                    << ", covered=" << coveredCount
                    << ", pending="
                    << pendingGuids.size()
                    << ", reason=ambiguous-partial-publication.\n";
                return false;
            }
        }

        std::vector<size_t> unknownResultIndexes;
        for (size_t index = 0; index < coveredCount; ++index) {
            const auto recordIt =
                m_postMatchRecordsByGuid.find(pendingGuids[index]);
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

        confirmationCallback =
            m_destroyedMatchConfirmationCallback;
        if (!unknownResultIndexes.empty() &&
            confirmationCallback &&
            oldestRecord.preMatchMmrIsPlaylistSpecific &&
            oldestRecord.preMatchMmr > 0 &&
            fetchedMmr > 0 &&
            unknownResultIndexes.size() < 16) {
            std::vector<int> unanimousResults(
                unknownResultIndexes.size(), -1);
            size_t feasibleAssignments = 0;
            const size_t assignmentCount =
                size_t{1} << unknownResultIndexes.size();
            for (size_t assignment = 0;
                 assignment < assignmentCount;
                 ++assignment) {
                std::vector<bool> candidateResults;
                candidateResults.reserve(coveredCount);
                size_t unknownOffset = 0;
                for (size_t index = 0; index < coveredCount; ++index) {
                    const auto recordIt =
                        m_postMatchRecordsByGuid.find(
                            pendingGuids[index]);
                    if (recordIt == m_postMatchRecordsByGuid.end()) {
                        candidateResults.push_back(false);
                        continue;
                    }
                    if (recordIt->second.resultKnown) {
                        candidateResults.push_back(
                            recordIt->second.won);
                    } else {
                        candidateResults.push_back(
                            (assignment &
                             (size_t{1} << unknownOffset)) != 0);
                        ++unknownOffset;
                    }
                }
                if (BuildDirectionalMmrPath(
                        oldestRecord.preMatchMmr,
                        fetchedMmr,
                        candidateResults)
                        .size() != coveredCount) {
                    continue;
                }

                ++feasibleAssignments;
                for (size_t offset = 0;
                     offset < unknownResultIndexes.size();
                     ++offset) {
                    const int candidate =
                        (assignment & (size_t{1} << offset)) != 0
                            ? 1
                            : 0;
                    if (unanimousResults[offset] == -1) {
                        unanimousResults[offset] = candidate;
                    } else if (
                        unanimousResults[offset] != candidate) {
                        unanimousResults[offset] = 2;
                    }
                }
            }

            if (feasibleAssignments > 0) {
                for (size_t offset = 0;
                     offset < unknownResultIndexes.size();
                     ++offset) {
                    if (unanimousResults[offset] != 0 &&
                        unanimousResults[offset] != 1) {
                        continue;
                    }
                    auto recordIt =
                        m_postMatchRecordsByGuid.find(
                            pendingGuids
                                [unknownResultIndexes[offset]]);
                    if (recordIt !=
                        m_postMatchRecordsByGuid.end()) {
                        recordIt->second.won =
                            unanimousResults[offset] == 1;
                        recordIt->second.resultKnown = true;
                    }
                }
            }
        }

        for (const size_t index : unknownResultIndexes) {
            auto recordIt =
                m_postMatchRecordsByGuid.find(pendingGuids[index]);
            if (recordIt == m_postMatchRecordsByGuid.end() ||
                recordIt->second.resultKnown ||
                !confirmationCallback) {
                continue;
            }

            auto& record = recordIt->second;
            if (record.localPlayerDisappeared) {
                record.won = false;
                record.resultKnown = true;
                continue;
            }
            if ((record.localTeam != 0 && record.localTeam != 1) ||
                record.score[0] == record.score[1]) {
                continue;
            }

            record.won =
                record.score[record.localTeam] >
                record.score[1 - record.localTeam];
            record.resultKnown = true;
        }

        const bool allCoveredResultsKnown =
            std::all_of(
                pendingGuids.begin(),
                pendingGuids.begin() +
                    static_cast<std::ptrdiff_t>(coveredCount),
                [&](const std::string& guid) {
                    const auto recordIt =
                        m_postMatchRecordsByGuid.find(guid);
                    return recordIt !=
                               m_postMatchRecordsByGuid.end() &&
                           recordIt->second.resultKnown;
                });
        if (!allCoveredResultsKnown) {
            std::cout
                << "[MMRFetcher] Pending destroyed match remains unconfirmed: "
                << "matchGuid="
                << PrivacyLog::Sensitive(
                       pendingGuids.front(), "match GUID")
                << ", previousMatches="
                << oldestRecord.preMatchMatchesPlayed
                << ", fetchedMatches=" << fetchedMatches
                << ", previousMmr=" << oldestRecord.preMatchMmr
                << ", fetchedMmr=" << fetchedMmr
                << ", action=awaiting-result-signal.\n";
            return false;
        }

        // A response that covers an earlier record is also the publication
        // baseline for every remaining record. With a missing fetched count,
        // carry the count advancement implied by the covered matches.
        const int carriedMatchesBaseline =
            fetchedMatches >= 0
                ? fetchedMatches
                : (publicationBaseline >= 0
                       ? publicationBaseline + static_cast<int>(coveredCount)
                       : -1);
        persistPublicationBaseline(carriedMatchesBaseline);
        for (size_t index = coveredCount; index < pendingGuids.size(); ++index) {
            const auto recordIt = m_postMatchRecordsByGuid.find(pendingGuids[index]);
            if (recordIt != m_postMatchRecordsByGuid.end()) {
                carryTrackerBaseline(recordIt->second, carriedMatchesBaseline);
            }
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
        auto& points = m_state->history.playlistMatchPoints[req.playlist];
        auto& projection = m_state->history.playlistHistoryY[req.playlist];

        int pathBaselineMmr = oldestRecord.preMatchMmrIsPlaylistSpecific
                                  ? oldestRecord.preMatchMmr
                                  : 0;
        size_t earliestPendingHistoryIndex = projection.size();
        for (const std::string& pendingGuid : pendingGuids) {
            const auto pointIt = std::find_if(
                points.begin(), points.end(), [&](const SessionMmrPoint& point) {
                    return point.matchGuid == pendingGuid;
                });
            if (pointIt != points.end()) {
                earliestPendingHistoryIndex =
                    (std::min)(earliestPendingHistoryIndex, pointIt->historyIndex);
            }
        }
        if (earliestPendingHistoryIndex > 0 &&
            earliestPendingHistoryIndex <= projection.size()) {
            pathBaselineMmr = static_cast<int>(
                std::lround(projection[earliestPendingHistoryIndex - 1]));
        }
        if (pathBaselineMmr <= 0) {
            const auto initialIt = m_state->history.playlistInitialMmr.find(req.playlist);
            if (initialIt != m_state->history.playlistInitialMmr.end()) {
                pathBaselineMmr = initialIt->second;
            }
        }

        std::vector<int> estimatedPath;
        estimatedPath.reserve(coveredCount);
        std::vector<bool> coveredResults;
        coveredResults.reserve(coveredCount);

        int lastEstimatedMmr = pathBaselineMmr;

        for (size_t index = 0; index < coveredCount; ++index) {
            const std::string& guid = pendingGuids[index];
            const auto recordIt = m_postMatchRecordsByGuid.find(guid);

            const auto pointIt = std::find_if(
                points.begin(),
                points.end(),
                [&](const SessionMmrPoint& point) {
                    return point.matchGuid == guid;
                });
            bool won = true;
            if (recordIt != m_postMatchRecordsByGuid.end()) {
                won = recordIt->second.won;
            } else if (index == 0) {
                won = req.won;
            }
            coveredResults.push_back(won);

            if (pointIt != points.end() &&
                pointIt->mmr > 0 &&
                pointIt->valueEstimated) {
                estimatedPath.push_back(pointIt->mmr);
                lastEstimatedMmr = pointIt->mmr;
            } else {
                const int step = won ? 9 : -9;
                int synthesizedMmr = lastEstimatedMmr + step;
                if (synthesizedMmr <= 0) {
                    synthesizedMmr = (std::max)(1, lastEstimatedMmr);
                }
                estimatedPath.push_back(synthesizedMmr);
                lastEstimatedMmr = synthesizedMmr;
            }
        }

        const std::vector<int> reconciledPath =
            ReconcileEstimatedPath(
                pathBaselineMmr,
                fetchedMmr,
                estimatedPath,
                coveredResults);

        const bool reconciledPathAvailable =
            reconciledPath.size() == coveredCount;
        int chainMmr = pathBaselineMmr;
        for (size_t index = 0; index < coveredCount; ++index) {
            const std::string guid = pendingGuids[index];
            auto recordIt = m_postMatchRecordsByGuid.find(guid);
            if (recordIt == m_postMatchRecordsByGuid.end()) continue;
            auto& record = recordIt->second;

            auto pointIt = std::find_if(points.begin(), points.end(), [&](const SessionMmrPoint& point) {
                return point.matchGuid == guid;
            });
            const bool valueEstimated = index < coveredCount - 1;
            int resolvedMmr = 0;
            if (!valueEstimated) {
                resolvedMmr = fetchedMmr;
            } else if (reconciledPathAvailable) {
                resolvedMmr = reconciledPath[index];
            } else if (pointIt != points.end() && pointIt->mmr > 0) {
                resolvedMmr = pointIt->mmr;
            } else {
                resolvedMmr = EstimatePostMatchMmr(chainMmr, record.won, projection);
            }
            if (resolvedMmr <= 0) resolvedMmr = (std::max)(1, chainMmr);

            const bool appended = pointIt == points.end();
            if (appended) {
                const size_t historyIndex = projection.size();
                projection.push_back(static_cast<float>(resolvedMmr));
                points.push_back(SessionMmrPoint{
                    .matchGuid = guid,
                    .historyIndex = historyIndex,
                    .mmr = resolvedMmr,
                    .trackerMatchesPlayed =
                        publicationBaseline >= 0
                            ? publicationBaseline + static_cast<int>(index) + 1
                            : fetchedMatches,
                    .trackerCovered = true,
                    .valueEstimated = valueEstimated});
                pointIt = std::prev(points.end());
            } else {
                pointIt->mmr = resolvedMmr;
                pointIt->trackerMatchesPlayed =
                    publicationBaseline >= 0
                        ? publicationBaseline + static_cast<int>(index) + 1
                        : fetchedMatches;
                pointIt->trackerCovered = true;
                pointIt->valueEstimated = valueEstimated;
                if (pointIt->historyIndex < projection.size()) {
                    projection[pointIt->historyIndex] = static_cast<float>(resolvedMmr);
                }
            }

            if (m_state->history.playlistInitialMmr.count(req.playlist) == 0) {
                m_state->history.playlistInitialMmr[req.playlist] =
                    record.preMatchMmrIsPlaylistSpecific && record.preMatchMmr > 0
                        ? record.preMatchMmr
                        : resolvedMmr;
            }
            record.provisionalMmr = resolvedMmr;
            record.graphPointAppended = true;
            record.databaseRowUpdated = true;
            record.trackerCovered = true;
            record.valueEstimated = valueEstimated;
            record.reconciliationState = PostMatchReconciliationState::Confirmed;
            m_completedPostMatchGuids.insert(guid);
            requestConfirmed = requestConfirmed || guid == req.matchGuid;
            chainMmr = resolvedMmr;
            dbUpdates.push_back({guid, record.primaryId, resolvedMmr, valueEstimated});
            if (record.destroyedMatch &&
                !record.databaseMatchFinalized) {
                record.databaseMatchFinalized = true;
                destroyedMatchConfirmations.emplace_back(
                    guid, record.won);
            }

            std::cout
                << "[MMRFetcher] Post-match reconciliation: matchGuid="
                << PrivacyLog::Sensitive(guid, "match GUID")
                << ", playlist=" << req.playlist
                << ", previousMmr=" << record.preMatchMmr
                << ", fetchedMmr=" << fetchedMmr
                << ", previousMatches=" << record.preMatchMatchesPlayed
                << ", fetchedMatches=" << fetchedMatches
                << ", pending=" << pendingGuids.size()
                << ", point=" << (appended ? "appended" : "replaced")
                << ", reconciliation="
                << (reconciledPathAvailable ? "adjusted" : "fallback")
                << ", coverage=tracker-covered"
                << ", value=" << (valueEstimated ? "estimated" : "exact") << ".\n";
            if (record.destroyedMatch) {
                std::cout
                    << "[MMRFetcher] Pending destroyed match confirmed: "
                    << "matchGuid="
                    << PrivacyLog::Sensitive(guid, "match GUID")
                    << ", matches="
                    << record.preMatchMatchesPlayed << "->"
                    << fetchedMatches
                    << ", mmr=" << record.preMatchMmr << "->"
                    << resolvedMmr
                    << ", result="
                    << (record.won ? "win" : "loss") << ".\n";
            }
        }

        for (size_t index = 0; index < coveredCount; ++index) {
            m_postMatchRecordsByGuid.erase(pendingGuids[index]);
        }
        pendingGuids.erase(pendingGuids.begin(), pendingGuids.begin() + static_cast<std::ptrdiff_t>(coveredCount));
        if (pendingGuids.empty()) m_pendingPostMatchesByPlaylist.erase(pendingIt);

        if (!projection.empty()) {
            m_state->game.sessionTotals.mmrChangeByPlaylist[req.playlist] =
                static_cast<int>(std::lround(projection.back())) -
                m_state->history.playlistInitialMmr[req.playlist];
        }
        UpdateSessionAggregateLocked();
        m_state->game.version++;
        m_state->history.version++;
    }

    if (confirmationCallback) {
        for (const auto& [matchGuid, won] :
             destroyedMatchConfirmations) {
            confirmationCallback(matchGuid, won);
        }
    }

    if (auto db = m_dbManager.lock()) {
        for (const auto& update : dbUpdates) {
            db->AsyncUpdateMatchPlayerMmr(
                update.matchGuid, update.primaryId, update.mmr,
                update.estimated);
        }
    }
    return requestConfirmed;
}

void MMRFetcher::EnsureProvisionalPoint(const MMRRequest& req,
                                        int baselineMmr,
                                        int fetchedMmr,
                                        int fetchedMatches) {
    int provisionalMmr = 0;
    bool appended = false;
    bool shouldUpdateDatabase = false;
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
        if (recordIt == m_postMatchRecordsByGuid.end() ||
            recordIt->second.reconciliationState == PostMatchReconciliationState::Confirmed) {
            return;
        }
        auto& record = recordIt->second;
        if (record.firstObservedMmr <= 0 && fetchedMmr > 0) {
            record.firstObservedMmr = fetchedMmr;
        }
        if (record.firstObservedMatchesPlayed < 0 &&
            record.preMatchMatchesPlayed < 0 &&
            fetchedMatches >= 0) {
            record.firstObservedMatchesPlayed = fetchedMatches;
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
        auto& projection = m_state->history.playlistHistoryY[req.playlist];
        auto& points = m_state->history.playlistMatchPoints[req.playlist];
        auto pointIt = std::find_if(points.begin(), points.end(), [&](const SessionMmrPoint& point) {
            return point.matchGuid == req.matchGuid;
        });
        if (pointIt != points.end()) return;

        bool canApplyResultDirection =
            record.resultKnown &&
            record.preMatchMmrIsPlaylistSpecific &&
            baselineMmr > 0;
        if (!projection.empty()) {
            const int latestHistoryMmr = static_cast<int>(std::lround(projection.back()));
            if (latestHistoryMmr > 0) {
                baselineMmr = latestHistoryMmr;
                canApplyResultDirection = true;
            }
        }
        if (!canApplyResultDirection) {
            const auto initialIt = m_state->history.playlistInitialMmr.find(req.playlist);
            if (initialIt != m_state->history.playlistInitialMmr.end() && initialIt->second > 0) {
                baselineMmr = initialIt->second;
                canApplyResultDirection = true;
            }
        }
        if (!canApplyResultDirection && fetchedMmr > 0) {
            // Without a captured pre-match value, Tracker's rating may already
            // include this match. Own the point, but do not apply the result twice.
            baselineMmr = fetchedMmr;
        }
        if (baselineMmr <= 0) {
            const auto playerIt = m_state->game.roster.find(req.primaryId);
            if (playerIt != m_state->game.roster.end()) {
                const auto playlistIt = playerIt->second.playlists.find(req.playlist);
                if (playlistIt != playerIt->second.playlists.end() && playlistIt->second > 0) {
                    baselineMmr = playlistIt->second;
                } else if (playerIt->second.mmr > 0) {
                    baselineMmr = playerIt->second.mmr;
                }
            }
        }
        provisionalMmr =
            canApplyResultDirection
                ? EstimatePostMatchMmr(
                      baselineMmr, record.won, projection)
                : baselineMmr;
        if (provisionalMmr <= 0) {
            std::cout
                << "[MMRFetcher] Post-match reconciliation: matchGuid="
                << PrivacyLog::Sensitive(
                       req.matchGuid, "match GUID")
                << ", playlist=" << req.playlist
                << ", previousMmr=" << req.previousMmr
                << ", fetchedMmr=" << fetchedMmr
                << ", previousMatches=" << req.previousMatches
                << ", fetchedMatches=" << fetchedMatches
                << ", pending=" << PendingPlaylistCountLocked(req.playlist)
                << ", point=deferred, state=awaiting-baseline.\n";
            return;
        }
        if (record.preMatchMmr <= 0 && canApplyResultDirection) {
            record.preMatchMmr = baselineMmr;
            record.preMatchMmrIsPlaylistSpecific = true;
        }

        const size_t historyIndex = projection.size();
        projection.push_back(static_cast<float>(provisionalMmr));
        points.push_back(SessionMmrPoint{
            .matchGuid = req.matchGuid,
            .historyIndex = historyIndex,
            .mmr = provisionalMmr,
            .trackerMatchesPlayed = req.previousMatches,
            .trackerCovered = false,
            .valueEstimated = true});
        if (m_state->history.playlistInitialMmr.count(req.playlist) == 0) {
            m_state->history.playlistInitialMmr[req.playlist] =
                baselineMmr > 0 ? baselineMmr : provisionalMmr;
        }
        record.provisionalMmr = provisionalMmr;
        record.graphPointAppended = true;
        record.databaseRowUpdated =
            !record.destroyedMatch ||
            record.databaseMatchFinalized;
        record.trackerCovered = false;
        record.valueEstimated = true;
        record.reconciliationState = PostMatchReconciliationState::Provisional;
        m_completedPostMatchGuids.insert(req.matchGuid);
        m_state->game.sessionTotals.mmrChangeByPlaylist[req.playlist] =
            provisionalMmr - m_state->history.playlistInitialMmr[req.playlist];
        UpdateSessionAggregateLocked();
        m_state->game.version++;
        m_state->history.version++;
        appended = true;
        shouldUpdateDatabase = record.databaseRowUpdated;

        std::cout
            << "[MMRFetcher] Post-match reconciliation: matchGuid="
            << PrivacyLog::Sensitive(
                   req.matchGuid, "match GUID")
            << ", playlist=" << req.playlist
            << ", previousMmr=" << req.previousMmr
            << ", fetchedMmr=" << fetchedMmr
            << ", previousMatches=" << req.previousMatches
            << ", fetchedMatches=" << fetchedMatches
            << ", pending=" << PendingPlaylistCountLocked(req.playlist)
            << ", point=appended, coverage=uncovered, value=estimated.\n";
    }

    if (appended && shouldUpdateDatabase) {
        if (auto db = m_dbManager.lock()) {
            db->AsyncUpdateMatchPlayerMmr(
                req.matchGuid, req.primaryId, provisionalMmr, true);
        }
    }
}

bool MMRFetcher::PublishProfileResult(const MMRRequest& req, const NormalizedProfileResult& profile) {
    int postMatchMmr = 0;
    int postMatchMatches = -1;
    bool postMatchConfirmed = false;
    if (req.reason == MMRRequestReason::PostMatch) {
        const auto playlistIt = profile.playlistMMRs.find(req.playlist);
        if (playlistIt != profile.playlistMMRs.end()) postMatchMmr = playlistIt->second;
        const auto matchesIt = profile.playlistMatches.find(req.playlist);
        if (matchesIt != profile.playlistMatches.end()) postMatchMatches = matchesIt->second;

        size_t pendingCount = 0;
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            pendingCount = PendingPlaylistCountLocked(req.playlist);
        }
        std::cout
            << "[MMRFetcher] Post-match refresh: matchGuid="
            << PrivacyLog::Sensitive(
                   req.matchGuid, "match GUID")
            << ", playlist=" << req.playlist
            << ", previousMmr=" << req.previousMmr
            << ", fetchedMmr=" << postMatchMmr
            << ", previousMatches=" << req.previousMatches
            << ", fetchedMatches=" << postMatchMatches
            << ", pending=" << pendingCount << ".\n";

        postMatchConfirmed = ReconcileTrackerResponse(req, postMatchMmr, postMatchMatches);
        if (!postMatchConfirmed && req.retriesRemaining > 0) {
            std::cout
                << "[MMRFetcher] Post-match reconciliation deferred: matchGuid="
                << PrivacyLog::Sensitive(
                       req.matchGuid, "match GUID")
                << ", playlist=" << req.playlist << ".\n";
            return ScheduleRetry(req, kStalePostMatchRetryDelay, "post-match match count not advanced");
        }
        if (!postMatchConfirmed) {
            EnsureProvisionalPoint(req, req.previousMmr, postMatchMmr, postMatchMatches);
        }
    } else if (req.primaryId == m_state->game.myPrimaryId) {
        // Roster refreshes may confirm provisional points, but the pending
        // match records remain the sole owners of graph cardinality.
        std::vector<MMRRequest> pendingPlaylists;
        {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            for (const auto& [playlist, guids] : m_pendingPostMatchesByPlaylist) {
                if (guids.empty()) continue;
                const auto recordIt = m_postMatchRecordsByGuid.find(guids.front());
                if (recordIt == m_postMatchRecordsByGuid.end()) continue;
                MMRRequest pendingReq;
                pendingReq.primaryId = recordIt->second.primaryId;
                pendingReq.matchGuid = recordIt->second.matchGuid;
                pendingReq.playlist = playlist;
                pendingReq.previousMmr = recordIt->second.preMatchMmr;
                pendingReq.previousMatches = recordIt->second.preMatchMatchesPlayed;
                pendingReq.previousMmrIsPlaylistSpecific =
                    recordIt->second.preMatchMmrIsPlaylistSpecific;
                pendingReq.won = recordIt->second.won;
                pendingReq.resultKnown =
                    recordIt->second.resultKnown;
                pendingPlaylists.push_back(std::move(pendingReq));
            }
        }
        for (const auto& pendingReq : pendingPlaylists) {
            const auto mmrIt = profile.playlistMMRs.find(pendingReq.playlist);
            if (mmrIt == profile.playlistMMRs.end()) continue;
            const auto matchesIt = profile.playlistMatches.find(pendingReq.playlist);
            const int fetchedMatches =
                matchesIt != profile.playlistMatches.end() ? matchesIt->second : -1;
            const bool confirmed =
                ReconcileTrackerResponse(pendingReq, mmrIt->second, fetchedMatches);
            if (!confirmed) {
                EnsureProvisionalPoint(
                    pendingReq, pendingReq.previousMmr, mmrIt->second, fetchedMatches);
            }
        }
    }

    // Cache only the local player's successful profile. A normal
    // roster result can populate the cache on first use (including solo
    // training). After a completed match, EnqueuePostMatch invalidates the
    // cache and only a fully reconciled result can make it valid again.
    StoreLocalProfileCache(req,
                           profile.bestMmr,
                           profile.bestTier,
                           profile.playlistMMRs,
                           profile.playlistTiers,
                           profile.playlistMatches,
                           profile.totalWins,
                           postMatchConfirmed);

    std::unordered_set<std::string> playlistsAwaitingPostMatch;
    {
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        for (const auto& [playlist, guids] : m_pendingPostMatchesByPlaylist) {
            if (!guids.empty()) playlistsAwaitingPostMatch.insert(playlist);
        }
    }

    {
        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);

        if (req.reason == MMRRequestReason::Roster &&
            req.primaryId == m_state->game.myPrimaryId &&
            m_state->game.inMatch &&
            !m_state->game.matchGuid.empty() &&
            m_state->game.preMatchMmrByGuid.count(m_state->game.matchGuid) == 0) {
            const bool hasPlaylistMmr = std::any_of(
                profile.playlistMMRs.begin(), profile.playlistMMRs.end(),
                [](const auto& entry) {
                    return entry.first != "best" && entry.second > 0;
                });
            if (hasPlaylistMmr) {
                m_state->game.preMatchMmrByGuid.emplace(
                    m_state->game.matchGuid,
                    LocalPreMatchMmrSnapshot{
                        .playlistMmrs = profile.playlistMMRs,
                        .playlistMatches = profile.playlistMatches});
                std::cout
                    << "[MMRFetcher] Captured pre-match MMR snapshot: matchGuid="
                    << PrivacyLog::Sensitive(
                           m_state->game.matchGuid,
                           "match GUID")
                    << ".\n";
            }
        }

        auto& player = m_state->game.roster[req.primaryId];
        player.primaryId = req.primaryId;
        if (player.name.empty()) player.name = req.name;
        player.playlists = profile.playlistMMRs;
        player.playlistTiers = profile.playlistTiers;
        player.playlistMatches = profile.playlistMatches;
        if (profile.totalWins >= 0) {
            player.totalWins = profile.totalWins;
        }
        player.mmr = profile.bestMmr;
        player.rankTier = profile.bestTier;
        player.fetched = true;
        player.fetchFailed = false;
        player.rankVerificationSource = profile.rankVerificationSource;

        if (req.primaryId == m_state->game.myPrimaryId) {
            if (m_state->history.initialMmr == -1 && profile.bestMmr > 0) {
                m_state->history.initialMmr = profile.bestMmr;
            }
            if (req.reason == MMRRequestReason::Roster && profile.bestMmr > 0) {
                m_state->history.mmrHistoryY.push_back(static_cast<float>(profile.bestMmr));
                m_state->history.mmrHistoryX.push_back(static_cast<float>(m_state->history.mmrHistoryY.size()));
            }

            for (const auto& [playlistName, fetchedMmr] : profile.playlistMMRs) {
                if (fetchedMmr <= 0 || playlistName == "best" || playlistName == "t" ||
                    playlistName == "casual") {
                    continue;
                }
                if (req.reason == MMRRequestReason::PostMatch && playlistName == req.playlist) continue;
                if (playlistsAwaitingPostMatch.count(playlistName) > 0) continue;

                auto& history = m_state->history.playlistHistoryY[playlistName];
                if (m_state->history.playlistInitialMmr.count(playlistName) == 0) {
                    m_state->history.playlistInitialMmr[playlistName] = fetchedMmr;
                }
                m_state->game.sessionTotals.mmrChangeByPlaylist[playlistName] =
                    fetchedMmr - m_state->history.playlistInitialMmr[playlistName];
                if (history.empty() || static_cast<int>(std::lround(history.back())) != fetchedMmr) {
                    history.push_back(static_cast<float>(fetchedMmr));
                }
            }
            UpdateSessionAggregateLocked();
        }

        std::cout << "[MMRFetcher] Updated: " << PrivacyLog::Sensitive(req.name, "player name")
                  << " -> Best: " << profile.bestMmr << "\n";
        m_state->game.version++;
        m_state->history.version++;
    }
    return false;
}
