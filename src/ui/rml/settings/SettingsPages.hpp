#pragma once

#include <memory>
#include <set>
#include <string>

#include "core/config/Config.hpp"
#include "network/api/DeviceAuthFlow.hpp"
#include "ui/rml/services/InputCaptureService.hpp"

class SessionState;
struct RmlRenderSnapshot;
namespace Tournaments {
    struct ScheduleSnapshot;
}

namespace SettingsPages {
    std::string RenderGeneral(const ConfigData& config);
    std::string RenderAppearance(const ConfigData& config, const std::string& editColorKey, float editColorHue, bool showAdvanced,
                                 bool pending, const std::string& nameDraft, const std::string& importDraft,
                                 const std::string& message, bool messageError, bool canUndo,
                                 const std::set<std::string>& openGroups, DashboardLayout::WidgetId cardTextWidget);
    std::string RenderCards(const ConfigData& config, bool showDetails, bool signedIn);
    std::string RenderReminders(const ConfigData& config, bool signedIn, const Tournaments::ScheduleSnapshot& tournaments);
    std::string RenderIntegrations(const ConfigData& config, const AccountStatusSnapshot& accountStatus,
                                   const std::string& pendingToken, bool showToken, bool showFallback,
                                   const std::shared_ptr<SessionState>& state, BindCaptureTarget bindCaptureTarget);
    std::string RenderTroubleshooting(const ConfigData& config, const std::shared_ptr<SessionState>& state, const std::string& statsApiPathError);
    std::string RenderData(const ConfigData& config, const std::shared_ptr<SessionState>& state, bool busy);
    std::string RenderShortcuts(const ConfigData& config, const std::shared_ptr<SessionState>& state, BindCaptureTarget bindCaptureTarget);
    std::string RenderRanks(const ConfigData& config, const RmlRenderSnapshot& snap);
}
