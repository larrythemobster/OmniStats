#pragma once

// Telemetry values that change while a match is running. The RmlUi data model
// named "live" binds these fields, and widget markup references them with
// {{field}} text and data-class-* expressions, so a telemetry tick updates only
// the text nodes whose bound value changed.

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "core/Config.hpp"
#include "core/SessionState.hpp"

struct RmlRenderSnapshot;

namespace Rml {
    class Context;
}

struct LiveValues {
    Rml::String match_saves;
    Rml::String match_shots;
    Rml::String match_assists;
    Rml::String match_demos;
    Rml::String match_demoed;
    Rml::String match_crossbars;
    Rml::String match_max_goal_speed;
    Rml::String match_max_ball_speed;
    Rml::String match_hardest_crossbar;
    Rml::String match_fastest_goal;
    Rml::String match_own_goals;

    Rml::String session_record;
    Rml::String session_goals;
    Rml::String session_saves;
    Rml::String session_assists;
    Rml::String session_demos;
    Rml::String session_boost;
    Rml::String session_goal_participation;
    Rml::String session_mmr;
    Rml::String session_net;

    Rml::String demo_game_count;
    Rml::String demo_game_kd;
    Rml::String demo_session_count;
    Rml::String demo_session_kd;

    Rml::String roster_arena;
    Rml::String dashboard_status;

    // -1 loss, 0 neutral, +1 win. Markup maps these to the win/loss/muted classes.
    int session_mmr_tone = 0;
    int demo_game_tone = 0;
    int demo_session_tone = 0;
};

struct LivePlayerStat {
    Rml::String text;
    bool visible = false;
    bool operator==(const LivePlayerStat&) const = default;
};

LiveValues ComputeLiveValues(const RmlRenderSnapshot& snapshot, const ConfigData& config);
LivePlayerStat ComputeLivePlayerStat(const PlayerData& player);

class RmlLiveModel {
  public:
    // Must run before any document that references the model is loaded.
    bool Create(Rml::Context* context);
    void Reset();

    const LiveValues& Values() const {
        return m_values;
    }

    // Stable per-player slot for `players[N]` bindings. Slots are never reused,
    // so markup rendered for an earlier roster can never point at another player.
    size_t PlayerSlot(const std::string& primaryId);

    // Returns true when any bound value changed.
    bool SetValues(LiveValues values);
    bool SetPlayer(size_t slot, LivePlayerStat stat);

  private:
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;
    LiveValues m_values;
    std::vector<LivePlayerStat> m_players;
    std::unordered_map<std::string, size_t> m_playerSlots;
};
