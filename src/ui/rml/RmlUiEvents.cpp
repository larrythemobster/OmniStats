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

void RmlUiController::ProcessEvent(Rml::Event& event) {
    if (m_rebuildingUi) return;
    Rml::Element* target = event.GetTargetElement();
    if (!target) return;
    const uint64_t configRevisionBefore = Config::Revision();
    const std::string type = event.GetType().c_str();
    if (type == "mousedown") m_pointerPressed = true;
    if (type == "click")
        HandleClick(target);
    else if (type == "change")
        HandleChange(target, event);
    else if (type == "input")
        HandleInput(target);
    else if (type == "blur") {
        const std::string key = Attribute(target, "data-setting");
        if (key == "statsapi_path") {
            HandleChange(target, event);
        } else {
            std::string_view colorKey;
            char component = 0;
            if (ParseThemeComponentKey(key, colorKey, component)) CommitColorPick();
        }
    } else if (type == "mousedown")
        HandleMouseDown(target, event);
    else if (type == "mousemove")
        HandleMouseMove(event);
    else if (type == "mouseup")
        HandleMouseUp(event);

    const uint64_t configRevisionAfter = Config::Revision();
    if (configRevisionAfter != configRevisionBefore) m_lastLocalConfigRevision = configRevisionAfter;
}

void RmlUiController::HandleClick(Rml::Element* target) {
    Rml::Element* actionTarget = target;
    std::string action;
    while (actionTarget && action.empty()) {
        action = Attribute(actionTarget, "data-action");
        if (action.empty()) actionTarget = actionTarget->GetParentNode();
    }
    if (!actionTarget || action.empty()) return;
    target = actionTarget;
    if (HandleViewAction(action, target)) return;

    if (action == "overlay-add-widget") {
        const auto widget = WidgetFromDom(Attribute(target, "data-widget"));
        const float screenWidth = static_cast<float>(m_width);
        const float screenHeight = static_cast<float>(m_height);
        const auto [defaultWidth, defaultHeight] = OverlayWidgetDefaultSize(widget, m_dpiScale);
        Config::Update([=](ConfigData& c) {
            bool alreadyPresent = false;
            for (const auto& container : c.overlay_layout.containers) {
                if (std::find(container.widgets.begin(), container.widgets.end(), widget) != container.widgets.end()) {
                    alreadyPresent = true;
                    break;
                }
            }
            if (alreadyPresent) return;
            OverlayLayout::ContainerConfig container;
            container.id = "container_" + std::to_string(GetTickCount64()) + "_" + std::to_string(static_cast<int>(widget));
            container.x = std::max(12.0f * SanitizedScale(m_dpiScale), screenWidth * 0.5f - defaultWidth * 0.5f);
            container.y = std::max(12.0f * SanitizedScale(m_dpiScale), screenHeight * 0.5f - defaultHeight * 0.5f);
            container.w = defaultWidth;
            container.h = defaultHeight;
            container.widgets = {widget};
            c.overlay_layout.containers.push_back(std::move(container));
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-remove-widget") {
        const auto widget = WidgetFromDom(Attribute(target, "data-widget"));
        const std::string sourceId = Attribute(target, "data-container");
        Config::Update([&](ConfigData& c) {
            for (auto it = c.overlay_layout.containers.begin(); it != c.overlay_layout.containers.end(); ++it) {
                if (it->id != sourceId) continue;
                it->widgets.erase(std::remove(it->widgets.begin(), it->widgets.end(), widget), it->widgets.end());
                if (it->widgets.empty()) c.overlay_layout.containers.erase(it);
                break;
            }
            OverlayLayout::Sanitize(c.overlay_layout);
        });
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-remove-container") {
        const std::string sourceId = Attribute(target, "data-container");
        Config::Update([&](ConfigData& c) {
            c.overlay_layout.containers.erase(
                std::remove_if(c.overlay_layout.containers.begin(), c.overlay_layout.containers.end(),
                               [&](const auto& container) { return container.id == sourceId; }),
                c.overlay_layout.containers.end());
            OverlayLayout::Sanitize(c.overlay_layout);
        },
                       true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "window-minimize") {
        if (m_hwnd) ShowWindow(m_hwnd, SW_MINIMIZE);
    } else if (action == "window-maximize") {
        if (m_hwnd) ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "window-close") {
        if (m_hwnd) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    } else if (action == "settings-page") {
        FinishBindCapture();
        m_showBallchasingToken = false;
        const int page = std::clamp(std::atoi(Attribute(target, "data-page").c_str()), 0, 7);
        m_settingsPage = static_cast<SettingsPage>(page);
        RebuildSettings();
    } else if (action == "close-settings") {
        FinishBindCapture();
        if (m_state) {
            if (m_state->ui.dashboardLayoutEditMode.exchange(false)) Config::RequestSave();
            m_state->ui.showMenu.store(false);
        }
        SetRootRml("settings-root", "");
        RebuildOverlay();
        if (m_config.second_monitor_mode) RebuildDashboard();
    } else if (action == "open-settings") {
        if (m_state) {
            m_state->ui.showMenu.store(true);
            m_state->ui.dashboardLayoutEditMode.store(false);
        }
        RebuildSettings();
        RebuildOverlay();
        if (m_config.second_monitor_mode) RebuildDashboard();
    } else if (action == "help-discord") {
        ShellExecuteA(nullptr, "open", "https://discord.gg/4KBW35ApvF", nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "open-player-tracker") {
        const std::string url = Attribute(target, "data-url");
        if (url.rfind("https://rocketleague.tracker.network/rocket-league/profile/", 0) == 0) {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == "update-app") {
        m_showUpdatePrompt = false;
        if (m_state && !m_state->ui.updateDownloading.load()) ExternalUpdaterLauncher::StartInteractiveUpdate(m_state);
        if (m_config.second_monitor_mode) RebuildDashboard();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    } else if (action == "dismiss-update") {
        m_showUpdatePrompt = false;
        RebuildDashboard();
    } else if (action == "dashboard-edit") {
        if (m_state) {
            const bool enabling = !m_state->ui.dashboardLayoutEditMode.load();
            m_state->ui.dashboardLayoutEditMode.store(enabling);
            if (!enabling) Config::RequestSave();
        }
        RebuildDashboard();
        RebuildOverlay();
    } else if (action == "reset-dashboard") {
        Config::Update([](ConfigData& c) { c.dashboard_layout = DashboardLayout::DefaultLayout(); });
        m_config = Config::Read();
        RebuildDashboard();
        RebuildSettings();
        ShowToast("Dashboard layout reset.");
    } else if (action == "overlay-toggle-toolbox") {
        Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = !c.overlay_layout.toolboxOpen; }, true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "overlay-close-toolbox") {
        Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
        m_config = Config::Read();
        RebuildOverlay();
    } else if (action == "reset-overlay") {
        Config::Update([](ConfigData& c) { c.overlay_layout = OverlayLayout::DefaultOverlayLayout(); });
        m_config = Config::Read();
        RebuildOverlay();
        RebuildSettings();
        ShowToast("Overlay layout reset.");
    } else if (action == "reset-theme-colors") {
        Config::Update([](ConfigData& c) { c.ResetThemeColors(); });
        m_config = Config::Read();
        m_editColorKey.clear();
        m_colorPickerGradientHue = -1;
        m_colorPickDirty = false;
        UpdateThemeProperties();
        RefreshThemeEditorControls();
        RebuildSettings();
        RebuildVisibleUi(true, true);
        ShowToast("Theme colors reset to default.");
    } else if (action == "reset-theme-and-layout") {
        Config::Update([](ConfigData& c) { c.ResetThemeAndLayout(); });
        m_config = Config::Read();
        m_editColorKey.clear();
        m_colorPickerGradientHue = -1;
        m_colorPickDirty = false;
        UpdateThemeProperties();
        RefreshThemeEditorControls();
        RebuildOverlay();
        RebuildSettings();
        RebuildVisibleUi(true, true);
        ShowToast("Theme and layout reset to default.");
    } else if (action == "graph-pan-older") {
        PanGraph(-1);
    } else if (action == "graph-pan-newer") {
        PanGraph(1);
    } else if (action == "capture-bind") {
        const int value = std::atoi(Attribute(target, "data-bind").c_str());
        if (value >= static_cast<int>(BindCaptureTarget::KeyOverlay) && value <= static_cast<int>(BindCaptureTarget::GamepadGraphPanRight) &&
            m_bindCaptureTarget != static_cast<BindCaptureTarget>(value)) {
            BeginBindCapture(static_cast<BindCaptureTarget>(value));
        }
        RebuildSettings();
    } else if (action == "cancel-bind") {
        FinishBindCapture();
        RebuildSettings();
    } else if (action == "clear-bind") {
        const int value = std::atoi(Attribute(target, "data-bind").c_str());
        if (value >= static_cast<int>(BindCaptureTarget::KeyOverlay) && value <= static_cast<int>(BindCaptureTarget::GamepadGraphPanRight)) ClearBind(static_cast<BindCaptureTarget>(value));
        m_config = Config::Read();
        RebuildSettings();
    } else if (action == "toggle-token") {
        m_showBallchasingToken = !m_showBallchasingToken;
        RebuildSettings();
    } else if (action == "edit-color") {
        const std::string key = Attribute(target, "data-color-key");
        m_editColorKey = ThemeColorForKey(m_config, key) ? key : std::string{};
        m_colorPickerGradientHue = -1;
        RebuildSettings();
        if (auto* root = Root("settings-root")) {
            if (auto* el = root->QuerySelector("#active-color-editor")) {
                el->ScrollIntoView(false);
            }
        }
    } else if (action == "close-color-editor") {
        m_editColorKey.clear();
        RebuildSettings();
    } else if (action == "confirm-replay-upload") {
        Config::Update([](ConfigData& c) { c.ballchasing_upload_notice_accepted = true; c.auto_upload_replays = true; });
        m_confirmReplayUploads = false;
        m_config = Config::Read();
        RebuildSettings();
    } else if (action == "cancel-replay-upload") {
        m_confirmReplayUploads = false;
        RebuildSettings();
    } else if (action == "open-data-folder") {
        const auto path = Storage::GetDataDirectory();
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == "export-data") {
        if (!m_dbManager) {
            ShowToast("Export failed: database is unavailable.", true);
        } else {
            std::string exportPath, error;
            if (m_dbManager->ExportLocalData(exportPath, error)) {
                ShowToast("Exported local history.");
                const std::string arg = "/select,\"" + exportPath + "matches.json\"";
                ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            } else
                ShowToast("Export failed: " + error, true);
        }
    } else if (action == "merge-database") {
        if (!m_dbManager) {
            ShowToast("Merge failed: database is unavailable.", true);
        } else {
            const std::string selectedPath = PromptForDatabaseFile(m_hwnd);
            if (!selectedPath.empty()) {
                const auto result = m_dbManager->MergeDatabase(selectedPath);
                if (result.success) {
                    if (result.matchesImported > 0) {
                        ShowToast("Merged " + std::to_string(result.matchesImported) + " matches (" +
                                  std::to_string(result.matchesSkipped) + " duplicates skipped).");
                    } else {
                        ShowToast("Merge complete: 0 new matches (" +
                                  std::to_string(result.matchesSkipped) + " duplicates skipped).");
                    }
                    if (m_state) {
                        {
                            std::unique_lock lock(m_state->history.mutex);
                            m_state->history.lifetimeMmrX.clear();
                            m_state->history.lifetimeMmrY.clear();
                            m_state->history.recentSavedMatches.clear();
                            m_state->history.pendingRecentMatches.clear();
                            m_state->history.recentSavedMatchesLoaded = false;
                            m_state->history.version++;
                        }
                        {
                            std::lock_guard lock(m_state->ui.dbStatsMutex);
                            m_state->ui.cachedDbStats = {};
                            m_state->ui.dbStatsDirty.store(true);
                            m_state->ui.dbStatsVersion.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    m_lastDbFetchPrimaryId.clear();
                    m_lastLifetimeHistoryPrimaryId.clear();
                    m_lastRecentMatchHistoryPrimaryId.clear();
                    RefreshAsyncData();
                    RebuildSettings();
                    RebuildVisibleUi(true);
                } else {
                    ShowToast("Merge failed: " + (result.error.empty() ? "unknown error." : result.error), true);
                }
            }
        }
    } else if (action == "delete-history") {
        m_confirmDeleteHistory = true;
        RebuildSettings();
    } else if (action == "cancel-delete-history") {
        m_confirmDeleteHistory = false;
        RebuildSettings();
    } else if (action == "confirm-delete-history") {
        DeleteLocalHistory();
        m_confirmDeleteHistory = false;
        m_config = Config::Read();
        SnapshotState();
        RebuildSettings();
        RebuildVisibleUi(true);
    } else if (action == "show-log") {
        const std::string path = Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
        const std::string arg = "/select,\"" + path + "\"";
        ShellExecuteA(nullptr, "open", "explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (action == "statsapi-check") {
        CheckStatsApi(false);
        RebuildSettings();
    } else if (action == "statsapi-fix") {
        CheckStatsApi(true);
        RebuildSettings();
    }
}

void RmlUiController::HandleInput(Rml::Element* target) {
    const std::string key = Attribute(target, "data-setting");
    if (key.empty()) return;

    // RmlUi text and range controls emit change events during editing.
    // Update their values without replacing the focused control.
    const std::string value = ControlValue(target);
    bool themeChanged = false;
    auto parseColor = [](std::string text, ColorRGBA& out) {
        if (!text.empty() && text.front() == '#') text.erase(text.begin());
        if (text.size() != 6 && text.size() != 8) return false;
        unsigned long parsed = 0;
        size_t consumed = 0;
        try {
            parsed = std::stoul(text, &consumed, 16);
        } catch (...) {
            return false;
        }
        if (consumed != text.size()) return false;
        if (text.size() == 6) parsed = (parsed << 8) | 0xffu;
        out.r = ((parsed >> 24) & 0xffu) / 255.0f;
        out.g = ((parsed >> 16) & 0xffu) / 255.0f;
        out.b = ((parsed >> 8) & 0xffu) / 255.0f;
        out.a = (parsed & 0xffu) / 255.0f;
        return true;
    };

    if (key == "ballchasing_token") {
        m_pendingBallchasingToken = value;
        Config::Update([&](ConfigData& c) { c.ballchasing_token = value; });
    } else if (key == "custom_api_key") {
        Config::Update([&](ConfigData& c) { c.custom_api_key = value; });
    } else if (key == "statsapi_path") {
        // Validate the completed path on blur, not after each character.
        return;
    } else if (key.rfind("theme_", 0) == 0) {
        std::string_view componentColorKey;
        char component = 0;
        if (ParseThemeComponentKey(key, componentColorKey, component)) {
            if (componentColorKey != m_editColorKey) return;
            // RGB/A sliders can emit an event for every pointer pixel. Stage the
            // value in the controller copy and keep this path DOM-local; saving
            // and rebuilding the theme stylesheet on every sample makes the
            // slider thumb lag for the same reason as the SV picker.
            if (!SetThemeComponent(m_config, key, value)) return;
            m_colorPickDirty = true;
            const ColorRGBA* stagedColor = ThemeColorForKey(m_config, componentColorKey);
            if (!stagedColor) return;
            const Hsv hsv = RgbToHsv(*stagedColor);
            if (hsv.s > 0.0f) m_editColorHue = hsv.h;
            RefreshColorPickPreview(component != 'a');

            if (auto* label = m_document ? m_document->GetElementById((std::string("theme-color-") + component + "-value").c_str()) : nullptr) {
                const ColorRGBA* color = ThemeColorForKey(m_config, componentColorKey);
                float channel = 0.0f;
                if (color) {
                    switch (component) {
                    case 'r':
                        channel = color->r;
                        break;
                    case 'g':
                        channel = color->g;
                        break;
                    case 'b':
                        channel = color->b;
                        break;
                    case 'a':
                        channel = color->a;
                        break;
                    default:
                        break;
                    }
                }
                SetElementText(label, std::to_string(std::clamp(static_cast<int>(std::lround(channel * 255.0f)), 0, 255)));
            }
            return;
        } else {
            ColorRGBA color;
            if (!parseColor(value, color)) return;
            Config::Update([&](ConfigData& c) {
                if (ColorRGBA* targetColor = ThemeColorForKey(c, key)) {
                    *targetColor = color;
                    themeChanged = true;
                }
            });
        }
    } else {
        return;
    }

    m_config = Config::Read();
    if (themeChanged) {
        UpdateThemeProperties();
        // Preserve the text field currently being typed into so canonicalizing
        // the hex value does not reset its caret/selection on every keystroke.
        const std::string_view preserveHexKey = key.find(':') == std::string::npos ? std::string_view(key) : std::string_view{};
        RefreshThemeEditorControls(preserveHexKey);
    }
}

void RmlUiController::HandleChange(Rml::Element* target, Rml::Event& event) {
    const std::string key = Attribute(target, "data-setting");
    if (key.empty()) return;
    if (event.GetType() != "blur" &&
        (key == "ballchasing_token" || key == "custom_api_key" || key == "statsapi_path" || key.rfind("theme_", 0) == 0)) {
        HandleInput(target);
        return;
    }
    std::string value = ControlValue(target);
    const bool checked = EventChecked(event, target);

    if (key == "overlay_edit_mode") {
        if (m_state) m_state->ui.dashboardLayoutEditMode.store(checked);
        if (checked)
            Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
        else
            Config::RequestSave();
        m_config = Config::Read();
        RebuildSettings();
        RebuildDashboard();
        RebuildOverlay();
        return;
    }
    if (key == "auto_upload_replays" && checked && !m_config.ballchasing_upload_notice_accepted) {
        m_confirmReplayUploads = true;
        RebuildSettings();
        return;
    }
    if (key == "statsapi_path") {
        std::string normalized;
        if (!ValidateStatsApiPath(value, normalized, m_statsApiPathError)) {
            RebuildSettings();
            return;
        }
        value = std::move(normalized);
        m_statsApiPathError.clear();
    }

    auto parseColor = [](std::string text, ColorRGBA& out) {
        if (!text.empty() && text.front() == '#') text.erase(text.begin());
        if (text.size() != 6 && text.size() != 8) return false;
        unsigned long value = 0;
        size_t consumed = 0;
        try {
            value = std::stoul(text, &consumed, 16);
        } catch (...) {
            return false;
        }
        if (consumed != text.size()) return false;
        if (text.size() == 6) value = (value << 8) | 0xffu;
        out.r = ((value >> 24) & 0xffu) / 255.0f;
        out.g = ((value >> 16) & 0xffu) / 255.0f;
        out.b = ((value >> 8) & 0xffu) / 255.0f;
        out.a = (value & 0xffu) / 255.0f;
        return true;
    };

    bool themeChanged = false;
    Config::Update([&](ConfigData& c) {
        if (key == "require_rl_focus")
            c.require_rl_focus = checked;
        else if (key == "second_monitor_mode")
            c.second_monitor_mode = checked;
        else if (key == "second_monitor_show_roster")
            c.second_monitor_show_roster = checked;
        else if (key == "second_monitor_show_session")
            c.second_monitor_show_session = checked;
        else if (key == "show_match_summary")
            c.show_match_summary = checked;
        else if (key == "show_running_indicator")
            c.show_running_indicator = checked;
        else if (key == "reset_session_on_close")
            c.reset_session_on_close = checked;
        else if (key == "show_session_recap_on_close")
            c.show_session_recap_on_close = checked;
        else if (key == "run_on_startup")
            c.run_on_startup = checked;
        else if (key == "enable_auto_updates")
            c.enable_auto_updates = checked;
        else if (key == "identity") {
            c.last_primary_id = value;
            if (!value.empty() && std::find(c.known_primary_ids.begin(), c.known_primary_ids.end(), value) == c.known_primary_ids.end()) c.known_primary_ids.push_back(value);
        } else if (key == "show_session_record")
            c.show_session_record = checked;
        else if (key == "show_session_goals")
            c.show_session_goals = checked;
        else if (key == "show_session_saves")
            c.show_session_saves = checked;
        else if (key == "show_session_demos")
            c.show_session_demos = checked;
        else if (key == "show_session_boost")
            c.show_session_boost = checked;
        else if (key == "show_session_assists")
            c.show_session_assists = checked;
        else if (key == "show_session_goal_participation")
            c.show_session_goal_participation = checked;
        else if (key == "show_session_mmr_change")
            c.show_session_mmr_change = checked;
        else if (key == "use_rank_icons")
            c.use_rank_icons = checked;
        else if (key == "show_lobby_ranks_overlay") {
            c.show_lobby_ranks_overlay = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::LobbyRanks) w.zone = checked ? DashboardLayout::Zone::Left : DashboardLayout::Zone::Hidden;
        } else if (key == "show_account_wins_overlay")
            c.show_account_wins_overlay = checked;
        else if (key == "show_demo_tracker_overlay") {
            c.show_demo_tracker_overlay = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::DemoTracker) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_previous_games_summary") {
            c.show_previous_games_summary = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::PreviousGames) w.zone = checked ? DashboardLayout::Zone::Top : DashboardLayout::Zone::Hidden;
        } else if (key == "show_streaks_stats") {
            c.show_streaks_stats = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::StreaksStats) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_gamemode_breakdown") {
            c.show_gamemode_breakdown = checked;
            for (auto& w : c.dashboard_layout.widgets)
                if (w.id == DashboardLayout::WidgetId::GamemodeBreakdown) w.zone = checked ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
        } else if (key == "show_lobby_rank_1v1")
            c.show_lobby_rank_1v1 = checked;
        else if (key == "show_lobby_rank_2v2")
            c.show_lobby_rank_2v2 = checked;
        else if (key == "show_lobby_rank_3v3")
            c.show_lobby_rank_3v3 = checked;
        else if (key == "show_lobby_rank_casual")
            c.show_lobby_rank_casual = checked;
        else if (key == "show_lobby_rank_tourny")
            c.show_lobby_rank_tourny = checked;
        else if (key == "show_lobby_rank_hoops")
            c.show_lobby_rank_hoops = checked;
        else if (key == "show_lobby_rank_rumble")
            c.show_lobby_rank_rumble = checked;
        else if (key == "show_lobby_rank_dropshot")
            c.show_lobby_rank_dropshot = checked;
        else if (key == "show_lobby_rank_snowday")
            c.show_lobby_rank_snowday = checked;
        else if (key == "show_lobby_rank_heatseeker")
            c.show_lobby_rank_heatseeker = checked;
        else if (key == "previous_games_limit")
            c.previous_games_limit = std::clamp(std::atoi(value.c_str()), 10, 50);
        else if (key == "show_longest_loss_streak")
            c.show_longest_loss_streak = checked;
        else if (key == "show_gamemode_record_1v1")
            c.show_gamemode_record_1v1 = checked;
        else if (key == "show_gamemode_record_2v2")
            c.show_gamemode_record_2v2 = checked;
        else if (key == "show_gamemode_record_3v3")
            c.show_gamemode_record_3v3 = checked;
        else if (key == "gamemode_breakdown_scope")
            c.gamemode_breakdown_scope = value;
        else if (key == "mmr_category")
            c.mmr_category = value;
        else if (key == "auto_switch_mmr_category")
            c.auto_switch_mmr_category = checked;
        else if (key == "show_extra_playlists") {
            c.show_extra_playlists = checked;
            if (!checked && IsExtraMmrCategory(StringToMmrCategory(c.mmr_category))) c.mmr_category = "best";
            if (!checked && IsExtraMmrCategory(StringToMmrCategory(c.graph_mmr_category))) c.graph_mmr_category = "2v2";
        } else if (key == "graph_follow_current_playlist")
            c.graph_follow_current_playlist = checked;
        else if (key == "graph_mmr_category")
            c.graph_mmr_category = value;
        else if (key == "ui_scale")
            c.ui_scale = SanitizedUiScale(std::strtof(value.c_str(), nullptr));
        else if (key == "speed_units")
            c.imperial_units = value == "imperial";
        else if (key == "crossbar_display_mode")
            c.crossbar_display_mode = value == "speed" ? "speed" : "raw";
        else if (key == "use_roman_numerals")
            c.use_roman_numerals = checked;
        else if (key == "custom_api_enabled")
            c.custom_api_enabled = checked;
        else if (key == "custom_api_key")
            c.custom_api_key = value;
        else if (key == "ballchasing_token") {
            c.ballchasing_token = value;
            m_pendingBallchasingToken = value;
        } else if (key == "auto_upload_replays")
            c.auto_upload_replays = checked;
        else if (key == "ballchasing_visibility")
            c.ballchasing_visibility = value;
        else if (key == "auto_save_replays")
            c.auto_save_replays = checked;
        else if (key == "discord_rpc_enabled")
            c.discord_rpc_enabled = checked;
        else if (key == "enable_mmr_tracking")
            c.enable_mmr_tracking = checked;
        else if (key == "crash_reports_enabled")
            c.crash_reports_enabled = checked;
        else if (key == "check_stats_api_config_on_startup")
            c.check_stats_api_config_on_startup = checked;
        else if (key == "debug_logging")
            c.debug_logging = checked;
        else if (key == "statsapi_path")
            c.rocket_league_stats_api_config_path = value;
        else if (key.rfind("theme_", 0) == 0) {
            std::string_view componentColorKey;
            char component = 0;
            if (ParseThemeComponentKey(key, componentColorKey, component)) {
                themeChanged = SetThemeComponent(c, key, value);
            } else {
                ColorRGBA parsed;
                if (parseColor(value, parsed)) {
                    if (ColorRGBA* targetColor = ThemeColorForKey(c, key)) {
                        *targetColor = parsed;
                        themeChanged = true;
                    }
                }
            }
        }

        // The rank table is a fixed-column layout, so enabling or disabling a
        // playlist changes how much room it needs. Drop the persisted size and
        // let the container re-fit to the new column count.
        if (key.rfind("show_lobby_rank_", 0) == 0 || key == "show_extra_playlists") {
            for (auto& container : c.overlay_layout.containers) {
                const bool hasLobbyRanks =
                    std::find(container.widgets.begin(), container.widgets.end(),
                              DashboardLayout::WidgetId::LobbyRanks) != container.widgets.end();
                if (!hasLobbyRanks) continue;
                container.w = 0.0f;
                container.h = 0.0f;
            }
        }
    });

    if (key == "run_on_startup") Config::SetWindowsAutoStart(checked);
    if (key == "identity" && m_state) {
        {
            std::unique_lock lock(m_state->game.mutex);
            m_state->game.myPrimaryId = value;
            m_state->game.myTeam = -1;
            if (!value.empty()) {
                if (auto it = m_state->game.roster.find(value); it != m_state->game.roster.end()) m_state->game.myTeam = it->second.team;
            }
            m_state->game.version.fetch_add(1, std::memory_order_relaxed);
        }
        // Force account-scoped async data to refresh for the newly selected identity.
        m_lastDbFetchPrimaryId.clear();
        m_lastLifetimeHistoryPrimaryId.clear();
        m_lastRecentMatchHistoryPrimaryId.clear();
    }
    if (key == "mmr_category" && m_state) m_state->ui.rosterMmrCategory.store(StringToMmrCategory(value));
    if (key == "graph_mmr_category" && m_state) m_state->ui.graphMmrCategory.store(StringToMmrCategory(value));
    if (key == "graph_follow_current_playlist" && !checked && m_state) m_state->ui.graphMmrCategory.store(StringToMmrCategory(m_config.graph_mmr_category));
    if (key == "show_extra_playlists" && !checked && m_state) {
        if (IsExtraMmrCategory(m_state->ui.rosterMmrCategory.load())) m_state->ui.rosterMmrCategory.store(MmrCategory::Best);
        if (IsExtraMmrCategory(m_state->ui.graphMmrCategory.load())) m_state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    }
    if (key == "statsapi_path") CheckStatsApi(false);

    m_config = Config::Read();
    if (target && target->GetTagName() == "select") {
        // Changing a <select> in settings dispatches a synchronous `change`
        // event from inside WidgetDropDown::ProcessEvent. Rebuilding the DOM
        // here would destroy the <select> before WidgetDropDown finishes
        // closing its selection box, causing a use-after-free crash.
        // Defer any scale update and UI rebuild to the next Update() cycle.
        return;
    }
    if (themeChanged) {
        UpdateThemeProperties();
        RefreshThemeEditorControls();
    }
    // Theme-only changes are handled entirely by the persistent stylesheet.
    // Do not rebuild either large root just to apply colors.
    RebuildVisibleUi(!themeChanged, !themeChanged);
}

void RmlUiController::BeginBindCapture(BindCaptureTarget target) {
    if (!m_state) return;
    if (m_state->ui.showMenu.load()) m_lastShowMenu = true;
    m_bindCaptureTarget = target;
    m_state->ui.inputCaptureActive.store(true);
    m_state->ui.lastKeyboardKeyPressed.store(-1);
    m_state->ui.lastControllerButtonPressed.store(-1);
    m_state->ui.lastRawControllerButtonPressed.store(-1);
}

void RmlUiController::FinishBindCapture() {
    m_bindCaptureTarget = BindCaptureTarget::None;
    if (m_state) m_state->ui.inputCaptureActive.store(false);
}

void RmlUiController::ClearBind(BindCaptureTarget target) {
    if (target == BindCaptureTarget::KeyMenu) return;
    Config::Update([target](ConfigData& c) {
        switch (target) {
        case BindCaptureTarget::KeyOverlay:
            c.key_overlay = -1;
            break;
        case BindCaptureTarget::KeyCycle:
            c.key_cycle = -1;
            break;
        case BindCaptureTarget::KeyExpand:
            c.key_expand = -1;
            break;
        case BindCaptureTarget::KeySession:
            c.key_session = -1;
            break;
        case BindCaptureTarget::KeySaveReplay:
            c.key_save_replay = -1;
            break;
        case BindCaptureTarget::KeyGraphPanLeft:
            c.key_graph_pan_left = -1;
            break;
        case BindCaptureTarget::KeyGraphPanRight:
            c.key_graph_pan_right = -1;
            break;
        case BindCaptureTarget::GamepadOverlay:
            c.gamepad_overlay = -1;
            c.gamepad_overlay_raw = false;
            c.gamepad_overlay_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadCycle:
            c.gamepad_cycle = -1;
            c.gamepad_cycle_raw = false;
            c.gamepad_cycle_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadExpand:
            c.gamepad_expand = -1;
            c.gamepad_expand_raw = false;
            c.gamepad_expand_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadSession:
            c.gamepad_session = -1;
            c.gamepad_session_raw = false;
            c.gamepad_session_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadMenu:
            c.gamepad_menu = -1;
            c.gamepad_menu_raw = false;
            c.gamepad_menu_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanLeft:
            c.gamepad_graph_pan_left = -1;
            c.gamepad_graph_pan_left_raw = false;
            c.gamepad_graph_pan_left_raw_button = -1;
            break;
        case BindCaptureTarget::GamepadGraphPanRight:
            c.gamepad_graph_pan_right = -1;
            c.gamepad_graph_pan_right_raw = false;
            c.gamepad_graph_pan_right_raw_button = -1;
            break;
        default:
            break;
        }
    });
}

void RmlUiController::UpdateInputCapture() {
    if (!m_state || m_bindCaptureTarget == BindCaptureTarget::None) return;
    const int vk = m_state->ui.lastKeyboardKeyPressed.load();
    if (vk == VK_ESCAPE) {
        FinishBindCapture();
        RebuildSettings();
        return;
    }
    const bool controllerTarget = m_bindCaptureTarget >= BindCaptureTarget::GamepadOverlay;
    if (controllerTarget) {
        const bool mapped = m_state->ui.controllerIsGameController.load();
        const int mappedButton = m_state->ui.lastControllerButtonPressed.load();
        const int rawButton = m_state->ui.lastRawControllerButtonPressed.load();
        if ((mapped && mappedButton >= 0) || rawButton >= 0) {
            const auto target = m_bindCaptureTarget;
            Config::Update([=](ConfigData& c) {
                const bool useRaw = !(mapped && mappedButton >= 0);
                const int button = useRaw ? rawButton : mappedButton;
                auto set = [&](int& normal, bool& raw, int& rawValue) { raw = useRaw; if (useRaw) rawValue = button; else normal = button; };
                switch (target) {
                case BindCaptureTarget::GamepadOverlay:
                    set(c.gamepad_overlay, c.gamepad_overlay_raw, c.gamepad_overlay_raw_button);
                    break;
                case BindCaptureTarget::GamepadCycle:
                    set(c.gamepad_cycle, c.gamepad_cycle_raw, c.gamepad_cycle_raw_button);
                    break;
                case BindCaptureTarget::GamepadExpand:
                    set(c.gamepad_expand, c.gamepad_expand_raw, c.gamepad_expand_raw_button);
                    break;
                case BindCaptureTarget::GamepadSession:
                    set(c.gamepad_session, c.gamepad_session_raw, c.gamepad_session_raw_button);
                    break;
                case BindCaptureTarget::GamepadMenu:
                    set(c.gamepad_menu, c.gamepad_menu_raw, c.gamepad_menu_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanLeft:
                    set(c.gamepad_graph_pan_left, c.gamepad_graph_pan_left_raw, c.gamepad_graph_pan_left_raw_button);
                    break;
                case BindCaptureTarget::GamepadGraphPanRight:
                    set(c.gamepad_graph_pan_right, c.gamepad_graph_pan_right_raw, c.gamepad_graph_pan_right_raw_button);
                    break;
                default:
                    break;
                }
            });
            FinishBindCapture();
            m_config = Config::Read();
            RebuildSettings();
        }
    } else if (vk > 0) {
        const auto target = m_bindCaptureTarget;
        Config::Update([=](ConfigData& c) {
            switch (target) {
            case BindCaptureTarget::KeyOverlay:
                c.key_overlay = vk;
                break;
            case BindCaptureTarget::KeyCycle:
                c.key_cycle = vk;
                break;
            case BindCaptureTarget::KeyExpand:
                c.key_expand = vk;
                break;
            case BindCaptureTarget::KeySession:
                c.key_session = vk;
                break;
            case BindCaptureTarget::KeyMenu:
                c.key_menu = vk;
                break;
            case BindCaptureTarget::KeySaveReplay:
                c.key_save_replay = vk;
                break;
            case BindCaptureTarget::KeyGraphPanLeft:
                c.key_graph_pan_left = vk;
                break;
            case BindCaptureTarget::KeyGraphPanRight:
                c.key_graph_pan_right = vk;
                break;
            default:
                break;
            }
        });
        FinishBindCapture();
        m_config = Config::Read();
        RebuildSettings();
    }
}
