#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/game/GamemodeUtils.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/state/MatchSaveSnapshot.hpp"
#include "core/state/MatchState.hpp"

struct LiveMatchSnapshot {
    uint64_t version = 1;

    // Match Lifecycle State
    bool inMatch = false;
    bool inReplay = false;
    int myTeam = -1;
    std::string myPrimaryId;
    std::string arenaName;
    std::string arenaAsset;
    std::string matchGuid;
    int playlistId = -1;
    uint64_t activeMatchGeneration = 0;
    std::array<int, 2> score{};
    int legacyMaxPlayersSeen = 0;

    bool roundEverStarted = false;
    bool localPlayerWasActive = false;
    bool localPlayerWasSpectator = false;
    bool localPlayerPresenceObserved = false;
    bool localPlayerPresentInLatestUpdate = false;
    bool explicitLocalForfeit = false;
    bool excludedEarlyExitContext = false;
    std::string earlyExitExclusionReason;
    bool fallbackCasualContext = false;
    bool fallbackNonRecordableContext = false;
    std::string fallbackNonRecordableReason;
    bool legacyLobbyWasEverFull = false;
    std::array<int, 2> legacyCurrentTeamPlayersSeen{0, 0};
    std::array<int, 2> legacyMaxTeamPlayersSeen{0, 0};

    bool lastMatchWasVoid = false;
    std::string lastMatchVoidReason;
    std::array<int, 2> matchSummaryScore{};
    int matchSummaryMyTeam = -1;
    int matchSummaryWinnerTeam = -1;
    float matchSummaryOvertimeSeconds = 0.0f;
    float timeSeconds = 0.0f;
    bool bOvertime = false;

    MatchStats currentMatch;
    SessionTotals sessionTotals;
    float lastGoalSpeed = 0.0f;
    float lastGoalSpeedSelf = 0.0f;
    std::unordered_map<std::string, PlayerData> roster;
    std::unordered_map<std::string, PlayerData> matchRoster;
    std::map<std::string, GamemodeStat> sessionGamemodes;
    std::unordered_map<std::string, LocalPreMatchMmrSnapshot> preMatchMmrByGuid;
    bool matchFinalized = false;
    int64_t sessionStartedAtUnix = 0;
    int64_t lastMatchEndedAtUnix = 0;
    std::vector<std::string> sessionMatchGuids;
    SessionRecap lastSessionRecap;
    uint64_t sessionGeneration = 0;

    int64_t lastMatchStartMs = 0;
    int64_t firstCountdownOfMatchMs = 0;
    int64_t lastCountdownMs = 0;
    int64_t lastGoalMs = 0;
    int64_t lastMatchEndMs = 0;
    int64_t lastPodiumMs = 0;
    bool inGoalReplay = false;
    bool showMatchSummary = false;
    int64_t matchSummaryStartMs = 0;

    LiveMatchSnapshot() = default;
    explicit LiveMatchSnapshot(const MatchState& state)
        : version(state.version),
          inMatch(state.inMatch),
          inReplay(state.inReplay),
          myTeam(state.myTeam),
          myPrimaryId(state.myPrimaryId),
          arenaName(state.arenaName),
          arenaAsset(state.arenaAsset),
          matchGuid(state.matchGuid),
          playlistId(state.playlistId),
          activeMatchGeneration(state.activeMatchGeneration),
          score(state.score),
          legacyMaxPlayersSeen(state.legacyMaxPlayersSeen),
          roundEverStarted(state.roundEverStarted),
          localPlayerWasActive(state.localPlayerWasActive),
          localPlayerWasSpectator(state.localPlayerWasSpectator),
          localPlayerPresenceObserved(state.localPlayerPresenceObserved),
          localPlayerPresentInLatestUpdate(state.localPlayerPresentInLatestUpdate),
          explicitLocalForfeit(state.explicitLocalForfeit),
          excludedEarlyExitContext(state.excludedEarlyExitContext),
          earlyExitExclusionReason(state.earlyExitExclusionReason),
          fallbackCasualContext(state.fallbackCasualContext),
          fallbackNonRecordableContext(state.fallbackNonRecordableContext),
          fallbackNonRecordableReason(state.fallbackNonRecordableReason),
          legacyLobbyWasEverFull(state.legacyLobbyWasEverFull),
          legacyCurrentTeamPlayersSeen(state.legacyCurrentTeamPlayersSeen),
          legacyMaxTeamPlayersSeen(state.legacyMaxTeamPlayersSeen),
          lastMatchWasVoid(state.lastMatchWasVoid),
          lastMatchVoidReason(state.lastMatchVoidReason),
          matchSummaryScore(state.matchSummaryScore),
          matchSummaryMyTeam(state.matchSummaryMyTeam),
          matchSummaryWinnerTeam(state.matchSummaryWinnerTeam),
          matchSummaryOvertimeSeconds(state.matchSummaryOvertimeSeconds),
          timeSeconds(state.timeSeconds),
          bOvertime(state.bOvertime),
          currentMatch(state.currentMatch),
          sessionTotals(state.sessionTotals),
          lastGoalSpeed(state.lastGoalSpeed),
          lastGoalSpeedSelf(state.lastGoalSpeedSelf),
          roster(state.roster),
          matchRoster(state.matchRoster),
          sessionGamemodes(state.sessionGamemodes),
          preMatchMmrByGuid(state.preMatchMmrByGuid),
          matchFinalized(state.matchFinalized),
          sessionStartedAtUnix(state.sessionStartedAtUnix),
          lastMatchEndedAtUnix(state.lastMatchEndedAtUnix),
          sessionMatchGuids(state.sessionMatchGuids),
          lastSessionRecap(state.lastSessionRecap),
          sessionGeneration(state.sessionGeneration),
          lastMatchStartMs(state.lastMatchStartMs),
          firstCountdownOfMatchMs(state.firstCountdownOfMatchMs),
          lastCountdownMs(state.lastCountdownMs),
          lastGoalMs(state.lastGoalMs),
          lastMatchEndMs(state.lastMatchEndMs),
          lastPodiumMs(state.lastPodiumMs),
          inGoalReplay(state.inGoalReplay),
          showMatchSummary(state.showMatchSummary),
          matchSummaryStartMs(state.matchSummaryStartMs) {}
};
