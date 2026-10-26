#include "network/mmr/MmrStatePublisher.hpp"
#include "network/mmr/MmrService.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace MmrPublisher {

    namespace {

        class ScopedMmrHistoryOwner {
          public:
            ScopedMmrHistoryOwner(database::store::HistoryData& data, const std::string& requestId,
                                  const std::string& currentId)
                : m_data(data), m_restoreId(requestId == currentId ? "" : currentId) {
                m_data.SelectMmrOwner(requestId);
            }

            ~ScopedMmrHistoryOwner() {
                if (!m_restoreId.empty()) m_data.SelectMmrOwner(m_restoreId);
            }

          private:
            database::store::HistoryData& m_data;
            std::string m_restoreId;
        };

        void SyncSessionMmrChange(const database::store::HistoryData& data, MatchState& match) {
            std::map<std::string, int> changes;
            for (const auto& [playlist, projection] : data.playlistHistoryY) {
                const auto initialIt = data.playlistInitialMmr.find(playlist);
                if (projection.empty() || initialIt == data.playlistInitialMmr.end() || initialIt->second <= 0) continue;
                changes[playlist] = static_cast<int>(std::lround(projection.back())) - initialIt->second;
            }
            match.sessionTotals.mmrChangeByPlaylist = std::move(changes);
            match.sessionTotals.totalMmrChange = static_cast<float>(
                CalculateTrackedSessionMmrChange(match.sessionTotals.mmrChangeByPlaylist));
        }

    } // namespace

    void PublishRosterFetchFailure(SessionState& state, const std::string& primaryId) {
        if (state.liveMatch) state.liveMatch->SetRosterFetchFailed(primaryId);
    }

    bool PublishCachedLocalRoster(SessionState& state,
                                  const std::string& primaryId,
                                  uint64_t sessionGeneration,
                                  const std::map<std::string, int>& playlists,
                                  const std::map<std::string, std::string>& playlistTiers,
                                  const std::map<std::string, int>& playlistMatches,
                                  int bestMmr,
                                  const std::string& bestTier,
                                  int totalWins) {
        if (!state.liveMatch) return false;
        bool published = false;
        state.liveMatch->Update([&](MatchState& match) {
            if (match.myPrimaryId != primaryId || match.sessionGeneration != sessionGeneration) return;
            const auto playerIt = match.roster.find(primaryId);
            if (playerIt == match.roster.end()) return;
            published = true;
            auto& player = playerIt->second;
            player.playlists = playlists;
            player.playlistTiers = playlistTiers;
            player.playlistMatches = playlistMatches;
            player.mmr = bestMmr;
            player.rankTier = bestTier;
            if (totalWins >= 0) player.totalWins = totalWins;
            player.fetched = true;
            player.fetchFailed = false;
            if (match.inMatch && !match.matchGuid.empty() && !match.preMatchMmrByGuid.count(match.matchGuid)) {
                const bool hasPlaylistMmr = std::any_of(playlists.begin(), playlists.end(),
                                                        [](const auto& entry) { return entry.first != "best" && entry.second > 0; });
                if (hasPlaylistMmr) {
                    match.preMatchMmrByGuid.emplace(match.matchGuid, LocalPreMatchMmrSnapshot{
                                                                         .playlistMmrs = playlists, .playlistMatches = playlistMatches});
                }
            }
        });
        return published;
    }

    bool PublishProfileResult(SessionState& state,
                              const MMRRequest& req,
                              const NormalizedProfileResult& profile,
                              const std::unordered_set<std::string>* playlistsAwaitingPostMatch) {
        if (!state.liveMatch) return false;
        state.history.Mutate([&](database::store::HistoryData& data) {
            state.liveMatch->Update([&](MatchState& match) {
                const bool fetchedThisSession = req.sessionGeneration == match.sessionGeneration;
                const bool isLocalPlayer = req.primaryId == match.myPrimaryId;
                if (req.reason == MMRRequestReason::Roster && fetchedThisSession &&
                    req.primaryId == match.myPrimaryId && match.inMatch && !match.matchGuid.empty() &&
                    !match.preMatchMmrByGuid.count(match.matchGuid)) {
                    const bool hasPlaylistMmr = std::any_of(profile.playlistMMRs.begin(), profile.playlistMMRs.end(),
                                                            [](const auto& entry) { return entry.first != "best" && entry.second > 0; });
                    if (hasPlaylistMmr) {
                        match.preMatchMmrByGuid.emplace(match.matchGuid, LocalPreMatchMmrSnapshot{
                                                                             .playlistMmrs = profile.playlistMMRs, .playlistMatches = profile.playlistMatches});
                    }
                }
                auto rosterIt = match.roster.find(req.primaryId);
                if (rosterIt == match.roster.end() && isLocalPlayer) {
                    rosterIt = match.roster.try_emplace(req.primaryId).first;
                }
                if (rosterIt != match.roster.end() && (isLocalPlayer || fetchedThisSession)) {
                    auto& player = rosterIt->second;
                    player.primaryId = req.primaryId;
                    if (player.name.empty()) player.name = req.name;
                    player.mmr = profile.bestMmr;
                    player.rankTier = profile.bestTier;
                    player.playlists = profile.playlistMMRs;
                    player.playlistTiers = profile.playlistTiers;
                    player.playlistMatches = profile.playlistMatches;
                    if (profile.totalWins >= 0) player.totalWins = profile.totalWins;
                    player.rankVerificationSource = profile.rankVerificationSource;
                    player.fetched = true;
                    player.fetchFailed = false;
                }
                if (isLocalPlayer && fetchedThisSession) {
                    data.SelectMmrOwner(req.primaryId);
                    if (data.initialMmr == -1 && profile.bestMmr > 0) {
                        data.initialMmr = profile.bestMmr;
                    }
                    if (req.reason == MMRRequestReason::Roster && profile.bestMmr > 0) {
                        data.mmrHistoryY.push_back(static_cast<float>(profile.bestMmr));
                        data.mmrHistoryX.push_back(static_cast<float>(data.mmrHistoryY.size()));
                    }

                    for (const auto& [playlistName, fetchedMmr] : profile.playlistMMRs) {
                        if (fetchedMmr <= 0 || playlistName == "best" || playlistName == "t" ||
                            playlistName == "casual" ||
                            (playlistsAwaitingPostMatch && playlistsAwaitingPostMatch->count(playlistName) > 0)) {
                            continue;
                        }
                        if (req.reason == MMRRequestReason::PostMatch && playlistName == req.playlist) continue;

                        auto& playlistHistory = data.playlistHistoryY[playlistName];
                        if (playlistHistory.empty() && data.playlistInitialMmr.count(playlistName) == 0) {
                            data.playlistInitialMmr[playlistName] = fetchedMmr;
                        }
                        if (playlistHistory.empty() || static_cast<int>(std::lround(playlistHistory.back())) != fetchedMmr) {
                            playlistHistory.push_back(static_cast<float>(fetchedMmr));
                        }
                    }
                    SyncSessionMmrChange(data, match);
                }
            });
        });
        return false;
    }

    std::optional<SessionRecap> PublishReconciledPoints(SessionState& state,
                                                        const ReconcilePublication& pub) {
        std::optional<SessionRecap> result;
        state.history.Mutate([&](database::store::HistoryData& data) {
            result = PublishReconciledPointsLocked(state, data, pub);
        });
        return result;
    }

    std::optional<SessionRecap> PublishReconciledPointsLocked(database::store::HistoryData& data,
                                                              const ReconcilePublication& pub, MatchState& match) {
        std::optional<SessionRecap> updatedRecap;
        const uint64_t currentSessionGeneration = match.sessionGeneration;
        const std::string& myPrimaryId = match.myPrimaryId;
        ScopedMmrHistoryOwner owner(data, pub.primaryId, myPrimaryId);
        auto& points = data.playlistMatchPoints[pub.playlist];
        auto& projection = data.playlistHistoryY[pub.playlist];

        for (const auto& update : pub.updates) {
            if (update.sessionGeneration != currentSessionGeneration) continue;

            auto pointIt = std::find_if(points.begin(), points.end(), [&](const SessionMmrPoint& pt) {
                return pt.matchGuid == update.matchGuid;
            });

            if (pointIt == points.end()) {
                const size_t historyIndex = projection.size();
                projection.push_back(static_cast<float>(update.mmr));
                points.push_back(SessionMmrPoint{
                    .matchGuid = update.matchGuid,
                    .historyIndex = historyIndex,
                    .mmr = update.mmr,
                    .trackerMatchesPlayed = update.matchesPlayed,
                    .trackerCovered = true,
                    .valueEstimated = update.valueEstimated});
                pointIt = std::prev(points.end());
                if (historyIndex == 0 && update.sessionStartMmr > 0 &&
                    data.playlistInitialMmr.count(pub.playlist) == 0) {
                    data.playlistInitialMmr[pub.playlist] = update.sessionStartMmr;
                }
            } else {
                pointIt->mmr = update.mmr;
                pointIt->trackerMatchesPlayed = update.matchesPlayed;
                pointIt->trackerCovered = true;
                pointIt->valueEstimated = update.valueEstimated;
                if (pointIt->historyIndex < projection.size()) {
                    projection[pointIt->historyIndex] = static_cast<float>(update.mmr);
                }
            }
            if (pointIt != points.end() && pointIt->historyIndex == 0 &&
                update.sessionStartMmr > 0 && data.playlistInitialMmr.count(pub.playlist) == 0) {
                data.playlistInitialMmr[pub.playlist] = update.sessionStartMmr;
            }
        }

        int seedMmr = pub.currentSessionSeedMmr;
        if (pub.sessionGeneration != currentSessionGeneration) {
            seedMmr = 0;
            const bool pendingPreviousSession = std::any_of(
                pub.pendingSessionGenerations.begin(), pub.pendingSessionGenerations.end(),
                [&](uint64_t generation) { return generation != currentSessionGeneration; });
            if (!pendingPreviousSession && !pub.updates.empty() &&
                pub.updates.back().sessionGeneration != currentSessionGeneration) {
                seedMmr = pub.updates.back().mmr;
            }
        }
        if (seedMmr > 0 && projection.empty() &&
            data.playlistInitialMmr.count(pub.playlist) == 0) {
            data.playlistInitialMmr[pub.playlist] = seedMmr;
            projection.push_back(static_cast<float>(seedMmr));
        }

        auto recap = match.lastSessionRecap;
        if (recap.valid && recap.sessionGeneration != currentSessionGeneration &&
            recap.mmrOwnerPrimaryId == pub.primaryId &&
            pub.pendingSessionGenerations.count(recap.sessionGeneration) == 0) {
            int recapLatestMmr = 0;
            for (const auto& update : pub.updates) {
                if (update.sessionGeneration == recap.sessionGeneration) {
                    recapLatestMmr = update.mmr;
                }
            }
            const auto recapInitialIt = recap.playlistInitialMmr.find(pub.playlist);
            if (recapLatestMmr > 0 &&
                recapInitialIt != recap.playlistInitialMmr.end() && recapInitialIt->second > 0) {
                recap.totals.mmrChangeByPlaylist[pub.playlist] = recapLatestMmr - recapInitialIt->second;
                recap.totals.totalMmrChange = static_cast<float>(
                    CalculateTrackedSessionMmrChange(recap.totals.mmrChangeByPlaylist));
                updatedRecap = recap;
                match.lastSessionRecap = recap;
            }
        }

        if (pub.shouldUpdatePreMatchSnapshots && pub.fetchedMatches >= 0 &&
            match.inMatch && !match.matchFinalized && pub.primaryId == match.myPrimaryId) {
            for (auto& [snapshotGuid, baseline] : match.preMatchMmrByGuid) {
                if (pub.protectedSnapshotGuids.count(snapshotGuid)) continue;
                auto matchesIt = baseline.playlistMatches.find(pub.playlist);
                if (matchesIt == baseline.playlistMatches.end() || matchesIt->second < 0 ||
                    matchesIt->second >= pub.fetchedMatches) continue;
                matchesIt->second = pub.fetchedMatches;
                baseline.playlistMmrs[pub.playlist] = pub.updates.empty() ? 0 : pub.updates.back().mmr;
            }
        }
        if (pub.primaryId == myPrimaryId) SyncSessionMmrChange(data, match);
        return updatedRecap;
    }

    std::optional<SessionRecap> PublishReconciledPointsLocked(SessionState& state, database::store::HistoryData& data,
                                                              const ReconcilePublication& pub) {
        if (state.liveMatch) {
            return state.liveMatch->Update([&](MatchState& match) {
                return PublishReconciledPointsLocked(data, pub, match);
            });
        }
        MatchState emptyMatch;
        return PublishReconciledPointsLocked(data, pub, emptyMatch);
    }

    void PublishProvisionalPoint(SessionState& state,
                                 const ProvisionalPublication& pub) {
        state.history.Mutate([&](database::store::HistoryData& data) {
            PublishProvisionalPointLocked(state, data, pub);
        });
    }

    void PublishProvisionalPointLocked(database::store::HistoryData& data,
                                       const ProvisionalPublication& pub, MatchState& match) {
        if (pub.sessionGeneration != match.sessionGeneration) return;
        ScopedMmrHistoryOwner owner(data, pub.primaryId, match.myPrimaryId);

        auto& points = data.playlistMatchPoints[pub.playlist];
        auto& projection = data.playlistHistoryY[pub.playlist];

        auto pointIt = std::find_if(points.begin(), points.end(), [&](const SessionMmrPoint& point) {
            return point.matchGuid == pub.matchGuid;
        });

        if (pointIt == points.end()) {
            const size_t historyIndex = projection.size();
            projection.push_back(static_cast<float>(pub.provisionalMmr));
            points.push_back(SessionMmrPoint{
                .matchGuid = pub.matchGuid,
                .historyIndex = historyIndex,
                .mmr = pub.provisionalMmr,
                .trackerMatchesPlayed = pub.matchesPlayed,
                .trackerCovered = false,
                .valueEstimated = true});

            if (historyIndex == 0 && pub.sessionStartMmr > 0 &&
                data.playlistInitialMmr.count(pub.playlist) == 0) {
                data.playlistInitialMmr[pub.playlist] = pub.sessionStartMmr;
            }
        } else {
            return;
        }

        if (pub.primaryId == match.myPrimaryId) SyncSessionMmrChange(data, match);
    }

    void PublishProvisionalPointLocked(SessionState& state, database::store::HistoryData& data,
                                       const ProvisionalPublication& pub) {
        if (!state.liveMatch) return;
        state.liveMatch->Update([&](MatchState& match) {
            PublishProvisionalPointLocked(data, pub, match);
        });
    }

} // namespace MmrPublisher
