#include "network/mmr/MmrStatePublisher.hpp"
#include "network/mmr/MmrService.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <mutex>
#include <shared_mutex>

namespace MmrPublisher {

    namespace {

        class ScopedMmrHistoryOwner {
          public:
            ScopedMmrHistoryOwner(HistoryState& history, const std::string& requestId,
                                  const std::string& currentId)
                : m_history(history), m_restoreId(requestId == currentId ? "" : currentId) {
                m_history.SelectMmrOwner(requestId);
            }

            ~ScopedMmrHistoryOwner() {
                if (!m_restoreId.empty()) m_history.SelectMmrOwner(m_restoreId);
            }

          private:
            HistoryState& m_history;
            std::string m_restoreId;
        };

    } // namespace

    void PublishRosterFetchFailure(SessionState& state, const std::string& primaryId) {
        if (state.liveMatch) state.liveMatch->SetRosterFetchFailed(primaryId);
    }

    bool PublishCachedLocalRoster(SessionState& state,
                                  const std::string& primaryId,
                                  const std::map<std::string, int>& playlists,
                                  const std::map<std::string, std::string>& playlistTiers,
                                  const std::map<std::string, int>& playlistMatches,
                                  int bestMmr,
                                  const std::string& bestTier,
                                  int totalWins) {
        if (!state.liveMatch) return false;
        const auto snapshot = state.liveMatch->GetSnapshot();
        if (!snapshot || snapshot->myPrimaryId != primaryId || !snapshot->roster.count(primaryId)) return false;
        state.liveMatch->PostAction([primaryId, playlists, playlistTiers, playlistMatches, bestMmr, bestTier, totalWins](MatchState& match, SideEffects&) {
            if (match.myPrimaryId != primaryId) return;
            const auto playerIt = match.roster.find(primaryId);
            if (playerIt == match.roster.end()) return;
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
        return true;
    }

    bool PublishProfileResult(SessionState& state,
                              const MMRRequest& req,
                              const NormalizedProfileResult& profile,
                              const std::unordered_set<std::string>* playlistsAwaitingPostMatch) {
        std::unique_lock<std::shared_mutex> historyLock(state.history.mutex);
        const auto snapshot = state.liveMatch ? state.liveMatch->GetSnapshot() : nullptr;
        const bool fetchedThisSession = snapshot && req.sessionGeneration == snapshot->sessionGeneration;
        const bool isLocalPlayer = snapshot && req.primaryId == snapshot->myPrimaryId;
        if (state.liveMatch) {
            state.liveMatch->PostAction([req, profile, fetchedThisSession, isLocalPlayer](MatchState& match, SideEffects&) {
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
            });
        }
        if (isLocalPlayer && fetchedThisSession) {
            state.history.SelectMmrOwner(req.primaryId);
            if (state.history.initialMmr == -1 && profile.bestMmr > 0) {
                state.history.initialMmr = profile.bestMmr;
            }
            if (req.reason == MMRRequestReason::Roster && profile.bestMmr > 0) {
                state.history.mmrHistoryY.push_back(static_cast<float>(profile.bestMmr));
                state.history.mmrHistoryX.push_back(static_cast<float>(state.history.mmrHistoryY.size()));
            }

            for (const auto& [playlistName, fetchedMmr] : profile.playlistMMRs) {
                if (fetchedMmr <= 0 || playlistName == "best" || playlistName == "t" ||
                    playlistName == "casual" ||
                    (playlistsAwaitingPostMatch && playlistsAwaitingPostMatch->count(playlistName) > 0)) {
                    continue;
                }
                if (req.reason == MMRRequestReason::PostMatch && playlistName == req.playlist) continue;

                auto& history = state.history.playlistHistoryY[playlistName];
                if (history.empty() && state.history.playlistInitialMmr.count(playlistName) == 0) {
                    state.history.playlistInitialMmr[playlistName] = fetchedMmr;
                }
                if (history.empty() || static_cast<int>(std::lround(history.back())) != fetchedMmr) {
                    history.push_back(static_cast<float>(fetchedMmr));
                }
            }
            state.syncSessionMmrChangeLocked();
        }

        state.history.version++;
        return false;
    }

    std::optional<SessionRecap> PublishReconciledPoints(SessionState& state,
                                                        const ReconcilePublication& pub) {
        std::unique_lock<std::shared_mutex> historyLock(state.history.mutex);
        return PublishReconciledPointsLocked(state, pub);
    }

    std::optional<SessionRecap> PublishReconciledPointsLocked(SessionState& state,
                                                              const ReconcilePublication& pub) {
        const auto snapshot = state.liveMatch ? state.liveMatch->GetSnapshot() : nullptr;
        const uint64_t currentSessionGeneration = snapshot ? snapshot->sessionGeneration : 0;
        const std::string myPrimaryId = snapshot ? snapshot->myPrimaryId : std::string{};
        ScopedMmrHistoryOwner owner(state.history, pub.primaryId, myPrimaryId);
        auto& points = state.history.playlistMatchPoints[pub.playlist];
        auto& projection = state.history.playlistHistoryY[pub.playlist];

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
                    state.history.playlistInitialMmr.count(pub.playlist) == 0) {
                    state.history.playlistInitialMmr[pub.playlist] = update.sessionStartMmr;
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
                update.sessionStartMmr > 0 && state.history.playlistInitialMmr.count(pub.playlist) == 0) {
                state.history.playlistInitialMmr[pub.playlist] = update.sessionStartMmr;
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
            state.history.playlistInitialMmr.count(pub.playlist) == 0) {
            state.history.playlistInitialMmr[pub.playlist] = seedMmr;
            projection.push_back(static_cast<float>(seedMmr));
        }

        std::optional<SessionRecap> updatedRecap;
        auto recap = snapshot ? snapshot->lastSessionRecap : SessionRecap{};
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
                if (state.liveMatch) state.liveMatch->UpdateLastSessionRecapMmr(
                    recap.totals.mmrChangeByPlaylist, recap.totals.totalMmrChange);
            }
        }

        if (pub.shouldUpdatePreMatchSnapshots && pub.fetchedMatches >= 0 && state.liveMatch) {
            state.liveMatch->PostAction([pub](MatchState& match, SideEffects&) {
                if (!match.inMatch || match.matchFinalized || pub.primaryId != match.myPrimaryId) return;
                for (auto& [snapshotGuid, baseline] : match.preMatchMmrByGuid) {
                    if (pub.protectedSnapshotGuids.count(snapshotGuid)) continue;
                    auto matchesIt = baseline.playlistMatches.find(pub.playlist);
                    if (matchesIt == baseline.playlistMatches.end() || matchesIt->second < 0 ||
                        matchesIt->second >= pub.fetchedMatches) continue;
                    matchesIt->second = pub.fetchedMatches;
                    baseline.playlistMmrs[pub.playlist] = pub.updates.empty() ? 0 : pub.updates.back().mmr;
                }
            });
        }
        if (pub.primaryId == myPrimaryId) state.syncSessionMmrChangeLocked();
        state.history.version++;
        return updatedRecap;
    }

    void PublishProvisionalPoint(SessionState& state,
                                 const ProvisionalPublication& pub) {
        std::unique_lock<std::shared_mutex> historyLock(state.history.mutex);
        PublishProvisionalPointLocked(state, pub);
    }

    void PublishProvisionalPointLocked(SessionState& state,
                                       const ProvisionalPublication& pub) {

        const auto snapshot = state.liveMatch ? state.liveMatch->GetSnapshot() : nullptr;
        if (!snapshot || pub.sessionGeneration != snapshot->sessionGeneration) return;
        ScopedMmrHistoryOwner owner(state.history, pub.primaryId, snapshot->myPrimaryId);

        auto& points = state.history.playlistMatchPoints[pub.playlist];
        auto& projection = state.history.playlistHistoryY[pub.playlist];

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
                state.history.playlistInitialMmr.count(pub.playlist) == 0) {
                state.history.playlistInitialMmr[pub.playlist] = pub.sessionStartMmr;
            }
        } else {
            return;
        }

        if (pub.primaryId == snapshot->myPrimaryId) state.syncSessionMmrChangeLocked();
        state.history.version++;
    }

} // namespace MmrPublisher
