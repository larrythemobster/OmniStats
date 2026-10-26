#include "network/telemetry/TelemetryEvents.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <nlohmann/json.hpp>
#include "core/config/Constants.hpp"

namespace TelemetryEvents {

    namespace {

        const nlohmann::json* Child(const nlohmann::json& j, const char* key) {
            if (!j.is_object()) return nullptr;
            const auto it = j.find(key);
            return it == j.end() ? nullptr : &*it;
        }

        std::optional<float> Number(const nlohmann::json& j, const char* key) {
            const auto* v = Child(j, key);
            if (!v || !v->is_number()) return std::nullopt;
            const float f = v->get<float>();
            return std::isfinite(f) ? std::optional<float>(f) : std::nullopt;
        }

        std::optional<int> Integer(const nlohmann::json& j, const char* key) {
            const auto* v = Child(j, key);
            if (!v || !v->is_number()) return std::nullopt;
            return static_cast<int>(std::lround(v->get<double>()));
        }

        std::optional<bool> Bool(const nlohmann::json& j, const char* key) {
            const auto* v = Child(j, key);
            if (!v || !v->is_boolean()) return std::nullopt;
            return v->get<bool>();
        }

        std::string String(const nlohmann::json& j, const char* key) {
            const auto* v = Child(j, key);
            return v && v->is_string() ? v->get<std::string>() : std::string{};
        }

        std::optional<FieldPoint> Point(const nlohmann::json& j, const char* key) {
            const auto* v = Child(j, key);
            if (!v || !v->is_object()) return std::nullopt;
            const auto x = Number(*v, "X");
            const auto y = Number(*v, "Y");
            const auto z = Number(*v, "Z");
            if (!x || !y || !z) return std::nullopt;
            return FieldPoint{*x, *y, *z};
        }

        std::string NormalizeId(const std::string& id, const std::string& name) {
            if (id == "Unknown" || id.rfind("Unknown|", 0) == 0) return "Unknown|" + name;
            return id;
        }

        int TeamOf(const nlohmann::json& j) {
            const auto team = Integer(j, "TeamNum");
            return team && (*team == 0 || *team == 1) ? *team : -1;
        }

        std::string ExtractGuid(const nlohmann::json& data) {
            if (!data.is_object()) return {};
            for (const char* key : {"MatchGuid", "match_guid"}) {
                if (data.contains(key) && data[key].is_string()) {
                    std::string guid = data[key].get<std::string>();
                    if (!guid.empty()) return guid;
                }
            }
            if (data.contains("Game") && data["Game"].is_object()) {
                const auto& game = data["Game"];
                for (const char* key : {"MatchGuid", "match_guid"}) {
                    if (game.contains(key) && game[key].is_string()) {
                        std::string guid = game[key].get<std::string>();
                        if (!guid.empty()) return guid;
                    }
                }
            }
            return {};
        }

        std::string EventId(const nlohmann::json& data) {
            for (const char* key : {"EventId", "EventID", "EventGuid"}) {
                const auto* value = Child(data, key);
                if (!value) continue;
                if (value->is_string()) return value->get<std::string>();
                if (value->is_number_integer()) return value->dump();
            }
            return {};
        }

        bool ReadTeamScores(const nlohmann::json& data, std::array<int, 2>& scores) {
            const auto* teams = Child(data, "Teams");
            if (!teams || !teams->is_array()) return false;

            bool found = false;
            for (const auto& team : *teams) {
                const int teamNumber = TeamOf(team);
                const auto score = Integer(team, "Score");
                if ((teamNumber == 0 || teamNumber == 1) && score.has_value()) {
                    scores[teamNumber] = *score;
                    found = true;
                }
            }
            return found;
        }

        bool ReadTrueBoolean(const nlohmann::json& object, std::initializer_list<const char*> keys) {
            if (!object.is_object()) return false;
            for (const char* key : keys) {
                const auto it = object.find(key);
                if (it == object.end()) continue;
                if (it->is_boolean() && it->get<bool>()) return true;
                if (it->is_number_integer() && it->get<int64_t>() != 0) return true;
                if (it->is_string()) {
                    std::string v = it->get<std::string>();
                    for (char& c : v)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (v == "1" || v == "true" || v == "yes") return true;
                }
            }
            return false;
        }

        std::optional<int> ReadInteger(const nlohmann::json& object, std::initializer_list<const char*> keys) {
            if (!object.is_object()) return std::nullopt;
            for (const char* key : keys) {
                const auto it = object.find(key);
                if (it == object.end()) continue;
                if (it->is_number_integer()) return static_cast<int>(it->get<int64_t>());
                if (it->is_string()) {
                    try {
                        return std::stoi(it->get<std::string>());
                    } catch (...) {
                    }
                }
            }
            return std::nullopt;
        }

        bool MatchTypeContains(const nlohmann::json& game, std::initializer_list<const char*> keywords) {
            if (!game.is_object()) return false;
            for (const char* key : {"MatchType", "match_type", "Type", "type", "Mode", "mode"}) {
                const auto it = game.find(key);
                if (it == game.end() || !it->is_string()) continue;
                std::string val = it->get<std::string>();
                for (char& c : val)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                for (const char* kw : keywords) {
                    if (val.find(kw) != std::string::npos) return true;
                }
            }
            return false;
        }

        PlayerRef ParsePlayerRef(const nlohmann::json& ref) {
            PlayerRef p;
            if (!ref.is_object()) return p;
            p.name = String(ref, "Name");
            std::string rawId = String(ref, "PrimaryId");
            p.primaryId = NormalizeId(rawId, p.name);
            p.team = TeamOf(ref);
            p.shortcut = Integer(ref, "Shortcut");
            return p;
        }

        std::string ExtractCameraTarget(const nlohmann::json& data) {
            if (!data.contains("Game") || !data["Game"].is_object()) return {};
            const auto& game = data["Game"];
            if (!game.contains("bHasTarget") || !game["bHasTarget"].is_boolean() || !game["bHasTarget"].get<bool>() ||
                !game.contains("Target") || !game["Target"].is_object())
                return {};
            const auto& target = game["Target"];
            if (!target.contains("Name") || !target["Name"].is_string()) return {};
            const std::string name = target["Name"].get<std::string>();
            const int team = target.contains("TeamNum") && target["TeamNum"].is_number_integer() ? target["TeamNum"].get<int>() : -1;
            if (name.empty() || !data.contains("Players") || !data["Players"].is_array()) return {};
            std::string match;
            for (const auto& p : data["Players"]) {
                if (!p.contains("PrimaryId") || !p["PrimaryId"].is_string() || !p.contains("Name") || !p["Name"].is_string() ||
                    p["Name"].get<std::string>() != name)
                    continue;
                if (team != -1 && (!p.contains("TeamNum") || !p["TeamNum"].is_number_integer() || p["TeamNum"].get<int>() != team))
                    continue;
                if (!match.empty()) return {};
                match = p["PrimaryId"].get<std::string>();
                if (match == "Unknown" || match.rfind("Unknown|", 0) == 0) match = "Unknown|" + name;
            }
            return match;
        }

        ForfeitScope ReadForfeitScope(const nlohmann::json& scope) {
            ForfeitScope s;
            s.localFlag = ReadTrueBoolean(scope, {"bLocalPlayerForfeit", "bLocalForfeit", "LocalPlayerForfeit",
                                                  "bLocalPlayerAbandoned", "bLocalPlayerAbandon", "LocalPlayerAbandoned"});
            s.forfeit = ReadTrueBoolean(scope, {"bForfeit", "Forfeit"});
            s.forfeitTeam = ReadInteger(scope, {"ForfeitTeamNum", "ForfeitingTeamNum", "AbandoningTeamNum"});
            s.winnerTeam = ReadInteger(scope, {"WinnerTeamNum", "winner_team_num"});
            return s;
        }

        std::vector<ForfeitScope> ReadForfeitScopes(const nlohmann::json& data) {
            std::vector<ForfeitScope> scopes{ReadForfeitScope(data)};
            if (const auto* game = Child(data, "Game"); game && game->is_object()) scopes.push_back(ReadForfeitScope(*game));
            return scopes;
        }

    } // namespace

    bool HasExplicitLocalForfeitSignal(const std::vector<ForfeitScope>& scopes, int localTeam) {
        for (const auto& scope : scopes) {
            if (scope.localFlag) return true;
            if (!scope.forfeit) continue;
            if (scope.forfeitTeam && (*scope.forfeitTeam == 0 || *scope.forfeitTeam == 1)) {
                if (*scope.forfeitTeam == localTeam) return true;
                continue;
            }
            if (scope.winnerTeam && (*scope.winnerTeam == 0 || *scope.winnerTeam == 1) &&
                (localTeam == 0 || localTeam == 1) && *scope.winnerTeam != localTeam) {
                return true;
            }
        }
        return false;
    }

    std::optional<TelemetryEvent> ParseEventFromJson(std::string_view eventName, const std::string& jsonStr) {
        nlohmann::json data;
        try {
            data = nlohmann::json::parse(jsonStr);
        } catch (...) {
            return std::nullopt;
        }

        if (eventName == Constants::EVT_MATCH_CREATED) {
            return MatchCreatedEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_MATCH_INITIALIZED) {
            return MatchInitializedEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_COUNTDOWN_BEGIN) {
            return CountdownBeginEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_ROUND_STARTED) {
            return RoundStartedEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_MATCH_PAUSED) {
            return MatchPausedEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_MATCH_UNPAUSED) {
            return MatchUnpausedEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_GOAL_REPLAY_START) {
            return GoalReplayStartEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_GOAL_REPLAY_WILL_END) {
            return GoalReplayWillEndEvent{};
        }
        if (eventName == Constants::EVT_GOAL_REPLAY_END) {
            return GoalReplayEndEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_PODIUM_START) {
            return PodiumStartEvent{ExtractGuid(data)};
        }
        if (eventName == Constants::EVT_REPLAY_CREATED) {
            return ReplayCreatedEvent{};
        }
        if (eventName == Constants::EVT_PLAYER_LEFT) {
            PlayerLeftEvent ev;
            ev.name = String(data, "PlayerName");
            std::string rawId = String(data, "PrimaryId");
            ev.primaryId = NormalizeId(rawId, ev.name);
            ev.matchGuid = ExtractGuid(data);
            return ev;
        }
        if (eventName == Constants::EVT_CROSSBAR_HIT) {
            CrossbarHitEvent ev;
            ev.matchGuid = ExtractGuid(data);
            if (const auto* touch = Child(data, "BallLastTouch"); touch && touch->is_object()) {
                if (const auto* player = Child(*touch, "Player")) ev.lastTouch = ParsePlayerRef(*player);
            }
            ev.impactForce = Number(data, "ImpactForce");
            ev.eventId = EventId(data);
            return ev;
        }
        if (eventName == Constants::EVT_MATCH_ENDED) {
            MatchEndedEvent ev;
            ev.matchGuid = ExtractGuid(data);
            ev.winnerTeam = ReadInteger(data, {"WinnerTeamNum", "winner_team_num"}).value_or(-1);
            ev.hasTeamScores = ReadTeamScores(data, ev.teamScores);
            ev.forfeitScopes = ReadForfeitScopes(data);
            return ev;
        }
        if (eventName == Constants::EVT_MATCH_DESTROYED) {
            MatchDestroyedEvent ev;
            ev.matchGuid = ExtractGuid(data);
            ev.hasTeamScores = ReadTeamScores(data, ev.teamScores);
            ev.forfeitScopes = ReadForfeitScopes(data);
            ev.winnerTeam = ReadInteger(data, {"WinnerTeamNum", "winner_team_num"});
            return ev;
        }
        if (eventName == Constants::EVT_STATFEED) {
            StatfeedEvent ev;
            ev.eventName = String(data, "EventName");
            if (const auto* main = Child(data, "MainTarget")) {
                ev.mainTarget = ParsePlayerRef(*main);
            } else if (const auto* p = Child(data, "Player")) {
                ev.mainTarget = ParsePlayerRef(*p);
            }
            if (const auto* sec = Child(data, "SecondaryTarget")) {
                ev.secondaryTarget = ParsePlayerRef(*sec);
            }
            ev.eventId = EventId(data);
            ev.matchGuid = ExtractGuid(data);
            return ev;
        }
        if (eventName == Constants::EVT_GOAL_SCORED) {
            GoalScoredEvent ev;
            ev.matchGuid = ExtractGuid(data);
            if (const auto* scorer = Child(data, "Scorer"); scorer && scorer->is_object()) {
                ev.scorer = ParsePlayerRef(*scorer);
                ev.hasScorer = true;
                // The replay-end repeat carries an empty scorer name.
                ev.isEcho = ev.scorer.name.empty();
            }
            if (const auto* assister = Child(data, "Assister")) {
                ev.assister = ParsePlayerRef(*assister);
            }
            ev.goalSpeed = Number(data, "GoalSpeed");
            if (const auto* touch = Child(data, "BallLastTouch")) {
                ev.lastTouchSpeed = Number(*touch, "Speed");
            }
            ev.roundSeconds = Number(data, "GoalTime");
            ev.impactLocation = Point(data, "ImpactLocation");
            ev.eventId = EventId(data);
            return ev;
        }
        if (eventName == Constants::EVT_BALL_HIT) {
            BallHitEvent ev;
            if (const auto* players = Child(data, "Players"); players && players->is_array()) {
                for (const auto& p : *players) {
                    ev.hitters.push_back(ParsePlayerRef(p));
                }
            }
            if (const auto* ball = Child(data, "Ball")) {
                ev.postHitSpeed = Number(*ball, "PostHitSpeed").value_or(0.0f);
                ev.location = Point(*ball, "Location");
            }
            ev.matchGuid = ExtractGuid(data);
            return ev;
        }
        if (eventName == Constants::EVT_BOOST_PICKUP) {
            BoostPickupEvent ev;
            ev.bReplay = Bool(data, "bReplay").value_or(false);
            if (const auto* player = Child(data, "Player")) {
                ev.player = ParsePlayerRef(*player);
            }
            ev.boostType = String(data, "BoostType");
            ev.location = Point(data, "Location");
            ev.matchGuid = ExtractGuid(data);
            return ev;
        }
        if (eventName == Constants::EVT_UPDATE_STATE) {
            UpdateStateEvent ev;
            ev.matchGuid = ExtractGuid(data);
            ev.cameraTargetPlayerId = ExtractCameraTarget(data);
            ev.forfeitScopes = ReadForfeitScopes(data);

            if (const auto* game = Child(data, "Game")) {
                ev.arena = String(*game, "Arena");
                ev.arenaAsset = String(*game, "ArenaAsset");
                if (auto pid = ReadInteger(*game, {"PlaylistId", "playlist_id"})) {
                    ev.playlistId = pid;
                }
                ev.bReplay = Bool(*game, "bReplay").value_or(false);
                ev.bOvertime = Bool(*game, "bOvertime").value_or(false);
                ev.bSpectator = Bool(*game, "bSpectator").value_or(false);
                ev.timeSeconds = Number(*game, "TimeSeconds");

                if (const auto* teams = Child(*game, "Teams"); teams && teams->is_array()) {
                    for (const auto& t : *teams) {
                        const int team = TeamOf(t);
                        if (team == 0 || team == 1) {
                            if (const auto score = Integer(t, "Score")) {
                                ev.teamScores[team] = *score;
                                ev.teamScorePresent[static_cast<size_t>(team)] = true;
                                ev.hasTeamScores = true;
                            }
                        }
                    }
                }

                if (const auto* ball = Child(*game, "Ball")) {
                    ev.hasBall = true;
                    if (const auto team = Integer(*ball, "TeamNum")) ev.ballTeam = *team;
                    if (const auto speed = Number(*ball, "Speed")) {
                        ev.ballSpeed = *speed;
                        ev.hasBallSpeed = true;
                    }
                    ev.ballLocation = Point(*ball, "Location");
                }

                // Context flags
                ev.explicitNonRecordable =
                    ReadTrueBoolean(*game, {"bPrivateMatch", "bTraining", "bExhibition", "bTournamentSpectator"}) ||
                    MatchTypeContains(*game, {"private", "training", "exhibition", "freeplay"});
                if (ev.explicitNonRecordable) {
                    ev.fallbackNonRecordableReason = "explicit_non_competitive_context";
                }
                ev.explicitCasual = ReadTrueBoolean(*game, {"bCasualMatch"}) || MatchTypeContains(*game, {"casual"});
            }

            if (const auto* players = Child(data, "Players"); players && players->is_array()) {
                for (const auto& pj : *players) {
                    UpdatePlayerState p;
                    std::string rawId = String(pj, "PrimaryId");
                    p.name = String(pj, "Name");
                    p.primaryId = NormalizeId(rawId, p.name);
                    p.team = TeamOf(pj);
                    p.score = Integer(pj, "Score");
                    p.goals = Integer(pj, "Goals");
                    p.assists = Integer(pj, "Assists");
                    p.saves = Integer(pj, "Saves");
                    p.shots = Integer(pj, "Shots");
                    p.demos = Integer(pj, "Demos");
                    if (!p.demos.has_value()) p.demos = Integer(pj, "Demolishes");
                    p.touches = Integer(pj, "Touches");
                    p.carTouches = Integer(pj, "CarTouches");
                    p.shortcut = Integer(pj, "Shortcut");

                    // Car
                    p.bDemolished = Bool(pj, "bDemolished").value_or(false);
                    const auto hasCar = Bool(pj, "bHasCar");
                    p.bHasCar = hasCar.value_or(false);
                    p.hasCarField = Child(pj, "bHasCar") != nullptr;
                    p.bSupersonic = Bool(pj, "bSupersonic");
                    p.bOnWall = Bool(pj, "bOnWall").value_or(false);
                    p.bOnGround = Bool(pj, "bOnGround").value_or(false);
                    p.bPowersliding = Bool(pj, "bPowersliding").value_or(false);
                    p.bBoosting = Bool(pj, "bBoosting").value_or(false);
                    p.speed = Number(pj, "Speed").value_or(0.0f);
                    p.boost = Integer(pj, "Boost");

                    ev.players.push_back(std::move(p));
                }
            }
            return ev;
        }

        return std::nullopt;
    }

} // namespace TelemetryEvents
