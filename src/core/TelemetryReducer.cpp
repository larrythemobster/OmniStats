#define NOMINMAX
#include "TelemetryReducer.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PrivacyLog.hpp"
#include <iostream>
#include <algorithm>
#include <utility>

static std::string FormatArenaName(const std::string& asset) {
    if (asset.empty()) return "";
    std::string base = asset;
    std::transform(base.begin(), base.end(), base.begin(), ::tolower);
    if (base.length() > 2 && base.substr(base.length() - 2) == "_p") {
        base = base.substr(0, base.length() - 2);
    }
    static const std::unordered_map<std::string, std::string> ARENA_BASE = {
        {"stadium", "DFH Stadium"}, {"park", "Beckwith Park"}, {"mannfield", "Mannfield"}, {"trainstation", "Urban Central"},
        {"haunted_trainstation", "Urban Central (Haunted)"}, {"underwater", "AquaDome"},
        {"wasteland", "Wasteland"}, {"neotokyo", "Neo Tokyo"}, {"neotokyo_standard", "Neo Tokyo"},
        {"eurostadium", "Champions Field"}, {"beach", "Salty Shores"}, {"beachvolley", "Salty Shores"},
        {"chinastadium", "Forbidden Temple"}, {"temple", "Forbidden Temple"}, {"cosmic", "Starbase ARC"}, {"arc_standard", "Starbase ARC"},
        {"throwback_stadium", "Throwback Stadium"}, {"hoops_dunkhouse", "DunkHouse"},
        {"music", "Estadio Vida"}, {"estadio_vida", "Estadio Vida"}, {"farm", "Farmstead"},
        {"outlaw_oasis", "Deadeye Canyon"}, {"canyon", "Deadeye Canyon"}, {"shattershot", "Core 707"},
        {"labs_octagon", "Octagon"}, {"labs_pillars", "Pillars"}, {"labs_cosmic", "Cosmic"},
        {"labs_double_goal", "Double Goal"}, {"labs_underpass", "Underpass"},
        {"labs_utopia", "Utopia Retro"}, {"neoasphalt", "Neon Fields"}, {"neon", "Neon Fields"},
        {"utopia", "Utopia Coliseum"}, {"sovereign", "Sovereign Heights"}
    };
    static const std::unordered_map<std::string, std::string> ARENA_VARIANT = {
        {"night", "Night"}, {"day", "Day"}, {"rainy", "Stormy"}, {"stormy", "Stormy"},
        {"race_day", "Stormy"}, {"snowy", "Snowy"}, {"snowfall", "Snowy"}, {"dawn", "Dawn"},
        {"spring", "Spring"}, {"spooky", "Spooky"}, {"circuit", "Circuit"}
    };
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
            if (!variant.empty()) { variant[0] = toupper(variant[0]); return candIt->second + " (" + variant + ")"; }
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
    return MmrCategory::Best;
}

static std::string InferModeFromMatchState(const GameState& game, MmrCategory rosterCat, MmrCategory graphCat) {
    std::string arenaKey = !game.arenaAsset.empty() ? game.arenaAsset : game.arenaName;
    return GamemodeUtils::InferFromSnapshot(
        game.maxPlayersSeen,
        static_cast<int>(game.roster.size()),
        rosterCat,
        graphCat,
        arenaKey
    );
}

static MmrCategory CategoryFromMatchContext(const GameState& game) {
    std::string arenaKey = !game.arenaAsset.empty() ? game.arenaAsset : game.arenaName;
    MmrCategory arenaCategory = CategoryFromMode(GamemodeUtils::InferFromArenaName(arenaKey));
    if (arenaCategory != MmrCategory::Best) return arenaCategory;
    return CategoryFromTeamCounts(game.maxTeamPlayersSeen, game.roundEverStarted);
}

TelemetryReducer::TelemetryReducer(std::shared_ptr<SessionState> state)
    : m_state(state) {
    m_cachedConf = Config::Read();
    m_lastConfigReadTime = std::chrono::steady_clock::now();
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
        m_state->game.inReplay = true;
        return effects;
    }

    if (eventName == Constants::EVT_MATCH_CREATED) {
        m_state->game.inReplay = false;
        m_roundActive = false;
        std::string newGuid = (data.contains("MatchGuid") && data["MatchGuid"].is_string()) ? data["MatchGuid"].get<std::string>() : "";
        if (newGuid != m_state->game.matchGuid) {
            std::string currentArena = m_state->game.arenaName;
            m_state->resetMatch(currentArena);
            m_autoSwitchedPlaylistCategory = MmrCategory::Best;
            m_lastPlayerBoost.clear();
            m_lastPlayerSeen.clear();
        }
        m_state->game.matchGuid = newGuid;
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
        m_roundActive = false;
        if (m_state->game.inMatch) {
            if (m_state->game.inMatch.load() &&
                !m_state->game.inReplay.load() &&
                !m_state->game.matchFinalized &&
                m_state->game.roundEverStarted &&
                m_state->game.localPlayerWasActive &&
                (m_state->game.myTeam == 0 || m_state->game.myTeam == 1)) {
                
                int winnerTeam = -1;
                if (m_state->game.score[0] != m_state->game.score[1]) {
                    winnerTeam = (m_state->game.score[0] > m_state->game.score[1]) ? 0 : 1;
                }
                FinalizeMatchLocked(winnerTeam, MatchFinalizeSource::MatchDestroyed, effects);
            }

            m_state->game.inMatch = false;
            m_state->game.arenaName = "";
            m_state->game.arenaAsset = "";
            m_state->game.myTeam = -1;
            m_state->game.inReplay = false;
            std::cout << "[Event] Match Destroyed (Back to Menu)\n";
            effects.pushDiscord = true;
            effects.discordSnapshot = BuildDiscordSnapshotLocked();
        }
    }

    return effects;
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
                m_state->resetMatch(currentArena, currentArenaAsset);
                m_roundActive = false;
                m_autoSwitchedPlaylistCategory = MmrCategory::Best;
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
                            .enqueued = true
                        };
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

            MmrCategory inferredPlaylistCat = CategoryFromMatchContext(m_state->game);
            MmrCategory lastAutoCat = m_autoSwitchedPlaylistCategory;
            bool inferredCategoryIsHiddenExtra = IsExtraMmrCategory(inferredPlaylistCat) && !m_cachedConf.show_extra_playlists;
            bool userChangedAfterAutoSwitch = lastAutoCat != MmrCategory::Best &&
                (m_state->ui.rosterMmrCategory.load() != lastAutoCat || m_state->ui.graphMmrCategory.load() != lastAutoCat);

            if (m_cachedConf.auto_switch_mmr_category && !inferredCategoryIsHiddenExtra && !userChangedAfterAutoSwitch &&
                inferredPlaylistCat != MmrCategory::Best && inferredPlaylistCat != lastAutoCat) {
                m_autoSwitchedPlaylistCategory = inferredPlaylistCat;
                std::cout << "[Identity] Auto-switching active playlist category to: " << MmrCategoryToString(inferredPlaylistCat) << "\n";
                m_state->ui.rosterMmrCategory.store(inferredPlaylistCat);
                m_state->ui.graphMmrCategory.store(inferredPlaylistCat);
                if (m_state->history.showLifetimeGraph.load() && !m_state->game.myPrimaryId.empty()) {
                    effects.fetchLifetimeHistory = true;
                    effects.lifetimePrimaryId = m_state->game.myPrimaryId;
                    effects.lifetimeCategory = MmrCategoryToString(inferredPlaylistCat);
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
            const MmrCategory rosterCat = m_state->ui.rosterMmrCategory.load();
            const MmrCategory graphCat = m_state->ui.graphMmrCategory.load();
            const std::string mode = InferModeFromMatchState(m_state->game, rosterCat, graphCat);
            const int expectedTeamSize = ExpectedTeamSizeForMode(mode);
            if (expectedTeamSize > 0 &&
                m_state->game.currentTeamPlayersSeen[0] >= expectedTeamSize &&
                m_state->game.currentTeamPlayersSeen[1] >= expectedTeamSize) {
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

TelemetryReducer::MatchEndDecision TelemetryReducer::ClassifyMatchEndLocked(int winnerTeam) const {
    MatchEndDecision decision;

    if (winnerTeam != 0 && winnerTeam != 1) {
        decision.voidReason = "invalid_winner";
        return decision;
    }

    if (m_state->game.myTeam != 0 && m_state->game.myTeam != 1) {
        decision.voidReason = m_state->game.localPlayerWasSpectator
            ? "local_player_spectator_bug"
            : "local_player_team_unknown";
        return decision;
    }

    if (!m_state->game.roundEverStarted) {
        decision.voidReason = "round_never_started";
        return decision;
    }

    if (!m_state->game.localPlayerWasActive) {
        decision.voidReason = m_state->game.localPlayerWasSpectator
            ? "local_player_spectator_bug"
            : "local_player_never_active";
        return decision;
    }

    const MmrCategory rosterCat = m_state->ui.rosterMmrCategory.load();
    const MmrCategory graphCat = m_state->ui.graphMmrCategory.load();
    const std::string mode = InferModeFromMatchState(m_state->game, rosterCat, graphCat);

    const int expectedTeamSize = ExpectedTeamSizeForMode(mode);
    if (expectedTeamSize > 0) {
        bool teamsWereEverFull = m_state->game.lobbyWasEverFull;
        if (!teamsWereEverFull) {
            teamsWereEverFull =
                m_state->game.maxTeamPlayersSeen[0] >= expectedTeamSize &&
                m_state->game.maxTeamPlayersSeen[1] >= expectedTeamSize;
        }

        if (!teamsWereEverFull) {
            decision.voidReason = "lobby_never_full";
            return decision;
        }
    }

    decision.shouldCount = true;
    decision.shouldPersist = true;
    decision.iWon = (winnerTeam == m_state->game.myTeam);
    decision.resultText = decision.iWon ? "Win" : "Loss";
    return decision;
}

void TelemetryReducer::FinalizeMatchLocked(int winnerTeam, MatchFinalizeSource source, SideEffects& effects) {
    if (m_state->game.matchFinalized) {
        std::cout << "[TelemetryReducer] Skipping duplicate save: match already finalized.\n";
        return;
    }

    if (!m_state->game.matchGuid.empty() && m_state->game.matchGuid == m_lastSavedMatchGuid) {
        m_state->game.matchFinalized = true;
        std::cout << "[TelemetryReducer] Skipping duplicate match save for guid: "
                  << PrivacyLog::Sensitive(m_state->game.matchGuid, "match GUID") << "\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = BuildDiscordSnapshotLocked();
        return;
    }

    if (source == MatchFinalizeSource::MatchDestroyed && winnerTeam == -1) {
        m_state->game.lastMatchWasVoid = true;
        m_state->game.lastMatchVoidReason = "destroyed_tied_score_no_winner";
        m_state->game.matchSummaryScore = m_state->game.score;
        m_state->game.matchSummaryMyTeam = m_state->game.myTeam;
        m_state->game.matchSummaryWinnerTeam = -1;

        m_state->game.matchFinalized = true;

        std::cout << "[Event] MATCH VOIDED: destroyed_tied_score_no_winner\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = BuildDiscordSnapshotLocked();
        return;
    }

    m_state->game.matchFinalized = true;

    MatchEndDecision decision = ClassifyMatchEndLocked(winnerTeam);

    m_state->game.lastMatchWasVoid = !decision.shouldCount;
    m_state->game.lastMatchVoidReason = decision.voidReason;
    m_state->game.matchSummaryScore = m_state->game.score;
    m_state->game.matchSummaryMyTeam = m_state->game.myTeam;
    m_state->game.matchSummaryWinnerTeam = winnerTeam;

    m_state->ui.showMatchSummary = true;
    m_state->ui.matchSummaryStartMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    auto fullRoster = m_state->game.matchRoster;
    for (const auto& [pid, player] : m_state->game.roster) {
        fullRoster[pid] = player; // live roster wins if newer
    }

    if (decision.shouldCount) {
        const bool iWon = decision.iWon;

        if (iWon) m_state->game.sessionTotals.wins++;
        else m_state->game.sessionTotals.losses++;

        auto& cm = m_state->game.currentMatch;
        auto& st = m_state->game.sessionTotals;
        st.goals += cm.goalsSelf; st.saves += cm.savesSelf; st.savesTotal += cm.saves;
        st.shots += cm.shotsSelf; st.shotsTotal += cm.shots; st.demos += cm.demosSelf;
        st.demosTotal += cm.demos; st.demoed += cm.demoedSelf; st.crossbars += cm.crossbarsSelf; st.crossbarsTotal += cm.crossbars;
        st.assists += cm.assistsSelf; st.assistsTotal += cm.assists;
        st.boostPickedUp += cm.boostPickedUp;
        st.maxGoalSpeed = std::max(st.maxGoalSpeed, cm.maxGoalSpeed);
        st.maxGoalSpeedSelf = std::max(st.maxGoalSpeedSelf, cm.maxGoalSpeedSelf);
        st.maxBallSpeed = std::max(st.maxBallSpeed, cm.maxBallSpeed);
        st.maxBallSpeedSelf = std::max(st.maxBallSpeedSelf, cm.maxBallSpeedSelf);
        st.maxImpactForce = std::max(st.maxImpactForce, cm.maxImpactForce);
        st.maxImpactForceSelf = std::max(st.maxImpactForceSelf, cm.maxImpactForceSelf);
        if (cm.fastestGoalTime > 0.0f && (st.fastestGoalTime == 0.0f || cm.fastestGoalTime < st.fastestGoalTime))
            st.fastestGoalTime = cm.fastestGoalTime;
        if (cm.fastestGoalTimeSelf > 0.0f && (st.fastestGoalTimeSelf == 0.0f || cm.fastestGoalTimeSelf < st.fastestGoalTimeSelf))
            st.fastestGoalTimeSelf = cm.fastestGoalTimeSelf;
        st.ownGoals += cm.ownGoals; st.ownGoalsSelf += cm.ownGoalsSelf;

        const MmrCategory rosterCat = m_state->ui.rosterMmrCategory.load();
        const MmrCategory graphCat = m_state->ui.graphMmrCategory.load();
        const std::string mode = InferModeFromMatchState(m_state->game, rosterCat, graphCat);
        const bool isOnesMatch = mode == "1v1";
        if (!isOnesMatch) {
            int teamGoalsThisMatch = 0;
            if (m_state->game.myTeam == 0 || m_state->game.myTeam == 1) {
                teamGoalsThisMatch = m_state->game.score[m_state->game.myTeam];
            }
            if (teamGoalsThisMatch <= 0) {
                int rosterTeamGoals = 0;
                for (const auto& [pid, player] : fullRoster) {
                    if (player.team == m_state->game.myTeam) {
                        rosterTeamGoals += player.goals;
                    }
                }
                teamGoalsThisMatch = rosterTeamGoals;
            }
            const int rawParticipationThisMatch = cm.goalsSelf + cm.assistsSelf;
            const int participationThisMatch = std::clamp(
                rawParticipationThisMatch,
                0,
                std::max(0, teamGoalsThisMatch)
            );
            st.teamGoals += std::max(0, teamGoalsThisMatch);
            st.goalParticipations += participationThisMatch;
        }

        if (GamemodeUtils::IsTrackedCompetitiveMode(mode)) {
            auto& gm = m_state->game.sessionGamemodes[mode];
            if (iWon) {
                gm.wins++;
            } else {
                gm.losses++;
            }
            gm.total++;
        }

        for (auto& [pid, player] : fullRoster) {
            bool isTeammate = (player.team == m_state->game.myTeam);
            if (isTeammate) { if (iWon) player.lifetimeWinsWith++; else player.lifetimeLossesWith++; }
            else { if (iWon) player.lifetimeWinsAgainst++; else player.lifetimeLossesAgainst++; }
            player.hasLifetimeData = true;

            if (m_state->game.roster.count(pid)) {
                m_state->game.roster[pid].lifetimeWinsWith = player.lifetimeWinsWith;
                m_state->game.roster[pid].lifetimeLossesWith = player.lifetimeLossesWith;
                m_state->game.roster[pid].lifetimeWinsAgainst = player.lifetimeWinsAgainst;
                m_state->game.roster[pid].lifetimeLossesAgainst = player.lifetimeLossesAgainst;
                m_state->game.roster[pid].hasLifetimeData = true;
            }
        }

        std::cout << "========================================\n";
        std::cout << "[Event] MATCH FINALIZED! Final Score: " << m_state->game.score[0] << "-" << m_state->game.score[1] << "\n";
        std::cout << "========================================\n";

        for (const auto& [pid, player] : fullRoster) {
            if (player.team == m_state->game.myTeam)
                effects.fetchMmrQueue.emplace_back(pid, player.name);
        }
    } else {
        std::cout << "[Event] MATCH VOIDED: " << decision.voidReason << "\n";
        effects.pushDiscord = true;
        effects.discordSnapshot = BuildDiscordSnapshotLocked();
        return;
    }

    if (decision.shouldPersist) {
        int myGoals = m_state->game.currentMatch.goalsSelf;
        int mySaves = m_state->game.currentMatch.savesSelf;
        int myDemos = m_state->game.currentMatch.demosSelf;
        float myFastest = m_state->game.currentMatch.fastestGoalTimeSelf;
        float myMaxSpeed = m_state->game.currentMatch.maxGoalSpeedSelf;

        nlohmann::json matchRecord = {
            {"match_guid", m_state->game.matchGuid},
            {"arena", m_state->game.arenaName},
            {"result", (winnerTeam == m_state->game.myTeam) ? "Win" : "Loss"},
            {"score", {m_state->game.score[0], m_state->game.score[1]}},
            {"stats", {{"goals", myGoals}, {"saves", mySaves}, {"demos", myDemos},
                       {"fastest_goal", myFastest}, {"max_ball_speed", myMaxSpeed}}},
            {"timestamp", std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())}
        };

        const MmrCategory rosterCat = m_state->ui.rosterMmrCategory.load();
        const MmrCategory graphCat = m_state->ui.graphMmrCategory.load();

        MatchSaveSnapshot snapshot;
        snapshot.arenaName = m_state->game.arenaName;
        snapshot.arenaAsset = m_state->game.arenaAsset;
        snapshot.matchGuid = m_state->game.matchGuid;
        snapshot.myTeam = m_state->game.myTeam;
        snapshot.winnerTeam = winnerTeam;
        snapshot.validResult = decision.shouldPersist;
        snapshot.voidReason = decision.voidReason;
        snapshot.maxTeamPlayersSeen = m_state->game.maxTeamPlayersSeen;
        snapshot.score[0] = m_state->game.score[0];
        snapshot.score[1] = m_state->game.score[1];
        snapshot.maxPlayersSeen = std::max(m_state->game.maxPlayersSeen, static_cast<int>(fullRoster.size()));
        snapshot.roster = std::move(fullRoster);
        snapshot.rosterMmrCategory = rosterCat;
        snapshot.graphMmrCategory = graphCat;
        snapshot.myPrimaryId = m_state->game.myPrimaryId;

        m_lastSavedMatchGuid = m_state->game.matchGuid;

        effects.saveMatch = true;
        effects.matchRecord = std::move(matchRecord);
        effects.saveSnapshot = std::move(snapshot);
    }

    effects.pushDiscord = true;
    effects.discordSnapshot = BuildDiscordSnapshotLocked();
}

void TelemetryReducer::HandleMatchEnded(const nlohmann::json& data, SideEffects& effects) {
    m_roundActive = false;

    int winner = (data.contains("WinnerTeamNum") && data["WinnerTeamNum"].is_number_integer())
        ? data["WinnerTeamNum"].get<int>()
        : -1;

    // Parse final scores if present in MatchEnded event
    if (data.contains("Teams") && data["Teams"].is_array()) {
        for (const auto& t : data["Teams"]) {
            if (t.contains("TeamNum") && t["TeamNum"].is_number_integer() &&
                t.contains("Score") && t["Score"].is_number_integer()) {
                const int teamNum = t["TeamNum"].get<int>();
                if (teamNum == 0 || teamNum == 1) {
                    m_state->game.score[teamNum] = t["Score"].get<int>();
                }
            }
        }
    }

    FinalizeMatchLocked(winner, MatchFinalizeSource::MatchEnded, effects);
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
