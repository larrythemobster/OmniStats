#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <mutex>

#include "database/DatabaseManager.hpp"
#include "network/api/AccountClient.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

void RmlUiController::OpenStatsApiFixIfNeeded() {
    if (!m_state || !m_config.check_stats_api_config_on_startup) return;
    bool needsFix = false;
    bool hasPath = false;
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        needsFix = m_state->ui.statsApiChecked.load() && m_state->ui.statsApiResult.status != StatsApiConfig::Status::Valid;
        hasPath = !m_state->ui.statsApiResult.path.empty();
    }
    if (needsFix) SetStatsApiPrompt(hasPath ? StatsApiPrompt::Asking : StatsApiPrompt::NotFound);
}

void RmlUiController::SetStatsApiPrompt(StatsApiPrompt state) {
    m_statsApiPrompt = state;
    RebuildStatsApiPrompt();
    m_renderDirty = true;
}

void RmlUiController::RebuildStatsApiPrompt() {
    if (m_statsApiPrompt == StatsApiPrompt::Hidden) {
        if (!m_sessionExpiredPrompt) {
            SetRootRml("prompt-root", "");
            return;
        }
        SetRootRml("prompt-root",
                   "<div class='prompt-backdrop'><div class='card prompt-card'>"
                   "<div class='row prompt-head'><img class='brand-logo' src='res://images/Logo.png'/><div class='prompt-title grow'>Sign in to OmniStats again</div></div>"
                   "<div class='prompt-body'>Your session expired or this PC was removed from your account, so player ranks can't load.</div>"
                   "<div class='prompt-detail'>Your match history is still saved on this PC.</div>"
                   "<div class='row gap-sm prompt-actions'><div class='grow'></div>" +
                       Button("session-prompt-close", "Not now", "ghost") + Button("session-sign-in", "Sign in", "primary") +
                       "</div></div></div>");
        return;
    }
    StatsApiConfig::CheckResult result;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
    }
    std::string title;
    std::string body;
    std::string detail;
    std::string actions;
    switch (m_statsApiPrompt) {
    case StatsApiPrompt::Asking:
        title = "Turn on live match data?";
        body = "Rocket League's Stats API is switched off, so OmniStats can't see your matches. OmniStats can turn it on by editing one Rocket League config file.";
        detail = "Windows will ask for permission because the file is in Rocket League's install folder.";
        if (result.rlRunning) detail += " Restart Rocket League afterwards.";
        actions = Button("statsapi-prompt-never", "Don't ask again", "ghost") + "<div class='grow'></div>" +
                  Button("statsapi-prompt-close", "Not now", "ghost") + Button("statsapi-prompt-fix", "Turn it on", "primary");
        break;
    case StatsApiPrompt::Repairing:
        title = "Turning on the Stats API...";
        body = "Approve the Windows permission prompt to continue. It may be behind other windows.";
        break;
    case StatsApiPrompt::Repaired:
        title = "Live match data is on";
        body = result.rlRunning ? "Restart Rocket League so it picks up the change." : "OmniStats will see your next match.";
        actions = "<div class='grow'></div>" + Button("statsapi-prompt-close", "Done", "primary");
        break;
    case StatsApiPrompt::Failed:
        title = "Couldn't turn on the Stats API";
        body = result.message.empty() ? "The config file wasn't changed." : result.message;
        actions = Button("statsapi-prompt-troubleshoot", "Troubleshooting", "ghost") + "<div class='grow'></div>" +
                  Button("statsapi-prompt-close", "Close", "ghost") + Button("statsapi-prompt-fix", "Try again", "primary");
        break;
    case StatsApiPrompt::NotFound:
        title = "Couldn't find Rocket League's Stats API config";
        body = "Set the file location in Settings > Data & support > Troubleshooting, then try again.";
        actions = "<div class='grow'></div>" + Button("statsapi-prompt-close", "Close", "ghost") +
                  Button("statsapi-prompt-troubleshoot", "Open troubleshooting", "primary");
        break;
    case StatsApiPrompt::Hidden:
        break;
    }
    std::string html = "<div class='prompt-backdrop'><div class='card prompt-card'>"
                       "<div class='row prompt-head'><img class='brand-logo' src='res://images/Logo.png'/><div class='prompt-title grow'>" +
                       Escape(title) + "</div></div><div class='prompt-body'>" + Escape(body) + "</div>";
    if (!detail.empty()) html += "<div class='prompt-detail'>" + Escape(detail) + "</div>";
    if (m_statsApiPrompt == StatsApiPrompt::Asking && !result.path.empty()) {
        html += "<div class='prompt-path mono'>" + Escape(result.path) + "</div>";
    }
    if (!actions.empty()) html += "<div class='row gap-sm prompt-actions'>" + actions + "</div>";
    SetRootRml("prompt-root", html + "</div></div>");
}

void RmlUiController::CheckAccountLinkPrompt() {
    if (!m_config.onboarding_completed || m_config.account_link_prompt_shown) {
        m_accountPromptPending = false;
        return;
    }
    const AccountStatusSnapshot accountStatus = m_accountClient ? m_accountClient->GetStatus() : AccountStatusSnapshot{};
    if (accountStatus.state == AccountAuthState::Migrating) {
        m_accountPromptPending = true;
        return;
    }
    m_accountPromptPending = false;
    if (accountStatus.state == AccountAuthState::SignedIn || (m_accountClient && m_accountClient->IsSignedIn()) ||
        !m_config.account_refresh_token.empty()) {
        return;
    }
    m_settingsPage = SettingsPage::Accounts;
    if (m_state) {
        m_state->ui.showMenu.store(true);
        m_state->ui.dashboardLayoutEditMode.store(false);
    }
    RebuildSettings();
    Config::Update([](ConfigData& c) { c.account_link_prompt_shown = true; });
    m_config = Config::Read();
    m_renderDirty = true;
}

bool RmlUiController::HandlePromptsAction(const std::string& action, Rml::Element*) {
    if (action == "statsapi-prompt-fix") {
        if (CheckStatsApi(true, false)) SetStatsApiPrompt(StatsApiPrompt::Repairing);
        return true;
    } else if (action == "statsapi-prompt-close") {
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        return true;
    } else if (action == "statsapi-prompt-never") {
        Config::Update([](ConfigData& c) { c.check_stats_api_config_on_startup = false; });
        m_config = Config::Read();
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        ShowToast("You can fix the Stats API later in Settings > Data & support > Troubleshooting.");
        return true;
    } else if (action == "statsapi-prompt-troubleshoot") {
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        m_settingsPage = SettingsPage::DataSupport;
        m_showTroubleshooting = true;
        if (m_state) m_state->ui.showMenu.store(true);
        m_renderDirty = true;
        return true;
    } else if (action == "session-prompt-close") {
        m_sessionExpiredPrompt = false;
        RebuildStatsApiPrompt();
        m_renderDirty = true;
        return true;
    } else if (action == "session-sign-in") {
        m_sessionExpiredPrompt = false;
        RebuildStatsApiPrompt();
        m_settingsPage = SettingsPage::Accounts;
        if (m_state) {
            m_state->ui.showMenu.store(true);
            m_state->ui.dashboardLayoutEditMode.store(false);
        }
        if (m_accountClient) m_accountClient->BeginSignInAsync(true);
        RebuildSettings();
        m_renderDirty = true;
        return true;
    } else if (action == "open-rank-settings") {
        m_settingsPage = SettingsPage::Accounts;
        if (m_state) m_state->ui.showMenu.store(true);
        m_renderDirty = true;
        return true;
    } else {
        return false;
    }
    return true;
}
