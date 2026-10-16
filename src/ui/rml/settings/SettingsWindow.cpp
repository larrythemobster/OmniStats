#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

void RmlUiController::RebuildSettings() {
    float scrollTop = 0.0f;
    if (auto* root = Root("settings-root")) {
        if (auto* page = root->QuerySelector(".settings-page");
            page && page->GetAttribute<int>("data-page", -1) == static_cast<int>(m_settingsPage)) {
            scrollTop = page->GetScrollTop();
        }
    }
    std::ostringstream content;
    switch (m_settingsPage) {
    case SettingsPage::Display:
        content << RenderSettingsGeneral() << RenderSettingsCards();
        break;
    case SettingsPage::Reminders:
        content << RenderSettingsReminders();
        break;
    case SettingsPage::Appearance:
        content << RenderSettingsAppearance();
        break;
    case SettingsPage::Controls:
        content << RenderSettingsShortcuts();
        break;
    case SettingsPage::Accounts:
        content << RenderSettingsIntegrations()
                << SectionStart("Rocket League account")
                << SelectRow("identity", "Local account", "Automatically detected in your first match.", IdentityOptions(), m_config.last_primary_id)
                << SectionEnd()
                << "<div class='settings-disclosure'>"
                << Button("toggle-rank-details", m_showRankDetails ? "Hide rank options" : "Show rank options", "ghost")
                << "</div>";
        if (m_showRankDetails) content << RenderSettingsRanks();
        break;
    case SettingsPage::DataSupport:
        content << RenderSettingsData()
                << SectionStart("Sessions")
                << ToggleControl("reset_session_on_close", "Reset session when OmniStats closes", "Start each launch with a fresh session record.", m_config.reset_session_on_close)
                << ToggleControl("show_session_recap_on_close", "Show a session recap afterwards", "Requires resetting the session on close.", m_config.show_session_recap_on_close, !m_config.reset_session_on_close)
                << SectionEnd()
                << SectionStart("Startup and updates")
                << ToggleControl("run_on_startup", "Run on Windows startup", "", m_config.run_on_startup)
                << ToggleControl("enable_auto_updates", "Automatically install updates", "Installs updates the next time OmniStats starts.", m_config.enable_auto_updates)
                << SectionEnd()
                << "<div class='settings-disclosure'>"
                << Button("toggle-troubleshooting", m_showTroubleshooting ? "Hide troubleshooting" : "Show troubleshooting", "ghost")
                << "</div>";
        if (m_showTroubleshooting) content << RenderSettingsTroubleshooting();
        break;
    case SettingsPage::Count:
        break;
    }

    const float rmlScale = SanitizedScale(m_dpiScale) * SanitizedUiScale(m_config.ui_scale);
    const float logicalWidth = static_cast<float>(m_width) / std::max(rmlScale, 0.01f);
    const bool compactViewport = logicalWidth <= 850.0f;
    const float settingsWidth = compactViewport ? static_cast<float>(m_width) * 0.96f
                                                : std::min(900.0f * rmlScale, static_cast<float>(m_width) * 0.94f);
    const float settingsHeight = compactViewport ? static_cast<float>(m_height) * 0.94f
                                                 : std::min(650.0f * rmlScale, static_cast<float>(m_height) * 0.92f);
    if (!m_settingsPositioned) {
        m_settingsX = std::max(0.0f, (static_cast<float>(m_width) - settingsWidth) * 0.5f);
        m_settingsY = std::max(0.0f, (static_cast<float>(m_height) - settingsHeight) * 0.5f);
    } else {
        m_settingsX = std::clamp(m_settingsX, 0.0f, std::max(0.0f, static_cast<float>(m_width) - settingsWidth));
        m_settingsY = std::clamp(m_settingsY, 0.0f, std::max(0.0f, static_cast<float>(m_height) - settingsHeight));
    }
    const float settingsLeftDp = m_settingsX / std::max(rmlScale, 0.01f);
    const float settingsTopDp = m_settingsY / std::max(rmlScale, 0.01f);

    std::ostringstream out;
    out << "<div class='settings-window' style='left:" << settingsLeftDp
        << "dp;top:" << settingsTopDp << "dp'><div class='settings-header row' data-action='settings-drag'>"
        << "<div class='grow'><div class='brand-title'>Settings</div><div class='label'>Changes apply automatically.</div></div>";
    if (m_state && m_state->ui.updateAvailable.load()) {
        std::string ver;
        {
            std::lock_guard lock(m_state->ui.updateMutex);
            ver = m_state->ui.updateAvailableVersion;
        }
        const bool failed = m_state->ui.updateDownloadFailed.load();
        const bool downloading = m_state->ui.updateDownloading.load();
        std::string label = downloading ? (ver.empty() ? "Starting updater..." : "Starting v" + ver + "...")
                            : failed    ? (ver.empty() ? "Retry update" : "Retry update v" + ver)
                                        : (ver.empty() ? "Update available" : "Update to v" + ver);
        out << Button("update-app", label, "primary");
    }
    out << "</div><div class='settings-body'><div class='settings-nav'>";
    for (int i = 0; i < static_cast<int>(SettingsPage::Count); ++i) {
        const auto page = static_cast<SettingsPage>(i);
        out << "<button class='" << (page == m_settingsPage ? "active" : "") << "' data-action='settings-page' data-page='" << i << "'>" << SettingsPageName(page) << "</button>";
    }
    out << "</div><div class='settings-page' data-page='" << static_cast<int>(m_settingsPage) << "'>" << RenderReleaseBanners() << content.str() << "</div></div><div class='settings-footer'>" << Button("history-open", "History", "ghost") << Button("accounts-open", "Accounts", "ghost") << Button("insights-open", "Insights", "ghost") << Button("help-discord", "Help on Discord", "ghost") << "<div class='grow'></div>" << Button("close-settings", "Done", "primary") << "</div></div>";

    if (m_confirmReplayUploads) {
        out << "<div class='confirm-backdrop'><div class='card privacy-dialog'><div class='card-title'>Before you upload replays</div>"
            << "<div class='setting-help'>Rocket League replay files can include player names, platform IDs, match timestamps, teams, scores, gameplay events, and other participants in the match.</div>"
            << "<div class='setting-help' style='margin-top:8dp'>Only enable this if you understand that replay data leaves your PC and is handled by Ballchasing under its own terms and privacy policy.</div>"
            << "<div class='setting-help' style='margin-top:8dp'>Current visibility: <b>" << Escape(m_config.ballchasing_visibility) << "</b></div>";
        if (m_config.ballchasing_token.empty()) out << "<div class='setting-help loss' style='margin-top:8dp'>Add a Ballchasing API token before uploads can succeed.</div>";
        out << "<div class='row gap-sm' style='margin-top:12dp'>" << Button("confirm-replay-upload", "Enable uploads", "primary") << Button("cancel-replay-upload", "Cancel", "ghost") << "</div></div></div>";
    }
    if (m_confirmDeleteHistory) {
        out << "<div class='confirm-backdrop'><div class='card' style='width:470dp'><div class='card-title loss'>Delete local history and saved identity?</div><div class='setting-help'>This removes local match history and your selected account identity. Settings, service tokens, and tracked accounts in Accounts are kept. This cannot be undone.</div><div class='row gap-sm' style='margin-top:12dp'>" << Button("confirm-delete-history", "Delete", "danger") << Button("cancel-delete-history", "Cancel", "ghost") << "</div></div></div>";
    }
    SetRootRml("settings-root", out.str());
    if (scrollTop > 0.0f && m_document) {
        m_document->UpdateDocument();
        if (auto* root = Root("settings-root")) {
            if (auto* page = root->QuerySelector(".settings-page")) page->SetScrollTop(scrollTop);
        }
    }
}
