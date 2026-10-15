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
#include "network/api/AccountClient.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string RmlUiController::RenderSettingsIntegrations() {
    if (m_pendingBallchasingToken.empty() && !m_config.ballchasing_token.empty()) m_pendingBallchasingToken = m_config.ballchasing_token;
    std::ostringstream out;
    const AccountStatusSnapshot accountStatus = AccountClient::Instance().GetStatus();
    out << SectionStart("Account");
    if (accountStatus.state == AccountAuthState::SignedIn) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name win'>Signed in as " << Escape(accountStatus.displayName)
            << " · Ranks: " << (m_config.custom_api_enabled ? "OmniStats" : "Off") << "</div>"
            << "<div class='setting-help'>OmniStats rank API is your primary rank source for all lobby players.</div>"
            << "</div><div class='row gap-xs'>"
            << "<button class='ghost' data-action='account-manage-devices'>Manage devices</button>"
            << "<button class='ghost' data-action='account-sign-out'>Sign out</button>"
            << "</div></div>";
    } else if (accountStatus.state == AccountAuthState::Migrating) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Upgrading account connection...</div>"
            << "<div class='setting-help'>Connecting this device using your saved credentials. Live ranks will be available shortly.</div>"
            << "</div><div class='row gap-xs'>"
            << "<button class='ghost' disabled='disabled'>Connecting...</button>"
            << "</div></div>";
    } else if (accountStatus.state == AccountAuthState::AwaitingApproval) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Waiting for browser approval...</div>"
            << "<div class='setting-help'>Confirm this code in your browser to finish signing in.</div>"
            << "</div><div class='row gap-xs'>"
            << "<button class='primary' data-action='account-open-verify'>Open browser</button>"
            << "<button class='ghost' data-action='account-cancel'>Cancel</button>"
            << "</div></div>"
            << "<div class='account-code-card'><div class='label'>Verification code</div>"
            << "<div class='account-user-code mono'>" << Escape(accountStatus.userCode) << "</div></div>";
    } else {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Link this device</div>"
            << "<div class='setting-help'>Link your OmniStats account to use live ranks. A browser page opens for approval.</div>";
        if (accountStatus.state == AccountAuthState::Error && !accountStatus.errorMessage.empty()) {
            out << "<div class='setting-help loss'>" << Escape(accountStatus.errorMessage) << "</div>";
        }
        out << "</div><div class='row gap-xs'>"
            << "<button class='primary' data-action='account-sign-in'>Sign in with OmniStats</button>";
        if (!m_config.custom_api_key.empty()) {
            out << "<button class='ghost' data-action='account-retry-migration'>Retry migration</button>";
        }
        out << "<button class='ghost' data-action='account-manage-devices'>Manage devices</button>"
            << "</div></div>";
    }
    out << ToggleControl("custom_api_enabled",
                         "Enable OmniStats rank API",
                         "Uses your linked OmniStats account for live ranks.",
                         m_config.custom_api_enabled);
    out << SectionEnd();
    out << SectionStart("Ballchasing Uploader")
        << "<div class='setting-help'>Upload saved replay files to your Ballchasing account. Add your API token from Ballchasing.com.</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API token</div></div><input type='" << (m_showBallchasingToken ? "text" : "password") << "' class='text' data-setting='ballchasing_token' value='" << Escape(m_pendingBallchasingToken) << "'/><button class='ghost' data-action='toggle-token'>" << (m_showBallchasingToken ? "Hide" : "Show") << "</button></div>";
    if (m_config.ballchasing_token.empty()) {
        out << "<div class='setting-help loss'>An API token is required before replay uploads can succeed.</div>";
    } else if (m_state && m_state->ui.ballchasingTokenRejected.load()) {
        out << "<div class='setting-help loss'>The Ballchasing API token was rejected (401/403). Uploads are paused until a valid token is entered.</div>";
    }

    if (m_state) {
        ReplayUploadStatus status;
        {
            std::lock_guard lock(m_state->ui.replayUploadMutex);
            status = m_state->ui.replayUploadStatus;
        }
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Queue status</div>"
            << "<div class='setting-help'>" << status.uploadedToday << " uploaded today · "
            << status.retrying << " retrying · "
            << status.failed << " failed</div></div>";
        if (status.failed > 0) {
            out << "<button class='ghost' data-action='retry-failed-uploads'>Retry failed</button>";
        }
        out << "</div>";
    }

    out
        << ToggleControl("auto_upload_replays", "Auto-upload new replays", "Uploads saved replay files using the selected privacy level.", m_config.auto_upload_replays)
        << SelectRow("ballchasing_visibility", "Upload visibility", "", {{"private", "private"}, {"unlisted", "unlisted"}, {"public", "public"}}, m_config.ballchasing_visibility)
        << ToggleControl("ballchasing_filter_ranked_only", "Upload ranked only", "Only upload replays from competitive matches.", m_config.ballchasing_filter_ranked_only)
        << ToggleControl("ballchasing_filter_wins_only", "Upload wins only", "Only upload replays from matches you won.", m_config.ballchasing_filter_wins_only)
        << SectionEnd();
    out << SectionStart("Replay Saving")
        << ToggleControl("auto_save_replays", "Auto-save replays", "EAC-friendly: triggers your configured Rocket League save-replay key.", m_config.auto_save_replays);
    if (m_config.auto_save_replays) {
        const bool active = m_bindCaptureTarget == BindCaptureTarget::KeySaveReplay;
        const std::string bindTarget = std::to_string(static_cast<int>(BindCaptureTarget::KeySaveReplay));
        out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Save Replay Bind</div>"
            << "<div class='setting-help'>Matches the Save Replay bind configured in Rocket League.</div></div><div class='row gap-xs'>"
            << "<button data-action='capture-bind' data-bind='" << bindTarget << "'>"
            << (active ? "Press a key..." : Escape(GetKeyDisplayName(m_config.key_save_replay))) << "</button>"
            << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind")
            << "' data-bind='" << bindTarget << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
    }
    out << SectionEnd();
    out << SectionStart("External Services")
        << ToggleControl("discord_rpc_enabled", "Discord Rich Presence", "Changing this may require an app restart.", m_config.discord_rpc_enabled)
        << ToggleControl("enable_mmr_tracking", "Enable MMR tracking (Tracker.gg)", "Sends lobby identifiers to Tracker.gg for rank lookup.", m_config.enable_mmr_tracking)
        << SectionEnd();
    return out.str();
}
