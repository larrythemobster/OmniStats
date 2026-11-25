#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <ctime>
#include <mutex>

#include "database/DatabaseManager.hpp"
#include "network/api/AccountClient.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

void RmlUiController::ShowOnboarding() {
    if (!m_onboarding.Document()) return;
    m_onboarding.model.SetStep(0);
    if (m_state && !m_state->ui.statsApiChecked.load()) CheckStatsApi(false, false);
    RefreshOnboarding();
    m_onboarding.Show();
    m_onboarding.Raise();
    m_renderDirty = true;
}

void RmlUiController::FinishOnboarding() {
    if (!Config::HasCurrentConsent(Config::Read())) return;
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::OnboardingCompleted);
    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = true;
    });
    m_config = Config::Read();
    m_onboarding.Hide();
    RebuildVisibleUi(true);
}

void RmlUiController::RefreshOnboarding() {
    StatsApiConfig::CheckResult result;
    bool checked = false;
    bool connected = false;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
        checked = m_state->ui.statsApiChecked.load();
        connected = m_state->ui.statsApiConnected.load(std::memory_order_relaxed);
    }
    m_lastStatsApiConnected = connected;
    m_onboarding.model.Refresh(m_config, result, checked, connected, !m_snap.myPrimaryId.empty(), m_accountClient ? m_accountClient->GetStatus() : AccountStatusSnapshot{});

    // The account list changes with the live roster, so it is rendered with
    // the same select helper as Settings instead of a bound option list.
    if (Rml::Element* slot = m_onboarding.Document() ? m_onboarding.Document()->GetElementById("onboarding-identity") : nullptr) {
        const std::string rml = SelectRow("identity", "Account", "", IdentityOptions(), m_config.last_primary_id);
        if (rml != m_lastOnboardingIdentityRml) {
            m_lastOnboardingIdentityRml = rml;
            SetElementRml(slot, rml, false);
        }
    }
    m_renderDirty = true;
}

bool RmlUiController::HandleOnboardingAction(const std::string& action, Rml::Element* target) {
    if (action == "onboarding-next") {
        m_onboarding.model.SetStep(m_onboarding.model.Step() + 1);
        RefreshOnboarding();
    } else if (action == "onboarding-back") {
        m_onboarding.model.SetStep(m_onboarding.model.Step() - 1);
        RefreshOnboarding();
    } else if (action == "onboarding-finish") {
        FinishOnboarding();
    } else if (action == "onboarding-accept-terms") {
        const std::string acceptedAt = std::to_string(std::time(nullptr));
        Config::Update([&acceptedAt](ConfigData& c) {
            c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
            c.terms_accepted_version = Config::CurrentTermsVersion;
            c.privacy_accepted_at = acceptedAt;
        });
        Config::Save();
        m_config = Config::Read();
        if (m_config.onboarding_completed) {
            // Returning users only had to re-accept updated terms.
            m_onboarding.Hide();
            RebuildVisibleUi(true);
            CheckAccountLinkPrompt();
        } else {
            m_onboarding.model.SetStep(0);
            RefreshOnboarding();
        }
    } else if (action == "onboarding-exit") {
        if (m_state) m_state->ui.appExitRequested.store(true);
    } else if (action == "open-legal-link") {
        const std::string url = Attribute(target, "data-url");
        if (url == "https://omnistats.org/privacy" || url == "https://omnistats.org/terms") {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == "onboarding-layout") {
        const bool dashboard = Attribute(target, "data-mode") == "dashboard";
        Config::Update([dashboard](ConfigData& c) { c.second_monitor_mode = dashboard; });
        m_config = Config::Read();
        RefreshOnboarding();
    } else if (action == "onboarding-statsapi-check" || action == "onboarding-statsapi-fix") {
        CheckStatsApi(action == "onboarding-statsapi-fix", false);
        RefreshOnboarding();
    } else if (action == "onboarding-statsapi-browse") {
        BrowseStatsApiPath();
        RefreshOnboarding();
    } else if (action == "account-sign-in") {
        if (m_accountClient) m_accountClient->BeginSignInAsync(true);
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboarding.IsVisible()) RefreshOnboarding();
    } else if (action == "account-retry-migration") {
        if (m_accountClient) m_accountClient->RetryKeyMigrationAsync();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboarding.IsVisible()) RefreshOnboarding();
    } else if (action == "account-cancel") {
        if (m_accountClient) m_accountClient->CancelAuthorization();
        m_onboarding.model.SetShowFallback(false);
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboarding.IsVisible()) RefreshOnboarding();
    } else if (action == "account-sign-out") {
        if (m_accountClient) m_accountClient->LogoutAsync();
        m_config = Config::Read();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboarding.IsVisible()) RefreshOnboarding();
    } else if (action == "account-open-verify") {
        if (m_accountClient) m_accountClient->OpenVerificationBrowser();
    } else if (action == "account-copy-link") {
        const auto status = m_accountClient ? m_accountClient->GetStatus() : AccountStatusSnapshot{};
        if (!status.verificationUri.empty()) {
            m_systemInterface.SetClipboardText(status.verificationUri);
            ShowToast("Copied sign-in link to clipboard.");
        }
    } else if (action == "account-copy-code") {
        const auto status = m_accountClient ? m_accountClient->GetStatus() : AccountStatusSnapshot{};
        if (!status.userCode.empty()) {
            m_systemInterface.SetClipboardText(status.userCode);
            ShowToast("Copied verification code to clipboard.");
        }
    } else if (action == "account-toggle-fallback") {
        m_onboarding.model.ToggleFallback();
        if (m_onboarding.IsVisible()) RefreshOnboarding();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    } else if (action == "account-manage-devices") {
        if (m_accountClient) m_accountClient->OpenManageDevicesBrowser();
    } else {
        return false;
    }
    return true;
}
