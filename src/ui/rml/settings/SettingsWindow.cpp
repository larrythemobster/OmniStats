#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "ui/rml/presenters/ReleaseBanners.hpp"

using namespace RmlUiDetail;

namespace {
    bool SameSettingsElement(Rml::Element* current, Rml::Element* desired) {
        if (current->GetTagName() != desired->GetTagName()) return false;
        for (const char* key : {"id", "data-setting", "data-action"}) {
            if (current->GetAttribute<Rml::String>(key, "") != desired->GetAttribute<Rml::String>(key, "")) return false;
        }
        return true;
    }

    void UpdateSettingsElement(Rml::Element* current, Rml::Element* desired, bool updateAttributes = true) {
        if (auto* text = dynamic_cast<Rml::ElementText*>(current)) {
            auto* newText = dynamic_cast<Rml::ElementText*>(desired);
            if (newText && text->GetText() != newText->GetText()) text->SetText(newText->GetText());
            return;
        }

        if (updateAttributes) {
            Rml::StringList removed;
            for (const auto& [name, value] : current->GetAttributes()) {
                if (!desired->HasAttribute(name) && !(current->GetTagName() == "select" && name == "value")) removed.push_back(name);
            }
            for (const auto& name : removed)
                current->RemoveAttribute(name);
            for (const auto& [name, value] : desired->GetAttributes()) {
                const auto* previous = current->GetAttribute(name);
                if (!previous || *previous != value) current->SetAttribute(name, value);
            }
        }

        if (auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(current)) {
            auto* desiredSelect = static_cast<Rml::ElementFormControlSelect*>(desired);
            const int count = desiredSelect->GetNumOptions();
            bool optionsChanged = current->GetNumChildren() != 0 || select->GetNumOptions() != count;
            int selection = -1;
            for (int i = 0; i < count; ++i) {
                auto* option = desiredSelect->GetOption(i);
                if (option->HasAttribute("selected")) selection = i;
                if (!optionsChanged) {
                    auto* existing = select->GetOption(i);
                    optionsChanged = existing->GetAttribute<Rml::String>("value", "") != option->GetAttribute<Rml::String>("value", "") ||
                                     existing->HasAttribute("disabled") != option->HasAttribute("disabled") ||
                                     existing->GetInnerRML() != option->GetInnerRML();
                }
            }
            if (optionsChanged) {
                select->RemoveAll();
                while (auto* child = current->GetFirstChild())
                    current->RemoveChild(child);
                for (int i = 0; i < count; ++i)
                    select->Add(desiredSelect->GetOption(i)->Clone());
            }
            if (selection >= 0 && select->GetSelection() != selection) select->SetSelection(selection);
            return;
        }

        int index = 0;
        while (index < desired->GetNumChildren()) {
            auto* next = desired->GetChild(index);
            auto* child = index < current->GetNumChildren() ? current->GetChild(index) : nullptr;
            if (!child) {
                current->AppendChild(next->Clone());
                ++index;
                continue;
            }
            if (!SameSettingsElement(child, next)) {
                current->ReplaceChild(next->Clone(), child);
                ++index;
                continue;
            }
            UpdateSettingsElement(child, next);
            ++index;
        }
        while (current->GetNumChildren() > desired->GetNumChildren()) {
            current->RemoveChild(current->GetLastChild());
        }
    }
}

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
    case SettingsPage::Accounts: {
        const std::string activeId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
        std::string activeName = "Auto-detect";
        for (const auto& option : IdentityOptions())
            if (option.value == activeId) activeName = option.label;
        content << RenderSettingsIntegrations()
                << SectionStart("Rocket League account")
                << ActionRow("Active account", activeName, Button("accounts-open", "Choose account", "ghost"))
                << SectionEnd()
                << "<div class='settings-disclosure'>"
                << Button("toggle-rank-details", m_showRankDetails ? "Hide rank options" : "Show rank options", "ghost")
                << "</div>";
        if (m_showRankDetails) content << RenderSettingsRanks();
        break;
    }
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
    out << "</div><div class='settings-page' data-page='" << static_cast<int>(m_settingsPage) << "'>" << RenderReleaseBanners(m_state, m_remoteConfig.get()) << content.str() << "</div></div><div class='settings-footer'>" << Button("history-open", "History", "ghost") << Button("accounts-open", "Accounts", "ghost") << Button("insights-open", "Insights", "ghost") << Button("help-discord", "Help on Discord", "ghost") << "<div class='grow'></div>" << Button("close-settings", "Done", "primary") << "</div></div>";

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
    auto* root = Root("settings-root");
    if (!root || !m_document) return;
    if (!root->GetFirstChild()) {
        SetRootRml("settings-root", out.str());
    } else {
        auto desired = m_document->CreateElement("div");
        desired->SetInnerRML(out.str());
        const bool wasRebuilding = m_rebuildingUi;
        m_rebuildingUi = true;
        m_systemInterface.BeginCursorUpdate();
        UpdateSettingsElement(root, desired.get(), false);
        if (m_hasPointerPosition && m_context) {
            m_context->Update();
            m_context->ProcessMouseMove(m_lastPointerX, m_lastPointerY, 0);
        }
        m_systemInterface.EndCursorUpdate();
        m_rebuildingUi = wasRebuilding;
        m_renderDirty = true;
    }
    m_document->UpdateDocument();
    if (auto* page = root->QuerySelector(".settings-page")) page->SetScrollTop(scrollTop);
}
