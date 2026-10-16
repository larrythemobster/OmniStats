#include "ui/config/WidgetSettings.hpp"
#include "ui/rml/widgets/TournamentWidget.hpp"
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
#include "network/api/AccountClient.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/api/RemoteConfig.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

bool RmlUiController::RankSourceAvailable() const {
    return m_config.enable_mmr_tracking || (m_config.custom_api_enabled && AccountClient::Instance().IsSignedIn());
}

std::string RmlUiController::RenderRankSourceHint(bool interactive) const {
    if (RankSourceAvailable()) return {};
    std::string html = "<div class='rank-source-hint row'><div class='grow'>Ranks are off. Connect live ranks in Settings &#x203A; Accounts &amp; services.</div>";
    if (interactive) html += "<button class='primary compact' data-action='open-rank-settings'>Connect</button>";
    return html + "</div>";
}

std::string RmlUiController::RenderWidget(DashboardLayout::WidgetId id, bool dashboard) {
    const auto controlRow = [](const char* label, const std::string& select) {
        return std::string("<div class='row widget-controls'><span class='label grow'>") + label + "</span>" + select + "</div>";
    };

    switch (id) {
    case DashboardLayout::WidgetId::LiveRoster: {
        const auto rosterCategory = m_state ? m_state->ui.rosterMmrCategory.load() : MmrCategory::Best;
        std::ostringstream html;
        if (dashboard) {
            html << controlRow("Rank view", SelectControl("mmr_category", MmrCategoryOptions(true, m_config.show_extra_playlists), MmrCategoryToString(rosterCategory), "compact-select"));
        } else {
            const std::string categoryName = MmrLabel(rosterCategory);
            const std::string playlistImage = PlaylistImageForName(MmrCategoryToString(rosterCategory));
            html << "<div class='roster-header'><div class='row'><div class='brand-mini grow'>OMNISTATS</div><div class='row gap-xs'>";
            if (m_config.use_rank_icons && !playlistImage.empty()) html << "<img class='playlist-icon' src='" << playlistImage << "'/>";
            html << "<span class='badge'>MMR · " << Escape(categoryName) << "</span></div></div>"
                 << "<div class='label'>{{roster_arena}}</div></div>";
        }
        html << RenderRankSourceHint(dashboard) << RenderPlayerRoster(0, "BLUE") << RenderPlayerRoster(1, "ORANGE");
        return html.str();
    }
    case DashboardLayout::WidgetId::LiveMatchStats:
        return RenderLiveMatchStats();
    case DashboardLayout::WidgetId::SessionStats:
        return RenderSessionStats(true, true);
    case DashboardLayout::WidgetId::MmrGraph: {
        std::string controls;
        if (dashboard) {
            const auto graphCategory = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
            const std::string lifetimeBadge = m_snap.showLifetimeGraph ? "<span class='badge graph-mode-badge'>Lifetime</span>" : "";
            controls = controlRow("Playlist", lifetimeBadge + SelectControl("graph_mmr_category", MmrCategoryOptions(false, m_config.show_extra_playlists), MmrCategoryToString(graphCategory), "compact-select graph-select") +
                                                  RenderGraphPanControls());
        }
        // On the dashboard the controls row names the playlist and holds the pan buttons.
        return controls + RenderMmrGraph(!dashboard, !dashboard);
    }
    case DashboardLayout::WidgetId::StreaksStats:
        return RenderStreaksStats();
    case DashboardLayout::WidgetId::GamemodeBreakdown: {
        std::string controls;
        if (dashboard) {
            controls = controlRow("Scope", SelectControl("gamemode_breakdown_scope", GamemodeScopeOptions(), GamemodeScopeValue(m_config), "compact-select"));
        }
        return controls + RenderGamemodeBreakdown(ScopeFromConfigString(m_config.gamemode_breakdown_scope));
    }
    case DashboardLayout::WidgetId::LobbyRanks:
        return RenderRankSourceHint(dashboard) + RenderLobbyRanks();
    case DashboardLayout::WidgetId::DemoTracker:
        return RenderDemoTracker();
    case DashboardLayout::WidgetId::PreviousGames:
        return RenderPreviousGames(!dashboard);
    case DashboardLayout::WidgetId::RankProgress:
        // The overlay keeps the bare bar so it reads as part of the card it is docked to.
        return RenderSelfRankProgress(dashboard);
    case DashboardLayout::WidgetId::Tournaments:
        return TournamentUi::RenderWidget(*Tournaments::TournamentScheduleService::Instance().Snapshot(), static_cast<int64_t>(std::time(nullptr)));
    }
    return {};
}

std::string RmlUiController::RenderOverlayContainer(const OverlayLayout::ContainerConfig& container, bool editMode) {
    const bool settingsOpen = m_state && m_state->ui.showMenu.load();
    const bool expanded = m_state && m_state->ui.h2hExpanded.load();
    const int64_t nowMs = SteadyNowMs();
    const OverlayLayout::VisibilityState visState =
        m_state ? OverlayLayout::SnapshotVisibilityState(*m_state, editMode)
                : OverlayLayout::VisibilityState{.editMode = editMode};
    const bool replayHidden = container.visibility.hideDuringReplay && visState.inReplay;
    auto visible = [&](DashboardLayout::WidgetId widget) {
        if (editMode) return true;
        if (!WidgetSettings::Enabled(m_config, widget, false)) return false;
        if (widget == DashboardLayout::WidgetId::Tournaments && (!m_config.show_tournaments || !AccountClient::Instance().IsSignedIn())) return false;
        if (settingsOpen) return true;

        const auto vis = OverlayLayout::GetWidgetVisibility(m_config.overlay_layout, widget);
        const bool widgetVisible = OverlayLayout::IsVisibilityActive(vis, visState, nowMs);

        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
        case DashboardLayout::WidgetId::MmrGraph:
        case DashboardLayout::WidgetId::SessionStats:
        case DashboardLayout::WidgetId::Tournaments:
            return widgetVisible;
        case DashboardLayout::WidgetId::LiveMatchStats:
            return widgetVisible && expanded;
        case DashboardLayout::WidgetId::StreaksStats:
            return widgetVisible && m_config.show_streaks_stats;
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            return widgetVisible && m_config.show_gamemode_breakdown;
        case DashboardLayout::WidgetId::LobbyRanks:
            return widgetVisible && m_config.show_lobby_ranks_overlay;
        case DashboardLayout::WidgetId::DemoTracker:
            if (!m_config.show_demo_tracker_overlay) return false;
            if (replayHidden || (vis.hideDuringReplay && visState.inReplay)) return false;
            if (vis.mode == OverlayLayout::Visibility::KeyHeld || container.visibility.mode == OverlayLayout::Visibility::KeyHeld) return m_snap.inMatch;
            return widgetVisible;
        case DashboardLayout::WidgetId::PreviousGames:
            return widgetVisible && m_config.show_previous_games_summary;
        case DashboardLayout::WidgetId::RankProgress:
            return widgetVisible && m_config.show_rank_progress;
        }
        return false;
    };
    std::vector<DashboardLayout::WidgetId> widgets;
    for (auto widget : container.widgets)
        if (visible(widget)) widgets.push_back(widget);
    if (widgets.empty()) return {};

    OverlayLayout::ContainerConfig visibleContainer = container;
    visibleContainer.widgets = widgets;
    auto [minW, minH] = OverlayContainerMinSize(visibleContainer, m_dpiScale, &m_config);
    auto [w, h] = OverlayContainerSize(visibleContainer, m_dpiScale, &m_config);

    const float dpi = SanitizedScale(m_dpiScale);
    const float rmlScale = dpi * SanitizedUiScale(m_config.ui_scale);

    // Lobby Ranks is a fixed-column compact table. Protect enough horizontal
    // room for the identity column and every enabled playlist so flex layout
    // never has to collapse the player's name or overlap rank/MMR text.
    if (std::find(widgets.begin(), widgets.end(), DashboardLayout::WidgetId::LobbyRanks) != widgets.end()) {
        const float lobbyContentMinDp = LobbyRanksContentMinDp(m_config);
        minW = std::max(minW, lobbyContentMinDp * rmlScale);
        w = std::max(w, minW);
    }

    // The legacy layout manager persisted explicit undersized dimensions after
    // clamping them. Keep that compatibility so old/corrupt layouts repair
    // themselves once instead of remaining invalid in Config forever. Stored
    // geometry is design pixels, so the rendered minimum converts back first.
    const float uiScale = SanitizedUiScale(m_config.ui_scale);
    const float minWStored = minW / uiScale;
    const float minHStored = minH / uiScale;
    const bool clampStoredWidth = container.w > 1.0f && container.w < minWStored;
    const bool clampStoredHeight = container.h > 1.0f && container.h < minHStored;
    if (clampStoredWidth || clampStoredHeight) {
        const std::string containerId = container.id;
        Config::Update([containerId, clampStoredWidth, clampStoredHeight, minWStored, minHStored](ConfigData& c) {
            for (auto& stored : c.overlay_layout.containers) {
                if (stored.id != containerId) continue;
                if (clampStoredWidth) stored.w = minWStored;
                if (clampStoredHeight) stored.h = minHStored;
                break;
            }
        },
                       true);
    }

    // Changing the app text size rescales every card, so a position saved at one
    // scale can push the card past the screen edge at another. Keep the whole
    // card on screen horizontally, and at least its minimum height vertically -
    // cards auto-size their height during play, so clamping against the (taller)
    // edit-mode height would shove them up for no reason.
    float x = container.x;
    if (x + w > static_cast<float>(m_width)) x = std::max(12.0f * dpi, static_cast<float>(m_width) - w - 18.0f * dpi);
    float y = container.y;
    const float visibleHeight = std::min(h, minH);
    if (y + visibleHeight > static_cast<float>(m_height)) y = std::max(0.0f, static_cast<float>(m_height) - visibleHeight);
    const auto toDp = [rmlScale](float pixels) { return pixels / std::max(rmlScale, 0.5f); };

    std::ostringstream out;
    out << "<div class='card overlay-card";
    if (settingsOpen || editMode) out << " interactive";
    if (editMode) out << " overlay-edit";
    out << "' data-container='" << Escape(container.id) << "'";
    if (settingsOpen || editMode) out << " data-action='overlay-drag'";
    out << " style='left:" << toDp(x) << "dp;top:" << toDp(y) << "dp;width:" << toDp(w) << "dp;'>";
    for (size_t i = 0; i < widgets.size(); ++i) {
        const auto widget = widgets[i];
        if (widgets.size() > 1 && widget != DashboardLayout::WidgetId::PreviousGames && widget != DashboardLayout::WidgetId::RankProgress && widget != DashboardLayout::WidgetId::Tournaments) {
            out << "<div class='overlay-widget-title' style='margin-top:" << (i ? 8 : 2) << "dp'>" << Escape(DashboardLayout::GetWidgetDisplayName(widget)) << "</div>";
        }
        out << "<div class='live-widget' data-live-widget='" << WidgetDomId(widget)
            << "' data-live-surface='overlay'>" << RenderWidget(widget, false) << "</div>";
        // The roster's key hints close the roster block, below a progress bar docked under it.
        const bool rosterThenProgress = widget == DashboardLayout::WidgetId::LiveRoster && i + 1 < widgets.size() &&
                                        widgets[i + 1] == DashboardLayout::WidgetId::RankProgress;
        const bool progressAfterRoster =
            widget == DashboardLayout::WidgetId::RankProgress && i > 0 && widgets[i - 1] == DashboardLayout::WidgetId::LiveRoster;
        if ((widget == DashboardLayout::WidgetId::LiveRoster && !rosterThenProgress) || progressAfterRoster) out << RenderRosterFooter();
    }
    if (editMode) out << "<div class='overlay-resize' data-action='overlay-resize' data-container='" << Escape(container.id) << "'></div>";
    out << "</div>";
    return out.str();
}

std::string RmlUiController::RenderRosterFooter() const {
    const std::string cycle = m_config.key_cycle > 0 ? GetKeyDisplayName(m_config.key_cycle) : "Unbound";
    const std::string expand = m_config.key_expand > 0 ? GetKeyDisplayName(m_config.key_expand) : "Unbound";
    const std::string session = m_config.key_session > 0 ? GetKeyDisplayName(m_config.key_session) : "Unbound";
    return "<div class='roster-footer'><span><b>" + Escape(cycle) + "</b> cycle MMR</span><span><b>" + Escape(expand) + "</b> " +
           (m_state && m_state->ui.h2hExpanded.load() ? "shrink" : "expand") + "</span><span><b>" + Escape(session) + "</b> session</span></div>";
}

std::string RmlUiController::RenderOverlayToolbox(bool editMode) {
    if (!editMode) return {};

    std::ostringstream out;
    out << "<button class='overlay-toolbox-toggle' data-action='overlay-toggle-toolbox' title='Toggle overlay toolbox'>Widgets</button>";
    if (!m_config.overlay_layout.toolboxOpen) return out.str();

    std::set<DashboardLayout::WidgetId> present;
    for (const auto& c : m_config.overlay_layout.containers)
        for (auto w : c.widgets)
            present.insert(w);
    out << "<div class='card overlay-toolbox'><div class='row'><div class='card-title grow'>Overlay layout</div>"
        << "<button class='window-control' data-action='overlay-close-toolbox'>×</button></div>"
        << "<div class='label'>Click to add to the center, or drag onto the overlay to place it.</div>"
        << "<div class='row gap-sm' style='margin-top:8dp'>" << Button("reset-overlay", "Reset", "ghost") << "</div>"
        << "<div class='col gap-xs' style='margin-top:8dp'>";
    const std::array<DashboardLayout::WidgetId, 11> all = {DashboardLayout::WidgetId::LiveRoster, DashboardLayout::WidgetId::LiveMatchStats, DashboardLayout::WidgetId::SessionStats, DashboardLayout::WidgetId::RankProgress, DashboardLayout::WidgetId::MmrGraph, DashboardLayout::WidgetId::StreaksStats, DashboardLayout::WidgetId::GamemodeBreakdown, DashboardLayout::WidgetId::LobbyRanks, DashboardLayout::WidgetId::DemoTracker, DashboardLayout::WidgetId::PreviousGames, DashboardLayout::WidgetId::Tournaments};
    for (auto w : all) {
        const bool active = present.count(w) != 0;
        out << "<div class='row" << (active ? "" : " toolbox-draggable") << "'";
        if (!active) out << " data-action='overlay-toolbox-drag' data-widget='" << WidgetDomId(w) << "'";
        out << "><div class='grow'>" << Escape(DashboardLayout::GetWidgetDisplayName(w)) << "</div>";
        if (active)
            out << "<span class='badge win'>ACTIVE</span>";
        else
            out << "<button class='compact ghost' data-action='overlay-add-widget' data-widget='" << WidgetDomId(w) << "'>Add / drag</button>";
        out << "</div>";
    }
    out << "</div></div>";
    return out.str();
}

void RmlUiController::RebuildOverlay() {
    if (m_onboardingVisible || m_config.second_monitor_mode) return;
    std::ostringstream out;
    if (m_config.show_running_indicator) out << "<div class='running-indicator'><span class='win'>●</span> OmniStats</div>";

    if (m_state && m_state->ui.showMatchSummary.load() && m_config.show_match_summary) {
        const int64_t elapsed = SteadyNowMs() - EffectiveMatchSummaryStartMs(*m_state);
        if (elapsed < MatchSummaryDurationMs(m_config))
            out << "<div class='live-special' data-live-special='match-summary'>" << RenderMatchSummary() << "</div>";
        else
            m_state->ui.showMatchSummary.store(false);
    }
    if (m_state && m_state->ui.showSessionView.load())
        out << "<div class='live-special' data-live-special='session-view'>" << RenderSessionView() << "</div>";

    const bool editMode = m_state && m_state->ui.dashboardLayoutEditMode.load();
    for (const auto& container : m_config.overlay_layout.containers)
        out << RenderOverlayContainer(container, editMode);
    out << RenderOverlayToolbox(editMode);
    if (editMode) {
        out << "<div id='overlay-snap-x' class='overlay-snap-guide vertical'></div>"
            << "<div id='overlay-snap-y' class='overlay-snap-guide horizontal'></div>"
            << "<div id='overlay-dock-indicator' class='overlay-dock-guide'></div>"
            << "<div class='layout-position-toolbar card row'><span class='label'>Move or resize cards. Visibility stays in Display settings.</span>"
            << Button("finish-position", "Done positioning", "primary compact") << "</div>";
    }
    SetRootRml("overlay-root", out.str());
    m_liveDomNeedsPrime = true;
}

std::string RmlUiController::RenderReleaseBanners() const {
    const RemoteConfigData remoteSnap = RemoteConfig::Instance().GetSnapshot();
    const bool belowMin = RemoteConfig::IsVersionBelowMinimum(AppVersion::Current, remoteSnap.min_supported_version);
    const auto announcements = RemoteConfig::Instance().ActiveAnnouncements();
    if (!belowMin && announcements.empty()) {
        return {};
    }

    bool updateDownloading = false;
    bool updateFailed = false;
    if (m_state) {
        updateDownloading = m_state->ui.updateDownloading.load();
        updateFailed = m_state->ui.updateDownloadFailed.load();
    }

    std::ostringstream out;
    out << "<div class='release-banners'>";
    if (belowMin) {
        const std::string msg = remoteSnap.min_version_message.empty()
                                    ? ("OmniStats v" + remoteSnap.min_supported_version + " or newer is recommended.")
                                    : remoteSnap.min_version_message;
        const std::string updateLabel = updateDownloading ? "Starting updater..."
                                        : updateFailed    ? "Retry update"
                                                          : "Update now";
        out << "<div class='release-banner severity-warning min-version-banner'>"
            << "<div class='release-banner-body'>"
            << "<div class='release-banner-title'>Update recommended · v" << Escape(remoteSnap.min_supported_version) << "+</div>"
            << "<div class='release-banner-text'>" << Escape(msg) << "</div>"
            << "</div><div class='release-banner-actions'>"
            << Button("update-app", updateLabel, "primary compact")
            << "</div></div>";
    }

    for (const auto& ann : announcements) {
        out << "<div class='release-banner severity-" << Escape(ann.severity)
            << "' data-announcement-id='" << Escape(ann.id) << "'>"
            << "<div class='release-banner-body'>"
            << "<div class='release-banner-title'>" << Escape(ann.title) << "</div>";
        if (!ann.body.empty()) {
            out << "<div class='release-banner-text'>" << Escape(ann.body) << "</div>";
        }
        out << "</div><div class='release-banner-actions'>";
        if (!ann.link_url.empty() && RemoteConfig::IsAllowedAnnouncementLinkUrl(ann.link_url)) {
            out << "<button class='ghost compact' data-action='open-announcement-link' data-url='"
                << Escape(ann.link_url) << "'>Learn more</button>";
        }
        if (ann.dismissible) {
            out << "<button class='ghost compact' data-action='dismiss-announcement' data-announcement-id='"
                << Escape(ann.id) << "'>Dismiss</button>";
        }
        out << "</div></div>";
    }
    out << "</div>";
    return out.str();
}

void RmlUiController::RebuildDashboard() {
    if (m_onboardingVisible || !m_config.second_monitor_mode) return;
    const bool editMode = m_state && m_state->ui.dashboardLayoutEditMode.load();
    DashboardLayout::LayoutConfig layout = m_config.dashboard_layout;
    DashboardLayout::Sanitize(layout);

    auto zoneWidgets = [&](DashboardLayout::Zone zone) {
        std::vector<DashboardLayout::WidgetPlacement> result;
        for (const auto& p : layout.widgets) {
            if (p.zone != zone) continue;
            if (!editMode && !WidgetSettings::Enabled(m_config, p.id, true)) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::LiveRoster && !m_config.second_monitor_show_roster) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::SessionStats && !m_config.second_monitor_show_session) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::LobbyRanks && !m_config.show_lobby_ranks_overlay) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::DemoTracker && !m_config.show_demo_tracker_overlay) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::PreviousGames && !m_config.show_previous_games_summary) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::RankProgress && !m_config.show_rank_progress) continue;
            if (!editMode && p.id == DashboardLayout::WidgetId::Tournaments && (!m_config.show_tournaments || !AccountClient::Instance().IsSignedIn())) continue;
            result.push_back(p);
        }
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.order < b.order; });
        return result;
    };
    auto renderZone = [&](DashboardLayout::Zone zone, const char* klass) {
        std::ostringstream z;
        const auto placements = zoneWidgets(zone);
        z << "<div class='" << klass << "' data-zone='" << ZoneName(zone) << "'>";
        if (editMode) {
            z << "<div class='dashboard-drop-slot' data-zone='" << ZoneName(zone)
              << "' data-drop-index='0'><span class='drop-hint'>── Drop at top ──</span></div>";
        }
        for (size_t index = 0; index < placements.size(); ++index) {
            const auto& placement = placements[index];
            const std::string id = WidgetDomId(placement.id);
            if (editMode && index > 0) {
                z << "<div class='dashboard-drop-slot' data-zone='" << ZoneName(zone)
                  << "' data-drop-index='" << index << "'><span class='drop-hint'>── Drop Here ──</span></div>";
            }
            z << "<div class='dashboard-widget' data-widget='" << id << "' data-zone='" << ZoneName(zone)
              << "' data-order='" << index << "'>"
              << "<div class='dashboard-widget-title' data-action='dashboard-drag' data-widget='" << id << "'><span class='name'>" << Escape(DashboardLayout::GetWidgetDisplayName(placement.id)) << "</span>";
            if (editMode) z << "<span class='badge accent'>DRAG</span>";
            z << "</div>";
            if (!placement.collapsed) {
                z << "<div class='live-widget' data-live-widget='" << id
                  << "' data-live-surface='dashboard'>" << RenderWidget(placement.id, true) << "</div>";
            }
            z << "</div>";
        }
        if (editMode && !placements.empty()) {
            z << "<div class='dashboard-drop-slot end' data-zone='" << ZoneName(zone)
              << "' data-drop-index='" << placements.size() << "'><span class='drop-hint'>── Drop at bottom ──</span></div>";
        }
        z << "</div>";
        return z.str();
    };

    std::string updateVersion;
    bool updateFailed = false;
    bool updateDownloading = false;
    bool updateAvailable = false;
    if (m_state) {
        updateAvailable = m_state->ui.updateAvailable.load();
        updateDownloading = m_state->ui.updateDownloading.load();
        updateFailed = m_state->ui.updateDownloadFailed.load();
        if (updateAvailable) {
            std::lock_guard lock(m_state->ui.updateMutex);
            updateVersion = m_state->ui.updateAvailableVersion;
        }
        if (updateAvailable && !m_config.enable_auto_updates && !m_state->ui.updatePromptShown.exchange(true)) m_showUpdatePrompt = true;
    }
    const bool dashboardHasVisibleWidgets = !zoneWidgets(DashboardLayout::Zone::Top).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Left).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Right).empty() ||
                                            !zoneWidgets(DashboardLayout::Zone::Bottom).empty();
    const bool isMaximized = m_hwnd && IsZoomed(m_hwnd);
    std::ostringstream out;
    out << "<div class='dashboard-shell" << (editMode ? " dashboard-edit-active" : "")
        << (isMaximized ? " maximized" : "") << "'><div class='dashboard-topbar'><img class='brand-logo' src='res://images/Logo.png'/>"
        << "<div class='row grow' style='align-items:center'><div class='brand-cluster'><div class='brand-title'>OmniStats <span class='version'>v" << Escape(AppVersion::Current) << "</span></div>";
    if (updateAvailable) {
        std::string updateTooltip;
        if (updateDownloading)
            updateTooltip = updateVersion.empty() ? "Starting update..." : "Starting update to v" + updateVersion + "...";
        else if (updateFailed)
            updateTooltip = updateVersion.empty() ? "Update failed - click to retry" : "Update to v" + updateVersion + " failed - click to retry";
        else
            updateTooltip = updateVersion.empty() ? "Update available - click to install and restart" : "Update to v" + updateVersion + " and restart OmniStats";

        out << "<button class='version-update tooltip-host" << (updateDownloading ? " updating" : "")
            << (updateFailed ? " failed" : "") << "' data-action='update-app' aria-label='" << Escape(updateTooltip) << "'>"
            << "<span class='version-update-icon'>&#8635;</span>"
            << "<span class='tooltip-bubble'>" << Escape(updateTooltip) << "</span></button>";
    }
    out << "</div><div id='dashboard-match-status' class='match-status'>{{dashboard_status}}</div></div>"
        << "<div id='tournament-topbar-root'>"
        << (m_config.show_tournaments && AccountClient::Instance().IsSignedIn() ? TournamentUi::RenderTopbar(*Tournaments::TournamentScheduleService::Instance().Snapshot(), static_cast<int64_t>(std::time(nullptr)), m_config.tournament_reminder_standard_only, m_config.tournament_reminder_minutes) : "")
        << "</div>"
        << "<div class='topbar-actions'>"
        << Button("history-open", "History", "ghost compact")
        << Button("accounts-open", "Accounts", "ghost compact")
        << Button("insights-open", "Insights", "ghost compact")
        << Button("dashboard-edit", editMode ? "Done editing" : "Edit layout", editMode ? "primary compact" : "ghost compact")
        << Button("open-settings", "Settings", "ghost compact")
        << "<button class='window-control' data-action='window-minimize'><span class='win-icon-min'></span></button>"
        << "<button class='window-control' data-action='window-maximize'>"
        << (isMaximized ? "<span class='win-icon-restore'><span class='restore-back'></span><span class='restore-front'></span></span>"
                        : "<span class='win-icon-max'></span>")
        << "</button>"
        << "<button class='window-control danger' data-action='window-close'><span class='win-icon-close'>&#215;</span></button>"
        << "</div></div>";

    out << "<div class='dashboard-content'>";
    out << RenderReleaseBanners();
    if (!editMode && !dashboardHasVisibleWidgets) {
        out << "<div class='card empty-dashboard'><div class='card-title'>Dashboard panels are hidden</div>"
            << "<div class='setting-help'>Open Settings or Edit layout to show dashboard widgets again.</div></div>";
    }
    out << renderZone(DashboardLayout::Zone::Top, "dashboard-zone-top")
        << "<div class='dashboard-columns'><div class='dashboard-column' style='flex-grow:" << std::max(layout.leftColumnWeight, 0.2f) << "'>" << renderZone(DashboardLayout::Zone::Left, "dashboard-zone-left") << "</div>"
        << "<div class='dashboard-column' style='flex-grow:" << std::max(1.0f - layout.leftColumnWeight, 0.2f) << "'>" << renderZone(DashboardLayout::Zone::Right, "dashboard-zone-right") << "</div></div>"
        << renderZone(DashboardLayout::Zone::Bottom, "dashboard-zone-bottom");
    if (editMode) out << "<div class='dashboard-hidden'><div class='card-title'>Hidden widgets</div>" << renderZone(DashboardLayout::Zone::Hidden, "dashboard-zone-hidden") << Button("reset-dashboard", "Reset dashboard layout", "ghost") << "</div>";
    out << "</div>";
    if (m_showUpdatePrompt && updateAvailable) {
        out << "<div class='dashboard-modal'><div class='card update-dialog'><div class='card-title'>Update available</div><div class='setting-help'>A new version of OmniStats is available";
        if (!updateVersion.empty()) out << ": v" << Escape(updateVersion);
        out << ". OmniStats restarts to finish updating.</div><div class='row gap-sm' style='margin-top:12dp'>" << Button("update-app", "Update and restart", "primary") << Button("dismiss-update", "Later", "ghost") << "</div></div></div>";
    }
    out << "</div>";
    SetRootRml("dashboard-root", out.str());
    m_liveDomNeedsPrime = true;
}

void RmlUiController::MoveDashboardWidget(DashboardLayout::WidgetId widget, DashboardLayout::Zone zone, int insertIndex) {
    Config::Update([=](ConfigData& c) {
        DashboardLayout::Sanitize(c.dashboard_layout);

        std::vector<DashboardLayout::WidgetId> zoneIds;
        int currentIndex = -1;
        int sourceIndex = 0;
        for (const auto& placement : c.dashboard_layout.widgets) {
            if (placement.zone != zone) continue;
            if (placement.id == widget)
                currentIndex = sourceIndex;
            else
                zoneIds.push_back(placement.id);
            ++sourceIndex;
        }

        int targetIndex = insertIndex < 0 ? static_cast<int>(zoneIds.size()) : insertIndex;
        if (currentIndex >= 0 && currentIndex < targetIndex) --targetIndex;
        targetIndex = std::clamp(targetIndex, 0, static_cast<int>(zoneIds.size()));
        zoneIds.insert(zoneIds.begin() + targetIndex, widget);

        for (auto& placement : c.dashboard_layout.widgets) {
            if (placement.id == widget) {
                placement.zone = zone;
                placement.enabled = true;
                break;
            }
        }
        for (int i = 0; i < static_cast<int>(zoneIds.size()); ++i) {
            for (auto& placement : c.dashboard_layout.widgets) {
                if (placement.id == zoneIds[i]) {
                    placement.order = i;
                    break;
                }
            }
        }
        DashboardLayout::Sanitize(c.dashboard_layout);
    });
    m_config = Config::Read();
}
