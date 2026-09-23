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
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string RmlUiController::RenderSettingsIntegrations() {
    if (m_pendingBallchasingToken.empty() && !m_config.ballchasing_token.empty()) m_pendingBallchasingToken = m_config.ballchasing_token;
    std::ostringstream out;
    out << SectionStart("Custom Rank API")
        << "<div class='setting-help'>Authenticate custom API rank lookups with the API key from your OmniStats account.</div>"
        << ToggleControl("custom_api_enabled", "Enable custom API fallback", "Used when Tracker.gg rank requests are blocked or unavailable.", m_config.custom_api_enabled)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API Key</div></div><input type='password' class='text' data-setting='custom_api_key' value='" << Escape(m_config.custom_api_key) << "'/></div>";
    if (m_config.custom_api_key.empty())
        out << "<div class='setting-help'>No key set. Sign in on the website and copy your API key from account settings.</div>";
    else
        out << "<div class='setting-help win'>API key saved and active.</div>";
    out << SectionEnd();
    out << SectionStart("Ballchasing Uploader")
        << "<div class='setting-help'>Upload saved replay files to your Ballchasing account. Add your API token from Ballchasing.com.</div>"
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>API token</div></div><input type='" << (m_showBallchasingToken ? "text" : "password") << "' class='text' data-setting='ballchasing_token' value='" << Escape(m_pendingBallchasingToken) << "'/><button class='ghost' data-action='toggle-token'>" << (m_showBallchasingToken ? "Hide" : "Show") << "</button></div>";
    if (m_config.ballchasing_token.empty()) out << "<div class='setting-help loss'>An API token is required before replay uploads can succeed.</div>";
    out
        << ToggleControl("auto_upload_replays", "Auto-upload new replays", "Uploads saved replay files using the selected privacy level.", m_config.auto_upload_replays)
        << "<div class='setting-row'><div class='setting-info'><div class='setting-name'>Upload visibility</div></div><select data-setting='ballchasing_visibility'>"
        << "<option value='private'" << Selected(m_config.ballchasing_visibility == "private") << ">private</option><option value='unlisted'" << Selected(m_config.ballchasing_visibility == "unlisted") << ">unlisted</option><option value='public'" << Selected(m_config.ballchasing_visibility == "public") << ">public</option></select></div>" << SectionEnd();
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
