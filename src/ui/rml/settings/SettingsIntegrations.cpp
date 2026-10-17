#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

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
    const bool signedIn = accountStatus.state == AccountAuthState::SignedIn;
    out << SectionStart("Live ranks");
    if (signedIn) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name win'>Connected as " << Escape(accountStatus.displayName) << "</div>"
            << "<div class='setting-help'>"
            << (m_config.custom_api_enabled ? "Lobby ranks come from your OmniStats account."
                                            : "OmniStats ranks are turned off for this PC.")
            << "</div></div><div class='row gap-xs'>"
            << "<button class='ghost' data-action='account-manage-devices'>Manage devices</button>"
            << "<button class='ghost' data-action='account-sign-out'>Sign out</button>"
            << "</div></div>";
        out << ToggleControl("custom_api_enabled", "Use OmniStats ranks", "Look up lobby ranks with your OmniStats account.",
                             m_config.custom_api_enabled);
    } else if (accountStatus.state == AccountAuthState::Migrating) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Upgrading account connection...</div>"
            << "<div class='setting-help'>Linking this PC using your saved account credentials.</div>"
            << "</div><div class='row gap-xs'>"
            << "<button class='ghost' disabled='disabled'>Connecting...</button>"
            << "</div></div>";
    } else if (accountStatus.state == AccountAuthState::AwaitingApproval) {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Finish connecting in your browser</div>"
            << "<div class='setting-help'>Sign in or create an account, then approve this PC. This page updates automatically.</div>"
            << "</div><div class='row gap-xs'>"
            << "<button class='primary' data-action='account-open-verify'>Open browser again</button>"
            << "<button class='ghost' data-action='account-cancel'>Cancel</button>"
            << "</div></div>"
            << "<button class='ghost compact' data-action='account-toggle-fallback'>Browser didn't open?</button>";
        if (m_onboarding.ShowFallback()) {
            out << "<div class='account-code-card'>"
                << "<div class='setting-help'>Paste the sign-in link into your browser. It already includes this code.</div>"
                << "<div class='label'>Verification code</div>"
                << "<div class='account-user-code mono'>" << Escape(accountStatus.userCode) << "</div>"
                << "<div class='row gap-xs center' style='margin-top:8dp'>"
                << "<button class='ghost compact' data-action='account-copy-code'>Copy code</button>"
                << "<button class='primary compact' data-action='account-copy-link'>Copy sign-in link</button>"
                << "</div></div>";
        }
    } else {
        out << "<div class='setting-row'><div class='setting-info'>"
            << "<div class='setting-name'>Connect live ranks</div>"
            << "<div class='setting-help'>Sign in to OmniStats to see player ranks and MMR. Your match history stays on this PC.</div>"
            << "<div class='setting-help'>Use an OmniStats account, not your Epic Games or Rocket League login.</div>";
        if (accountStatus.state == AccountAuthState::Error && !accountStatus.errorMessage.empty()) {
            out << "<div class='setting-help loss'>" << Escape(accountStatus.errorMessage) << "</div>";
        }
        out << "</div><div class='row gap-xs'>"
            << "<button class='primary' data-action='account-sign-in'>Sign in with OmniStats</button>";
        if (!m_config.custom_api_key.empty()) {
            out << "<button class='ghost' data-action='account-retry-migration'>Retry migration</button>";
        }
        out << "</div></div>";
    }
    const bool omniRanks = signedIn && m_config.custom_api_enabled;
    out << ToggleControl("enable_mmr_tracking", "Tracker Network rank lookup",
                         omniRanks ? "Fills in players OmniStats can't find. Sends lobby player names and platform IDs to Tracker Network."
                                   : "Sends lobby player names and platform IDs to Tracker Network to look up public ranks.",
                         m_config.enable_mmr_tracking);
    out << SectionEnd();
    out << SectionStart("Ballchasing uploads")
        << "<div class='setting-help section-help'>Upload saved replay files to your Ballchasing account. Get an API token from ballchasing.com.</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API token</div></div><input type='" << (m_showBallchasingToken ? "text" : "password") << "' class='text' data-setting='ballchasing_token' value='" << Escape(m_pendingBallchasingToken) << "'/><button class='ghost' data-action='toggle-token'>" << (m_showBallchasingToken ? "Hide" : "Show") << "</button></div>";
    if (m_config.auto_upload_replays && m_config.ballchasing_token.empty()) {
        out << "<div class='setting-help loss'>Add an API token to start uploading replays.</div>";
    } else if (m_config.auto_upload_replays && m_state && m_state->ui.ballchasingTokenRejected.load()) {
        out << "<div class='setting-help loss'>Ballchasing rejected this token. Uploads are paused until you enter a valid one.</div>";
    }

    out << ToggleControl("auto_upload_replays", "Auto-upload new replays", "Sends each saved replay to Ballchasing.", m_config.auto_upload_replays);
    if (m_config.auto_upload_replays) {
        out << SelectRow("ballchasing_visibility", "Upload visibility", "", {{"private", "Private"}, {"unlisted", "Unlisted"}, {"public", "Public"}}, m_config.ballchasing_visibility)
            << ToggleControl("ballchasing_filter_ranked_only", "Ranked matches only", "", m_config.ballchasing_filter_ranked_only)
            << ToggleControl("ballchasing_filter_wins_only", "Wins only", "", m_config.ballchasing_filter_wins_only);
    }

    if (m_state) {
        ReplayUploadStatus status;
        {
            std::lock_guard lock(m_state->ui.replayUploadMutex);
            status = m_state->ui.replayUploadStatus;
        }
        if (m_config.auto_upload_replays || status.uploadedToday > 0 || status.retrying > 0 || status.failed > 0) {
            out << "<div class='setting-row'><div class='setting-info'>"
                << "<div class='setting-name'>Upload queue</div>"
                << "<div class='setting-help'>" << status.uploadedToday << " uploaded today · "
                << status.retrying << " retrying · "
                << status.failed << " failed</div></div>";
            if (status.failed > 0) {
                out << "<button class='ghost' data-action='retry-failed-uploads'>Retry failed</button>";
            }
            out << "</div>";
        }
    }
    out << SectionEnd();
    out << SectionStart("Replay saving")
        << ToggleControl("auto_save_replays", "Auto-save replays", "Presses your Rocket League save-replay key for you. Nothing is injected into the game.", m_config.auto_save_replays);
    if (m_config.auto_save_replays) {
        const bool active = m_bindCaptureTarget == BindCaptureTarget::KeySaveReplay;
        const std::string bindTarget = std::to_string(static_cast<int>(BindCaptureTarget::KeySaveReplay));
        out << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Save replay key</div>"
            << "<div class='setting-help'>Matches the Save Replay bind configured in Rocket League.</div></div><div class='row gap-xs'>"
            << "<button data-action='capture-bind' data-bind='" << bindTarget << "'>"
            << (active ? "Press a key..." : Escape(GetKeyDisplayName(m_config.key_save_replay))) << "</button>"
            << "<button class='ghost' data-action='" << (active ? "cancel-bind" : "clear-bind")
            << "' data-bind='" << bindTarget << "'>" << (active ? "Cancel" : "Clear") << "</button></div></div>";
    }
    out << SectionEnd();
    out << SectionStart("Discord")
        << ToggleControl("discord_rpc_enabled", "Discord Rich Presence", "Shows your current match on your Discord profile. Takes effect after you restart OmniStats.", m_config.discord_rpc_enabled)
        << SectionEnd();
    return out.str();
}
