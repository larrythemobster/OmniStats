#include "core/reducer/TelemetryReducer.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <utility>

#include "core/config/Config.hpp"
#include "core/game/ArenaNames.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "core/reducer/TelemetryReducerDetail.hpp"
#include "utils/PrivacyLog.hpp"

using namespace TelemetryReducerDetail;

namespace TelemetryReducerDetail {

    void CapturePreMatchMmr(MatchState& state) {
        const std::string& matchGuid = state.matchGuid;
        if (matchGuid.empty() ||
            state.preMatchMmrByGuid.count(matchGuid) > 0 ||
            state.myPrimaryId.empty()) {
            return;
        }

        const auto playerIt = state.roster.find(state.myPrimaryId);
        if (playerIt == state.roster.end()) return;

        bool hasPlaylistMmr = false;
        for (const auto& [playlist, mmr] : playerIt->second.playlists) {
            if (mmr > 0) {
                hasPlaylistMmr = true;
                break;
            }
        }
        if (!hasPlaylistMmr) return;

        state.preMatchMmrByGuid.emplace(
            matchGuid,
            LocalPreMatchMmrSnapshot{
                .playlistMmrs = playerIt->second.playlists,
                .playlistMatches = playerIt->second.playlistMatches});
    }

    bool AttachTerminalGuidToCurrent(MatchState& state, const std::string& eventMatchGuid) {
        if (eventMatchGuid.empty() ||
            !state.inMatch ||
            state.matchFinalized ||
            !state.matchGuid.empty() ||
            state.missingGuidAssociationBlockedByReconnect ||
            state.finalizedMatchGuids.count(eventMatchGuid) > 0 ||
            state.pendingDestroyedMatches.count(eventMatchGuid) > 0) {
            return false;
        }

        auto guidlessSnapshot = state.preMatchMmrByGuid.extract("");
        if (!guidlessSnapshot.empty()) {
            guidlessSnapshot.key() = eventMatchGuid;
            state.preMatchMmrByGuid.insert(std::move(guidlessSnapshot));
        }
        state.matchGuid = eventMatchGuid;
        CapturePreMatchMmr(state);
        return true;
    }

    void UpdateLifecycleSignals(MatchState& state, const TelemetryEvents::UpdateStateEvent& ev) {
        if (TelemetryEvents::HasExplicitLocalForfeitSignal(ev.forfeitScopes, state.myTeam)) {
            state.explicitLocalForfeit = true;
        }

        if (PlaylistMetadata::HasAuthoritativeId(state.playlistId)) {
            state.excludedEarlyExitContext = false;
            state.earlyExitExclusionReason.clear();
            if (!PlaylistMetadata::IsKnown(state.playlistId)) {
                state.excludedEarlyExitContext = true;
                state.earlyExitExclusionReason = "unknown_playlist_id";
            } else if (const char* reason = PlaylistMetadata::NonRecordableReason(state.playlistId)) {
                state.excludedEarlyExitContext = true;
                state.earlyExitExclusionReason = reason;
            } else if (PlaylistMetadata::IsCasual(state.playlistId)) {
                state.excludedEarlyExitContext = true;
                state.earlyExitExclusionReason = "casual_playlist";
            }
            state.fallbackCasualContext = false;
            state.fallbackNonRecordableContext = false;
            state.fallbackNonRecordableReason.clear();
        } else {
            if (ev.explicitNonRecordable) {
                state.fallbackNonRecordableContext = true;
                state.fallbackNonRecordableReason = ev.fallbackNonRecordableReason.empty()
                                                        ? "explicit_non_competitive_context"
                                                        : ev.fallbackNonRecordableReason;
            } else if (ev.explicitCasual) {
                state.fallbackCasualContext = true;
            }

            if (state.fallbackNonRecordableContext) {
                state.excludedEarlyExitContext = true;
                state.earlyExitExclusionReason = state.fallbackNonRecordableReason;
            } else if (state.fallbackCasualContext) {
                state.excludedEarlyExitContext = true;
                state.earlyExitExclusionReason = "casual_context";
            }
        }
    }

    void HandleUpdateState(MatchState& state,
                           const TelemetryEvents::UpdateStateEvent& ev,
                           const ReducerContext& ctx,
                           SideEffects& effects) {
        const int64_t nowMs = ctx.nowMs > 0 ? ctx.nowMs : SteadyNowMs();
        if (!state.nonLiveReplayActive) {
            (void)AcceptsUiMatchEvent(state, ev.matchGuid, nowMs, effects);
        }

        bool gameReplayActive = ev.bReplay;
        bool isSpectator = ev.bSpectator;
        state.inReplay = gameReplayActive;

        if (ev.bOvertime) {
            state.bOvertime = true;
            if (!state.overtimeStartedAtMs.has_value()) {
                state.overtimeStartedAtMs = nowMs;
            }
        }

        if (ev.timeSeconds.has_value()) {
            const float clock = *ev.timeSeconds;
            state.timeSeconds = clock;
            state.hasObservedMatchClock = std::isfinite(clock) && clock >= 0.0f && clock <= 3600.0f;
            if (state.hasObservedMatchClock) state.lastMatchClockSampleAtMs = nowMs;
            if (!state.bOvertime) {
                state.regulationClockStart = (std::max)(state.regulationClockStart.value_or(clock), clock);
                state.regulationClockLatest = clock;
            }
        }
        if (state.bOvertime) state.regulationClockLatest = 0.0f;

        if (!ev.arena.empty()) {
            std::string currentArenaAsset = ev.arenaAsset.empty() ? ev.arena : ev.arenaAsset;
            std::string currentArena = ArenaNames::DisplayName(currentArenaAsset);
            if (!currentArena.empty() &&
                (currentArena != state.arenaName || currentArenaAsset != state.arenaAsset)) {
                const bool previousIsNonRecordable =
                    state.fallbackNonRecordableContext ||
                    PlaylistMetadata::IsNonRecordable(state.playlistId);
                const bool establishedActiveMatch =
                    state.inMatch && state.roundEverStarted && !previousIsNonRecordable;

                if (establishedActiveMatch) {
                    state.arenaName = currentArena;
                    state.arenaAsset = currentArenaAsset;
                    effects.pushDiscord = true;
                    effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
                } else {
                    const std::string matchGuid = state.matchGuid;
                    const int previousPlaylistId = state.playlistId;
                    const bool startsNewLifecycle = !state.inMatch || previousIsNonRecordable;

                    LocalPreMatchMmrSnapshot preservedMmrSnapshot;
                    bool hasPreservedMmrSnapshot = false;
                    const auto snapshotIt = state.preMatchMmrByGuid.find(matchGuid);
                    if (snapshotIt != state.preMatchMmrByGuid.end()) {
                        preservedMmrSnapshot = std::move(snapshotIt->second);
                        hasPreservedMmrSnapshot = true;
                    }

                    const int64_t savedMatchStartMs = state.lastMatchStartMs;
                    const int64_t savedFirstCountdownMs = state.firstCountdownOfMatchMs;
                    const int64_t savedLastCountdownMs = state.lastCountdownMs;
                    const int64_t savedLastGoalMs = state.lastGoalMs;
                    const int64_t savedLastMatchEndMs = state.lastMatchEndMs;
                    const int64_t savedLastPodiumMs = state.lastPodiumMs;
                    const bool savedInGoalReplay = state.inGoalReplay;

                    state.ResetMatch(currentArena, currentArenaAsset);
                    if (startsNewLifecycle) {
                        state.matchEvents.clear();
                        state.analytics = MatchAnalyticsTracker{};
                        state.eventNativeKeys.clear();
                        state.lastEventSignature.clear();
                        state.activeMatchGeneration = ++state.nextMatchGeneration;
                        state.missingGuidAssociationBlockedByReconnect = false;
                        state.lastMatchStartMs = nowMs;
                        state.firstCountdownOfMatchMs = 0;
                        state.lastCountdownMs = 0;
                        state.lastGoalMs = 0;
                        state.lastMatchEndMs = 0;
                        state.lastPodiumMs = 0;
                        state.inGoalReplay = false;
                        state.uiMatchGuid.clear();
                        state.countdownSeenThisRound = false;
                        state.ResetTiming();
                        effects.uiUpdate = UiStateUpdate{.resetTimestamps = true, .resetMatchStartMs = nowMs};
                    } else {
                        state.lastMatchStartMs = savedMatchStartMs > 0 ? savedMatchStartMs : nowMs;
                        state.firstCountdownOfMatchMs = savedFirstCountdownMs;
                        state.lastCountdownMs = savedLastCountdownMs;
                        state.lastGoalMs = savedLastGoalMs;
                        state.lastMatchEndMs = savedLastMatchEndMs;
                        state.lastPodiumMs = savedLastPodiumMs;
                        state.inGoalReplay = savedInGoalReplay;
                    }

                    if (!startsNewLifecycle && hasPreservedMmrSnapshot) {
                        state.preMatchMmrByGuid.emplace(matchGuid, std::move(preservedMmrSnapshot));
                    } else if (startsNewLifecycle) {
                        state.matchGuid.clear();
                    }

                    if (!startsNewLifecycle && !ev.playlistId.has_value() &&
                        PlaylistMetadata::HasAuthoritativeId(previousPlaylistId)) {
                        state.playlistId = previousPlaylistId;
                    }

                    state.roundActive = false;
                    state.autoSwitchedPlaylistCategory = MmrCategory::Best;
                    state.followedGraphPlaylistCategory = MmrCategory::Best;
                    state.lastPlayerBoost.clear();
                    state.lastPlayerSeenMs.clear();
                    effects.pushDiscord = true;
                    effects.discordSnapshot = TelemetryReducer::BuildDiscordSnapshot(state);
                }
            }
        }

        if (ev.playlistId.has_value()) {
            const int newPlaylistId = *ev.playlistId;
            const int currentPlaylistId = state.playlistId;
            const bool latchCurrentMatchPlaylist =
                state.inMatch && state.roundEverStarted && !state.matchFinalized &&
                PlaylistMetadata::IsKnown(currentPlaylistId) &&
                !PlaylistMetadata::IsNonRecordable(currentPlaylistId) &&
                newPlaylistId != currentPlaylistId;

            if (!latchCurrentMatchPlaylist) {
                const bool previousIsNonRecordable =
                    state.fallbackNonRecordableContext ||
                    PlaylistMetadata::IsNonRecordable(currentPlaylistId);
                const bool nonRecordableToDifferentPlaylist =
                    state.inMatch && previousIsNonRecordable && newPlaylistId != currentPlaylistId;

                if (nonRecordableToDifferentPlaylist) {
                    LocalPreMatchMmrSnapshot initialMmrSnapshot;
                    bool hasInitialMmrSnapshot = false;
                    if (!state.myPrimaryId.empty()) {
                        const auto playerIt = state.roster.find(state.myPrimaryId);
                        if (playerIt != state.roster.end() && !playerIt->second.playlists.empty()) {
                            initialMmrSnapshot.playlistMmrs = playerIt->second.playlists;
                            initialMmrSnapshot.playlistMatches = playerIt->second.playlistMatches;
                            hasInitialMmrSnapshot = true;
                        }
                    }

                    const std::string matchGuid = state.matchGuid;
                    const std::string currentArena = state.arenaName;
                    const std::string currentArenaAsset = state.arenaAsset;
                    state.ResetMatch(currentArena, currentArenaAsset);
                    state.matchEvents.clear();
                    state.analytics = MatchAnalyticsTracker{};
                    state.eventNativeKeys.clear();
                    state.lastEventSignature.clear();
                    state.lastMatchStartMs = nowMs;
                    state.activeMatchGeneration = ++state.nextMatchGeneration;
                    state.matchGuid = matchGuid;
                    state.uiMatchGuid = matchGuid;
                    state.missingGuidAssociationBlockedByReconnect = false;
                    if (hasInitialMmrSnapshot) {
                        state.preMatchMmrByGuid.emplace(matchGuid, std::move(initialMmrSnapshot));
                    }
                    state.ResetTiming();
                    state.roundActive = false;
                    state.countdownSeenThisRound = false;
                    state.autoSwitchedPlaylistCategory = MmrCategory::Best;
                    state.followedGraphPlaylistCategory = MmrCategory::Best;
                    state.lastPlayerBoost.clear();
                    state.lastPlayerSeenMs.clear();
                }
                state.playlistId = newPlaylistId;
            }
        }

        if (ev.hasTeamScores) {
            state.score = ev.teamScores;
        }

        UpdateLifecycleSignals(state, ev);

        if (isSpectator && !gameReplayActive) {
            state.localPlayerWasSpectator = true;
        }

        if (gameReplayActive) return;

        std::array<int, 2> teamCounts{0, 0};
        for (const auto& p : ev.players) {
            if (p.team == 0 || p.team == 1) {
                teamCounts[p.team]++;
            }
        }

        if (!PlaylistMetadata::HasAuthoritativeId(state.playlistId)) {
            state.legacyCurrentTeamPlayersSeen = teamCounts;
            state.legacyMaxTeamPlayersSeen[0] = (std::max)(state.legacyMaxTeamPlayersSeen[0], teamCounts[0]);
            state.legacyMaxTeamPlayersSeen[1] = (std::max)(state.legacyMaxTeamPlayersSeen[1], teamCounts[1]);
            const int observedPlayerCount = teamCounts[0] + teamCounts[1];
            state.legacyMaxPlayersSeen = (std::max)(state.legacyMaxPlayersSeen, observedPlayerCount);
        }

        for (const auto& p : ev.players) {
            const std::string& pid = p.primaryId;
            const int team = p.team;
            if (pid.empty()) continue;

            if (p.boost.has_value()) {
                const int currentBoost = *p.boost;
                if (state.roundActive) {
                    const auto previous = state.lastPlayerBoost.find(pid);
                    if (previous != state.lastPlayerBoost.end()) {
                        const int lastBoost = previous->second;
                        const bool respawning = state.boostRespawnPending.count(pid) != 0;
                        if (respawning && currentBoost > 0 && currentBoost != lastBoost) {
                            state.boostRespawnPending.erase(pid);
                        } else if (!respawning && currentBoost > lastBoost) {
                            const int diff = currentBoost - lastBoost;
                            state.currentMatch.boostPickedUp += diff;
                            if (pid == state.myPrimaryId) {
                                state.currentMatch.boostPickedUpSelf += diff;
                            }
                        }
                    }
                }
                state.lastPlayerBoost[pid] = currentBoost;
            }

            if (team == 0 || team == 1) {
                auto rosterIt = state.roster.find(pid);
                if (rosterIt == state.roster.end()) {
                    bool isBot = (pid.rfind("Unknown|", 0) == 0);
                    state.roster[pid] = PlayerData{
                        .primaryId = pid,
                        .name = p.name,
                        .team = team,
                        .fetched = isBot,
                        .enqueued = true};
                    effects.fetchMmrQueue.emplace_back(pid, p.name);
                    if (!isBot) effects.fetchEncounterQueue.push_back(pid);
                } else {
                    rosterIt->second.name = p.name;
                    rosterIt->second.team = team;
                }

                auto& playerRef = state.roster[pid];
                if (p.score.has_value()) playerRef.score = *p.score;
                if (p.goals.has_value()) playerRef.goals = *p.goals;
                if (p.shots.has_value()) playerRef.shots = *p.shots;
                if (p.assists.has_value()) playerRef.assists = *p.assists;
                if (p.saves.has_value()) playerRef.saves = *p.saves;
                if (p.demos.has_value()) playerRef.demos = *p.demos;
                if (p.touches.has_value()) playerRef.touches = *p.touches;
                if (p.carTouches.has_value()) playerRef.carTouches = *p.carTouches;
                state.matchRoster[pid] = playerRef;
            }
        }

        if (!isSpectator) {
            bool hasLocalPlayerFeeds = false;
            bool seenMyIdInLobby = false;
            std::unordered_set<std::string> currentCandidates;
            const std::string& cameraTargetId = ev.cameraTargetPlayerId;
            std::string mySavedId = state.myPrimaryId;
            if (mySavedId != state.lastKnownMyId) {
                state.lastKnownMyId = mySavedId;
                state.identityCandidates.clear();
                state.missedMyIdCount = 0;
            }
            const std::string lifetimeCategory = MmrCategoryToString(ctx.graphMmrCategory);
            const auto requestAccountRefresh = [&](const std::string& primaryId) {
                effects.fetchLifetimeHistory = true;
                effects.lifetimePrimaryId = primaryId;
                effects.lifetimeCategory = lifetimeCategory;
                effects.refreshDbStats = true;
                effects.refreshStatsPrimaryId = primaryId;
            };
            const auto adoptIdentity = [&](const std::string& primaryId) {
                state.myPrimaryId = primaryId;
                effects.selectMmrOwner = primaryId;
                for (const auto& p : ev.players) {
                    if (p.primaryId == primaryId) {
                        if (p.team != -1) state.myTeam = p.team;
                        break;
                    }
                }
                effects.persistLocalPrimaryId = primaryId;
                state.identityCandidates.clear();
                if (const auto selfIt = state.roster.find(primaryId); selfIt != state.roster.end()) {
                    auto& self = selfIt->second;
                    if (!self.enqueued && self.mmr == 0) {
                        self.enqueued = true;
                        effects.fetchMmrQueue.emplace_back(primaryId, self.name);
                    }
                }
                requestAccountRefresh(primaryId);
                RequeueEncounterRecords(state, effects);
            };

            for (const auto& p : ev.players) {
                const std::string& pid = p.primaryId;
                if (pid.empty() || p.team == -1) continue;
                std::string platform = pid.substr(0, pid.find('|'));
                std::transform(platform.begin(), platform.end(), platform.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                const bool isPcPlatform = pid.find('|') != std::string::npos &&
                                          (platform == "steam" || platform == "epic" || platform == "epicgames");
                if (isPcPlatform && !cameraTargetId.empty() && pid == cameraTargetId) {
                    hasLocalPlayerFeeds = true;
                    currentCandidates.insert(pid);
                }
                if (!mySavedId.empty() && pid == mySavedId) {
                    seenMyIdInLobby = true;
                    if (state.myTeam == -1) {
                        state.myTeam = p.team;
                        requestAccountRefresh(mySavedId);
                    }
                }
            }

            if (hasLocalPlayerFeeds) {
                // A saved id missing from the lobby while one local feed is present means the user switched accounts.
                if (!mySavedId.empty() && !seenMyIdInLobby && !currentCandidates.empty()) {
                    const std::string newId = *std::min_element(currentCandidates.begin(), currentCandidates.end());
                    if (newId != mySavedId) {
                        std::cout << "[Identity] Detected account switch. New ID: "
                                  << PrivacyLog::Sensitive(newId, "player ID") << "\n";
                        adoptIdentity(newId);
                        state.missedMyIdCount = 0;
                    }
                }

                if (!mySavedId.empty()) {
                    if (seenMyIdInLobby) {
                        state.missedMyIdCount = 0;
                    } else if (++state.missedMyIdCount >= 2) {
                        std::cout << "[Identity] Missed saved ID " << PrivacyLog::Sensitive(mySavedId, "player ID")
                                  << " for 2 matches. Resetting...\n";
                        state.myPrimaryId.clear();
                        state.myTeam = -1;
                        mySavedId.clear();
                        state.missedMyIdCount = 0;
                        state.identityCandidates.clear();
                        effects.persistLocalPrimaryId = std::string{};
                    }
                }

                if (mySavedId.empty() && !currentCandidates.empty()) {
                    if (state.identityCandidates.empty()) {
                        state.identityCandidates = currentCandidates;
                        std::cout << "[Identity] Process of elimination started. Candidates: "
                                  << state.identityCandidates.size() << "\n";
                    } else {
                        std::unordered_set<std::string> intersection;
                        for (const auto& candidate : state.identityCandidates) {
                            if (currentCandidates.count(candidate)) intersection.insert(candidate);
                        }
                        state.identityCandidates = std::move(intersection);
                        std::cout << "[Identity] Intersected candidates. Remaining: "
                                  << state.identityCandidates.size() << "\n";
                    }

                    if (state.identityCandidates.size() == 1) {
                        const std::string identifiedId = *state.identityCandidates.begin();
                        std::cout << "[Identity] Identified local player: "
                                  << PrivacyLog::Sensitive(identifiedId, "player ID") << "\n";
                        adoptIdentity(identifiedId);
                    } else if (state.identityCandidates.empty()) {
                        state.identityCandidates = currentCandidates;
                    }
                }

                const std::string& confirmedId = state.myPrimaryId;
                if (!confirmedId.empty() && currentCandidates.count(confirmedId) > 0) {
                    const auto selfIt = state.roster.find(confirmedId);
                    const std::string confirmedName = selfIt != state.roster.end() ? selfIt->second.name : std::string();
                    std::string key = confirmedId + '\x1f' + confirmedName;
                    if (key != state.lastConfirmedLocalAccount) {
                        state.lastConfirmedLocalAccount = std::move(key);
                        effects.confirmedLocalAccount = ConfirmedLocalAccount{confirmedId, confirmedName};
                    }
                }
            }

            const MmrCategory inferredPlaylistCat = CategoryFromMatchContext(state);
            const bool inferredCategoryIsHiddenExtra =
                IsExtraMmrCategory(inferredPlaylistCat) &&
                !ctx.config.show_extra_playlists;

            const MmrCategory lastAutoCat = state.autoSwitchedPlaylistCategory;
            const bool liveCategoryChangedAfterAutoSwitch =
                lastAutoCat != MmrCategory::Best && ctx.rosterMmrCategory != lastAutoCat;
            if (ctx.config.auto_switch_mmr_category &&
                !inferredCategoryIsHiddenExtra &&
                !liveCategoryChangedAfterAutoSwitch &&
                IsSupportedMmrCategory(inferredPlaylistCat) &&
                inferredPlaylistCat != lastAutoCat) {
                state.autoSwitchedPlaylistCategory = inferredPlaylistCat;
                if (!effects.uiUpdate) effects.uiUpdate = UiStateUpdate{};
                effects.uiUpdate->rosterMmrCategory = inferredPlaylistCat;
            }

            if (ctx.config.graph_follow_current_playlist) {
                if (!state.nonLiveReplayActive &&
                    !inferredCategoryIsHiddenExtra &&
                    IsSupportedMmrCategory(inferredPlaylistCat) &&
                    inferredPlaylistCat != state.followedGraphPlaylistCategory) {
                    state.followedGraphPlaylistCategory = inferredPlaylistCat;
                    if (!effects.uiUpdate) effects.uiUpdate = UiStateUpdate{};
                    effects.uiUpdate->graphMmrCategory = inferredPlaylistCat;
                }
            } else {
                state.followedGraphPlaylistCategory = MmrCategory::Best;
                MmrCategory fallbackCategory = StringToMmrCategory(ctx.config.graph_mmr_category);
                if (fallbackCategory == MmrCategory::Best ||
                    (!ctx.config.show_extra_playlists && IsExtraMmrCategory(fallbackCategory))) {
                    fallbackCategory = MmrCategory::TwoVTwo;
                }
                if (!effects.uiUpdate) effects.uiUpdate = UiStateUpdate{};
                effects.uiUpdate->graphMmrCategory = fallbackCategory;
            }

            if (ctx.config.rank_progress_follow_current_playlist) {
                MmrCategory shown = ctx.rankProgressMmrCategory;
                if (!ctx.config.show_extra_playlists && IsExtraMmrCategory(shown)) {
                    shown = MmrCategory::TwoVTwo;
                }
                if (!state.nonLiveReplayActive &&
                    !inferredCategoryIsHiddenExtra &&
                    IsSupportedMmrCategory(inferredPlaylistCat)) {
                    shown = inferredPlaylistCat;
                }
                if (shown != ctx.rankProgressMmrCategory) {
                    if (!effects.uiUpdate) effects.uiUpdate = UiStateUpdate{};
                    effects.uiUpdate->rankProgressMmrCategory = shown;
                }
            } else {
                MmrCategory fallbackCategory = StringToMmrCategory(ctx.config.rank_progress_mmr_category);
                if (!ctx.config.show_extra_playlists && IsExtraMmrCategory(fallbackCategory)) {
                    fallbackCategory = MmrCategory::TwoVTwo;
                }
                if (fallbackCategory != ctx.rankProgressMmrCategory) {
                    if (!effects.uiUpdate) effects.uiUpdate = UiStateUpdate{};
                    effects.uiUpdate->rankProgressMmrCategory = fallbackCategory;
                }
            }
        }

        for (const auto& p : ev.players) {
            if (!p.primaryId.empty()) {
                state.lastPlayerSeenMs[p.primaryId] = nowMs;
            }
        }

        auto it = state.roster.begin();
        while (it != state.roster.end()) {
            const std::string pid = it->first;
            bool shouldPrune = false;
            auto seenIt = state.lastPlayerSeenMs.find(pid);
            if (seenIt == state.lastPlayerSeenMs.end()) {
                state.lastPlayerSeenMs[pid] = nowMs;
            } else {
                if (nowMs - seenIt->second >= 5000) shouldPrune = true;
            }
            if (shouldPrune && pid != state.myPrimaryId) {
                state.matchRoster[pid] = it->second;
                state.lastPlayerBoost.erase(pid);
                state.boostRespawnPending.erase(pid);
                state.lastPlayerSeenMs.erase(pid);
                it = state.roster.erase(it);
            } else {
                if (shouldPrune && pid == state.myPrimaryId) state.lastPlayerSeenMs[pid] = nowMs;
                ++it;
            }
        }

        if (!PlaylistMetadata::HasAuthoritativeId(state.playlistId)) {
            const std::string mode = InferModeFromMatchState(state, ctx.rosterMmrCategory);
            const int expectedTeamSize = LegacyExpectedTeamSizeForMode(mode);
            if (expectedTeamSize > 0 &&
                state.legacyCurrentTeamPlayersSeen[0] >= expectedTeamSize &&
                state.legacyCurrentTeamPlayersSeen[1] >= expectedTeamSize) {
                state.legacyLobbyWasEverFull = true;
            }
        }

        if (!isSpectator) {
            if (!state.myPrimaryId.empty() && state.roster.count(state.myPrimaryId)) {
                const auto& self = state.roster.at(state.myPrimaryId);
                if (self.team == 0 || self.team == 1) {
                    state.localPlayerWasActive = true;
                    state.myTeam = self.team;
                }
            }
        }

        if (!state.myPrimaryId.empty()) {
            bool localPlayerPresent = false;
            for (const auto& p : ev.players) {
                if (p.primaryId == state.myPrimaryId) {
                    localPlayerPresent = true;
                    if (p.goals.has_value()) state.currentMatch.goalsSelf = *p.goals;
                    if (p.shots.has_value()) state.currentMatch.shotsSelf = *p.shots;
                    if (p.assists.has_value()) state.currentMatch.assistsSelf = *p.assists;
                    if (p.saves.has_value()) state.currentMatch.savesSelf = *p.saves;
                    if (p.demos.has_value()) state.currentMatch.demosSelf = *p.demos;
                    break;
                }
            }
            state.localPlayerPresenceObserved = true;
            state.localPlayerPresentInLatestUpdate = localPlayerPresent;
        }

        CapturePreMatchMmr(state);
    }

} // namespace TelemetryReducerDetail
