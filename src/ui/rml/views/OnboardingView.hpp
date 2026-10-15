#pragma once

// Data model behind resources/rml/onboarding.rml, the first-run wizard:
// Stats API connection, layout, live ranks account, optional services, and Rocket League account.

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include "core/config/Config.hpp"
#include "core/config/StatsApiConfig.hpp"

namespace Rml {
    class Context;
}

class OnboardingView {
  public:
    static constexpr int kStepCount = 5;
    static constexpr int kRanksStep = 2;
    static constexpr int kAccountStep = 4;

    // Must run before onboarding.rml is loaded.
    bool Create(Rml::Context* context);
    void Reset();

    int Step() const {
        return m_step;
    }
    void SetStep(int step);
    void Refresh(const ConfigData& config, const StatsApiConfig::CheckResult& statsApi, bool statsApiChecked,
                 bool accountDetected);

    void ToggleFallback();
    void SetShowFallback(bool show);
    bool ShowFallback() const {
        return m_accountShowFallback;
    }

  private:
    template <typename T>
    void Set(T& field, T value, const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;

    int m_step = 0;
    Rml::String m_stepTitle;
    Rml::String m_statsStatus;
    Rml::String m_statsPath;
    Rml::String m_statsMessage;
    bool m_statsOk = false;
    bool m_statsCanFix = false;
    bool m_showRestartHint = false;
    bool m_secondMonitor = false;
    bool m_consentRequired = false;
    bool m_mmrTracking = false;
    bool m_discord = false;
    bool m_crashReports = false;
    Rml::String m_identityHint;
    bool m_accountSignedIn = false;
    bool m_accountAwaiting = false;
    bool m_accountMigrating = false;
    bool m_accountHasError = false;
    bool m_accountShowFallback = false;
    bool m_customApiEnabled = false;
    Rml::String m_accountDisplayName;
    Rml::String m_accountStatus;
    Rml::String m_accountUserCode;
};
