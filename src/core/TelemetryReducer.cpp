#define NOMINMAX
#include "TelemetryReducer.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <cctype>
#include <optional>

static std::string FormatArenaName(const std::string& asset) {
    if (asset.empty()) return "";
    std::string base = asset;
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    if (base.length() > 2 && base.substr(base.length() - 2) == "_p") {
        base = base.substr(0, base.length() - 2);
    }
    static const std::unordered_map<std::string, std::string> ARENA_BASE = {
        {"stadium", "DFH Stadium"}, {"park", "Beckwith Park"}, {"mannfield", "Mannfield"}, {"trainstation", "Urban Central"}, {"haunted_trainstation", "Urban Central (Haunted)"}, {"underwater", "AquaDome"}, {"wasteland", "Wasteland"}, {"neotokyo", "Neo Tokyo"}, {"neotokyo_standard", "Neo Tokyo"}, {"eurostadium", "Champions Field"}, {"beach", "Salty Shores"}, {"beachvolley", "Salty Shores"}, {"chinastadium", "Forbidden Temple"}, {"temple", "Forbidden Temple"}, {"cosmic", "Starbase ARC"}, {"arc_standard", "Starbase ARC"}, {"throwback_stadium", "Throwback Stadium"}, {"hoops_dunkhouse", "DunkHouse"}, {"music", "Estadio Vida"}, {"estadio_vida", "Estadio Vida"}, {"farm", "Farmstead"}, {"outlaw_oasis", "Deadeye Canyon"}, {"canyon", "Deadeye Canyon"}, {"shattershot", "Core 707"}, {"labs_octagon", "Octagon"}, {"labs_pillars", "Pillars"}, {"labs_cosmic", "Cosmic"}, {"labs_double_goal", "Double Goal"}, {"labs_underpass", "Underpass"}, {"labs_utopia", "Utopia Retro"}, {"neoasphalt", "Neon Fields"}, {"neon", "Neon Fields"}, {"utopia", "Utopia Coliseum"}, {"sovereign", "Sovereign Heights"}};
    static const std::unordered_map<std::string, std::string> ARENA_VARIANT = {
        {"night", "Night"}, {"day", "Day"}, {"rainy", "Stormy"}, {"stormy", "Stormy"}, {"race_day", "Stormy"}, {"snowy", "Snowy"}, {"snowfall", "Snowy"}, {"dawn", "Dawn"}, {"spring", "Spring"}, {"spooky", "Spooky"}, {"circuit", "Circuit"}};
    auto baseIt = ARENA_BASE.find(base);
    if (baseIt != ARENA_BASE.end()) return baseIt->second;
    size_t lastUnderscore = base.find_last_of('_');
    if (lastUnderscore != std::string::npos) {
        std::string candidate = base.substr(0, lastUnderscore);
        std::string variant = base.substr(lastUnderscore + 1);
        auto candIt = ARENA_BASE.find(candidate);
        if (candIt != ARENA_BASE.end()) {
            auto varIt = ARENA_VARIANT.find(variant);
            if (varIt != ARENA_VARIANT.end()) return candIt->second + " (" + varIt->second + ")";
            if (!variant.empty()) {
                variant[0] = toupper(variant[0]);
                return candIt->second + " (" + variant + ")";
            }
            return candIt->second;
        }
    }
    return asset;
}

static MmrCategory CategoryFromTeamCounts(const std::array<int, 2>& teamCounts, bool roundStarted) {
    int teamSize = std::min(teamCounts[0], teamCounts[1]);
    if (teamSize >= 3) return MmrCategory::ThreeVThree;
    if (teamSize >= 2) return MmrCategory::TwoVTwo;
    if (roundStarted && teamCounts[0] == 1 && teamCounts[1] == 1) return MmrCategory::OneVOne;
    return MmrCategory::Best;
}

static MmrCategory CategoryFromMode(const std::string& mode) {
    if (mode == "1v1") return MmrCategory::OneVOne;
    if (mode == "2v2") return MmrCategory::TwoVTwo;
    if (mode == "3v3") return MmrCategory::ThreeVThree;
    if (mode == "hoops") return MmrCategory::Hoops;
    if (mode == "rumble") return MmrCategory::Rumble;
    if (mode == "dropshot") return MmrCategory::Dropshot;
    if (mode == "snowday") return MmrCategory::SnowDay;
    if (mode == "heatseeker") return MmrCategory::Heatseeker;
    return MmrCategory::Best;
}

static std::string InferModeFromMatchState(
    const GameState& game,
    MmrCategory rosterCategory) {
    std::string arenaKey = !game.arenaAsset.empty()
                               ? game.arenaAsset
                               : game.arenaName;
    return GamemodeUtils::InferFromSnapshot(
        game.maxPlayersSeen,
        static_cast<int>(game.roster.size()),
        rosterCategory,
        MmrCategory::Best,
        arenaKey);
}

static MmrCategory CategoryFromMatchContext(const GameState& game) {
    std::string arenaKey = !game.arenaAsset.empty() ? game.arenaAsset : game.arenaName;
    MmrCategory arenaCategory = CategoryFromMode(GamemodeUtils::InferFromArenaName(arenaKey));
    if (arenaCategory != MmrCategory::Best) return arenaCategory;
    return CategoryFromTeamCounts(game.maxTeamPlayersSeen, game.roundEverStarted);
}

static bool IsSupportedGraphCategory(MmrCategory category) {
    return category == MmrCategory::OneVOne ||
           category == MmrCategory::TwoVTwo ||
           category == MmrCategory::ThreeVThree ||
           IsExtraMmrCategory(category);
}

static bool IsTrackedRankedEarlyExitMode(const std::string& mode) {
    return mode == "1v1" || mode == "2v2" || mode == "3v3" ||
           mode == "hoops" || mode == "rumble" || mode == "dropshot" ||
           mode == "snowday" || mode == "heatseeker";
}

static std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

static bool ReadTrueBoolean(const nlohmann::json& object,
                            std::initializer_list<const char*> keys) {
    if (!object.is_object()) return false;
    for (const char* key : keys) {
        if (object.contains(key) && object[key].is_boolean() &&
            object[key].get<bool>()) {
            return true;
        }
    }
    return false;
}

static std::optional<int> ReadInteger(const nlohmann::json& object,
                                      std::initializer_list<const char*> keys) {
    if (!object.is_object()) return std::nullopt;
    for (const char* key : keys) {
        if (object.contains(key) && object[key].is_number_integer()) {
            return object[key].get<int>();
        }
    }
    return std::nullopt;
}

static bool ContainsExcludedMatchType(const nlohmann::json& game) {
    if (!game.is_object()) return false;
    for (const char* key : {"Playlist", "PlaylistName", "MatchType", "GameMode"}) {
        if (!game.contains(key) || !game[key].is_string()) continue;
        const std::string value = Lowercase(game[key].get<std::string>());
        if (value.find("casual") != std::string::npos ||
            value.find("private") != std::string::npos ||
            value.find("training") != std::string::npos ||
            value.find("exhibition") != std::string::npos) {
            return true;
        }
    }
    return false;
}

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
        const bool startsNewLifecycle =
            !wasInMatch ||
            (hasIncomingGuid && hadCurrentGuid &&
             incomingGuid != m_state->game.matchGuid);
        const bool attachesGuid =
            wasInMatch && hasIncomingGuid &&
            !hadCurrentGuid;

        const char* action = "preserve-current";
        const char* reason =
            hasIncomingGuid
                ? "same-explicit-guid"
                : "missing-guid-active-lifecycle";

        if (startsNewLifecycle) {
            action = "start-new";
            reason = wasInMatch
                         ? "different-explicit-guid"
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

    if (ReadTrueBoolean(
            *game,
            {"bPrivateMatch",
             "bTraining",
             "bExhibition",
             "bCasualMatch",
             "bTournamentSpectator"}) ||
        ContainsExcludedMatchType(*game)) {
        m_state->game.excludedEarlyExitContext = true;
        m_state->game.earlyExitExclusionReason =
            "explicit_non_competitive_context";
    }
}

void TelemetryReducer::HandleUpdateState(const nlohmann::json& data, SideEffects& effects) {
    bool gameReplayActive = false;
    bool isSpectator = false;

    if (data.contains("Game") && data["Game"].is_object()) {
        auto game = data["Game"];
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
                (currentArena != m_state->game.arenaName || currentArenaAsset != m_state->game.arenaAsset)) {
                const std::string matchGuid = m_state->game.matchGuid;
                const bool startsNewLifecycle =
                    !m_state->game.inMatch;
                LocalPreMatchMmrSnapshot preservedMmrSnapshot;
                bool hasPreservedMmrSnapshot = false;
                const auto snapshotIt = m_state->game.preMatchMmrByGuid.find(matchGuid);
                if (snapshotIt !=
                    m_state->game.preMatchMmrByGuid.end()) {
                    preservedMmrSnapshot = std::move(snapshotIt->second);
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
                        matchGuid, std::move(preservedMmrSnapshot));
                } else if (startsNewLifecycle) {
                    m_state->game.matchGuid.clear();
                }
                m_roundActive = false;
                m_autoSwitchedPlaylistCategory = MmrCategory::Best;
                m_followedGraphPlaylistCategory = MmrCategory::Best;
                m_lastPlayerBoost.clear();
                m_lastPlayerSeen.clear();
                std::cout << "\n========================================\n";
                std::cout << "[Event] Match Started in: " << m_state->game.arenaName << "\n";
                effects.pushDiscord = true;
                effects.discordSnapshot = BuildDiscordSnapshotLocked();
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
        m_state->game.currentTeamPlayersSeen = teamCounts;
        m_state->game.maxTeamPlayersSeen[0] = std::max(m_state->game.maxTeamPlayersSeen[0], teamCounts[0]);
        m_state->game.maxTeamPlayersSeen[1] = std::max(m_state->game.maxTeamPlayersSeen[1], teamCounts[1]);

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
                                        m_state->game.sessionTotals.boostPickedUp += diff;
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
                IsSupportedGraphCategory(inferredPlaylistCat) &&
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
                    !m_state->game.excludedEarlyExitContext &&
                    !inferredCategoryIsHiddenExtra &&
                    IsSupportedGraphCategory(inferredPlaylistCat) &&
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
        m_state->game.maxPlayersSeen = std::max(m_state->game.maxPlayersSeen, static_cast<int>(m_state->game.roster.size()));

        {
            const MmrCategory rosterCategory =
                m_state->ui.rosterMmrCategory.load();
            const std::string mode = InferModeFromMatchState(
                m_state->game, rosterCategory);
            const int expectedTeamSize =
                ExpectedTeamSizeForMode(mode);
            if (expectedTeamSize > 0 &&
                m_state->game.currentTeamPlayersSeen[0] >=
                    expectedTeamSize &&
                m_state->game.currentTeamPlayersSeen[1] >=
                    expectedTeamSize) {
                m_state->game.lobbyWasEverFull = true;
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

void TelemetryReducer::HandleStatFeed(const nlohmann::json& data) {
    if (m_state->game.inReplay) return;
    if (!data.contains("EventName") || !data["EventName"].is_string()) return;
    std::string feedEvent = data["EventName"].get<std::string>();
    std::string mainName = "", mainId = "", secName = "", secId = "";

    if (data.contains("MainTarget") && data["MainTarget"].is_object()) {
        auto mt = data["MainTarget"];
        if (mt.contains("Name") && mt["Name"].is_string()) mainName = mt["Name"].get<std::string>();
        if (mt.contains("PrimaryId") && mt["PrimaryId"].is_string()) mainId = mt["PrimaryId"].get<std::string>();
    } else if (data.contains("Player") && data["Player"].is_object()) {
        auto p = data["Player"];
        if (p.contains("Name") && p["Name"].is_string()) mainName = p["Name"].get<std::string>();
        if (p.contains("PrimaryId") && p["PrimaryId"].is_string()) mainId = p["PrimaryId"].get<std::string>();
    }
    if (mainId == "Unknown" || mainId.rfind("Unknown|", 0) == 0) mainId = "Unknown|" + mainName;
    if (data.contains("SecondaryTarget") && data["SecondaryTarget"].is_object()) {
        auto st = data["SecondaryTarget"];
        if (st.contains("Name") && st["Name"].is_string()) secName = st["Name"].get<std::string>();
        if (st.contains("PrimaryId") && st["PrimaryId"].is_string()) secId = st["PrimaryId"].get<std::string>();
    }
    if (secId == "Unknown" || secId.rfind("Unknown|", 0) == 0) secId = "Unknown|" + secName;

    bool isMainSelf = !mainId.empty() ? IsSelfById(mainId) : IsSelf(mainName);
    bool isSecSelf = !secId.empty() ? IsSelfById(secId) : IsSelf(secName);

    if (feedEvent == "Save" || feedEvent == "EpicSave" || feedEvent == "Epic Save") {
        m_state->game.currentMatch.saves++;
        if (isMainSelf) m_state->game.currentMatch.savesSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].saves++;
    } else if (feedEvent == "Shot") {
        m_state->game.currentMatch.shots++;
        if (isMainSelf) m_state->game.currentMatch.shotsSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].shots++;
    } else if (feedEvent == "Demolish") {
        m_state->game.currentMatch.demos++;
        if (isMainSelf) m_state->game.currentMatch.demosSelf++;
        if (isSecSelf) m_state->game.currentMatch.demoedSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].demos++;
    } else if (feedEvent == "OwnGoal") {
        m_state->game.currentMatch.ownGoals++;
        if (isMainSelf) m_state->game.currentMatch.ownGoalsSelf++;
        std::cout << "[Event] OWN GOAL by " << PrivacyLog::Sensitive(mainName, "player name") << "!\n";
    } else if (feedEvent == "Assist") {
        m_state->game.currentMatch.assists++;
        if (isMainSelf) m_state->game.currentMatch.assistsSelf++;
        if (!mainId.empty() && m_state->game.roster.count(mainId)) m_state->game.roster[mainId].assists++;
    }
}

void TelemetryReducer::HandleGoalScored(const nlohmann::json& data, SideEffects& effects) {
    if (m_state->game.inReplay) return;
    m_roundActive = false;
    m_state->game.currentMatch.goals++;
    std::string scorerName = "", scorerId = "";
    nlohmann::json scorer;
    bool hasScorer = false;
    if (data.contains("Scorer") && data["Scorer"].is_object()) {
        scorer = data["Scorer"];
        if (scorer.contains("Name") && scorer["Name"].is_string()) scorerName = scorer["Name"].get<std::string>();
        if (scorer.contains("PrimaryId") && scorer["PrimaryId"].is_string()) scorerId = scorer["PrimaryId"].get<std::string>();
        hasScorer = true;
    }
    if (scorerId == "Unknown" || scorerId.rfind("Unknown|", 0) == 0) scorerId = "Unknown|" + scorerName;
    bool isScorerSelf = !scorerId.empty() ? IsSelfById(scorerId) : IsSelf(scorerName);
    if (hasScorer) {
        if (isScorerSelf) m_state->game.currentMatch.goalsSelf++;
        if (!scorerId.empty() && m_state->game.roster.count(scorerId))
            m_state->game.roster[scorerId].goals++;
    }
    if (data.contains("GoalSpeed") && data["GoalSpeed"].is_number()) {
        float currentSpeed = data["GoalSpeed"].get<float>();
        m_state->game.currentMatch.maxGoalSpeed = std::max(m_state->game.currentMatch.maxGoalSpeed, currentSpeed);
        if (hasScorer && isScorerSelf)
            m_state->game.currentMatch.maxGoalSpeedSelf = std::max(m_state->game.currentMatch.maxGoalSpeedSelf, currentSpeed);
        if (!scorerId.empty() && m_state->game.roster.count(scorerId))
            m_state->game.roster[scorerId].maxGoalSpeed = std::max(m_state->game.roster[scorerId].maxGoalSpeed, currentSpeed);
    }
    if (data.contains("GoalTime") && data["GoalTime"].is_number()) {
        float time = data["GoalTime"].get<float>();
        if (time > 0) {
            if (m_state->game.currentMatch.fastestGoalTime == 0.0f || time < m_state->game.currentMatch.fastestGoalTime)
                m_state->game.currentMatch.fastestGoalTime = time;
            if (hasScorer && isScorerSelf)
                if (m_state->game.currentMatch.fastestGoalTimeSelf == 0.0f || time < m_state->game.currentMatch.fastestGoalTimeSelf)
                    m_state->game.currentMatch.fastestGoalTimeSelf = time;
            if (!scorerId.empty() && m_state->game.roster.count(scorerId))
                if (m_state->game.roster[scorerId].fastestGoalTime == 0.0f || time < m_state->game.roster[scorerId].fastestGoalTime)
                    m_state->game.roster[scorerId].fastestGoalTime = time;
        }
    }
    std::cout << "[Event] GOAL SCORED!\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

void TelemetryReducer::HandleBallHit(const nlohmann::json& data) {
    if (data.contains("Ball") && data["Ball"].is_object()) {
        auto ball = data["Ball"];
        if (ball.contains("PostHitSpeed") && ball["PostHitSpeed"].is_number()) {
            float sp = ball["PostHitSpeed"].get<float>();
            m_state->game.currentMatch.maxBallSpeed = std::max(m_state->game.currentMatch.maxBallSpeed, sp);
            if (data.contains("Players") && data["Players"].is_array()) {
                for (const auto& h : data["Players"]) {
                    if (h.is_object()) {
                        std::string pid = (h.contains("PrimaryId") && h["PrimaryId"].is_string()) ? h["PrimaryId"].get<std::string>() : "";
                        std::string name = (h.contains("Name") && h["Name"].is_string()) ? h["Name"].get<std::string>() : "";
                        if (pid == "Unknown" || pid.rfind("Unknown|", 0) == 0) pid = "Unknown|" + name;
                        if (!pid.empty() ? IsSelfById(pid) : IsSelf(name)) {
                            m_state->game.currentMatch.maxBallSpeedSelf = std::max(m_state->game.currentMatch.maxBallSpeedSelf, sp);
                            break;
                        }
                    }
                }
            }
        }
    }
}

void TelemetryReducer::HandleCrossbarHit(const nlohmann::json& data) {
    m_state->game.currentMatch.crossbars++;
    std::string toucherName = "", toucherId = "";
    if (data.contains("BallLastTouch") && data["BallLastTouch"].is_object()) {
        auto lt = data["BallLastTouch"];
        if (lt.contains("Player") && lt["Player"].is_object()) {
            auto ltp = lt["Player"];
            if (ltp.contains("Name") && ltp["Name"].is_string()) toucherName = ltp["Name"].get<std::string>();
            if (ltp.contains("PrimaryId") && ltp["PrimaryId"].is_string()) toucherId = ltp["PrimaryId"].get<std::string>();
        }
    }
    if (toucherId == "Unknown" || toucherId.rfind("Unknown|", 0) == 0) toucherId = "Unknown|" + toucherName;
    bool isToucherSelf = !toucherId.empty() ? IsSelfById(toucherId) : IsSelf(toucherName);
    if (isToucherSelf) m_state->game.currentMatch.crossbarsSelf++;
    if (data.contains("ImpactForce") && data["ImpactForce"].is_number()) {
        float ifo = data["ImpactForce"].get<float>();
        m_state->game.currentMatch.maxImpactForce = std::max(m_state->game.currentMatch.maxImpactForce, ifo);
        if (isToucherSelf) m_state->game.currentMatch.maxImpactForceSelf = std::max(m_state->game.currentMatch.maxImpactForceSelf, ifo);
    }
    std::cout << "[Event] CROSSBAR HIT! Toucher: " << (toucherName.empty() ? "None" : PrivacyLog::Sensitive(toucherName, "player name")) << "\n";
}

int TelemetryReducer::ExpectedTeamSizeForMode(const std::string& mode) {
    if (mode == "1v1") return 1;
    if (mode == "2v2" || mode == "hoops" || mode == "heatseeker") return 2;
    if (mode == "3v3" || mode == "rumble" || mode == "dropshot" || mode == "snowday") return 3;
    return 0;
}

TelemetryReducer::CapturedMatch TelemetryReducer::CaptureMatchLocked() const {
    const auto& game = m_state->game;
    CapturedMatch match;
    match.arenaName = game.arenaName;
    match.arenaAsset = game.arenaAsset;
    match.matchGuid = game.matchGuid;
    match.matchGeneration = game.activeMatchGeneration;
    match.myPrimaryId = game.myPrimaryId;
    match.myTeam = game.myTeam;
    match.score = game.score;
    match.maxPlayersSeen = game.maxPlayersSeen;
    match.maxTeamPlayersSeen = game.maxTeamPlayersSeen;
    match.roundEverStarted = game.roundEverStarted;
    match.localPlayerWasActive = game.localPlayerWasActive;
    match.localPlayerWasSpectator = game.localPlayerWasSpectator;
    match.lobbyWasEverFull = game.lobbyWasEverFull;
    match.localPlayerDisappeared =
        game.localPlayerPresenceObserved &&
        !game.localPlayerPresentInLatestUpdate;
    match.explicitLocalForfeit = game.explicitLocalForfeit;
    match.nonLiveReplay = m_nonLiveReplayActive;
    match.stats = game.currentMatch;
    match.roster = game.matchRoster;
    for (const auto& [primaryId, player] : game.roster) {
        match.roster[primaryId] = player;
    }
    match.rosterMmrCategory = m_state->ui.rosterMmrCategory.load();
    match.graphMmrCategory = m_state->ui.graphMmrCategory.load();
    match.mode = InferModeFromMatchState(
        game, match.rosterMmrCategory);
    const auto snapshotIt = game.preMatchMmrByGuid.find(game.matchGuid);
    if (snapshotIt != game.preMatchMmrByGuid.end()) {
        match.preMatchMmr = snapshotIt->second;
        match.hasPreMatchMmr = true;
    }
    match.endedAtUnixMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return match;
}

bool TelemetryReducer::BuildPostMatchMmrRefreshLocked(
    const CapturedMatch& match,
    bool won,
    PostMatchMmrRefresh& refresh) const {
    if (match.matchGuid.empty() || match.myPrimaryId.empty() ||
        !GamemodeUtils::IsTrackedCompetitiveMode(match.mode)) {
        return false;
    }

    const auto playerIt = match.roster.find(match.myPrimaryId);
    if (playerIt == match.roster.end()) return false;
    const auto& player = playerIt->second;

    int previousMmr = player.mmr;
    int previousMatches = -1;
    bool previousMmrIsPlaylistSpecific = false;
    bool usedCapturedSnapshot = false;
    if (match.hasPreMatchMmr) {
        const auto mmrIt = match.preMatchMmr.playlistMmrs.find(match.mode);
        if (mmrIt != match.preMatchMmr.playlistMmrs.end() &&
            mmrIt->second > 0) {
            previousMmr = mmrIt->second;
            previousMmrIsPlaylistSpecific = true;
            usedCapturedSnapshot = true;
            const auto matchesIt =
                match.preMatchMmr.playlistMatches.find(match.mode);
            if (matchesIt != match.preMatchMmr.playlistMatches.end()) {
                previousMatches = matchesIt->second;
            }
        }
    }
    if (!usedCapturedSnapshot) {
        const auto playlistIt = player.playlists.find(match.mode);
        if (playlistIt != player.playlists.end() && playlistIt->second > 0) {
            previousMmr = playlistIt->second;
            previousMmrIsPlaylistSpecific = true;
        }
        const auto matchesIt = player.playlistMatches.find(match.mode);
        if (matchesIt != player.playlistMatches.end()) {
            previousMatches = matchesIt->second;
        }
    }

    refresh = PostMatchMmrRefresh{
        .primaryId = match.myPrimaryId,
        .name = player.name,
        .matchGuid = match.matchGuid,
        .playlist = match.mode,
        .previousMmr = previousMmr,
        .previousMatches = previousMatches,
        .previousMmrIsPlaylistSpecific =
            previousMmrIsPlaylistSpecific,
        .won = won};
    return true;
}

TelemetryReducer::MatchEndDecision TelemetryReducer::ClassifyMatchEndLocked(
    const CapturedMatch& match,
    int winnerTeam) const {
    MatchEndDecision decision;

    if (match.nonLiveReplay) {
        decision.voidReason = "non_live_replay";
        return decision;
    }

    if (winnerTeam != 0 && winnerTeam != 1) {
        decision.voidReason = "invalid_winner";
        return decision;
    }

    if (match.myTeam != 0 && match.myTeam != 1) {
        decision.voidReason = match.localPlayerWasSpectator
                                  ? "local_player_spectator_bug"
                                  : "local_player_team_unknown";
        return decision;
    }

    if (!match.roundEverStarted) {
        decision.voidReason = "round_never_started";
        return decision;
    }

    if (!match.localPlayerWasActive) {
        decision.voidReason = match.localPlayerWasSpectator
                                  ? "local_player_spectator_bug"
                                  : "local_player_never_active";
        return decision;
    }

    const int expectedTeamSize = ExpectedTeamSizeForMode(match.mode);
    if (expectedTeamSize > 0) {
        bool teamsWereEverFull = match.lobbyWasEverFull;
        if (!teamsWereEverFull) {
            teamsWereEverFull =
                match.maxTeamPlayersSeen[0] >= expectedTeamSize &&
                match.maxTeamPlayersSeen[1] >= expectedTeamSize;
        }

        if (!teamsWereEverFull) {
            decision.voidReason = "lobby_never_full";
            return decision;
        }
    }

    decision.shouldCount = true;
    decision.shouldPersist = true;
    decision.iWon = winnerTeam == match.myTeam;
    decision.resultText = decision.iWon ? "Win" : "Loss";
    return decision;
}

bool TelemetryReducer::IsValidEarlyCompetitiveExitLocked(
    std::string& mode,
    std::string& voidReason) const {
    const auto& game = m_state->game;
    if (!game.inMatch) {
        voidReason = "match_not_active";
        return false;
    }
    if (game.matchFinalized) {
        voidReason = "match_already_finalized";
        return false;
    }
    if (m_nonLiveReplayActive) {
        voidReason = "non_live_replay";
        return false;
    }
    if (game.localPlayerWasSpectator) {
        voidReason = "local_player_spectator_bug";
        return false;
    }
    if (!game.roundEverStarted) {
        voidReason = "round_never_started";
        return false;
    }
    if (!game.localPlayerWasActive) {
        voidReason = "local_player_never_active";
        return false;
    }
    if (game.myTeam != 0 && game.myTeam != 1) {
        voidReason = "local_player_team_unknown";
        return false;
    }
    if (game.matchGuid.empty()) {
        voidReason = "missing_match_guid";
        return false;
    }
    if (game.excludedEarlyExitContext) {
        voidReason = game.earlyExitExclusionReason.empty()
                         ? "explicit_non_competitive_context"
                         : game.earlyExitExclusionReason;
        return false;
    }

    const MmrCategory rosterCategory =
        m_state->ui.rosterMmrCategory.load();
    mode = InferModeFromMatchState(game, rosterCategory);
    if (!IsTrackedRankedEarlyExitMode(mode)) {
        voidReason = "untracked_or_non_competitive_playlist";
        return false;
    }

    const int expectedTeamSize = ExpectedTeamSizeForMode(mode);
    if (expectedTeamSize <= 0) {
        voidReason = "unknown_competitive_team_size";
        return false;
    }
    const bool lobbyWasFull =
        game.lobbyWasEverFull ||
        (game.maxTeamPlayersSeen[0] >= expectedTeamSize &&
         game.maxTeamPlayersSeen[1] >= expectedTeamSize);
    if (!lobbyWasFull) {
        voidReason = "lobby_never_full";
        return false;
    }

    return true;
}

void TelemetryReducer::RecordTerminalMatchGuidLocked(
    const std::string& matchGuid) {
    if (!matchGuid.empty()) {
        m_finalizedMatchGuids.insert(matchGuid);
    }
}

void TelemetryReducer::MarkDestroyedMatchVoidLocked(
    const std::string& reason,
    SideEffects& effects) {
    RecordTerminalMatchGuidLocked(m_state->game.matchGuid);
    m_state->game.lastMatchWasVoid = true;
    m_state->game.lastMatchVoidReason = reason;
    m_state->game.matchSummaryScore = m_state->game.score;
    m_state->game.matchSummaryMyTeam = m_state->game.myTeam;
    m_state->game.matchSummaryWinnerTeam = -1;
    m_state->game.matchFinalized = true;
    std::cout << "[Event] MATCH VOIDED: " << reason << "\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

void TelemetryReducer::FinalizeMatchLocked(
    int winnerTeam,
    MatchFinalizeSource source,
    SideEffects& effects) {
    const std::string matchGuid = m_state->game.matchGuid;
    if (m_state->game.matchFinalized ||
        (!matchGuid.empty() &&
         m_finalizedMatchGuids.count(matchGuid) > 0)) {
        std::cout
            << "[TelemetryReducer] Skipping duplicate save: match already finalized.\n";
        return;
    }

    FinalizeCapturedMatchLocked(
        CaptureMatchLocked(), winnerTeam, source, true, effects);
}

void TelemetryReducer::FinalizeCapturedMatchLocked(
    CapturedMatch match,
    int winnerTeam,
    MatchFinalizeSource source,
    bool enqueueMmrRefresh,
    SideEffects& effects) {
    if (!match.matchGuid.empty() &&
        m_finalizedMatchGuids.count(match.matchGuid) > 0) {
        std::cout << "[TelemetryReducer] Skipping duplicate match save for guid: "
                  << PrivacyLog::Sensitive(match.matchGuid, "match GUID")
                  << "\n";
        return;
    }

    const MatchEndDecision decision =
        ClassifyMatchEndLocked(match, winnerTeam);
    RecordTerminalMatchGuidLocked(match.matchGuid);
    const bool isCurrentMatch =
        match.matchGeneration ==
            m_state->game.activeMatchGeneration &&
        match.matchGuid == m_state->game.matchGuid;

    if (isCurrentMatch) {
        m_state->game.matchFinalized = true;
        m_state->game.lastMatchWasVoid = !decision.shouldCount;
        m_state->game.lastMatchVoidReason = decision.voidReason;
        m_state->game.matchSummaryScore = match.score;
        m_state->game.matchSummaryMyTeam = match.myTeam;
        m_state->game.matchSummaryWinnerTeam = winnerTeam;
        m_state->ui.showMatchSummary = true;
        m_state->ui.matchSummaryStartMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    }

    if (!decision.shouldCount) {
        std::cout << "[Event] MATCH VOIDED: " << decision.voidReason
                  << "\n";
        if (isCurrentMatch) {
            effects.pushDiscord = true;
            effects.discordSnapshot = BuildDiscordSnapshotLocked();
        }
        return;
    }

    if (!match.matchGuid.empty()) {
        m_lastSavedMatchGuid = match.matchGuid;
    }

    const bool iWon = decision.iWon;
    if (iWon) {
        m_state->game.sessionTotals.wins++;
    } else {
        m_state->game.sessionTotals.losses++;
    }

    const auto& currentMatch = match.stats;
    auto& sessionTotals = m_state->game.sessionTotals;
    sessionTotals.goals += currentMatch.goalsSelf;
    sessionTotals.saves += currentMatch.savesSelf;
    sessionTotals.savesTotal += currentMatch.saves;
    sessionTotals.shots += currentMatch.shotsSelf;
    sessionTotals.shotsTotal += currentMatch.shots;
    sessionTotals.demos += currentMatch.demosSelf;
    sessionTotals.demosTotal += currentMatch.demos;
    sessionTotals.demoed += currentMatch.demoedSelf;
    sessionTotals.crossbars += currentMatch.crossbarsSelf;
    sessionTotals.crossbarsTotal += currentMatch.crossbars;
    sessionTotals.assists += currentMatch.assistsSelf;
    sessionTotals.assistsTotal += currentMatch.assists;
    sessionTotals.boostPickedUp += currentMatch.boostPickedUp;
    sessionTotals.maxGoalSpeed =
        std::max(sessionTotals.maxGoalSpeed, currentMatch.maxGoalSpeed);
    sessionTotals.maxGoalSpeedSelf = std::max(
        sessionTotals.maxGoalSpeedSelf, currentMatch.maxGoalSpeedSelf);
    sessionTotals.maxBallSpeed =
        std::max(sessionTotals.maxBallSpeed, currentMatch.maxBallSpeed);
    sessionTotals.maxBallSpeedSelf = std::max(
        sessionTotals.maxBallSpeedSelf, currentMatch.maxBallSpeedSelf);
    sessionTotals.maxImpactForce = std::max(
        sessionTotals.maxImpactForce, currentMatch.maxImpactForce);
    sessionTotals.maxImpactForceSelf = std::max(
        sessionTotals.maxImpactForceSelf, currentMatch.maxImpactForceSelf);
    if (currentMatch.fastestGoalTime > 0.0f &&
        (sessionTotals.fastestGoalTime == 0.0f ||
         currentMatch.fastestGoalTime <
             sessionTotals.fastestGoalTime)) {
        sessionTotals.fastestGoalTime =
            currentMatch.fastestGoalTime;
    }
    if (currentMatch.fastestGoalTimeSelf > 0.0f &&
        (sessionTotals.fastestGoalTimeSelf == 0.0f ||
         currentMatch.fastestGoalTimeSelf <
             sessionTotals.fastestGoalTimeSelf)) {
        sessionTotals.fastestGoalTimeSelf =
            currentMatch.fastestGoalTimeSelf;
    }
    sessionTotals.ownGoals += currentMatch.ownGoals;
    sessionTotals.ownGoalsSelf += currentMatch.ownGoalsSelf;

    if (match.mode != "1v1") {
        int teamGoalsThisMatch = 0;
        if (match.myTeam == 0 || match.myTeam == 1) {
            teamGoalsThisMatch = match.score[match.myTeam];
        }
        if (teamGoalsThisMatch <= 0) {
            for (const auto& [primaryId, player] : match.roster) {
                if (player.team == match.myTeam) {
                    teamGoalsThisMatch += player.goals;
                }
            }
        }
        const int participationThisMatch = std::clamp(
            currentMatch.goalsSelf + currentMatch.assistsSelf,
            0,
            std::max(0, teamGoalsThisMatch));
        sessionTotals.teamGoals += std::max(0, teamGoalsThisMatch);
        sessionTotals.goalParticipations += participationThisMatch;
    }

    if (GamemodeUtils::IsTrackedCompetitiveMode(match.mode)) {
        auto& gamemode = m_state->game.sessionGamemodes[match.mode];
        if (iWon) {
            gamemode.wins++;
        } else {
            gamemode.losses++;
        }
        gamemode.total++;
    }

    for (auto& [primaryId, player] : match.roster) {
        const bool isTeammate = player.team == match.myTeam;
        if (isTeammate) {
            if (iWon) {
                player.lifetimeWinsWith++;
            } else {
                player.lifetimeLossesWith++;
            }
        } else {
            if (iWon) {
                player.lifetimeWinsAgainst++;
            } else {
                player.lifetimeLossesAgainst++;
            }
        }
        player.hasLifetimeData = true;

        const auto livePlayer = m_state->game.roster.find(primaryId);
        if (livePlayer != m_state->game.roster.end()) {
            livePlayer->second.lifetimeWinsWith =
                player.lifetimeWinsWith;
            livePlayer->second.lifetimeLossesWith =
                player.lifetimeLossesWith;
            livePlayer->second.lifetimeWinsAgainst =
                player.lifetimeWinsAgainst;
            livePlayer->second.lifetimeLossesAgainst =
                player.lifetimeLossesAgainst;
            livePlayer->second.hasLifetimeData = true;
        }
    }

    const char* sourceName = "match-ended";
    if (source == MatchFinalizeSource::MatchDestroyed) {
        sourceName = "destroyed-authoritative-winner";
    } else if (source == MatchFinalizeSource::LocalForfeit) {
        sourceName = "local-forfeit";
    } else if (
        source == MatchFinalizeSource::TrackerConfirmedDestroyed) {
        sourceName = "tracker-confirmed-destruction";
    }
    std::cout << "========================================\n";
    std::cout << "[Event] MATCH FINALIZED! Final Score: "
              << match.score[0] << "-" << match.score[1]
              << ", source=" << sourceName << "\n";
    std::cout << "========================================\n";

    if (enqueueMmrRefresh) {
        PostMatchMmrRefresh refresh;
        const bool hasLocalRefresh =
            BuildPostMatchMmrRefreshLocked(match, iWon, refresh);
        if (hasLocalRefresh) {
            refresh.provisionalImmediately =
                source == MatchFinalizeSource::LocalForfeit;
            effects.postMatchMmrRefresh = std::move(refresh);
        }
        for (const auto& [primaryId, player] : match.roster) {
            if (player.team != match.myTeam) continue;
            if (hasLocalRefresh && primaryId == match.myPrimaryId) {
                continue;
            }
            effects.fetchMmrQueue.emplace_back(primaryId, player.name);
        }
    }

    nlohmann::json matchRecord = {
        {"match_guid", match.matchGuid},
        {"arena", match.arenaName},
        {"result", iWon ? "Win" : "Loss"},
        {"score", {match.score[0], match.score[1]}},
        {"stats",
         {{"goals", currentMatch.goalsSelf},
          {"saves", currentMatch.savesSelf},
          {"demos", currentMatch.demosSelf},
          {"fastest_goal", currentMatch.fastestGoalTimeSelf},
          {"max_ball_speed", currentMatch.maxGoalSpeedSelf}}},
        {"timestamp", match.endedAtUnixMs / 1000}};

    MatchSaveSnapshot snapshot;
    snapshot.arenaName = match.arenaName;
    snapshot.arenaAsset = match.arenaAsset;
    snapshot.matchGuid = match.matchGuid;
    snapshot.myTeam = match.myTeam;
    snapshot.winnerTeam = winnerTeam;
    snapshot.validResult = decision.shouldPersist;
    snapshot.voidReason = decision.voidReason;
    snapshot.maxTeamPlayersSeen = match.maxTeamPlayersSeen;
    snapshot.score[0] = match.score[0];
    snapshot.score[1] = match.score[1];
    snapshot.maxPlayersSeen =
        std::max(match.maxPlayersSeen,
                 static_cast<int>(match.roster.size()));
    snapshot.roster = std::move(match.roster);
    snapshot.rosterMmrCategory = match.rosterMmrCategory;
    snapshot.graphMmrCategory = match.graphMmrCategory;
    snapshot.myPrimaryId = match.myPrimaryId;
    snapshot.endedAtUnixMs = match.endedAtUnixMs;

    effects.saveMatch = true;
    effects.matchRecord = std::move(matchRecord);
    effects.saveSnapshot = std::move(snapshot);

    if (isCurrentMatch) {
        effects.pushDiscord = true;
        effects.discordSnapshot = BuildDiscordSnapshotLocked();
    }
}

void TelemetryReducer::HandleMatchDestroyed(
    const nlohmann::json& data,
    SideEffects& effects) {
    std::string eventMatchGuid;
    for (const char* key : {"MatchGuid", "match_guid"}) {
        if (data.contains(key) && data[key].is_string()) {
            eventMatchGuid = data[key].get<std::string>();
            if (!eventMatchGuid.empty()) break;
        }
    }
    const bool hasExplicitEventGuid = !eventMatchGuid.empty();
    if (hasExplicitEventGuid) {
        AttachTerminalGuidToCurrentLocked(eventMatchGuid);
    }

    if (m_state->game.inMatch) {
        const char* ownershipFailure = nullptr;
        if (hasExplicitEventGuid &&
            eventMatchGuid != m_state->game.matchGuid) {
            ownershipFailure = "explicit-guid-not-current";
        } else if (
            !hasExplicitEventGuid &&
            !m_pendingDestroyedMatches.empty()) {
            ownershipFailure = "older-pending-match";
        } else if (
            !hasExplicitEventGuid &&
            !m_state->game.matchGuid.empty()) {
            ownershipFailure =
                "current-match-requires-explicit-guid";
        } else if (
            !hasExplicitEventGuid &&
            m_missingGuidAssociationBlockedByReconnect) {
            ownershipFailure = "telemetry-reconnect";
        }

        if (ownershipFailure) {
            std::cout
                << "[TelemetryReducer] MatchDestroyed: "
                << "eventGuid="
                << (hasExplicitEventGuid ? "present" : "missing")
                << ", currentGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", currentGeneration="
                << m_state->game.activeMatchGeneration
                << ", pendingMatches="
                << m_pendingDestroyedMatches.size()
                << ", action=ignore, reason="
                << ownershipFailure << ".\n";
            return;
        }
    }

    m_roundActive = false;
    UpdateLifecycleSignalsLocked(data);

    if (!m_state->game.inMatch) {
        if (m_pendingDestroyedMatches.count(
                m_state->game.matchGuid) > 0) {
            std::cout
                << "[TelemetryReducer] Ignoring duplicate MatchDestroyed for pending match.\n";
        }
        return;
    }

    if (data.contains("Teams") && data["Teams"].is_array()) {
        for (const auto& team : data["Teams"]) {
            if (!team.contains("TeamNum") ||
                !team["TeamNum"].is_number_integer() ||
                !team.contains("Score") ||
                !team["Score"].is_number_integer()) {
                continue;
            }
            const int teamNumber = team["TeamNum"].get<int>();
            if (teamNumber == 0 || teamNumber == 1) {
                m_state->game.score[teamNumber] =
                    team["Score"].get<int>();
            }
        }
    }

    const bool replayActiveAtDestruction = m_state->game.inReplay;

    const auto authoritativeWinner =
        ReadInteger(data, {"WinnerTeamNum", "winner_team_num"});
    if (!m_state->game.matchFinalized &&
        authoritativeWinner &&
        (*authoritativeWinner == 0 || *authoritativeWinner == 1)) {
        const MatchFinalizeSource source =
            m_state->game.explicitLocalForfeit &&
                    *authoritativeWinner != m_state->game.myTeam
                ? MatchFinalizeSource::LocalForfeit
                : MatchFinalizeSource::MatchDestroyed;
        FinalizeMatchLocked(*authoritativeWinner, source, effects);
    } else if (!m_state->game.matchFinalized) {
        std::string mode;
        std::string voidReason;
        const bool validEarlyCompetitiveExit =
            IsValidEarlyCompetitiveExitLocked(mode, voidReason);
        if (replayActiveAtDestruction) {
            const int expectedTeamSize =
                ExpectedTeamSizeForMode(mode);
            const bool lobbyWasFull =
                m_state->game.lobbyWasEverFull ||
                (expectedTeamSize > 0 &&
                 m_state->game.maxTeamPlayersSeen[0] >=
                     expectedTeamSize &&
                 m_state->game.maxTeamPlayersSeen[1] >=
                     expectedTeamSize);
            const char* replayContext =
                m_nonLiveReplayActive
                    ? "saved-replay"
                    : (m_state->game.localPlayerWasSpectator
                           ? "spectator"
                           : "goal-replay");
            std::cout
                << "[TelemetryReducer] MatchDestroyed during replay: "
                << "matchGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", replayContext=" << replayContext
                << ", roundStarted="
                << (m_state->game.roundEverStarted ? "true"
                                                   : "false")
                << ", lobbyWasFull="
                << (lobbyWasFull ? "true" : "false")
                << ", playlist="
                << (mode.empty() ? "unknown" : mode)
                << ", action="
                << (!validEarlyCompetitiveExit
                        ? "void"
                        : (m_state->game.explicitLocalForfeit
                               ? "finalize-local-loss"
                               : "pending-tracker-confirmation"));
            if (!validEarlyCompetitiveExit) {
                std::cout << ", reason=" << voidReason;
            }
            std::cout << ".\n";
        }
        if (!validEarlyCompetitiveExit) {
            MarkDestroyedMatchVoidLocked(voidReason, effects);
        } else if (m_state->game.explicitLocalForfeit) {
            const int winnerTeam = 1 - m_state->game.myTeam;
            std::cout
                << "[TelemetryReducer] Competitive match destroyed before MatchEnded: "
                << "matchGuid="
                << PrivacyLog::Sensitive(
                       m_state->game.matchGuid, "match GUID")
                << ", playlist=" << mode
                << ", localTeam=" << m_state->game.myTeam
                << ", score=" << m_state->game.score[0] << "-"
                << m_state->game.score[1]
                << ", explicitLocalForfeit=true"
                << ", action=finalize-local-loss.\n";
            FinalizeMatchLocked(
                winnerTeam, MatchFinalizeSource::LocalForfeit, effects);
        } else {
            CapturedMatch match = CaptureMatchLocked();
            match.mode = mode;
            PostMatchMmrRefresh refresh;
            if (!BuildPostMatchMmrRefreshLocked(
                    match, false, refresh)) {
                MarkDestroyedMatchVoidLocked(
                    "missing_local_tracker_identity", effects);
            } else {
                PendingDestroyedMatchMmrRefresh pending;
                pending.primaryId = refresh.primaryId;
                pending.name = refresh.name;
                pending.matchGuid = refresh.matchGuid;
                pending.playlist = refresh.playlist;
                pending.localTeam = match.myTeam;
                pending.score = match.score;
                pending.previousMmr = refresh.previousMmr;
                pending.previousMatches = refresh.previousMatches;
                pending.previousMmrIsPlaylistSpecific =
                    refresh.previousMmrIsPlaylistSpecific;
                pending.localPlayerDisappeared =
                    match.localPlayerDisappeared;
                pending.explicitLocalForfeit =
                    match.explicitLocalForfeit;
                pending.destroyedAtUnixMs = match.endedAtUnixMs;
                pending.validCompetitiveMatch = true;

                m_pendingDestroyedMatches.emplace(
                    match.matchGuid, std::move(match));
                m_state->game.lastMatchWasVoid = true;
                m_state->game.lastMatchVoidReason =
                    "destroyed_pending_tracker_confirmation";
                m_state->game.matchSummaryScore =
                    m_state->game.score;
                m_state->game.matchSummaryMyTeam =
                    m_state->game.myTeam;
                m_state->game.matchSummaryWinnerTeam = -1;
                effects.pendingDestroyedMatch = std::move(pending);

                std::cout
                    << "[TelemetryReducer] Competitive match destroyed before MatchEnded: "
                    << "matchGuid="
                    << PrivacyLog::Sensitive(
                           m_state->game.matchGuid, "match GUID")
                    << ", playlist=" << mode
                    << ", localTeam=" << m_state->game.myTeam
                    << ", score=" << m_state->game.score[0] << "-"
                    << m_state->game.score[1]
                    << ", explicitLocalForfeit="
                    << (m_state->game.explicitLocalForfeit
                            ? "true"
                            : "false")
                    << ", localPlayerDisappeared="
                    << (m_state->game.localPlayerPresenceObserved &&
                                !m_state->game
                                     .localPlayerPresentInLatestUpdate
                            ? "true"
                            : "false")
                    << ", action=pending-tracker-confirmation.\n";
            }
        }
    }

    m_state->game.inMatch = false;
    m_state->game.arenaName.clear();
    m_state->game.arenaAsset.clear();
    m_state->game.myTeam = -1;
    m_state->game.inReplay = false;
    m_nonLiveReplayActive = false;
    std::cout << "[Event] Match Destroyed (Back to Menu)\n";
    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

SideEffects TelemetryReducer::ConfirmPendingDestroyedMatch(
    const std::string& matchGuid,
    bool won) {
    SideEffects effects;
    std::unique_lock<std::shared_mutex> lock(m_state->game.mutex);
    const auto pendingIt =
        m_pendingDestroyedMatches.find(matchGuid);
    if (pendingIt == m_pendingDestroyedMatches.end() ||
        m_finalizedMatchGuids.count(matchGuid) > 0) {
        std::cout
            << "[TelemetryReducer] Skipping duplicate destroyed-match confirmation.\n";
        return effects;
    }

    CapturedMatch match = std::move(pendingIt->second);
    m_pendingDestroyedMatches.erase(pendingIt);
    const int winnerTeam = won ? match.myTeam : 1 - match.myTeam;
    FinalizeCapturedMatchLocked(
        std::move(match),
        winnerTeam,
        MatchFinalizeSource::TrackerConfirmedDestroyed,
        false,
        effects);
    m_state->game.version++;
    return effects;
}

void TelemetryReducer::HandleMatchEnded(
    const nlohmann::json& data,
    SideEffects& effects) {
    const auto winnerValue =
        ReadInteger(data, {"WinnerTeamNum", "winner_team_num"});
    const int winner = winnerValue ? *winnerValue : -1;

    std::string eventMatchGuid;
    for (const char* key : {"MatchGuid", "match_guid"}) {
        if (data.contains(key) && data[key].is_string()) {
            eventMatchGuid = data[key].get<std::string>();
            if (!eventMatchGuid.empty()) break;
        }
    }
    const bool hasExplicitEventGuid = !eventMatchGuid.empty();
    const auto pendingIt = hasExplicitEventGuid
                               ? m_pendingDestroyedMatches.find(
                                     eventMatchGuid)
                               : m_pendingDestroyedMatches.end();
    if (hasExplicitEventGuid &&
        pendingIt == m_pendingDestroyedMatches.end()) {
        AttachTerminalGuidToCurrentLocked(eventMatchGuid);
    }
    const bool targetsCurrentByGuid =
        hasExplicitEventGuid &&
        eventMatchGuid == m_state->game.matchGuid;

    const auto logDecision =
        [&](const char* action, const char* reason) {
            std::cout
                << "[TelemetryReducer] MatchEnded: "
                << "eventGuid="
                << (hasExplicitEventGuid ? "present" : "missing")
                << ", currentGuid="
                << (m_state->game.matchGuid.empty()
                        ? "missing"
                        : "present")
                << ", currentGeneration="
                << m_state->game.activeMatchGeneration
                << ", pendingMatches="
                << m_pendingDestroyedMatches.size()
                << ", winnerTeam=" << winner
                << ", forfeit="
                << (ReadTrueBoolean(
                        data, {"bForfeit", "Forfeit"})
                        ? "true"
                        : "false")
                << ", action=" << action;
            if (reason && *reason) {
                std::cout << ", reason=" << reason;
            }
            std::cout << ".\n";
        };

    if (pendingIt != m_pendingDestroyedMatches.end()) {
        if (winner != 0 && winner != 1) {
            logDecision(
                "ignore",
                "invalid-winner-pending-match");
            return;
        }

        logDecision("finalize-pending", "");
        CapturedMatch match = std::move(pendingIt->second);
        m_pendingDestroyedMatches.erase(pendingIt);
        if (data.contains("Teams") && data["Teams"].is_array()) {
            for (const auto& team : data["Teams"]) {
                if (team.contains("TeamNum") &&
                    team["TeamNum"].is_number_integer() &&
                    team.contains("Score") &&
                    team["Score"].is_number_integer()) {
                    const int teamNumber =
                        team["TeamNum"].get<int>();
                    if (teamNumber == 0 || teamNumber == 1) {
                        match.score[teamNumber] =
                            team["Score"].get<int>();
                    }
                }
            }
        }
        const bool localForfeit =
            HasExplicitLocalForfeitSignal(data, match.myTeam) &&
            winner != match.myTeam;
        const bool won = winner == match.myTeam;
        const std::string resolvedGuid = match.matchGuid;
        FinalizeCapturedMatchLocked(
            std::move(match),
            winner,
            localForfeit ? MatchFinalizeSource::LocalForfeit
                         : MatchFinalizeSource::MatchEnded,
            false,
            effects);
        effects.resolvedDestroyedMatch =
            ResolvedDestroyedMatch{
                .matchGuid = resolvedGuid,
                .won = won};
        return;
    }

    bool targetsCurrent = targetsCurrentByGuid;
    if (!hasExplicitEventGuid) {
        const char* ambiguityReason = nullptr;
        if (!m_state->game.inMatch) {
            ambiguityReason = "no-active-match";
        } else if (m_state->game.matchFinalized) {
            ambiguityReason = "current-match-finalized";
        } else if (!m_pendingDestroyedMatches.empty()) {
            ambiguityReason = "older-pending-match";
        } else if (!m_state->game.matchGuid.empty()) {
            ambiguityReason =
                "current-match-requires-explicit-guid";
        } else if (
            m_missingGuidAssociationBlockedByReconnect) {
            ambiguityReason = "telemetry-reconnect";
        }

        if (ambiguityReason) {
            logDecision("ignore", ambiguityReason);
            return;
        }
        targetsCurrent = true;
    }

    if (!targetsCurrent) {
        logDecision("ignore", "explicit-guid-not-current");
        return;
    }

    if (!m_state->game.inMatch) {
        logDecision("ignore", "no-active-match");
        return;
    }
    if (m_state->game.matchFinalized) {
        logDecision("ignore", "current-match-finalized");
        return;
    }

    m_roundActive = false;
    if (data.contains("Teams") && data["Teams"].is_array()) {
        for (const auto& team : data["Teams"]) {
            if (team.contains("TeamNum") &&
                team["TeamNum"].is_number_integer() &&
                team.contains("Score") &&
                team["Score"].is_number_integer()) {
                const int teamNumber =
                    team["TeamNum"].get<int>();
                if (teamNumber == 0 || teamNumber == 1) {
                    m_state->game.score[teamNumber] =
                        team["Score"].get<int>();
                }
            }
        }
    }

    const bool localForfeit =
        HasExplicitLocalForfeitSignal(
            data, m_state->game.myTeam) &&
        winner != m_state->game.myTeam;
    logDecision("finalize-current", "");
    FinalizeMatchLocked(
        winner,
        localForfeit ? MatchFinalizeSource::LocalForfeit
                     : MatchFinalizeSource::MatchEnded,
        effects);
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
