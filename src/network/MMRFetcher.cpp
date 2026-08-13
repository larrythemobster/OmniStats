#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
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

namespace {

    struct TournamentRankThreshold {
        int minMmr;
        const char* tier;
        const char* division;
    };

    static constexpr std::array<TournamentRankThreshold, 85> TournamentRankThresholds = {{{-4, "Bronze I", "Div I"},
                                                                                          {119, "Bronze I", "Div II"},
                                                                                          {138, "Bronze I", "Div III"},
                                                                                          {157, "Bronze I", "Div IV"},
                                                                                          {162, "Bronze II", "Div I"},
                                                                                          {179, "Bronze II", "Div II"},
                                                                                          {198, "Bronze II", "Div III"},
                                                                                          {217, "Bronze II", "Div IV"},
                                                                                          {221, "Bronze III", "Div I"},
                                                                                          {239, "Bronze III", "Div II"},
                                                                                          {258, "Bronze III", "Div III"},
                                                                                          {277, "Bronze III", "Div IV"},
                                                                                          {283, "Silver I", "Div I"},
                                                                                          {299, "Silver I", "Div II"},
                                                                                          {318, "Silver I", "Div III"},
                                                                                          {337, "Silver I", "Div IV"},
                                                                                          {342, "Silver II", "Div I"},
                                                                                          {359, "Silver II", "Div II"},
                                                                                          {378, "Silver II", "Div III"},
                                                                                          {397, "Silver II", "Div IV"},
                                                                                          {403, "Silver III", "Div I"},
                                                                                          {419, "Silver III", "Div II"},
                                                                                          {438, "Silver III", "Div III"},
                                                                                          {457, "Silver III", "Div IV"},
                                                                                          {461, "Gold I", "Div I"},
                                                                                          {478, "Gold I", "Div II"},
                                                                                          {498, "Gold I", "Div III"},
                                                                                          {517, "Gold I", "Div IV"},
                                                                                          {521, "Gold II", "Div I"},
                                                                                          {539, "Gold II", "Div II"},
                                                                                          {558, "Gold II", "Div III"},
                                                                                          {577, "Gold II", "Div IV"},
                                                                                          {581, "Gold III", "Div I"},
                                                                                          {598, "Gold III", "Div II"},
                                                                                          {618, "Gold III", "Div III"},
                                                                                          {637, "Gold III", "Div IV"},
                                                                                          {641, "Platinum I", "Div I"},
                                                                                          {659, "Platinum I", "Div II"},
                                                                                          {678, "Platinum I", "Div III"},
                                                                                          {697, "Platinum I", "Div IV"},
                                                                                          {701, "Platinum II", "Div I"},
                                                                                          {719, "Platinum II", "Div II"},
                                                                                          {738, "Platinum II", "Div III"},
                                                                                          {757, "Platinum II", "Div IV"},
                                                                                          {761, "Platinum III", "Div I"},
                                                                                          {779, "Platinum III", "Div II"},
                                                                                          {798, "Platinum III", "Div III"},
                                                                                          {817, "Platinum III", "Div IV"},
                                                                                          {824, "Diamond I", "Div I"},
                                                                                          {844, "Diamond I", "Div II"},
                                                                                          {868, "Diamond I", "Div III"},
                                                                                          {892, "Diamond I", "Div IV"},
                                                                                          {901, "Diamond II", "Div I"},
                                                                                          {924, "Diamond II", "Div II"},
                                                                                          {948, "Diamond II", "Div III"},
                                                                                          {972, "Diamond II", "Div IV"},
                                                                                          {981, "Diamond III", "Div I"},
                                                                                          {1004, "Diamond III", "Div II"},
                                                                                          {1028, "Diamond III", "Div III"},
                                                                                          {1052, "Diamond III", "Div IV"},
                                                                                          {1061, "Champion I", "Div I"},
                                                                                          {1096, "Champion I", "Div II"},
                                                                                          {1128, "Champion I", "Div III"},
                                                                                          {1162, "Champion I", "Div IV"},
                                                                                          {1181, "Champion II", "Div I"},
                                                                                          {1215, "Champion II", "Div II"},
                                                                                          {1254, "Champion II", "Div III"},
                                                                                          {1282, "Champion II", "Div IV"},
                                                                                          {1301, "Champion III", "Div I"},
                                                                                          {1337, "Champion III", "Div II"},
                                                                                          {1368, "Champion III", "Div III"},
                                                                                          {1402, "Champion III", "Div IV"},
                                                                                          {1421, "Grand Champion I", "Div I"},
                                                                                          {1460, "Grand Champion I", "Div II"},
                                                                                          {1498, "Grand Champion I", "Div III"},
                                                                                          {1537, "Grand Champion I", "Div IV"},
                                                                                          {1561, "Grand Champion II", "Div I"},
                                                                                          {1600, "Grand Champion II", "Div II"},
                                                                                          {1638, "Grand Champion II", "Div III"},
                                                                                          {1677, "Grand Champion II", "Div IV"},
                                                                                          {1701, "Grand Champion III", "Div I"},
                                                                                          {1745, "Grand Champion III", "Div II"},
                                                                                          {1788, "Grand Champion III", "Div III"},
                                                                                          {1832, "Grand Champion III", "Div IV"},
                                                                                          {1861, "Supersonic Legend", ""}}};

    static bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out) {
        for (const char* key : keys) {
            if (!stats.contains(key) || !stats[key].is_object()) continue;
            const auto& stat = stats[key];
            if (stat.contains("value") && stat["value"].is_number()) {
                out = stat["value"].get<int>();
                return true;
            }
        }
        return false;
    }

    static bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr) {
        auto it = playlistMMRs.find(playlistName);
        return it == playlistMMRs.end() || mmr > it->second;
    }

#ifdef OMNISTATS_TEST_ENVIRONMENT
    static constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(20);
    static constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(20);
    static constexpr auto kTransientRetryDelay = std::chrono::milliseconds(20);
#else
    static constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(2500);
    static constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(3000);
    static constexpr auto kTransientRetryDelay = std::chrono::milliseconds(3000);
#endif

    static bool MmrPathPreservesResults(
        int initialMmr,
        const std::vector<int>& path,
        const std::vector<bool>& results);

    static std::vector<int> BuildDirectionalMmrPath(
        int initialMmr,
        int finalMmr,
        const std::vector<bool>& results) {
        if (results.empty() || initialMmr <= 0 || finalMmr <= 0)
            return {};

        const size_t n = results.size();
        size_t wins = 0;
        size_t losses = 0;
        for (bool won : results) {
            if (won)
                ++wins;
            else
                ++losses;
        }
        const int totalDelta = finalMmr - initialMmr;
        if (totalDelta > 0 && wins == 0) return {};
        if (totalDelta < 0 && losses == 0) return {};
        if (totalDelta == 0 && (wins == 0 || losses == 0) && (wins > 0 || losses > 0)) return {};

        double winStep = 9.0;
        double lossStep = 9.0;

        if (totalDelta > 0) {
            lossStep = 9.0;
            winStep = (totalDelta + lossStep * losses) / static_cast<double>(wins);
        } else if (totalDelta < 0) {
            winStep = 9.0;
            lossStep = (winStep * wins - totalDelta) / static_cast<double>(losses);
        } else if (wins > 0 && losses > 0) {
            winStep = 9.0;
            lossStep = (winStep * wins) / static_cast<double>(losses);
        }

        std::vector<int> path;
        path.reserve(n);
        double currentMmr = static_cast<double>(initialMmr);

        for (size_t i = 0; i < n; ++i) {
            if (results[i]) {
                currentMmr += winStep;
            } else {
                currentMmr -= lossStep;
            }
            if (currentMmr <= 0)
                return {};
            if (i == n - 1) {
                path.push_back(finalMmr);
            } else {
                path.push_back(static_cast<int>(std::round(currentMmr)));
            }
        }

        return MmrPathPreservesResults(initialMmr, path, results)
                   ? path
                   : std::vector<int>{};
    }

    static bool MmrPathPreservesResults(
        int initialMmr,
        const std::vector<int>& path,
        const std::vector<bool>& results) {
        if (initialMmr <= 0 || path.size() != results.size())
            return false;

        int previousMmr = initialMmr;
        for (size_t i = 0; i < path.size(); ++i) {
            const int currentMmr = path[i];
            if (currentMmr <= 0)
                return false;
            if (results[i] ? currentMmr <= previousMmr
                           : currentMmr >= previousMmr) {
                return false;
            }
            previousMmr = currentMmr;
        }
        return true;
    }

    static std::vector<int> ReconcileEstimatedPath(
        int initialMmr,
        int finalMmr,
        const std::vector<int>& estimatedPath,
        const std::vector<bool>& results) {
        if (estimatedPath.empty() ||
            estimatedPath.size() != results.size() ||
            initialMmr <= 0 ||
            finalMmr <= 0) {
            return {};
        }

        std::vector<int> path = estimatedPath;
        path.back() = finalMmr;
        if (MmrPathPreservesResults(initialMmr, path, results))
            return path;

        const int estimatedFinal = estimatedPath.back();
        const int error = finalMmr - estimatedFinal;
        const int pathLength = static_cast<int>(path.size());

        for (int i = 0; i < pathLength; ++i) {
            const int correction =
                static_cast<int>(std::round(
                    static_cast<double>(error) * (i + 1) /
                    pathLength));
            path[i] = estimatedPath[i] + correction;
        }
        path.back() = finalMmr;

        if (MmrPathPreservesResults(initialMmr, path, results))
            return path;

        return BuildDirectionalMmrPath(
            initialMmr, finalMmr, results);
    }

} // namespace

// Helper function for libcurl to write the HTTP response into a std::string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static int CIProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    std::atomic<bool>* running = (std::atomic<bool>*)clientp;
    if (running && !running->load()) {
        return 1; // Abort
    }
    return 0;
}

MMRFetcher::MMRFetcher(std::shared_ptr<SessionState> state,
                       std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)) {}

MMRFetcher::~MMRFetcher() {
    Stop();
}

std::string MMRFetcher::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";

    const TournamentRankThreshold* threshold = nullptr;
    for (const auto& entry : TournamentRankThresholds) {
        if (mmr < entry.minMmr) break;
        threshold = &entry;
    }

    if (!threshold) return "Unranked";
    if (threshold->division[0] == '\0') return threshold->tier;
    return std::string(threshold->tier) + " " + threshold->division;
}

std::string MMRFetcher::PlaylistNameForTrackerId(int playlistId) {
    switch (playlistId) {
    case 10:
        return "1v1";
    case 11:
        return "2v2";
    case 13:
        return "3v3";
    case 27:
        return "hoops";
    case 28:
        return "rumble";
    case 29:
        return "dropshot";
    case 30:
        return "snowday";
    case 34:
        return "t";
    case 43:
        return "heatseeker";
    case 0:
        return "casual";
    default:
        return "";
    }
}

MMRProfileTotals MMRFetcher::ExtractProfileTotals(const nlohmann::json& jsonResp) {
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
                !recordIt->second.localPlayerDisappeared ||
                !confirmationCallback) {
                continue;
            }
            recordIt->second.won = false;
            recordIt->second.resultKnown = true;
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
            dbUpdates.push_back({guid, record.primaryId, resolvedMmr});
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
            db->AsyncUpdateMatchPlayerMmr(update.matchGuid, update.primaryId, update.mmr);
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
                req.matchGuid, req.primaryId, provisionalMmr);
        }
    }
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
#endif

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

void MMRFetcher::Enqueue(const std::string& primaryId, const std::string& name) {
    if (!Config::Read().enable_mmr_tracking || primaryId.empty()) return;

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
        if (m_postMatchRecordsByGuid.count(matchGuid) ||
            m_pendingPostMatchGuids.count(matchGuid) ||
            m_completedPostMatchGuids.count(matchGuid)) {
            return;
        }

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
        if (m_postMatchRecordsByGuid.count(pending.matchGuid) ||
            m_pendingPostMatchGuids.count(pending.matchGuid) ||
            m_completedPostMatchGuids.count(pending.matchGuid)) {
            return;
        }

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

std::string MMRFetcher::GetTRNPlatform(const std::string& primaryId) {
    size_t delim = primaryId.find('|');
    if (delim == std::string::npos) return "";

    std::string plat = primaryId.substr(0, delim);
    // Trim potential spaces
    plat.erase(0, plat.find_first_not_of(" \t\r\n"));
    plat.erase(plat.find_last_not_of(" \t\r\n") + 1);

    // Convert to lowercase for case-insensitive matching
    std::transform(plat.begin(), plat.end(), plat.begin(), ::tolower);

    if (plat == "epic" || plat == "epicgames") return "epic";
    if (plat == "steam") return "steam";
    if (plat == "ps4" || plat == "psn" || plat == "playstation") return "psn";
    if (plat == "xboxone" || plat == "xbox" || plat == "xbl") return "xbl";
    if (plat == "switch" || plat == "nintendo") return "switch";
    return "";
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

            auto nextIt = std::min_element(
                m_queue.begin(), m_queue.end(),
                [](const MMRRequest& lhs, const MMRRequest& rhs) {
                    return lhs.notBefore < rhs.notBefore;
                });

            const auto now = std::chrono::steady_clock::now();
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
        m_cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return !m_isRunning; });
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

bool MMRFetcher::FetchProfile(MMRRequest req) {
    if (req.reason == MMRRequestReason::PostMatch) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
        if (recordIt != m_postMatchRecordsByGuid.end() &&
            recordIt->second.reconciliationState == PostMatchReconciliationState::Confirmed) {
            return false;
        }
    }
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        std::cout << "[MMRFetcher] Skipping " << PrivacyLog::Sensitive(req.name, "player name")
                  << ": curl-impersonate not loaded\n";
        return false;
    }

    const std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) return false;

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return false;

    const size_t delim = req.primaryId.find('|');
    std::string ident;
    if (plat == "steam") {
        if (delim != std::string::npos) {
            std::string sub = req.primaryId.substr(delim + 1);
            const size_t secondDelim = sub.find('|');
            ident = secondDelim != std::string::npos ? sub.substr(0, secondDelim) : sub;
        } else {
            ident = req.primaryId;
        }
    } else {
        ident = req.name;
    }

    char* escapedIdent = ci.easy_escape(ci_curl, ident.c_str(), static_cast<int>(ident.length()));
    if (!escapedIdent) {
        ci.easy_cleanup(ci_curl);
        return false;
    }
    const std::string finalIdent = escapedIdent;
    ci.free_ptr(escapedIdent);

    const std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + finalIdent;
    std::cout << "[MMRFetcher] Fetching " << PrivacyLog::Sensitive(req.name, "player name") << " via " << plat << "...\n";

    ci.easy_impersonate(ci_curl, "chrome136", 0);

    std::string readBuffer;
    void* headers = nullptr;
    headers = ci.slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = ci.slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = ci.slist_append(headers, "Origin: https://rocketleague.tracker.network");
    headers = ci.slist_append(headers, "Referer: https://rocketleague.tracker.network/");
    headers = ci.slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36");

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 15L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFODATA, &m_isRunning);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 0L);

    const int res = ci.easy_perform(ci_curl);
    long httpCode = 0;
    ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0 || httpCode != 200) {
        std::cout << "[MMRFetcher] Failed to fetch " << PrivacyLog::Sensitive(req.name, "player name")
                  << " (HTTP " << httpCode << ") - Curl error: " << res << "\n";

        if (httpCode == 429) {
            if (ScheduleRetry(req, std::chrono::seconds(15), "rate limited")) return true;
        } else if (res != 0 || httpCode == 408 || (httpCode >= 500 && httpCode <= 599)) {
            if (ScheduleRetry(req, kTransientRetryDelay, "transient Tracker failure")) return true;
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }

    try {
        const nlohmann::json jsonResp = nlohmann::json::parse(readBuffer);
        const MMRProfileTotals profileTotals = ExtractProfileTotals(jsonResp);
        if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
            !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
            if (ScheduleRetry(req, kTransientRetryDelay, "incomplete Tracker response")) return true;
            return false;
        }

        int bestMMR = 0;
        std::string bestTier = "Unranked";
        std::string bestPlaylistName = "best";
        std::map<std::string, int> playlistMMRs;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;

        for (const auto& seg : jsonResp["data"]["segments"]) {
            if (!seg.is_object() || !seg.contains("type") || !seg["type"].is_string() || seg["type"] != "playlist" ||
                !seg.contains("attributes") || !seg["attributes"].is_object()) {
                continue;
            }

            const auto& attrs = seg["attributes"];
            if (!attrs.contains("playlistId") || !attrs["playlistId"].is_number_integer()) continue;

            const std::string playlistName = PlaylistNameForTrackerId(attrs["playlistId"].get<int>());
            if (playlistName.empty() || !seg.contains("stats") || !seg["stats"].is_object()) continue;

            const auto& stats = seg["stats"];
            if (!stats.contains("rating") || !stats["rating"].is_object()) continue;
            const auto& rating = stats["rating"];
            if (!rating.contains("value") || !rating["value"].is_number()) continue;

            const int mmr = rating["value"].get<int>();
            std::string tier = "Unranked";
            if (stats.contains("tier") && stats["tier"].is_object()) {
                const auto& tierObj = stats["tier"];
                if (tierObj.contains("metadata") && tierObj["metadata"].is_object() &&
                    tierObj["metadata"].contains("name") && tierObj["metadata"]["name"].is_string()) {
                    tier = tierObj["metadata"]["name"].get<std::string>();
                }
            }

            std::string division;
            if (stats.contains("division") && stats["division"].is_object()) {
                const auto& divObj = stats["division"];
                if (divObj.contains("metadata") && divObj["metadata"].is_object() &&
                    divObj["metadata"].contains("name") && divObj["metadata"]["name"].is_string()) {
                    division = divObj["metadata"]["name"].get<std::string>();
                }
                if (division.empty() && divObj.contains("displayValue") && divObj["displayValue"].is_string()) {
                    division = divObj["displayValue"].get<std::string>();
                }
                if (division.empty() && divObj.contains("value") && divObj["value"].is_number()) {
                    switch (divObj["value"].get<int>()) {
                    case 1:
                        division = "Div I";
                        break;
                    case 2:
                        division = "Div II";
                        break;
                    case 3:
                        division = "Div III";
                        break;
                    case 4:
                        division = "Div IV";
                        break;
                    default:
                        break;
                    }
                }
            }

            if (!division.empty()) {
                const size_t pos = division.find("Division ");
                if (pos != std::string::npos) division.replace(pos, 9, "Div ");
                if (tier != "Unranked") tier += " " + division;
            }
            if (playlistName == "t") tier = GetTournamentTierForMmr(mmr);

            int matches = -1;
            if (stats.contains("matchesPlayed") && stats["matchesPlayed"].is_object() &&
                stats["matchesPlayed"].contains("value") && stats["matchesPlayed"]["value"].is_number()) {
                matches = stats["matchesPlayed"]["value"].get<int>();
            }

            if (ShouldReplacePlaylistBucket(playlistMMRs, playlistName, mmr)) {
                playlistMMRs[playlistName] = mmr;
                playlistTiers[playlistName] = tier;
            }
            if (matches >= 0) playlistMatches[playlistName] += matches;

            if (playlistName != "casual" && playlistName != "t" && mmr > bestMMR) {
                bestMMR = mmr;
                bestTier = tier;
                bestPlaylistName = playlistName;
            }
        }

        playlistMMRs["best"] = bestMMR;
        playlistTiers["best"] = bestTier;
        if (playlistMatches.count(bestPlaylistName)) {
            playlistMatches["best"] = playlistMatches[bestPlaylistName];
        }

        int postMatchMmr = 0;
        int postMatchMatches = -1;
        bool postMatchConfirmed = false;
        if (req.reason == MMRRequestReason::PostMatch) {
            const auto playlistIt = playlistMMRs.find(req.playlist);
            if (playlistIt != playlistMMRs.end()) postMatchMmr = playlistIt->second;
            const auto matchesIt = playlistMatches.find(req.playlist);
            if (matchesIt != playlistMatches.end()) postMatchMatches = matchesIt->second;

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
                const auto mmrIt = playlistMMRs.find(pendingReq.playlist);
                if (mmrIt == playlistMMRs.end()) continue;
                const auto matchesIt = playlistMatches.find(pendingReq.playlist);
                const int fetchedMatches =
                    matchesIt != playlistMatches.end() ? matchesIt->second : -1;
                const bool confirmed =
                    ReconcileTrackerResponse(pendingReq, mmrIt->second, fetchedMatches);
                if (!confirmed) {
                    EnsureProvisionalPoint(
                        pendingReq, pendingReq.previousMmr, mmrIt->second, fetchedMatches);
                }
            }
        }

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
                    playlistMMRs.begin(), playlistMMRs.end(),
                    [](const auto& entry) {
                        return entry.first != "best" && entry.second > 0;
                    });
                if (hasPlaylistMmr) {
                    m_state->game.preMatchMmrByGuid.emplace(
                        m_state->game.matchGuid,
                        LocalPreMatchMmrSnapshot{
                            .playlistMmrs = playlistMMRs,
                            .playlistMatches = playlistMatches});
                    std::cout
                        << "[MMRFetcher] Captured pre-match MMR snapshot: matchGuid="
                        << PrivacyLog::Sensitive(
                               m_state->game.matchGuid,
                               "match GUID")
                        << ".\n";
                }
            }

            if (m_state->game.roster.count(req.primaryId)) {
                auto& player = m_state->game.roster[req.primaryId];
                player.playlists = playlistMMRs;
                player.playlistTiers = playlistTiers;
                player.playlistMatches = playlistMatches;
                player.totalWins = profileTotals.totalWins;
                player.mmr = bestMMR;
                player.rankTier = bestTier;
                player.fetched = true;
                player.fetchFailed = false;
            }

            if (req.primaryId == m_state->game.myPrimaryId) {
                if (m_state->history.initialMmr == -1 && bestMMR > 0) {
                    m_state->history.initialMmr = bestMMR;
                }
                if (req.reason == MMRRequestReason::Roster && bestMMR > 0) {
                    m_state->history.mmrHistoryY.push_back(static_cast<float>(bestMMR));
                    m_state->history.mmrHistoryX.push_back(static_cast<float>(m_state->history.mmrHistoryY.size()));
                }

                for (const auto& [playlistName, fetchedMmr] : playlistMMRs) {
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
                      << " -> Best: " << bestMMR << "\n";
            m_state->game.version++;
            m_state->history.version++;
        }
        return false;
    } catch (const std::exception& e) {
        std::cout << "[MMRFetcher] JSON Parse Error: " << e.what() << "\n";
        if (ScheduleRetry(req, kTransientRetryDelay, "invalid Tracker response")) return true;

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }
}
