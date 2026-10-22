#include "core/reducer/TelemetryReducer.hpp"
#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "utils/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>
#include "core/reducer/TelemetryReducerDetail.hpp"

using namespace TelemetryReducerDetail;

TelemetryReducer::TelemetryReducer(std::shared_ptr<SessionState> state)
    : m_state(state) {
    m_cachedConfigRevision = Config::Revision();
    m_cachedConf = Config::Read();
    if (m_state) {
        if (!m_cachedConf.show_extra_playlists &&
            IsExtraMmrCategory(m_state->ui.rankProgressMmrCategory.load())) {
            m_state->ui.rankProgressMmrCategory.store(MmrCategory::TwoVTwo);
        }
        if (!m_cachedConf.rank_progress_follow_current_playlist) {
            MmrCategory fallback = StringToMmrCategory(m_cachedConf.rank_progress_mmr_category);
            if (!m_cachedConf.show_extra_playlists && IsExtraMmrCategory(fallback)) {
                fallback = MmrCategory::TwoVTwo;
            }
            m_state->ui.rankProgressMmrCategory.store(fallback);
        }
    }
}

void TelemetryReducer::SetClockForTests(std::function<std::chrono::steady_clock::time_point()> clockFn) {
    m_clockFn = std::move(clockFn);
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
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    m_cachedConfigRevision = Config::Revision();
    m_cachedConf = Config::Read();
    if (m_state) {
        if (!m_cachedConf.show_extra_playlists &&
            IsExtraMmrCategory(m_state->ui.rankProgressMmrCategory.load())) {
            m_state->ui.rankProgressMmrCategory.store(MmrCategory::TwoVTwo);
        }
        if (!m_cachedConf.rank_progress_follow_current_playlist) {
            MmrCategory fallback = StringToMmrCategory(m_cachedConf.rank_progress_mmr_category);
            if (!m_cachedConf.show_extra_playlists && IsExtraMmrCategory(fallback)) {
                fallback = MmrCategory::TwoVTwo;
            }
            m_state->ui.rankProgressMmrCategory.store(fallback);
        }
    }
}

bool TelemetryReducer::AcceptsUiMatchEventLocked(const nlohmann::json& data, int64_t nowMs) {
    if (m_nonLiveReplayActive) return false;
    const std::string incomingGuid = ExtractEventMatchGuid(data);
    if (!incomingGuid.empty()) {
        if (m_finalizedMatchGuids.count(incomingGuid) > 0 ||
            m_pendingDestroyedMatches.count(incomingGuid) > 0) {
            return false;
        }
        if (!m_state->game.matchGuid.empty() && m_state->game.inMatch &&
            !m_state->game.matchFinalized && incomingGuid != m_state->game.matchGuid) {
            return false;
        }
        if ((!m_uiMatchGuid.empty() && incomingGuid != m_uiMatchGuid) ||
            m_state->game.matchFinalized) {
            m_state->ui.ResetMatchTimestamps(nowMs);
            m_countdownSeenThisRound = false;
        }
        m_uiMatchGuid = incomingGuid;
        return true;
    }
    if (m_state->game.matchFinalized) {
        return false;
    }
    return true;
}

bool TelemetryReducer::TargetsCurrentActiveMatchUiLocked(const nlohmann::json& data) {
    if (!m_state->game.inMatch || m_nonLiveReplayActive) {
        return false;
    }
    const std::string incomingGuid = ExtractEventMatchGuid(data);
    if (!incomingGuid.empty()) {
        if ((!m_state->game.matchGuid.empty() && incomingGuid != m_state->game.matchGuid) ||
            (!m_uiMatchGuid.empty() && incomingGuid != m_uiMatchGuid) ||
            m_pendingDestroyedMatches.count(incomingGuid) > 0) {
            return false;
        }
        if (m_state->game.matchGuid.empty() && m_uiMatchGuid.empty() &&
            m_finalizedMatchGuids.count(incomingGuid) > 0) {
            return false;
        }
        if (m_uiMatchGuid.empty()) m_uiMatchGuid = incomingGuid;
        return true;
    }
    if (!m_pendingDestroyedMatches.empty() || m_missingGuidAssociationBlockedByReconnect) {
        return false;
    }
    return true;
}

SideEffects TelemetryReducer::Reduce(const std::string& eventName, const nlohmann::json& data) {
    SideEffects effects;
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    m_state->game.version++;

    const uint64_t revision = Config::Revision();
    if (revision != m_cachedConfigRevision) {
        m_cachedConfigRevision = revision;
        m_cachedConf = Config::Read();
        if (m_state && !m_cachedConf.show_extra_playlists &&
            IsExtraMmrCategory(m_state->ui.rankProgressMmrCategory.load())) {
            m_state->ui.rankProgressMmrCategory.store(MmrCategory::TwoVTwo);
        }
    }

    if (eventName == Constants::EVT_REPLAY_CREATED) {
        // ReplayCreated is documented for Match History replay playback,
        // not an in-match goal replay.
        m_nonLiveReplayActive = true;
        m_state->game.inReplay = true;
        m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_CREATED) {
        const int64_t nowMs = SteadyNowMs();
        const std::string incomingGuid = ExtractEventMatchGuid(data);
        const bool wasInMatch = m_state->game.inMatch;
        const bool wasFinalized = m_state->game.matchFinalized;
        const bool hadPriorUiCountdown =
            m_state->ui.firstCountdownOfMatchMs.load(std::memory_order_relaxed) > 0 ||
            m_state->ui.lastMatchEndMs.load(std::memory_order_relaxed) > 0;
        const bool hasIncomingGuid = !incomingGuid.empty();
        const bool hadCurrentGuid =
            !m_state->game.matchGuid.empty();
        const bool isSameActiveUiGuid =
            hasIncomingGuid && incomingGuid == m_uiMatchGuid &&
            wasInMatch && !wasFinalized;
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

            if (!m_cachedConf.reset_session_on_close) {
                const int64_t nowUnix =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
                if (m_state->closeSessionIfInactiveLocked(nowUnix)) {
                    effects.persistSession = m_state->game.lastSessionRecap;
                }
            }
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
            m_matchEvents.clear();
            m_analytics.Reset();
            m_eventNativeKeys.clear();
            m_lastEventSignature.clear();
            m_lastPlayerBoost.clear();
            m_boostRespawnPending.clear();
            m_lastPlayerSeen.clear();
            ResetMatchTimingState();
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

        if (!isSameActiveUiGuid) {
            if (startsNewLifecycle || wasFinalized || hadPriorUiCountdown ||
                (hasIncomingGuid && !m_uiMatchGuid.empty() && incomingGuid != m_uiMatchGuid)) {
                m_state->ui.ResetMatchTimestamps(nowMs);
                m_countdownSeenThisRound = false;
            } else {
                m_state->ui.lastMatchStartMs.store(nowMs, std::memory_order_relaxed);
                m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
            }
            m_uiMatchGuid = incomingGuid;
        } else {
            if (m_state->ui.lastMatchStartMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.lastMatchStartMs.store(nowMs, std::memory_order_relaxed);
            }
            m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
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
        if (startsNewLifecycle || attachesGuid) {
            effects.pushDiscord = true;
            effects.discordSnapshot = BuildDiscordSnapshotLocked();
        }
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_INITIALIZED) {
        const int64_t nowMs = SteadyNowMs();
        if (AcceptsUiMatchEventLocked(data, nowMs)) {
            if (m_state->ui.lastMatchStartMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.lastMatchStartMs.store(nowMs, std::memory_order_relaxed);
            }
            if (m_state->ui.firstCountdownOfMatchMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.firstCountdownOfMatchMs.store(nowMs, std::memory_order_relaxed);
            }
            if (m_state->ui.lastCountdownMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.lastCountdownMs.store(nowMs, std::memory_order_relaxed);
            }
            m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
            m_countdownSeenThisRound = true;
        }
        return effects;
    }

    if (eventName == Constants::EVT_COUNTDOWN_BEGIN) {
        const int64_t nowMs = SteadyNowMs();
        if (AcceptsUiMatchEventLocked(data, nowMs)) {
            if (m_state->ui.lastMatchStartMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.lastMatchStartMs.store(nowMs, std::memory_order_relaxed);
            }
            if (m_state->ui.firstCountdownOfMatchMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.firstCountdownOfMatchMs.store(nowMs, std::memory_order_relaxed);
            }
            m_state->ui.lastCountdownMs.store(nowMs, std::memory_order_relaxed);
            m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
            m_countdownSeenThisRound = true;
            m_roundActive = false;
            m_lastPlayerBoost.clear();
        }
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnCountdownBegin();
        return effects;
    }

    if (eventName == Constants::EVT_ROUND_STARTED) {
        const int64_t nowMs = SteadyNowMs();
        if (AcceptsUiMatchEventLocked(data, nowMs)) {
            if (m_state->ui.lastMatchStartMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.lastMatchStartMs.store(nowMs, std::memory_order_relaxed);
            }
            if (m_state->ui.firstCountdownOfMatchMs.load(std::memory_order_relaxed) <= 0) {
                m_state->ui.firstCountdownOfMatchMs.store(nowMs, std::memory_order_relaxed);
            }
            if (!m_countdownSeenThisRound) {
                m_state->ui.lastCountdownMs.store(nowMs, std::memory_order_relaxed);
            }
            m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
            m_countdownSeenThisRound = false;
        }
        if (!m_roundActive) {
            m_lastPlayerBoost.clear();
            m_boostRespawnPending.clear();
        }
        m_roundActive = true;
        m_state->game.roundEverStarted = true;
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnRoundStarted(AnalyticsNowSeconds());
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_PAUSED) {
        if (!m_currentPauseStart.has_value()) {
            m_currentPauseStart = Now();
        }
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnPaused(AnalyticsNowSeconds());
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_UNPAUSED) {
        if (m_currentPauseStart.has_value()) {
            m_pauseIntervals.push_back({*m_currentPauseStart, Now()});
            m_currentPauseStart.reset();
        }
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnUnpaused(AnalyticsNowSeconds());
        return effects;
    }

    if (eventName == Constants::EVT_GOAL_REPLAY_START) {
        if (TargetsCurrentActiveMatchUiLocked(data)) {
            m_state->ui.inGoalReplay.store(true, std::memory_order_relaxed);
        }
        return effects;
    }

    // GoalReplayWillEnd fires when the ball explodes while the replay is still on screen; do not clear inGoalReplay.
    if (eventName == Constants::EVT_GOAL_REPLAY_WILL_END) {
        return effects;
    }

    if (eventName == Constants::EVT_GOAL_REPLAY_END) {
        const std::string incomingGuid = ExtractEventMatchGuid(data);
        if (!incomingGuid.empty() &&
            ((!m_state->game.matchGuid.empty() && incomingGuid != m_state->game.matchGuid) ||
             (!m_uiMatchGuid.empty() && incomingGuid != m_uiMatchGuid) ||
             m_pendingDestroyedMatches.count(incomingGuid) > 0)) {
            return effects;
        }
        m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
        return effects;
    }

    if (eventName == Constants::EVT_PODIUM_START) {
        if (!TargetsCurrentActiveMatchUiLocked(data)) {
            return effects;
        }
        m_state->ui.inGoalReplay.store(false, std::memory_order_relaxed);
        if (m_state->ui.lastPodiumMs.load(std::memory_order_relaxed) <= 0) {
            const int64_t nowMs = SteadyNowMs();
            m_state->ui.lastPodiumMs.store(nowMs, std::memory_order_relaxed);
            if (m_state->ui.showMatchSummary.load(std::memory_order_relaxed) ||
                m_state->game.matchFinalized) {
                m_state->ui.matchSummaryStartMs.store(nowMs, std::memory_order_relaxed);
            }
        }
        return effects;
    }
    if (eventName == Constants::EVT_UPDATE_STATE) {
        const std::string preArena = m_state->game.arenaName;
        const std::array<int, 2> preScore = m_state->game.score;
        const bool preInMatch = m_state->game.inMatch;
        HandleUpdateState(data, effects);
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnUpdateState(data, AnalyticsNowSeconds());
        if (!effects.pushDiscord && (m_state->game.arenaName != preArena ||
                                     m_state->game.score != preScore ||
                                     m_state->game.inMatch != preInMatch)) {
            effects.pushDiscord = true;
            effects.discordSnapshot = BuildDiscordSnapshotLocked();
        }
    } else if (eventName == Constants::EVT_STATFEED) {
        if (data.contains("EventName") && data["EventName"].is_string()) {
            const std::string name = data["EventName"].get<std::string>();
            const std::string type = name == "EpicSave" || name == "Epic Save" ? "epic_save" : name == "Save"   ? "save"
                                                                                           : name == "Shot"     ? "shot"
                                                                                           : name == "Assist"   ? "assist"
                                                                                           : name == "OwnGoal"  ? "own_goal"
                                                                                           : name == "Demolish" ? "demolition"
                                                                                                                : "";
            if (!type.empty()) RecordJournalEventLocked(type, data,
                                                        data.contains("MainTarget") ? "MainTarget" : "Player", "SecondaryTarget", "statfeed");
        }
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnStatfeed(data);
        HandleStatFeed(data);
    } else if (eventName == Constants::EVT_GOAL_SCORED) {
        if (AcceptsAnalyticsEventLocked(data, true)) m_analytics.OnGoalScored(data);
        // Skipped replays can deliver the replay-end echo outside replay state; it is not a new goal.
        if (!IsGoalReplayEcho(data)) {
            RecordJournalEventLocked("goal", data, "Scorer", "Assister", "goal");
            HandleGoalScored(data, effects);
        }
    } else if (eventName == Constants::EVT_BALL_HIT) {
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnBallHit(data, AnalyticsNowSeconds());
        HandleBallHit(data);
    } else if (eventName == Constants::EVT_BOOST_PICKUP) {
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnBoostPickup(data);
    } else if (eventName == Constants::EVT_PLAYER_LEFT) {
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnPlayerLeft(data);
    } else if (eventName == Constants::EVT_CROSSBAR_HIT) {
        RecordJournalEventLocked("crossbar", data, nullptr, nullptr, "crossbar");
        HandleCrossbarHit(data);
    } else if (eventName == Constants::EVT_MATCH_ENDED) {
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnMatchEnded();
        HandleMatchEnded(data, effects);
    } else if (eventName == Constants::EVT_MATCH_DESTROYED) {
        if (AcceptsAnalyticsEventLocked(data)) m_analytics.OnMatchEnded();
        HandleMatchDestroyed(data, effects);
    }

    return effects;
}

void TelemetryReducer::RequeueEncounterRecordsLocked(SideEffects& effects) {
    for (auto& [pid, player] : m_state->game.roster) {
        player.lifetimeWinsWith = 0;
        player.lifetimeLossesWith = 0;
        player.lifetimeWinsAgainst = 0;
        player.lifetimeLossesAgainst = 0;
        player.hasLifetimeData = false;
        if (pid.rfind("Unknown|", 0) != 0) effects.fetchEncounterQueue.push_back(pid);
    }
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

std::string TelemetryReducer::ResolveRosterPlayerIdLocked(const nlohmann::json& target) const {
    if (!target.is_object()) return {};
    const std::string name = target.contains("Name") && target["Name"].is_string() ? target["Name"].get<std::string>() : std::string{};
    if (target.contains("PrimaryId") && target["PrimaryId"].is_string()) {
        const std::string id = target["PrimaryId"].get<std::string>();
        if (id == "Unknown" || id.rfind("Unknown|", 0) == 0) return "Unknown|" + name;
        return id;
    }
    // Event payloads identify players by Name/TeamNum only; map them back to the roster.
    if (name.empty()) return {};
    const bool hasTeam = target.contains("TeamNum") && target["TeamNum"].is_number_integer();
    const int team = hasTeam ? target["TeamNum"].get<int>() : -1;
    std::string match;
    for (const auto& [pid, player] : m_state->game.roster) {
        if (player.name != name || (hasTeam && player.team != team)) continue;
        if (!match.empty()) return {};
        match = pid;
    }
    return match;
}

DiscordPresenceSnapshot TelemetryReducer::BuildDiscordSnapshotLocked() const {
    DiscordPresenceSnapshot snapshot;
    snapshot.showPresence = true;
    snapshot.inMatch = m_state->game.inMatch;
    snapshot.matchGuid = m_state->game.matchGuid;
    snapshot.arenaName = m_state->game.arenaName;
    snapshot.score[0] = m_state->game.score[0];
    snapshot.score[1] = m_state->game.score[1];
    snapshot.myTeam = m_state->game.myTeam;
    snapshot.sessionWins = m_state->game.sessionTotals.wins;
    snapshot.sessionLosses = m_state->game.sessionTotals.losses;
    return snapshot;
}

float TelemetryReducer::PausedSecondsWithin(std::chrono::steady_clock::time_point windowStart,
                                            std::chrono::steady_clock::time_point windowEnd) const {
    if (windowEnd <= windowStart) return 0.0f;
    auto total = std::chrono::steady_clock::duration::zero();
    auto addInterval = [&](std::chrono::steady_clock::time_point pStart, std::chrono::steady_clock::time_point pEnd) {
        auto s = std::max(windowStart, pStart);
        auto e = std::min(windowEnd, pEnd);
        if (e > s) {
            total += (e - s);
        }
    };
    for (const auto& pi : m_pauseIntervals) {
        addInterval(pi.start, pi.end);
    }
    if (m_currentPauseStart.has_value()) {
        addInterval(*m_currentPauseStart, windowEnd);
    }
    return std::chrono::duration<float>(total).count();
}

bool TelemetryReducer::AcceptsAnalyticsEventLocked(const nlohmann::json& data, bool duringReplay) const {
    const auto& game = m_state->game;
    if (!game.inMatch || game.matchFinalized || (game.inReplay && !duringReplay) || m_nonLiveReplayActive ||
        game.fallbackNonRecordableContext || PlaylistMetadata::IsNonRecordable(game.playlistId)) return false;
    const std::string guid = ExtractEventMatchGuid(data);
    if (guid.empty()) return !(m_missingGuidAssociationBlockedByReconnect && game.matchGuid.empty());
    if (!game.matchGuid.empty() && guid != game.matchGuid) return false;
    return m_finalizedMatchGuids.count(guid) == 0 && m_pendingDestroyedMatches.count(guid) == 0;
}

double TelemetryReducer::AnalyticsNowSeconds() const {
    return std::chrono::duration<double>(Now().time_since_epoch()).count();
}

void TelemetryReducer::ResetMatchTimingState() {
    m_hasObservedMatchClock = false;
    m_regulationClockStart.reset();
    m_regulationClockLatest = 0.0f;
    m_overtimeStartedAt.reset();
    m_currentPauseStart.reset();
    m_pauseIntervals.clear();
}
