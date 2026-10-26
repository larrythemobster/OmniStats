#pragma once

#include <memory>
#include <string>
#include "core/config/Config.hpp"
#include "network/api/DeviceAuthFlow.hpp"
#include "ui/rml/services/InputCaptureService.hpp"

class SessionState;
#include "ui/rml/controller/RmlRenderSnapshot.hpp"

class SettingsPresenter {
  public:
    SettingsPresenter() = default;

    std::string RenderGeneral(const ConfigData& config);
    std::string RenderAppearance(const ConfigData& config, const std::string& editColorKey, float editColorHue, bool showAdvanced = true);
    std::string RenderCards(const ConfigData& config, bool showDetails, bool signedIn = false);
    std::string RenderIntegrations(
        const ConfigData& config,
        const AccountStatusSnapshot& accountStatus,
        const std::string& pendingToken,
        bool showToken,
        bool showFallback = false,
        const std::shared_ptr<SessionState>& state = nullptr,
        BindCaptureTarget bindCaptureTarget = BindCaptureTarget::None);
    std::string RenderTroubleshooting(const ConfigData& config, const std::shared_ptr<SessionState>& state, const std::string& statsApiPathError = "");
    std::string RenderData(const ConfigData& config, const std::shared_ptr<SessionState>& state, bool busy);
    std::string RenderShortcuts(const ConfigData& config, const std::shared_ptr<SessionState>& state, BindCaptureTarget bindCaptureTarget = BindCaptureTarget::None);
    std::string RenderRanks(const ConfigData& config, const RmlRenderSnapshot& snap);
};
