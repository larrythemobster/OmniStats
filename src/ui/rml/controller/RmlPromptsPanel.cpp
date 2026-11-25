#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

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
    RebuildPromptRoot();
    m_renderDirty = true;
}

void RmlUiController::RebuildPromptRoot() {
    if (m_statsApiPrompt == StatsApiPrompt::Hidden) {
        if (m_sessionExpiredPrompt) {
            SetRootRml("prompt-root",
                       "<div class='prompt-backdrop'><div class='card prompt-card'>"
                       "<div class='row prompt-head'><img class='brand-logo' src='res://images/Logo.png'/><div class='prompt-title grow'>Sign in to OmniStats again</div></div>"
                       "<div class='prompt-body'>Your session expired or this PC was removed from your account, so player ranks can't load.</div>"
                       "<div class='prompt-detail'>Your match history is still saved on this PC.</div>"
                       "<div class='row gap-sm prompt-actions'><div class='grow'></div>" +
                           Button("session-prompt-close", "Not now", "ghost") + Button("session-sign-in", "Sign in", "primary") +
                           "</div></div></div>");
        } else {
            SetRootRml("prompt-root", m_dejaVuPrompt ? RenderDejaVuPrompt() : std::string{});
        }
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

namespace {
    std::string GroupThousands(int value) {
        std::string digits = std::to_string(value);
        for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
            digits.insert(static_cast<size_t>(i), ",");
        return digits;
    }
}

bool RmlUiController::DejaVuPromptReady() const {
    if (m_dejaVuPrompt || !m_state || m_state->ui.dejaVuPlayers.load() <= 0) return false;
    if (!m_config.onboarding_completed || !Config::HasCurrentConsent(m_config) || m_onboarding.IsVisible()) return false;
    if (TrackedAccounts::FromPrimaryId(m_config.last_primary_id)) return true;
    std::lock_guard lock(m_state->ui.dejaVuMutex);
    return !m_state->ui.dejaVuAccounts.empty();
}

void RmlUiController::CheckDejaVuPrompt() {
    if (!DejaVuPromptReady() || m_statsApiPrompt != StatsApiPrompt::Hidden || m_sessionExpiredPrompt) return;
    m_dejaVuAccounts.clear();
    {
        std::lock_guard lock(m_state->ui.dejaVuMutex);
        m_dejaVuPath = m_state->ui.dejaVuPath;
        m_dejaVuPlayers = m_state->ui.dejaVuPlayers.exchange(0);
        for (const auto& [id, name] : m_state->ui.dejaVuAccounts) {
            const auto identity = TrackedAccounts::FromPrimaryId(id);
            m_dejaVuAccounts.push_back({id, identity ? name + " (" + TrackedAccounts::PlatformLabel(identity->platform) + ")" : name});
        }
    }
    std::sort(m_dejaVuAccounts.begin(), m_dejaVuAccounts.end(), [](const SelectOption& a, const SelectOption& b) {
        return _stricmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    const std::string& current = m_config.last_primary_id;
    const bool currentListed = std::any_of(m_dejaVuAccounts.begin(), m_dejaVuAccounts.end(), [&](const SelectOption& o) { return o.value == current; });
    if (!currentListed) {
        // The current account has no recorded match yet, so it has no name to show.
        if (const auto identity = TrackedAccounts::FromPrimaryId(current))
            m_dejaVuAccounts.insert(m_dejaVuAccounts.begin(), {current, std::string("Current ") + TrackedAccounts::PlatformLabel(identity->platform) + " account"});
    }
    m_dejaVuAccount = m_dejaVuAccounts.front().value;
    for (const auto& option : m_dejaVuAccounts)
        if (option.value == current) m_dejaVuAccount = current;
    m_dejaVuPrompt = true;
    RebuildPromptRoot();
    m_renderDirty = true;
}

std::string RmlUiController::RenderDejaVuPrompt() const {
    std::string html =
        "<div class='prompt-backdrop'><div class='card prompt-card'>"
        "<div class='row prompt-head'><img class='brand-logo' src='res://images/Logo.png'/><div class='prompt-title grow'>Import your DejaVu history?</div></div>"
        "<div class='prompt-body'>" +
        Escape("OmniStats found BakkesMod DejaVu records for " + GroupThousands(m_dejaVuPlayers) +
               " players on this PC. Importing adds your old wins and losses with and against them to your OmniStats records.") +
        "</div><div class='prompt-detail'>OmniStats only reads this file. You're asked once: if you skip it, OmniStats won't ask again.</div>"
        "<div class='prompt-path mono'>" +
        Escape(m_dejaVuPath) + "</div>";
    if (m_dejaVuAccounts.size() > 1) {
        html += "<div class='prompt-field'><div class='prompt-field-label'>Add to account</div>"
                "<div class='prompt-field-help'>DejaVu didn't save which of your accounts played, so pick one.</div>" +
                SelectControl("dejavu_account", m_dejaVuAccounts, m_dejaVuAccount) + "</div>";
    }
    html += "<div class='row gap-sm prompt-actions'>" + Button("dejavu-prompt-decline", "Don't import", "ghost") +
            "<div class='grow'></div>" + Button("dejavu-prompt-import", "Import", "primary") + "</div></div></div>";
    return html;
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
        RebuildPromptRoot();
        m_renderDirty = true;
        return true;
    } else if (action == "session-sign-in") {
        m_sessionExpiredPrompt = false;
        RebuildPromptRoot();
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
    } else if (action == "dejavu-prompt-import") {
        if (m_dbManager) {
            const bool started = StartBackgroundTask(
                "Importing DejaVu history...",
                [this, db = m_dbManager, path = m_dejaVuPath, account = m_dejaVuAccount](std::stop_token) -> std::function<void()> {
                    const auto result = db->ImportDejaVuHistory(std::filesystem::path(path), account);
                    return [this, result] {
                        if (result.imported) {
                            ShowToast("Imported DejaVu records for " + GroupThousands(result.players) + " players.");
                        } else {
                            ShowToast("DejaVu import failed: " + result.error, true);
                        }
                    };
                });
            if (!started) return true;
        }
        m_dejaVuPrompt = false;
        RebuildPromptRoot();
        m_renderDirty = true;
        return true;
    } else if (action == "dejavu-prompt-decline") {
        if (m_dbManager) m_dbManager->AsyncDeclineDejaVuImport();
        m_dejaVuPrompt = false;
        RebuildPromptRoot();
        m_renderDirty = true;
        return true;
    } else {
        return false;
    }
    return true;
}
