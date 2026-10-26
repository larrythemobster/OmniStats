#include "ui/rml/presenters/DashboardPresenter.hpp"
#include <sstream>
#include "core/config/AppVersion.hpp"
#include "network/api/RemoteConfig.hpp"
#include "network/omninet/OmniNet.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
std::string DashboardPresenter::RenderReleaseBanners(const std::shared_ptr<SessionState>& state,
                                                     const RemoteConfig* remoteConfig) const {
    const RemoteConfigData remoteSnap = remoteConfig ? remoteConfig->GetSnapshot() : RemoteConfigData{};
    const bool belowMin = RemoteConfig::IsVersionBelowMinimum(AppVersion::Current, remoteSnap.min_supported_version);
    const bool wireUpdateRequired = OmniNet::UpdateRequired();
    const auto announcements = remoteConfig ? remoteConfig->ActiveAnnouncements() : std::vector<RemoteAnnouncement>{};
    if (!belowMin && !wireUpdateRequired && announcements.empty()) {
        return {};
    }

    bool updateDownloading = false;
    bool updateFailed = false;
    if (state) {
        updateDownloading = state->ui.updateDownloading.load();
        updateFailed = state->ui.updateDownloadFailed.load();
    }

    std::ostringstream out;
    out << "<div class='release-banners'>";
    if (wireUpdateRequired || belowMin) {
        const std::string title = wireUpdateRequired
                                      ? "Update required"
                                      : ("Update recommended · v" + RmlUiDetail::Escape(remoteSnap.min_supported_version) + "+");
        const std::string msg = wireUpdateRequired
                                    ? "Update OmniStats to keep using online features."
                                    : (remoteSnap.min_version_message.empty()
                                           ? ("OmniStats v" + remoteSnap.min_supported_version + " or newer is recommended.")
                                           : remoteSnap.min_version_message);
        const std::string updateLabel = updateDownloading ? "Starting updater..."
                                        : updateFailed    ? "Retry update"
                                                          : "Update now";
        out << "<div class='release-banner severity-warning min-version-banner'>"
            << "<div class='release-banner-body'>"
            << "<div class='release-banner-title'>" << RmlUiDetail::Escape(title) << "</div>"
            << "<div class='release-banner-text'>" << RmlUiDetail::Escape(msg) << "</div>"
            << "</div><div class='release-banner-actions'>"
            << RmlUiDetail::Button("update-app", updateLabel, "primary compact")
            << "</div></div>";
    }

    for (const auto& ann : announcements) {
        out << "<div class='release-banner severity-" << RmlUiDetail::Escape(ann.severity)
            << "' data-announcement-id='" << RmlUiDetail::Escape(ann.id) << "'>"
            << "<div class='release-banner-body'>"
            << "<div class='release-banner-title'>" << RmlUiDetail::Escape(ann.title) << "</div>";
        if (!ann.body.empty()) {
            out << "<div class='release-banner-text'>" << RmlUiDetail::Escape(ann.body) << "</div>";
        }
        out << "</div><div class='release-banner-actions'>";
        if (!ann.link_url.empty() && RemoteConfig::IsAllowedAnnouncementLinkUrl(ann.link_url)) {
            out << "<button class='ghost compact' data-action='open-announcement-link' data-url='"
                << RmlUiDetail::Escape(ann.link_url) << "'>Learn more</button>";
        }
        if (ann.dismissible) {
            out << "<button class='ghost compact' data-action='dismiss-announcement' data-announcement-id='"
                << RmlUiDetail::Escape(ann.id) << "'>Dismiss</button>";
        }
        out << "</div></div>";
    }
    out << "</div>";
    return out.str();
}
