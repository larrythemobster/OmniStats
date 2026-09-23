#include "TelemetryReducer.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>
#include "core/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

TelemetryReducer::TelemetryReducer(std::shared_ptr<SessionState> state)
    : m_state(state) {
    m_cachedConf = Config::Read();
    m_lastConfigReadTime = std::chrono::steady_clock::now();
}

void TelemetryReducer::OnTelemetryDisconnected() {
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    if (m_state->game.inMatch && !m_state->game.matchFinalized) {
        m_missingGuidAssociationBlockedByReconnect = true;
        std::cout
            << "[TelemetryReducer] Telemetry disconnected: "
            << "currentGeneration="
            << m_state->game.activeMatchGeneration
            << ", action=block-missing-guid-match-end.\n";
    }
}

void TelemetryReducer::OnConfigChanged() {
    m_cachedConf = Config::Read();
}

SideEffects TelemetryReducer::Reduce(const std::string& eventName, const nlohmann::json& data) {
    SideEffects effects;
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    m_state->game.version++;

    m_cachedConf = Config::Read();

    if (eventName == Constants::EVT_REPLAY_CREATED) {
        // ReplayCreated is documented for Match History replay playback,
        // not an in-match goal replay.
        m_nonLiveReplayActive = true;
        m_state->game.inReplay = true;
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_CREATED) {
        const std::string incomingGuid =
            (data.contains("MatchGuid") &&
             data["MatchGuid"].is_string())
                ? data["MatchGuid"].get<std::string>()
                : "";
        const bool wasInMatch = m_state->game.inMatch;
        const bool hasIncomingGuid = !incomingGuid.empty();
        const bool hadCurrentGuid =
            !m_state->game.matchGuid.empty();
        const uint64_t currentGeneration =
            m_state->game.activeMatchGeneration;
        const bool currentIsNonRecordable =
            wasInMatch &&
            (m_state->game.fallbackNonRecordableContext ||
             PlaylistMetadata::IsNonRecordable(m_state->game.playlistId));
        const bool startsNewLifecycle =
            !wasInMatch ||
            (hasIncomingGuid && currentIsNonRecordable) ||
            (hasIncomingGuid && hadCurrentGuid &&
             incomingGuid != m_state->game.matchGuid);
        const bool attachesGuid =
            wasInMatch && hasIncomingGuid &&
            !hadCurrentGuid &&
            !currentIsNonRecordable;

        const char* action = "preserve-current";
        const char* reason =
            hasIncomingGuid
                ? "same-explicit-guid"
                : "missing-guid-active-lifecycle";

        if (startsNewLifecycle) {
            action = "start-new";
            reason = wasInMatch
                         ? (currentIsNonRecordable ? "non-recordable-transition" : "different-explicit-guid")
                         : "no-active-match";

            LocalPreMatchMmrSnapshot initialMmrSnapshot;
            bool hasInitialMmrSnapshot = false;
            if (!m_state->game.myPrimaryId.empty()) {
                const auto playerIt =
                    m_state->game.roster.find(
                        m_state->game.myPrimaryId);
                if (playerIt !=
                        m_state->game.roster.end() &&
                    !playerIt->second.playlists.empty()) {
                    initialMmrSnapshot.playlistMmrs =
                        playerIt->second.playlists;
                    initialMmrSnapshot.playlistMatches =
                        playerIt->second.playlistMatches;
                    hasInitialMmrSnapshot = true;
                }
            }

            const std::string currentArena =
                m_state->game.arenaName;
            m_state->resetMatch(currentArena);
            m_state->game.activeMatchGeneration =
                ++m_nextMatchGeneration;
            m_state->game.matchGuid = incomingGuid;
            m_missingGuidAssociationBlockedByReconnect =
                false;
            if (hasInitialMmrSnapshot) {
                m_state->game.preMatchMmrByGuid.emplace(
                    incomingGuid,
                    std::move(initialMmrSnapshot));
            }
            m_roundActive = false;
            m_autoSwitchedPlaylistCategory =
                MmrCategory::Best;
            m_followedGraphPlaylistCategory = MmrCategory::Best;
            m_lastPlayerBoost.clear();
            m_lastPlayerSeen.clear();
        } else if (attachesGuid) {
            action = "attach-guid";
            reason = "late-guid-enrichment";
            auto guidlessSnapshot =
                m_state->game.preMatchMmrByGuid.extract("");
            if (!guidlessSnapshot.empty()) {
                guidlessSnapshot.key() = incomingGuid;
                m_state->game.preMatchMmrByGuid.insert(
                    std::move(guidlessSnapshot));
            }
            m_state->game.matchGuid = incomingGuid;
            CapturePreMatchMmrLocked();
        }

        std::cout
            << "[TelemetryReducer] MatchCreated: "
            << "incomingGuid="
            << (hasIncomingGuid ? "present" : "missing")
            << ", currentGuid="
            << (hadCurrentGuid ? "present" : "missing")
            << ", currentGeneration="
            << currentGeneration
            << ", inMatch="
            << (wasInMatch ? "true" : "false")
            << ", action=" << action
            << ", reason=" << reason << ".\n";
        return effects;
    }

    if (eventName == Constants::EVT_ROUND_STARTED) {
        m_roundActive = true;
        m_state->game.roundEverStarted = true;
        return effects;
    }

    if (eventName == Constants::EVT_UPDATE_STATE) {
        HandleUpdateState(data, effects);
    } else if (eventName == Constants::EVT_STATFEED) {
        HandleStatFeed(data);
    } else if (eventName == Constants::EVT_GOAL_SCORED) {
        HandleGoalScored(data, effects);
    } else if (eventName == Constants::EVT_BALL_HIT) {
        HandleBallHit(data);
    } else if (eventName == Constants::EVT_CROSSBAR_HIT) {
        HandleCrossbarHit(data);
    } else if (eventName == Constants::EVT_MATCH_ENDED) {
        HandleMatchEnded(data, effects);
    } else if (eventName == Constants::EVT_MATCH_DESTROYED) {
        HandleMatchDestroyed(data, effects);
    }

    return effects;
}

bool TelemetryReducer::IsSelf(const std::string& name) const {
    if (name.empty() || m_state->game.myPrimaryId.empty()) return false;
    for (const auto& [pid, player] : m_state->game.roster)
        if (player.name == name) return pid == m_state->game.myPrimaryId;
    return false;
}

bool TelemetryReducer::IsSelfById(const std::string& pid) const {
    return !pid.empty() && pid == m_state->game.myPrimaryId;
}

DiscordPresenceSnapshot TelemetryReducer::BuildDiscordSnapshotLocked() const {
    DiscordPresenceSnapshot snapshot;
    snapshot.showPresence = true;
    snapshot.inMatch = m_state->game.inMatch;
    snapshot.arenaName = m_state->game.arenaName;
    snapshot.score[0] = m_state->game.score[0];
    snapshot.score[1] = m_state->game.score[1];
    snapshot.myTeam = m_state->game.myTeam;
    snapshot.sessionWins = m_state->game.sessionTotals.wins;
    snapshot.sessionLosses = m_state->game.sessionTotals.losses;
    return snapshot;
}
