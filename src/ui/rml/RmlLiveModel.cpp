#include "ui/rml/RmlLiveModel.hpp"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <cmath>
#include <cstring>

#include "ui/Formatting.hpp"
#include "ui/rml/RmlUiController.hpp"

namespace {
    struct StringField {
        const char* name;
        Rml::String LiveValues::* member;
    };

    struct ToneField {
        const char* name;
        int LiveValues::* member;
    };

    constexpr StringField kStringFields[] = {
        {"match_saves", &LiveValues::match_saves},
        {"match_shots", &LiveValues::match_shots},
        {"match_assists", &LiveValues::match_assists},
        {"match_demos", &LiveValues::match_demos},
        {"match_demoed", &LiveValues::match_demoed},
        {"match_crossbars", &LiveValues::match_crossbars},
        {"match_max_goal_speed", &LiveValues::match_max_goal_speed},
        {"match_max_ball_speed", &LiveValues::match_max_ball_speed},
        {"match_hardest_crossbar", &LiveValues::match_hardest_crossbar},
        {"match_fastest_goal", &LiveValues::match_fastest_goal},
        {"match_own_goals", &LiveValues::match_own_goals},
        {"session_record", &LiveValues::session_record},
        {"session_goals", &LiveValues::session_goals},
        {"session_saves", &LiveValues::session_saves},
        {"session_assists", &LiveValues::session_assists},
        {"session_demos", &LiveValues::session_demos},
        {"session_boost", &LiveValues::session_boost},
        {"session_goal_participation", &LiveValues::session_goal_participation},
        {"session_mmr", &LiveValues::session_mmr},
        {"session_net", &LiveValues::session_net},
        {"demo_game_count", &LiveValues::demo_game_count},
        {"demo_game_kd", &LiveValues::demo_game_kd},
        {"demo_session_count", &LiveValues::demo_session_count},
        {"demo_session_kd", &LiveValues::demo_session_kd},
        {"roster_arena", &LiveValues::roster_arena},
        {"dashboard_status", &LiveValues::dashboard_status},
    };

    constexpr ToneField kToneFields[] = {
        {"session_mmr_tone", &LiveValues::session_mmr_tone},
        {"demo_game_tone", &LiveValues::demo_game_tone},
        {"demo_session_tone", &LiveValues::demo_session_tone},
    };

    int KdTone(int demos, int demoed) {
        const char* cls = RmlUiController::DemoKdClass(demos, demoed);
        if (std::strcmp(cls, "win") == 0) return 1;
        if (std::strcmp(cls, "loss") == 0) return -1;
        return 0;
    }

    std::string Speed(float all, float self, bool imperial) {
        return Format::PairSpeed(all, self, true, " kph", imperial);
    }
}

LiveValues ComputeLiveValues(const RmlRenderSnapshot& snapshot, const ConfigData& config) {
    const auto& match = snapshot.currentMatch;
    const auto& session = snapshot.sessionTotals;
    const int sessionMmr = static_cast<int>(std::lround(session.totalMmrChange));
    const float gp = session.teamGoals > 0
                         ? 100.0f * static_cast<float>(session.goalParticipations) / static_cast<float>(session.teamGoals)
                         : 0.0f;
    const auto demos = CalculateSessionDemolitionCounts(session, match, snapshot.matchFinalized);

    LiveValues v;
    v.match_saves = Format::PairCount(match.saves, match.savesSelf);
    v.match_shots = Format::PairCount(match.shots, match.shotsSelf);
    v.match_assists = Format::PairCount(match.assists, match.assistsSelf);
    v.match_demos = Format::PairCount(match.demos, match.demosSelf);
    v.match_demoed = std::to_string(match.demoedSelf);
    v.match_crossbars = Format::PairCount(match.crossbars, match.crossbarsSelf);
    v.match_max_goal_speed = Speed(match.maxGoalSpeed, match.maxGoalSpeedSelf, config.imperial_units);
    v.match_max_ball_speed = Speed(match.maxBallSpeed, match.maxBallSpeedSelf, config.imperial_units);
    v.match_hardest_crossbar = config.crossbar_display_mode == "speed"
                                   ? Speed(match.maxImpactForce * 0.036f, match.maxImpactForceSelf * 0.036f, config.imperial_units)
                                   : Format::PairSpeed(match.maxImpactForce, match.maxImpactForceSelf, true, "", false);
    v.match_fastest_goal = Format::PairFastest(match.fastestGoalTime, match.fastestGoalTimeSelf);
    v.match_own_goals = Format::PairCount(match.ownGoals, match.ownGoalsSelf);

    v.session_record = RmlUiDetail::FormatRecord(session.wins, session.losses);
    v.session_goals = std::to_string(session.goals);
    v.session_saves = std::to_string(session.saves);
    v.session_assists = std::to_string(session.assists);
    v.session_demos = std::to_string(session.demos);
    v.session_boost = std::to_string(CalculateSessionBoostPickedUp(session, match, snapshot.matchFinalized));
    v.session_goal_participation = session.teamGoals > 0 ? RmlUiDetail::FormatNumber(gp, 0) + "%" : "--";
    v.session_mmr = (sessionMmr >= 0 ? "+" : "") + std::to_string(sessionMmr);
    v.session_mmr_tone = sessionMmr > 0 ? 1 : sessionMmr < 0 ? -1
                                                             : 0;
    v.session_net = std::to_string(session.wins - session.losses);

    v.demo_game_count = std::to_string(match.demosSelf) + "-" + std::to_string(match.demoedSelf);
    v.demo_game_kd = RmlUiController::FormatDemoKd(match.demosSelf, match.demoedSelf);
    v.demo_game_tone = KdTone(match.demosSelf, match.demoedSelf);
    v.demo_session_count = std::to_string(demos.demos) + "-" + std::to_string(demos.demoed);
    v.demo_session_kd = RmlUiController::FormatDemoKd(demos.demos, demos.demoed);
    v.demo_session_tone = KdTone(demos.demos, demos.demoed);

    v.roster_arena = snapshot.arenaName.empty() ? (snapshot.inMatch ? "Active match" : "No active match connected") : snapshot.arenaName;
    v.dashboard_status = snapshot.inMatch ? "ACTIVE MATCH · " + snapshot.arenaName : "WAITING IN LOBBY";
    return v;
}

LivePlayerStat ComputeLivePlayerStat(const PlayerData& player) {
    LivePlayerStat stat;
    stat.visible = player.goals || player.saves || player.shots || player.assists || player.demos;
    stat.text = "G" + std::to_string(player.goals) + " S" + std::to_string(player.saves) +
                " A" + std::to_string(player.assists) + " Sh" + std::to_string(player.shots) +
                " D" + std::to_string(player.demos);
    return stat;
}

bool RmlLiveModel::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("live");
    if (!constructor) return false;

    for (const auto& field : kStringFields)
        constructor.Bind(field.name, &(m_values.*field.member));
    for (const auto& field : kToneFields)
        constructor.Bind(field.name, &(m_values.*field.member));

    if (auto player = constructor.RegisterStruct<LivePlayerStat>()) {
        player.RegisterMember("text", &LivePlayerStat::text);
        player.RegisterMember("visible", &LivePlayerStat::visible);
    }
    constructor.RegisterArray<std::vector<LivePlayerStat>>();
    constructor.Bind("players", &m_players);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void RmlLiveModel::Reset() {
    m_handle = {};
    m_bound = false;
}

void RmlLiveModel::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

size_t RmlLiveModel::PlayerSlot(const std::string& primaryId) {
    auto [it, inserted] = m_playerSlots.try_emplace(primaryId, m_players.size());
    if (inserted) {
        m_players.emplace_back();
        Dirty("players");
    }
    return it->second;
}

bool RmlLiveModel::SetValues(LiveValues values) {
    bool changed = false;
    for (const auto& field : kStringFields) {
        Rml::String& current = m_values.*field.member;
        Rml::String& next = values.*field.member;
        if (current == next) continue;
        current = std::move(next);
        Dirty(field.name);
        changed = true;
    }
    for (const auto& field : kToneFields) {
        if (m_values.*field.member == values.*field.member) continue;
        m_values.*field.member = values.*field.member;
        Dirty(field.name);
        changed = true;
    }
    return changed;
}

bool RmlLiveModel::SetPlayer(size_t slot, LivePlayerStat stat) {
    if (slot >= m_players.size() || m_players[slot] == stat) return false;
    m_players[slot] = std::move(stat);
    Dirty("players");
    return true;
}
