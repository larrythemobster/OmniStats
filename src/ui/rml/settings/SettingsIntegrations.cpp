#include "ui/rml/RmlUiController.hpp"
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

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "network/AccountClient.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

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
            << "<div class='setting-name'>Not signed in</div>"
            << "<div class='setting-help'>Sign in with OmniStats for one-click browser approval and verified rank lookups.</div>";
        if (accountStatus.state == AccountAuthState::Error && !accountStatus.errorMessage.empty()) {
            out << "<div class='setting-help loss'>" << Escape(accountStatus.errorMessage) << "</div>";
        }
        out << "</div><div class='row gap-xs'>"
            << "<button class='primary' data-action='account-sign-in'>Sign in with OmniStats</button>"
            << "<button class='ghost' data-action='account-manage-devices'>Manage devices</button>"
            << "</div></div>";
    }
    out << ToggleControl("custom_api_enabled",
                         "Enable OmniStats rank API",
                         "Primary rank source for all players when signed in; used as a fallback when an API key is configured.",
                         m_config.custom_api_enabled)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Advanced: use API key instead</div>"
        << "<div class='setting-help'>Paste an API key from your OmniStats account instead of signing in. Keys you already set up keep working.</div></div>"
        << "<button class='ghost' data-action='toggle-advanced-api-key'>" << (m_showAdvancedApiKey ? "Hide" : "Show") << "</button></div>";
    if (m_showAdvancedApiKey) {
        out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API Key</div></div>"
            << "<input type='password' class='text' data-setting='custom_api_key' value='" << Escape(m_config.custom_api_key) << "'/></div>";
        if (m_config.custom_api_key.empty())
            out << "<div class='setting-help'>No key set. Sign in on the website and copy your API key from account settings.</div>";
        else if (m_state && m_state->ui.customApiKeyRejected.load())
            out << "<div class='setting-help loss'>This API key was rejected. Custom API lookups are paused until you enter a valid key from your account settings.</div>";
        else
            out << "<div class='setting-help win'>API key saved and active.</div>";
    }
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
