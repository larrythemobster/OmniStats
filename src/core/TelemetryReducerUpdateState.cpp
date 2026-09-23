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

void TelemetryReducer::CapturePreMatchMmrLocked() {
    const std::string& matchGuid = m_state->game.matchGuid;
    if (matchGuid.empty() ||
        m_state->game.preMatchMmrByGuid.count(matchGuid) > 0 ||
        m_state->game.myPrimaryId.empty()) {
        return;
    }

    const auto playerIt = m_state->game.roster.find(m_state->game.myPrimaryId);
    if (playerIt == m_state->game.roster.end()) return;

    bool hasPlaylistMmr = false;
    for (const auto& [playlist, mmr] : playerIt->second.playlists) {
        if (playlist != "best" && mmr > 0) {
            hasPlaylistMmr = true;
            break;
        }
    }
    if (!hasPlaylistMmr) return;

    m_state->game.preMatchMmrByGuid.emplace(
        matchGuid,
        LocalPreMatchMmrSnapshot{
            .playlistMmrs = playerIt->second.playlists,
            .playlistMatches = playerIt->second.playlistMatches});
    std::cout
        << "[TelemetryReducer] Captured pre-match MMR snapshot: matchGuid="
        << PrivacyLog::Sensitive(matchGuid, "match GUID")
        << ".\n";
}

bool TelemetryReducer::AttachTerminalGuidToCurrentLocked(
    const std::string& eventMatchGuid) {
    if (eventMatchGuid.empty() ||
        !m_state->game.inMatch ||
        m_state->game.matchFinalized ||
        !m_state->game.matchGuid.empty() ||
        m_missingGuidAssociationBlockedByReconnect ||
        m_finalizedMatchGuids.count(eventMatchGuid) > 0 ||
        m_pendingDestroyedMatches.count(eventMatchGuid) > 0) {
        return false;
    }

    auto guidlessSnapshot =
        m_state->game.preMatchMmrByGuid.extract("");
    if (!guidlessSnapshot.empty()) {
        guidlessSnapshot.key() = eventMatchGuid;
        m_state->game.preMatchMmrByGuid.insert(
            std::move(guidlessSnapshot));
    }
    m_state->game.matchGuid = eventMatchGuid;
    CapturePreMatchMmrLocked();
    return true;
}

bool TelemetryReducer::HasExplicitLocalForfeitSignal(const nlohmann::json& data,
                                                     int localTeam) {
    const auto scopeHasSignal = [&](const nlohmann::json& scope) {
        if (ReadTrueBoolean(
                scope,
                {"bLocalPlayerForfeit",
                 "bLocalForfeit",
                 "LocalPlayerForfeit",
                 "bLocalPlayerAbandoned",
                 "bLocalPlayerAbandon",
                 "LocalPlayerAbandoned"})) {
            return true;
        }

        if (!ReadTrueBoolean(scope, {"bForfeit", "Forfeit"})) return false;

        const auto forfeitingTeam = ReadInteger(
            scope,
            {"ForfeitTeamNum", "ForfeitingTeamNum", "AbandoningTeamNum"});
        if (forfeitingTeam && (*forfeitingTeam == 0 || *forfeitingTeam == 1)) {
            return *forfeitingTeam == localTeam;
        }

        const auto winner =
            ReadInteger(scope, {"WinnerTeamNum", "winner_team_num"});
        return winner && (*winner == 0 || *winner == 1) &&
               (localTeam == 0 || localTeam == 1) && *winner != localTeam;
    };

    if (scopeHasSignal(data)) return true;
    return data.contains("Game") && data["Game"].is_object() &&
           scopeHasSignal(data["Game"]);
}

void TelemetryReducer::UpdateLifecycleSignalsLocked(const nlohmann::json& data) {
    if (HasExplicitLocalForfeitSignal(data, m_state->game.myTeam)) {
        m_state->game.explicitLocalForfeit = true;
    }

    const nlohmann::json* game =
        data.contains("Game") && data["Game"].is_object() ? &data["Game"] : nullptr;
    if (!game) return;

    // PlaylistId is stronger than optional flags/strings. Only consult the
    // latter when Rocket League genuinely omitted PlaylistId; stale UI/context
    // flags must not demote an authoritative ranked playlist.
    if (PlaylistMetadata::HasAuthoritativeId(m_state->game.playlistId)) {
        // Recompute this from the authoritative ID on every update. A transient
        // intermission/unknown ID can arrive before the real match playlist;
        // once a known recordable playlist arrives it must clear the stale
        // exclusion rather than poisoning the rest of the match.
        m_state->game.excludedEarlyExitContext = false;
        m_state->game.earlyExitExclusionReason.clear();
        if (!PlaylistMetadata::IsKnown(m_state->game.playlistId)) {
            m_state->game.excludedEarlyExitContext = true;
            m_state->game.earlyExitExclusionReason = "unknown_playlist_id";
        } else if (const char* reason =
                       PlaylistMetadata::NonRecordableReason(
                           m_state->game.playlistId)) {
            m_state->game.excludedEarlyExitContext = true;
            m_state->game.earlyExitExclusionReason = reason;
        } else if (PlaylistMetadata::IsCasual(m_state->game.playlistId)) {
            m_state->game.excludedEarlyExitContext = true;
            m_state->game.earlyExitExclusionReason = "casual_playlist";
        }
        m_state->game.fallbackCasualContext = false;
        m_state->game.fallbackNonRecordableContext = false;
        m_state->game.fallbackNonRecordableReason.clear();
    } else {
        const bool explicitNonRecordable =
            ReadTrueBoolean(
                *game,
                {"bPrivateMatch",
                 "bTraining",
                 "bExhibition",
                 "bTournamentSpectator"}) ||
            MatchTypeContains(
                *game, {"private", "training", "exhibition", "freeplay"});
        const bool explicitCasual =
            ReadTrueBoolean(*game, {"bCasualMatch"}) ||
            MatchTypeContains(*game, {"casual"});

        if (explicitNonRecordable) {
            m_state->game.fallbackNonRecordableContext = true;
            m_state->game.fallbackNonRecordableReason =
                "explicit_non_competitive_context";
        } else if (explicitCasual) {
            m_state->game.fallbackCasualContext = true;
        }

        if (m_state->game.fallbackNonRecordableContext) {
            m_state->game.excludedEarlyExitContext = true;
            m_state->game.earlyExitExclusionReason =
                m_state->game.fallbackNonRecordableReason;
        } else if (m_state->game.fallbackCasualContext) {
            m_state->game.excludedEarlyExitContext = true;
            m_state->game.earlyExitExclusionReason = "casual_context";
        }
    }
}

void TelemetryReducer::HandleUpdateState(const nlohmann::json& data, SideEffects& effects) {
    bool gameReplayActive = false;
    bool isSpectator = false;

    if (data.contains("Game") && data["Game"].is_object()) {
        auto game = data["Game"];
        std::optional<int> incomingPlaylistId;
        if (game.contains("PlaylistId") &&
            game["PlaylistId"].is_number_integer()) {
            incomingPlaylistId = game["PlaylistId"].get<int>();
        }

        if (game.contains("bReplay") && game["bReplay"].is_boolean()) {
            gameReplayActive = game["bReplay"].get<bool>();
            m_state->game.inReplay = gameReplayActive;
        }
        if (game.contains("bSpectator") && game["bSpectator"].is_boolean()) {
            isSpectator = game["bSpectator"].get<bool>();
        }
        if (game.contains("Arena") && game["Arena"].is_string()) {
            std::string currentArenaAsset = game["Arena"].get<std::string>();
            std::string currentArena = FormatArenaName(currentArenaAsset);
            if (!currentArena.empty() &&
                (currentArena != m_state->game.arenaName ||
                 currentArenaAsset != m_state->game.arenaAsset)) {
                const bool previousIsNonRecordable =
                    m_state->game.fallbackNonRecordableContext ||
                    PlaylistMetadata::IsNonRecordable(m_state->game.playlistId);
                const bool establishedActiveMatch =
                    m_state->game.inMatch &&
                    m_state->game.roundEverStarted &&
                    !previousIsNonRecordable;

                if (establishedActiveMatch) {
                    // Arena metadata can arrive independently. Never destroy a
                    // live match's stats/playlist merely because the arena
                    // string changed after the round had already started.
                    m_state->game.arenaName = currentArena;
                    m_state->game.arenaAsset = currentArenaAsset;
                    std::cout
                        << "[TelemetryReducer] Arena metadata changed during "
                           "active match; preserving match state.\n";
                } else {
                    const std::string matchGuid = m_state->game.matchGuid;
                    const int previousPlaylistId =
                        m_state->game.playlistId;
                    const bool startsNewLifecycle =
                        !m_state->game.inMatch || previousIsNonRecordable;
                    LocalPreMatchMmrSnapshot preservedMmrSnapshot;
                    bool hasPreservedMmrSnapshot = false;
                    const auto snapshotIt =
                        m_state->game.preMatchMmrByGuid.find(matchGuid);
                    if (snapshotIt !=
                        m_state->game.preMatchMmrByGuid.end()) {
                        preservedMmrSnapshot =
                            std::move(snapshotIt->second);
                        hasPreservedMmrSnapshot = true;
                    }
                    m_state->resetMatch(currentArena, currentArenaAsset);
                    if (startsNewLifecycle) {
                        m_state->game.activeMatchGeneration =
                            ++m_nextMatchGeneration;
                        m_missingGuidAssociationBlockedByReconnect =
                            false;
                    }
                    if (!startsNewLifecycle &&
                        hasPreservedMmrSnapshot) {
                        m_state->game.preMatchMmrByGuid.emplace(
                            matchGuid,
                            std::move(preservedMmrSnapshot));
                    } else if (startsNewLifecycle) {
                        m_state->game.matchGuid.clear();
                    }

                    // resetMatch intentionally clears playlist state for a new
                    // lifecycle. During same-lifecycle arena initialization,
                    // preserve an already-authoritative ID if this telemetry
                    // frame omitted PlaylistId.
                    if (!startsNewLifecycle &&
                        !incomingPlaylistId.has_value() &&
                        PlaylistMetadata::HasAuthoritativeId(
                            previousPlaylistId)) {
                        m_state->game.playlistId =
                            previousPlaylistId;
                    }

                    m_roundActive = false;
                    m_autoSwitchedPlaylistCategory =
                        MmrCategory::Best;
                    m_followedGraphPlaylistCategory =
                        MmrCategory::Best;
                    m_lastPlayerBoost.clear();
                    m_lastPlayerSeen.clear();
                    std::cout << "\n========================================\n";
                    std::cout << "[Event] Match Started in: "
                              << m_state->game.arenaName << "\n";
                    effects.pushDiscord = true;
                    effects.discordSnapshot =
                        BuildDiscordSnapshotLocked();
                }
            }
        }

        if (incomingPlaylistId.has_value()) {
            const int newPlaylistId = *incomingPlaylistId;
            const int currentPlaylistId =
                m_state->game.playlistId;
            const bool latchCurrentMatchPlaylist =
                m_state->game.inMatch &&
                m_state->game.roundEverStarted &&
                !m_state->game.matchFinalized &&
                PlaylistMetadata::IsKnown(currentPlaylistId) &&
                !PlaylistMetadata::IsNonRecordable(
                    currentPlaylistId) &&
                newPlaylistId != currentPlaylistId;
            if (latchCurrentMatchPlaylist) {
                std::cout
                    << "[Playlist] Ignoring mid-match PlaylistId transition "
                    << currentPlaylistId << " -> " << newPlaylistId
                    << "; keeping the playlist latched to this match.\n";
            } else {
                const bool previousIsNonRecordable =
                    m_state->game.fallbackNonRecordableContext ||
                    PlaylistMetadata::IsNonRecordable(currentPlaylistId);
                const bool nonRecordableToDifferentPlaylist =
                    m_state->game.inMatch &&
                    previousIsNonRecordable &&
                    newPlaylistId != currentPlaylistId;

                if (nonRecordableToDifferentPlaylist) {
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

                    const std::string matchGuid = m_state->game.matchGuid;
                    const std::string currentArena = m_state->game.arenaName;
                    const std::string currentArenaAsset = m_state->game.arenaAsset;
                    m_state->resetMatch(currentArena, currentArenaAsset);
                    m_state->game.activeMatchGeneration =
                        ++m_nextMatchGeneration;
                    m_state->game.matchGuid = matchGuid;
                    m_missingGuidAssociationBlockedByReconnect = false;
                    if (hasInitialMmrSnapshot) {
                        m_state->game.preMatchMmrByGuid.emplace(
                            matchGuid,
                            std::move(initialMmrSnapshot));
                    }
                    m_roundActive = false;
                    m_autoSwitchedPlaylistCategory = MmrCategory::Best;
                    m_followedGraphPlaylistCategory = MmrCategory::Best;
                    m_lastPlayerBoost.clear();
                    m_lastPlayerSeen.clear();
                }
                if (newPlaylistId != currentPlaylistId) {
                    const std::string mode =
                        PlaylistMetadata::CanonicalMode(
                            newPlaylistId);
                    std::cout
                        << "[Playlist] PlaylistId=" << newPlaylistId
                        << ", mode="
                        << (mode.empty() ? "unknown" : mode);
                    if (!PlaylistMetadata::IsKnown(newPlaylistId)) {
                        std::cout
                            << ", action=no-heuristic-fallback";
                    }
                    std::cout << "\n";
                }
                m_state->game.playlistId = newPlaylistId;
            }
        }
        if (game.contains("Teams") && game["Teams"].is_array()) {
            for (const auto& t : game["Teams"]) {
                if (t.contains("TeamNum") && t.contains("Score")) {
                    int teamNum = t["TeamNum"];
                    if (teamNum == 0 || teamNum == 1)
                        m_state->game.score[teamNum] = t["Score"];
                }
            }
        }
    }
    UpdateLifecycleSignalsLocked(data);

    if (isSpectator && !gameReplayActive) {
        m_state->game.localPlayerWasSpectator = true;
    }

    if (gameReplayActive) return;

    if (data.contains("Players") && data["Players"].is_array()) {
        std::array<int, 2> teamCounts{0, 0};
        for (const auto& p : data["Players"]) {
            if (p.contains("TeamNum") && p["TeamNum"].is_number_integer()) {
                int team = p["TeamNum"].get<int>();
                if (team == 0 || team == 1) {
                    teamCounts[team]++;
                }
            }
        }
        // Player-count observations are legacy fallback state only. Once Rocket
        // League supplies Game.PlaylistId, they must not participate in match
        // identity, MMR ownership, or validity decisions.
        if (!PlaylistMetadata::HasAuthoritativeId(m_state->game.playlistId)) {
            m_state->game.legacyCurrentTeamPlayersSeen = teamCounts;
            m_state->game.legacyMaxTeamPlayersSeen[0] =
                std::max(m_state->game.legacyMaxTeamPlayersSeen[0], teamCounts[0]);
            m_state->game.legacyMaxTeamPlayersSeen[1] =
                std::max(m_state->game.legacyMaxTeamPlayersSeen[1], teamCounts[1]);
            const int observedPlayerCount = teamCounts[0] + teamCounts[1];
            m_state->game.legacyMaxPlayersSeen =
                std::max(m_state->game.legacyMaxPlayersSeen, observedPlayerCount);
        }
        for (const auto& p : data["Players"]) {
            if (p.contains("PrimaryId") && p["PrimaryId"].is_string() &&
                p.contains("TeamNum") && p["TeamNum"].is_number_integer() &&
                p.contains("Name") && p["Name"].is_string()) {
                std::string pid = p["PrimaryId"].get<std::string>();
                if (pid == "Unknown" || pid.rfind("Unknown|", 0) == 0)
                    pid = "Unknown|" + p["Name"].get<std::string>();
                int team = p["TeamNum"].get<int>();

                if (p.contains("Boost") && p["Boost"].is_number_integer()) {
                    int currentBoost = p["Boost"].get<int>();
                    if (!gameReplayActive && m_roundActive) {
                        if (m_lastPlayerBoost.count(pid)) {
                            int lastBoost = m_lastPlayerBoost[pid];
                            if (currentBoost > lastBoost) {
                                int diff = currentBoost - lastBoost;
                                if (currentBoost != 33) {
                                    m_state->game.currentMatch.boostPickedUp += diff;
                                    if (pid == m_state->game.myPrimaryId) {
                                        m_state->game.currentMatch.boostPickedUpSelf += diff;
                                    }
                                }
                            }
                        }
                    }
                    m_lastPlayerBoost[pid] = currentBoost;
                }

                if (team == 0 || team == 1) {
                    if (m_state->game.roster.find(pid) == m_state->game.roster.end()) {
                        bool isBot = (pid.rfind("Unknown|", 0) == 0);
                        m_state->game.roster[pid] = PlayerData{
                            .primaryId = pid,
                            .name = p["Name"].get<std::string>(),
                            .team = team,
                            .fetched = isBot,
                            .enqueued = true};
                        effects.fetchMmrQueue.emplace_back(pid, p["Name"].get<std::string>());
                        effects.fetchEncounterQueue.push_back(pid);
                    } else {
                        auto& existing = m_state->game.roster[pid];
                        existing.name = p["Name"].get<std::string>();
                        existing.team = team;
                    }
                    m_state->game.matchRoster[pid] = m_state->game.roster[pid];
                }
            }
        }

        if (!isSpectator && !gameReplayActive) {
            bool hasLocalPlayerFeeds = false;
            bool seenMyIdInLobby = false;
            std::unordered_set<std::string> currentCandidates;
            std::string mySavedId = m_state->game.myPrimaryId;
            static std::string lastKnownMyId = "";
            if (mySavedId != lastKnownMyId) {
                lastKnownMyId = mySavedId;
                m_identityCandidates.clear();
                m_missedMyIdCount = 0;
            }

            for (const auto& p : data["Players"]) {
                if (p.contains("PrimaryId") && p["PrimaryId"].is_string() &&
                    p.contains("TeamNum") && p["TeamNum"].is_number_integer() &&
                    p.contains("Name") && p["Name"].is_string()) {
                    std::string pid = p["PrimaryId"].get<std::string>();
                    if (pid == "Unknown" || pid.rfind("Unknown|", 0) == 0)
                        pid = "Unknown|" + p["Name"].get<std::string>();
                    int team = p["TeamNum"].get<int>();

                    bool isPcPlatform = false;
                    size_t delimPlat = pid.find('|');
                    if (delimPlat != std::string::npos) {
                        std::string plat = pid.substr(0, delimPlat);
                        std::transform(plat.begin(), plat.end(), plat.begin(), ::tolower);
                        if (plat == "steam" || plat == "epic" || plat == "epicgames")
                            isPcPlatform = true;
                    }

                    bool isLocalPlayerFeed = isPcPlatform && (p.contains("Boost") || p.contains("bOnGround"));
                    if (isLocalPlayerFeed) {
                        hasLocalPlayerFeeds = true;
                        currentCandidates.insert(pid);
                    }

                    if (!mySavedId.empty() && pid == mySavedId) {
                        seenMyIdInLobby = true;
                        if (m_state->game.myTeam == -1) {
                            m_state->game.myTeam = team;
                            std::cout << "[Identity] Assigned myTeam: " << team << " based on saved ID " << PrivacyLog::Sensitive(mySavedId, "player ID") << "\n";
                            effects.fetchLifetimeHistory = true;
                            effects.lifetimePrimaryId = mySavedId;
                            effects.lifetimeCategory = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                            effects.refreshDbStats = true;
                            effects.refreshStatsPrimaryId = mySavedId;
                        }
                    }
                }
            }

            if (hasLocalPlayerFeeds) {
                // If we already have a saved id but we do NOT see it in the lobby and
                // we observe exactly one local feed candidate, assume the user switched
                // accounts and update immediately to that candidate.
                if (!mySavedId.empty() && !seenMyIdInLobby && !currentCandidates.empty()) {
                    // Deterministically pick a candidate (lexicographically smallest) to avoid
                    // non-determinism from unordered_set iteration order.
                    std::string newId = *std::min_element(currentCandidates.begin(), currentCandidates.end());
                    if (newId != mySavedId) {
                        m_state->game.myPrimaryId = newId;
                        // Try to pick up team info from the current players array
                        for (const auto& p : data["Players"]) {
                            if (p.contains("PrimaryId") && p["PrimaryId"].is_string() && p["PrimaryId"].get<std::string>() == newId) {
                                if (p.contains("TeamNum") && p["TeamNum"].is_number_integer())
                                    m_state->game.myTeam = p["TeamNum"].get<int>();
                                break;
                            }
                        }
                        Config::Update([&newId](ConfigData& c) { c.last_primary_id = newId; });
                        std::cout << "[Identity] Detected account switch. New ID: " << PrivacyLog::Sensitive(newId, "player ID") << "\n";
                        m_identityCandidates.clear();
                        if (m_state->game.roster.count(newId)) {
                            auto& self = m_state->game.roster[newId];
                            if (!self.enqueued && self.mmr == 0) {
                                self.enqueued = true;
                                std::cout << "[Identity] Identified local player: " << PrivacyLog::Sensitive(self.name, "player name") << ". Fetching self MMR...\n";
                                effects.fetchMmrQueue.emplace_back(newId, self.name);
                            }
                        }
                        effects.fetchLifetimeHistory = true;
                        effects.lifetimePrimaryId = newId;
                        effects.lifetimeCategory = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                        effects.refreshDbStats = true;
                        effects.refreshStatsPrimaryId = newId;
                        // reset missed count since we resolved identity
                        m_missedMyIdCount = 0;
                    }
                }

                if (!mySavedId.empty()) {
                    if (seenMyIdInLobby) {
                        m_missedMyIdCount = 0;
                    } else {
                        m_missedMyIdCount++;
                        if (m_missedMyIdCount >= 2) {
                            std::cout << "[Identity] Missed saved ID " << PrivacyLog::Sensitive(mySavedId, "player ID") << " for 2 matches. Resetting...\n";
                            m_state->game.myPrimaryId = "";
                            m_state->game.myTeam = -1;
                            mySavedId = "";
                            m_missedMyIdCount = 0;
                            m_identityCandidates.clear();
                            Config::Update([](ConfigData& c) { c.last_primary_id = ""; });
                        }
                    }
                }

                if (mySavedId.empty() && !currentCandidates.empty()) {
                    // Standard process-of-elimination flow when we don't already have a saved identity.
                    if (m_identityCandidates.empty()) {
                        m_identityCandidates = currentCandidates;
                        std::cout << "[Identity] Process of elimination started. Candidates: " << m_identityCandidates.size() << "\n";
                    } else {
                        std::unordered_set<std::string> intersection;
                        for (const auto& cand : m_identityCandidates)
                            if (currentCandidates.count(cand)) intersection.insert(cand);
                        m_identityCandidates = intersection;
                        std::cout << "[Identity] Intersected candidates. Remaining: " << m_identityCandidates.size() << "\n";
                    }

                    if (m_identityCandidates.size() == 1) {
                        std::string identifiedId = *m_identityCandidates.begin();
                        m_state->game.myPrimaryId = identifiedId;
                        for (const auto& p : data["Players"]) {
                            if (p.contains("PrimaryId") && p["PrimaryId"].is_string() && p["PrimaryId"].get<std::string>() == identifiedId) {
                                if (p.contains("TeamNum") && p["TeamNum"].is_number_integer())
                                    m_state->game.myTeam = p["TeamNum"].get<int>();
                                break;
                            }
                        }
                        Config::Update([&identifiedId](ConfigData& c) { c.last_primary_id = identifiedId; });
                        std::cout << "[Identity] Identified local player: " << PrivacyLog::Sensitive(identifiedId, "player ID") << "\n";
                        m_identityCandidates.clear();
                        if (m_state->game.roster.count(identifiedId)) {
                            auto& self = m_state->game.roster[identifiedId];
                            if (!self.enqueued && self.mmr == 0) {
                                self.enqueued = true;
                                std::cout << "[Identity] Identified local player: " << PrivacyLog::Sensitive(self.name, "player name") << ". Fetching self MMR...\n";
                                effects.fetchMmrQueue.emplace_back(identifiedId, self.name);
                            }
                        }
                        effects.fetchLifetimeHistory = true;
                        effects.lifetimePrimaryId = identifiedId;
                        effects.lifetimeCategory = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                        effects.refreshDbStats = true;
                        effects.refreshStatsPrimaryId = identifiedId;
                    } else if (m_identityCandidates.empty()) {
                        m_identityCandidates = currentCandidates;
                    }
                }
            }

            const MmrCategory inferredPlaylistCat =
                CategoryFromMatchContext(m_state->game);
            const bool inferredCategoryIsHiddenExtra =
                IsExtraMmrCategory(inferredPlaylistCat) &&
                !m_cachedConf.show_extra_playlists;

            const MmrCategory lastAutoCat =
                m_autoSwitchedPlaylistCategory;
            const bool liveCategoryChangedAfterAutoSwitch =
                lastAutoCat != MmrCategory::Best &&
                m_state->ui.rosterMmrCategory.load() != lastAutoCat;
            if (m_cachedConf.auto_switch_mmr_category &&
                !inferredCategoryIsHiddenExtra &&
                !liveCategoryChangedAfterAutoSwitch &&
                IsSupportedMmrCategory(inferredPlaylistCat) &&
                inferredPlaylistCat != lastAutoCat) {
                m_autoSwitchedPlaylistCategory = inferredPlaylistCat;
                if (m_state->ui.rosterMmrCategory.load() !=
                    inferredPlaylistCat) {
                    m_state->ui.rosterMmrCategory.store(
                        inferredPlaylistCat);
                    std::cout
                        << "[Identity] Live MMR following playlist category: "
                        << MmrCategoryToString(inferredPlaylistCat) << "\n";
                }
            }

            if (m_cachedConf.graph_follow_current_playlist) {
                if (!m_nonLiveReplayActive &&
                    !inferredCategoryIsHiddenExtra &&
                    IsSupportedMmrCategory(inferredPlaylistCat) &&
                    inferredPlaylistCat !=
                        m_followedGraphPlaylistCategory) {
                    m_followedGraphPlaylistCategory =
                        inferredPlaylistCat;
                    if (m_state->ui.graphMmrCategory.load() !=
                        inferredPlaylistCat) {
                        m_state->ui.graphMmrCategory.store(
                            inferredPlaylistCat);
                        std::cout
                            << "[Identity] Graph following playlist category: "
                            << MmrCategoryToString(inferredPlaylistCat)
                            << "\n";
                    }
                }
            } else {
                m_followedGraphPlaylistCategory = MmrCategory::Best;
                MmrCategory fallbackCategory = StringToMmrCategory(
                    m_cachedConf.graph_mmr_category);
                if (fallbackCategory == MmrCategory::Best ||
                    (!m_cachedConf.show_extra_playlists &&
                     IsExtraMmrCategory(fallbackCategory))) {
                    fallbackCategory = MmrCategory::TwoVTwo;
                }
                if (m_state->ui.graphMmrCategory.load() !=
                    fallbackCategory) {
                    m_state->ui.graphMmrCategory.store(
                        fallbackCategory);
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        for (const auto& p : data["Players"]) {
            if (p.contains("PrimaryId") && p["PrimaryId"].is_string()) {
                std::string pid = p["PrimaryId"].get<std::string>();
                if (pid == "Unknown" || pid.rfind("Unknown|", 0) == 0) {
                    if (p.contains("Name") && p["Name"].is_string())
                        pid = "Unknown|" + p["Name"].get<std::string>();
                }
                m_lastPlayerSeen[pid] = now;
            }
        }

        auto it = m_state->game.roster.begin();
        while (it != m_state->game.roster.end()) {
            std::string pid = it->first;
            bool shouldPrune = false;
            if (m_lastPlayerSeen.find(pid) == m_lastPlayerSeen.end()) {
                m_lastPlayerSeen[pid] = now;
            } else {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastPlayerSeen[pid]).count();
                if (elapsed >= 5) shouldPrune = true;
            }
            if (shouldPrune && pid != m_state->game.myPrimaryId) {
                std::cout << "[StatsClient] Removing player who left: " << PrivacyLog::Sensitive(it->second.name, "player name") << " (" << PrivacyLog::Sensitive(pid, "player ID") << ")\n";
                m_state->game.matchRoster[pid] = it->second;
                m_lastPlayerBoost.erase(pid);
                m_lastPlayerSeen.erase(pid);
                it = m_state->game.roster.erase(it);
            } else {
                if (shouldPrune && pid == m_state->game.myPrimaryId) m_lastPlayerSeen[pid] = now;
                ++it;
            }
        }

        if (!PlaylistMetadata::HasAuthoritativeId(m_state->game.playlistId)) {
            const MmrCategory rosterCategory =
                m_state->ui.rosterMmrCategory.load();
            const std::string mode = InferModeFromMatchState(
                m_state->game, rosterCategory);
            const int expectedTeamSize = LegacyExpectedTeamSizeForMode(mode);
            if (expectedTeamSize > 0 &&
                m_state->game.legacyCurrentTeamPlayersSeen[0] >= expectedTeamSize &&
                m_state->game.legacyCurrentTeamPlayersSeen[1] >= expectedTeamSize) {
                m_state->game.legacyLobbyWasEverFull = true;
            }
        }

        if (!isSpectator && !gameReplayActive) {
            if (!m_state->game.myPrimaryId.empty() &&
                m_state->game.roster.count(m_state->game.myPrimaryId)) {

                const auto& self = m_state->game.roster.at(m_state->game.myPrimaryId);
                if (self.team == 0 || self.team == 1) {
                    m_state->game.localPlayerWasActive = true;
                    m_state->game.myTeam = self.team;
                }
            }
        }
        if (!m_state->game.myPrimaryId.empty()) {
            bool localPlayerPresent = false;
            for (const auto& player : data["Players"]) {
                if (!player.contains("PrimaryId") ||
                    !player["PrimaryId"].is_string()) {
                    continue;
                }
                std::string playerId = player["PrimaryId"].get<std::string>();
                if (playerId == "Unknown" ||
                    playerId.rfind("Unknown|", 0) == 0) {
                    if (player.contains("Name") && player["Name"].is_string()) {
                        playerId =
                            "Unknown|" + player["Name"].get<std::string>();
                    }
                }
                if (playerId == m_state->game.myPrimaryId) {
                    localPlayerPresent = true;
                    break;
                }
            }
            m_state->game.localPlayerPresenceObserved = true;
            m_state->game.localPlayerPresentInLatestUpdate =
                localPlayerPresent;
        }

        CapturePreMatchMmrLocked();
    }

    if (m_cachedConf.auto_save_replays && !m_state->game.matchGuid.empty() && m_state->game.myTeam != -1) {
        if (m_lastQueuedReplayGuid != m_state->game.matchGuid) {
            m_lastQueuedReplayGuid = m_state->game.matchGuid;
            effects.replayKeyToPress = m_cachedConf.key_save_replay;
            std::cout << "[StatsClient] Auto-Save Replay: Queueing save replay keybind (VK: " << effects.replayKeyToPress << ") in 3 seconds...\n";
        }
    }
}
