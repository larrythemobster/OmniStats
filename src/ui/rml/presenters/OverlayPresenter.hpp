#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/config/Config.hpp"
#include "core/game/StatsScope.hpp"
#include "ui/config/DashboardLayoutConfig.hpp"
#include "ui/config/OverlayLayoutConfig.hpp"
#include "ui/rml/model/OverlayWidgetModel.hpp"

#include "ui/rml/controller/RmlRenderSnapshot.hpp"

namespace Rml {
    class Context;
    class DataModelConstructor;
    class Element;
}

class SessionState;
namespace Tournaments {
    class TournamentScheduleService;
}

class OverlayPresenter {
  public:
    OverlayPresenter() = default;

    bool Bind(Rml::DataModelConstructor& constructor);
    void Reset();

    // Updates widget data model values from snapshot and marks changed variables dirty.
    bool Update(const RmlRenderSnapshot& snap,
                const ConfigData& config,
                const std::shared_ptr<SessionState>& state,
                const Tournaments::TournamentScheduleService* tournaments);

    // Widget markup generators used during layout build and headless tests
    std::string RenderPlayerRoster(int team, const char* label, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state);
    std::string RenderLobbyRanks(const RmlRenderSnapshot& snap, const ConfigData& config);
    std::string RenderMmrGraph(bool showCategoryBadge, bool showPanControls, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state);
    std::string RenderGraphPanControls(const RmlRenderSnapshot& snap, const std::shared_ptr<SessionState>& state);
    std::string RenderPreviousGames(bool includeHeading, const RmlRenderSnapshot& snap, const ConfigData& config);
    std::string RenderLiveMatchStats(const RmlRenderSnapshot& snap, const ConfigData& config);
    std::string RenderSessionStats(bool compact, bool includeStreak, const RmlRenderSnapshot& snap, const ConfigData& config);
    std::string RenderStreaksStats(const std::shared_ptr<SessionState>& state, const ConfigData& config);
    std::string RenderGamemodeBreakdown(GamemodeBreakdownScope scope, const RmlRenderSnapshot& snap, const std::shared_ptr<SessionState>& state, const ConfigData& config);
    std::string RenderDemoTracker();
    std::string RenderMatchSummary(const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state, bool interactive, float dpiScale);
    std::string RenderSessionView(const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state, bool interactive, float dpiScale);
    std::string RenderSelfRankProgress(bool card, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state);

    OverlayWidgetModel& Model() noexcept {
        return m_model;
    }
    const OverlayWidgetModel& Model() const noexcept {
        return m_model;
    }

  private:
    OverlayWidgetModel m_model;
};
