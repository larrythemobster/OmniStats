#include "ui/rml/views/OnboardingView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>

namespace {
    const char* StepTitle(int step) {
        switch (step) {
        case 0:
            return "Connect Rocket League";
        case 1:
            return "Choose a layout";
        case 2:
            return "Optional services";
        default:
            return "Your account";
        }
    }
}

bool OnboardingView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("onboarding");
    if (!constructor) return false;

    constructor.Bind("step", &m_step);
    constructor.Bind("step_title", &m_stepTitle);
    constructor.Bind("stats_status", &m_statsStatus);
    constructor.Bind("stats_path", &m_statsPath);
    constructor.Bind("stats_message", &m_statsMessage);
    constructor.Bind("stats_ok", &m_statsOk);
    constructor.Bind("stats_can_fix", &m_statsCanFix);
    constructor.Bind("show_restart_hint", &m_showRestartHint);
    constructor.Bind("second_monitor", &m_secondMonitor);
    constructor.Bind("mmr_tracking", &m_mmrTracking);
    constructor.Bind("discord", &m_discord);
    constructor.Bind("crash_reports", &m_crashReports);
    constructor.Bind("identity_hint", &m_identityHint);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    m_stepTitle = StepTitle(m_step);
    return true;
}

void OnboardingView::Reset() {
    m_handle = {};
    m_bound = false;
}

template <typename T>
void OnboardingView::Set(T& field, T value, const char* name) {
    if (field == value) return;
    field = std::move(value);
    if (m_bound) m_handle.DirtyVariable(name);
}

void OnboardingView::SetStep(int step) {
    Set(m_step, std::clamp(step, 0, kStepCount - 1), "step");
    Set(m_stepTitle, Rml::String(StepTitle(m_step)), "step_title");
}

void OnboardingView::Refresh(const ConfigData& config, const StatsApiConfig::CheckResult& statsApi, bool statsApiChecked,
                             bool accountDetected) {
    const bool ok = statsApiChecked && statsApi.status == StatsApiConfig::Status::Valid;
    Set(m_statsOk, ok, "stats_ok");
    Set(m_statsStatus, Rml::String(!statsApiChecked ? "Checking the Stats API config..." : ok ? "Stats API is on. You're connected."
                                                                                              : StatsApiConfig::GetStatusMessage(statsApi.status)),
        "stats_status");
    Set(m_statsPath, Rml::String(statsApi.path.empty() ? "DefaultStatsAPI.ini not found" : statsApi.path), "stats_path");
    Set(m_statsMessage, Rml::String(ok ? "" : statsApi.message), "stats_message");
    Set(m_statsCanFix, !ok && !statsApi.path.empty(), "stats_can_fix");
    Set(m_showRestartHint, !ok && statsApi.rlRunning, "show_restart_hint");

    Set(m_secondMonitor, config.second_monitor_mode, "second_monitor");
    Set(m_mmrTracking, config.enable_mmr_tracking, "mmr_tracking");
    Set(m_discord, config.discord_rpc_enabled, "discord");
    Set(m_crashReports, config.crash_reports_enabled, "crash_reports");
    Set(m_identityHint,
        Rml::String(accountDetected ? "Your account was detected from a match. You can change it later in Settings > General."
                                    : "Only accounts OmniStats has already seen are listed. Auto-detect works for new installs."),
        "identity_hint");
}
