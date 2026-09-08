#include "SettingsPanel.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "core/Storage.hpp"
#include "core/StatsScope.hpp"
#include "core/DashboardLayoutConfig.hpp"
#include "core/StatsApiConfig.hpp"
#include "network/TelemetryManager.hpp"
#include "core/AppVersion.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "ui/Overlay.hpp"
#include "ui/ThemeManager.hpp"
#include "ui/widgets/ToggleWidget.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include <iostream>
#include <SDL_gamecontroller.h>
#include <algorithm>
#include <gdiplus.h>
#include <shellapi.h>

namespace {
    const char* GetGamepadButtonName(int button) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return "A";
        case SDL_CONTROLLER_BUTTON_B:
            return "B";
        case SDL_CONTROLLER_BUTTON_X:
            return "X";
        case SDL_CONTROLLER_BUTTON_Y:
            return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:
            return "Select/Back";
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return "Guide";
        case SDL_CONTROLLER_BUTTON_START:
            return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return "Left Thumb";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return "Right Thumb";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return "Left Bumper";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return "Right Bumper";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return "D-Pad Right";
        case 22:
            return "Left Stick Up";
        case 23:
            return "Left Stick Down";
        case 24:
            return "Left Stick Left";
        case 25:
            return "Left Stick Right";
        case 26:
            return "Right Stick Up";
        case 27:
            return "Right Stick Down";
        case 28:
            return "Right Stick Left";
        case 29:
            return "Right Stick Right";
        default:
            return nullptr;
        }
    }

    std::string GetGamepadBindName(int mappedButton, bool isRaw, int rawButton) {
        if (isRaw && rawButton >= 0) {
            return "Raw Button " + std::to_string(rawButton);
        }

        if (!isRaw) {
            const char* name = GetGamepadButtonName(mappedButton);
            if (name) return name;
            if (mappedButton >= 0) return "Controller Button " + std::to_string(mappedButton);
        }

        return "None";
    }
}

SettingsPanel::SettingsPanel(RenderContext context) : ctx(std::move(context)) {}

SettingsPanel::~SettingsPanel() {
    if (m_bindCaptureTarget != BindCaptureTarget::None) {
        ctx.state.ui.inputCaptureActive.store(false);
    }
}

SettingsResult SettingsPanel::Render() {
    bool styleChanged = false;
    bool windowChanged = false;
    ImGuiIO& io = ImGui::GetIO();

    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImVec4 settingsBackground = Format::C(ctx.config.themeBg);
    settingsBackground.w = 1.0f;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, settingsBackground);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Format::C(ctx.config.themeSettingsPanel));
    ImGui::PushFont(ctx.fontRegular);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * ctx.dpiScale, 12.0f * ctx.dpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f * ctx.dpiScale, 4.0f * ctx.dpiScale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * ctx.dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * ctx.dpiScale, 4.0f * ctx.dpiScale));

    ImGui::SetNextWindowSize(ImVec2(
        fminf(620.0f * ctx.dpiScale, io.DisplaySize.x - 24.0f * ctx.dpiScale),
        fminf(500.0f * ctx.dpiScale, io.DisplaySize.y - 24.0f * ctx.dpiScale)));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration;
    if (ImGui::Begin("Settings", nullptr, flags)) {
        RenderContent("Main", styleChanged, windowChanged);
    }
    ImGui::End();

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();

    return {.styleChanged = styleChanged, .windowChanged = windowChanged};
}

void SettingsPanel::RenderContent(const std::string& idSuffix, bool& styleChanged, bool& windowChanged) {
    ImGui::PushID(idSuffix.c_str());
    // Window storage survives the panel rebuild when interface size reloads fonts.
    ImGuiStorage* settingsStorage = ImGui::GetStateStorage();
    const ImGuiID categoryId = ImGui::GetID("SelectedCategory");
    m_page = static_cast<Page>(settingsStorage->GetInt(categoryId, static_cast<int>(Page::General)));
    ImGui::PushFont(ctx.fontBold);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::Text("OmniStats v%s", AppVersion::Current);

    if (ctx.state.ui.updateAvailable.load()) {
        ImGui::SameLine();
        std::string ver;
        {
            std::lock_guard<std::mutex> lock(ctx.state.ui.updateMutex);
            ver = ctx.state.ui.updateAvailableVersion;
        }

        ImGui::PushStyleColor(ImGuiCol_Button, Format::C(ctx.config.themeWin));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(Format::C(ctx.config.themeWin).x * 1.1f, Format::C(ctx.config.themeWin).y * 1.1f, Format::C(ctx.config.themeWin).z * 1.1f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f * ctx.dpiScale);
        ImGui::PushFont(ctx.fontSmallBold);

        std::string btnLabel;
        if (ctx.state.ui.updateDownloading.load()) {
            btnLabel = ver.empty() ? "Starting updater..." : "Starting v" + ver + "...";
        } else if (ctx.state.ui.updateDownloadFailed.load()) {
            btnLabel = ver.empty() ? "Retry Update" : "Retry Update v" + ver;
        } else {
            btnLabel = ver.empty() ? "Update Available" : "Update Available: v" + ver;
        }

        if (ImGui::Button(btnLabel.c_str())) {
            if (!ctx.state.ui.updateDownloading.load()) {
                ExternalUpdaterLauncher::StartInteractiveUpdate(ctx.state.shared_from_this());
            }
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            if (ctx.state.ui.updateDownloading.load()) {
                ImGui::SetTooltip("Updater is preparing the update...");
            } else {
                ImGui::SetTooltip("Click to launch the updater and restart.");
            }
        }
    }

    ImGui::TextWrapped("Changes apply automatically. No Save button needed.");
    ImGui::Separator();

    auto finishBindCapture = [&]() {
        m_bindCaptureTarget = BindCaptureTarget::None;
        ctx.state.ui.inputCaptureActive.store(false);
    };

    auto beginBindCapture = [&](BindCaptureTarget target) {
        m_bindCaptureTarget = target;
        ctx.state.ui.inputCaptureActive.store(true);
        ctx.state.ui.lastKeyboardKeyPressed.store(-1);
        ctx.state.ui.lastControllerButtonPressed.store(-1);
        ctx.state.ui.lastRawControllerButtonPressed.store(-1);
    };

    auto clearBind = [&](BindCaptureTarget target) {
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
            default:
                break;
            }
        });
        ctx.config = Config::Read();
    };

    if (m_bindCaptureTarget != BindCaptureTarget::None) {
        const int vk = ctx.state.ui.lastKeyboardKeyPressed.load();
        if (vk == VK_ESCAPE) {
            finishBindCapture();
        } else if (m_bindCaptureTarget == BindCaptureTarget::GamepadOverlay ||
                   m_bindCaptureTarget == BindCaptureTarget::GamepadCycle ||
                   m_bindCaptureTarget == BindCaptureTarget::GamepadExpand ||
                   m_bindCaptureTarget == BindCaptureTarget::GamepadSession ||
                   m_bindCaptureTarget == BindCaptureTarget::GamepadMenu) {
            const bool isGameController = ctx.state.ui.controllerIsGameController.load();
            const int mappedButton = ctx.state.ui.lastControllerButtonPressed.load();
            const int rawButton = ctx.state.ui.lastRawControllerButtonPressed.load();
            if (isGameController && mappedButton >= 0) {
                const BindCaptureTarget target = m_bindCaptureTarget;
                Config::Update([target, mappedButton](ConfigData& c) {
                    switch (target) {
                    case BindCaptureTarget::GamepadOverlay:
                        c.gamepad_overlay_raw = false;
                        c.gamepad_overlay = mappedButton;
                        break;
                    case BindCaptureTarget::GamepadCycle:
                        c.gamepad_cycle_raw = false;
                        c.gamepad_cycle = mappedButton;
                        break;
                    case BindCaptureTarget::GamepadExpand:
                        c.gamepad_expand_raw = false;
                        c.gamepad_expand = mappedButton;
                        break;
                    case BindCaptureTarget::GamepadSession:
                        c.gamepad_session_raw = false;
                        c.gamepad_session = mappedButton;
                        break;
                    case BindCaptureTarget::GamepadMenu:
                        c.gamepad_menu_raw = false;
                        c.gamepad_menu = mappedButton;
                        break;
                    default:
                        break;
                    }
                });
                ctx.config = Config::Read();
                finishBindCapture();
            } else if (rawButton >= 0) {
                const BindCaptureTarget target = m_bindCaptureTarget;
                Config::Update([target, rawButton](ConfigData& c) {
                    switch (target) {
                    case BindCaptureTarget::GamepadOverlay:
                        c.gamepad_overlay_raw = true;
                        c.gamepad_overlay_raw_button = rawButton;
                        break;
                    case BindCaptureTarget::GamepadCycle:
                        c.gamepad_cycle_raw = true;
                        c.gamepad_cycle_raw_button = rawButton;
                        break;
                    case BindCaptureTarget::GamepadExpand:
                        c.gamepad_expand_raw = true;
                        c.gamepad_expand_raw_button = rawButton;
                        break;
                    case BindCaptureTarget::GamepadSession:
                        c.gamepad_session_raw = true;
                        c.gamepad_session_raw_button = rawButton;
                        break;
                    case BindCaptureTarget::GamepadMenu:
                        c.gamepad_menu_raw = true;
                        c.gamepad_menu_raw_button = rawButton;
                        break;
                    default:
                        break;
                    }
                });
                ctx.config = Config::Read();
                finishBindCapture();
            }
        } else if (vk > 0) {
            const BindCaptureTarget target = m_bindCaptureTarget;
            Config::Update([target, vk](ConfigData& c) {
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
                default:
                    break;
                }
            });
            ctx.config = Config::Read();
            finishBindCapture();
        }
    }

    auto renderBind = [&](BindCaptureTarget target, const char* label, const char* binding,
                          const char* prompt, bool canClear) {
        const bool active = m_bindCaptureTarget == target;
        ImGui::PushID(static_cast<int>(target));
        if (ImGui::BeginTable("ShortcutRow", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed, 176.0f * ctx.dpiScale);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", label);
            ImGui::TableNextColumn();
            const float actionWidth = 64.0f * ctx.dpiScale;
            const float bindingWidth = 106.0f * ctx.dpiScale;
            if (ImGui::Button(active ? prompt : binding, ImVec2(bindingWidth, 0.0f)) && !active) {
                beginBindCapture(target);
            }
            if (active || canClear) {
                ImGui::SameLine();
                if (ImGui::Button(active ? "Cancel" : "Clear", ImVec2(actionWidth, 0.0f))) {
                    if (active)
                        finishBindCapture();
                    else
                        clearBind(target);
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    };

    auto renderKeyboardBind = [&](BindCaptureTarget target, const char* label, int currentVK) {
        const std::string keyName = currentVK <= 0 ? "Not set" : GetKeyDisplayName(currentVK);
        renderBind(target, label, keyName.c_str(), "Press key...", currentVK > 0 && target != BindCaptureTarget::KeyMenu);
    };

    auto renderControllerBind = [&](BindCaptureTarget target, const char* label, int mappedButton, bool isRaw, int rawButton) {
        const std::string bindName = GetGamepadBindName(mappedButton, isRaw, rawButton);
        renderBind(target, label, bindName.c_str(), "Press button...", mappedButton >= 0 || (isRaw && rawButton >= 0));
    };

    struct Category {
        Page page;
        const char* label;
        const char* description;
    };
    static constexpr Category categories[] = {
        {Page::General, "General", "Window behavior, startup, updates, and your account."},
        {Page::Cards, "Overlay & cards", "Choose the cards and stats you see while playing."},
        {Page::Ranks, "Ranks & graph", "Choose playlists for player ranks and your personal MMR graph."},
        {Page::Shortcuts, "Shortcuts", "Choose keyboard and controller shortcuts."},
        {Page::Appearance, "Appearance", "Adjust interface size, colors, and units."},
        {Page::Integrations, "Replays & services", "Save replays, upload to Ballchasing, and connect external services."},
        {Page::Data, "Data & privacy", "Manage local history, exports, and diagnostic sharing."},
        {Page::Troubleshooting, "Troubleshooting", "Check the game connection and inspect diagnostic logs."},
    };
    const bool compact = ImGui::GetContentRegionAvail().x < 590.0f * ctx.dpiScale;
    const float footerHeight = ImGui::GetFrameHeightWithSpacing() + 12.0f * ctx.dpiScale;
    if (compact) {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##SettingsCategory", categories[static_cast<int>(m_page)].label)) {
            for (const auto& category : categories) {
                if (ImGui::Selectable(category.label, m_page == category.page)) {
                    if (m_page != category.page) finishBindCapture();
                    m_page = category.page;
                    m_showReplayToken = false;
                }
            }
            ImGui::EndCombo();
        }
    } else {
        ImGui::BeginChild("SettingsNavigation", ImVec2(134.0f * ctx.dpiScale, -footerHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        for (const auto& category : categories) {
            if (ImGui::Selectable(category.label, m_page == category.page, 0,
                                  ImVec2(0.0f, 26.0f * ctx.dpiScale))) {
                if (m_page != category.page) finishBindCapture();
                m_page = category.page;
                m_showReplayToken = false;
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::SameLine();
    }

    ImGui::PushID(static_cast<int>(m_page));
    if (ImGui::BeginChild("SettingsPage", ImVec2(0.0f, -footerHeight))) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::PushItemWidth(fminf(230.0f * ctx.dpiScale, ImGui::GetContentRegionAvail().x * 0.48f));
        ImGui::PushFont(ctx.fontBold);
        ImGui::TextUnformatted(categories[static_cast<int>(m_page)].label);
        ImGui::PopFont();
        auto selectSection = [&](const char* options) {
            const ImGuiID sectionId = ImGui::GetID("SelectedSection");
            int section = settingsStorage->GetInt(sectionId, 0);
            ImGui::SetNextItemWidth(230.0f * ctx.dpiScale);
            if (ImGui::Combo("##Section", &section, options)) {
                finishBindCapture();
                m_showReplayToken = false;
                settingsStorage->SetInt(sectionId, section);
            }
            ImGui::Spacing();
            return section;
        };
        ImGui::Spacing();
        ImGui::Separator();

        if (m_page == Page::General) {
            const int section = selectSection("Window & visibility\0Match behavior\0Startup & updates\0Your account\0");
            if (section == 0) {
                ImGui::Spacing();

                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Window & Visibility");
                ImGui::Dummy(ImVec2(0, 4));

                bool requireFocus = ctx.config.require_rl_focus;
                std::string requireFocusLabel = "Only Show Over Rocket League##" + idSuffix;
                if (ImGui::Checkbox(requireFocusLabel.c_str(), &requireFocus)) {
                    Config::Update([requireFocus](ConfigData& c) { c.require_rl_focus = requireFocus; });
                }
                ImGui::TextWrapped("Hide the overlay when you switch away from the game.");

                bool secondMonitor = ctx.config.second_monitor_mode;
                std::string secondMonitorLabel = "Use a second-monitor dashboard##" + idSuffix;
                if (ImGui::Checkbox(secondMonitorLabel.c_str(), &secondMonitor)) {
                    Config::Update([secondMonitor](ConfigData& c) { c.second_monitor_mode = secondMonitor; });
                    windowChanged = true;
                }
                ImGui::TextWrapped("Switch from the in-game overlay to a separate stats dashboard.");

                if (secondMonitor) {
                    ImGui::Indent(16.0f);

                    bool showRoster = ctx.config.second_monitor_show_roster;
                    std::string showRosterLabel = "Show Live Match & Roster##" + idSuffix + "Roster";
                    if (ImGui::Checkbox(showRosterLabel.c_str(), &showRoster)) {
                        Config::Update([showRoster](ConfigData& c) { c.second_monitor_show_roster = showRoster; });
                    }

                    bool showSession = ctx.config.second_monitor_show_session;
                    std::string showSessionLabel = "Show Session Stats & Graph##" + idSuffix + "Session";
                    if (ImGui::Checkbox(showSessionLabel.c_str(), &showSession)) {
                        Config::Update([showSession](ConfigData& c) { c.second_monitor_show_session = showSession; });
                    }

                    ImGui::Spacing();
                    if (ImGui::Button("Reset Dashboard Layout")) {
                        Config::Update([](ConfigData& c) {
                            c.dashboard_layout = DashboardLayout::DefaultLayout();
                        });
                    }
                    ImGui::TextColored(Format::C(ctx.config.themeDim), "Tip: click 'EDIT' on dashboard title bar to drag cards.");

                    ImGui::Unindent(16.0f);
                }
            }
            if (section == 1) {

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "During & after a match");
                ImGui::Dummy(ImVec2(0, 4));

                bool showSummary = ctx.config.show_match_summary;
                std::string showSummaryLabel = "Show a summary after each match##" + idSuffix;
                if (ImGui::Checkbox(showSummaryLabel.c_str(), &showSummary)) {
                    Config::Update([showSummary](ConfigData& c) { c.show_match_summary = showSummary; });
                }

                bool showIndicator = ctx.config.show_running_indicator;
                std::string showIndicatorLabel = "Show Running Indicator##" + idSuffix;
                if (ImGui::Checkbox(showIndicatorLabel.c_str(), &showIndicator)) {
                    Config::Update([showIndicator](ConfigData& c) { c.show_running_indicator = showIndicator; });
                }
                bool resetSession = ctx.config.reset_session_on_close;
                std::string resetSessionLabel = "Reset Session on Game Close##" + idSuffix;
                if (ImGui::Checkbox(resetSessionLabel.c_str(), &resetSession)) {
                    Config::Update([resetSession](ConfigData& c) { c.reset_session_on_close = resetSession; });
                }
            }
            if (section == 2) {

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Startup & updates");
                bool runOnStartup = ctx.config.run_on_startup;
                std::string runOnStartupLabel = "Run OmniStats on Startup##" + idSuffix;
                if (ImGui::Checkbox(runOnStartupLabel.c_str(), &runOnStartup)) {
                    Config::Update([runOnStartup](ConfigData& c) { c.run_on_startup = runOnStartup; });
                    Config::SetWindowsAutoStart(runOnStartup);
                }
                bool checkForUpdates = ctx.config.check_for_updates;
                std::string checkForUpdatesLabel = "Check for Updates on Startup##" + idSuffix;
                if (ImGui::Checkbox(checkForUpdatesLabel.c_str(), &checkForUpdates)) {
                    Config::Update([checkForUpdates](ConfigData& c) {
                        c.check_for_updates = checkForUpdates;
                        if (!checkForUpdates) c.enable_auto_updates = false;
                    });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Checks OmniStats servers for available updates on startup.");
                }

                bool autoUpdates = ctx.config.enable_auto_updates;
                std::string autoUpdatesLabel = "Install updates automatically##" + idSuffix;
                if (ImGui::Checkbox(autoUpdatesLabel.c_str(), &autoUpdates)) {
                    Config::Update([autoUpdates](ConfigData& c) {
                        c.enable_auto_updates = autoUpdates;
                        if (autoUpdates) c.check_for_updates = true;
                    });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("When an update is found at startup, launches OmniStatsUpdater.exe to install it and restart OmniStats.");
                }
                ImGui::TextWrapped("Automatic installation also enables update checks and restarts OmniStats when an update is ready.");
            }
            if (section == 3) {

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Your Rocket League account");
                ImGui::TextWrapped("Stats assigned to the wrong player? Join a match, then select yourself below. Your choice is remembered.");

                std::vector<std::string> lobbyNames;
                std::vector<std::string> lobbyIds;
                lobbyNames.push_back("Detect automatically");
                lobbyIds.push_back("");

                if (ctx.snap) {
                    for (const auto& [pid, player] : ctx.snap->roster) {
                        if (pid.rfind("Unknown|", 0) != 0) {
                            lobbyNames.push_back(player.name);
                            lobbyIds.push_back(pid);
                        }
                    }
                }

                int currentSelection = 0;
                const std::string& savedMyId = ctx.snap && !ctx.snap->myPrimaryId.empty()
                                                   ? ctx.snap->myPrimaryId
                                                   : ctx.config.last_primary_id;
                for (size_t i = 1; i < lobbyIds.size(); ++i) {
                    if (lobbyIds[i] == savedMyId) {
                        currentSelection = static_cast<int>(i);
                        break;
                    }
                }
                if (currentSelection == 0 && !savedMyId.empty()) {
                    currentSelection = static_cast<int>(lobbyIds.size());
                    lobbyNames.push_back("Saved account, not in lobby");
                    lobbyIds.push_back(savedMyId);
                }
                if (lobbyIds.size() == 1) {
                    ImGui::TextWrapped("No players available yet. Join a Rocket League match to select your account.");
                }

                std::vector<const char*> lobbyNamesCStr;
                for (const auto& name : lobbyNames) {
                    lobbyNamesCStr.push_back(name.c_str());
                }

                std::string identityLabel = "My Account##" + idSuffix;
                if (ImGui::Combo(identityLabel.c_str(), &currentSelection, lobbyNamesCStr.data(), static_cast<int>(lobbyNamesCStr.size()))) {
                    if (currentSelection == 0) {
                        {
                            std::unique_lock<std::shared_mutex> gameLock(ctx.state.game.mutex);
                            ctx.state.game.myPrimaryId = "";
                            ctx.state.game.myTeam = -1;
                        }
                        Config::Update([](ConfigData& c) {
                            c.last_primary_id = "";
                        });
                        std::cout << "[Identity] Manually cleared local identity.\n";
                    } else {
                        std::string newId = lobbyIds[currentSelection];
                        {
                            std::unique_lock<std::shared_mutex> gameLock(ctx.state.game.mutex);
                            ctx.state.game.myPrimaryId = newId;
                            if (ctx.state.game.roster.count(newId)) {
                                ctx.state.game.myTeam = ctx.state.game.roster[newId].team;
                            }
                        }
                        Config::Update([&newId](ConfigData& c) {
                            c.last_primary_id = newId;
                        });
                        std::cout << "[Identity] Manually selected local identity. ID: " << PrivacyLog::Sensitive(newId, "player ID") << "\n";
                        if (ctx.db) {
                            ctx.db->AsyncGetLifetimeMmrHistory(newId, MmrCategoryToString(ctx.state.ui.graphMmrCategory.load()));
                            ctx.db->AsyncRefreshDbStats(newId);
                        }
                    }
                }
            }

            ImGui::Spacing();
        }

        if (m_page == Page::Cards) {
            const int section = selectSection("Layout editing\0Session stats\0Visible cards\0Lobby playlists\0History & records\0");
            if (section == 0) {
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Overlay Layout");
                ImGui::Separator();
                ImGui::Spacing();

                bool overlayEditMode = ctx.state.ui.dashboardLayoutEditMode.load();
                std::string overlayEditModeLabel = "Overlay Layout Edit Mode##" + idSuffix;
                if (ImGui::Checkbox(overlayEditModeLabel.c_str(), &overlayEditMode)) {
                    ctx.state.ui.dashboardLayoutEditMode.store(overlayEditMode);
                    if (overlayEditMode) {
                        Config::Update([](ConfigData& c) { c.overlay_layout.toolboxOpen = false; }, true);
                    } else {
                        Config::RequestSave();
                    }
                }
                ImGui::TextColored(Format::C(ctx.config.themeMuted), "Turn this on to move, resize, dock, or remove overlay cards.");
                ImGui::Spacing();
            }
            if (section == 1) {

                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Session Card Stats");
                ImGui::Separator();
                ImGui::Spacing();

                bool showSessionCardInGame = ctx.config.show_session_card_in_game;
                std::string showSessionCardInGameLabel = "Always Show Session Card In-Game##" + idSuffix;
                if (ImGui::Checkbox(showSessionCardInGameLabel.c_str(), &showSessionCardInGame)) {
                    Config::Update([showSessionCardInGame](ConfigData& c) { c.show_session_card_in_game = showSessionCardInGame; });
                }

                bool showRecord = ctx.config.show_session_record;
                std::string showRecordLabel = "Show Record (W/L)##" + idSuffix;
                if (ImGui::Checkbox(showRecordLabel.c_str(), &showRecord)) {
                    Config::Update([showRecord](ConfigData& c) { c.show_session_record = showRecord; });
                }
                bool showGoals = ctx.config.show_session_goals;
                std::string showGoalsLabel = "Show Goals##" + idSuffix;
                if (ImGui::Checkbox(showGoalsLabel.c_str(), &showGoals)) {
                    Config::Update([showGoals](ConfigData& c) { c.show_session_goals = showGoals; });
                }
                bool showSaves = ctx.config.show_session_saves;
                std::string showSavesLabel = "Show Saves##" + idSuffix;
                if (ImGui::Checkbox(showSavesLabel.c_str(), &showSaves)) {
                    Config::Update([showSaves](ConfigData& c) { c.show_session_saves = showSaves; });
                }
                bool showDemos = ctx.config.show_session_demos;
                std::string showDemosLabel = "Show Demos##" + idSuffix;
                if (ImGui::Checkbox(showDemosLabel.c_str(), &showDemos)) {
                    Config::Update([showDemos](ConfigData& c) { c.show_session_demos = showDemos; });
                }
                bool showBoost = ctx.config.show_session_boost;
                std::string showBoostLabel = "Show Boost Picked Up##" + idSuffix;
                if (ImGui::Checkbox(showBoostLabel.c_str(), &showBoost)) {
                    Config::Update([showBoost](ConfigData& c) { c.show_session_boost = showBoost; });
                }
                bool showAssists = ctx.config.show_session_assists;
                std::string showAssistsLabel = "Show Assists##" + idSuffix;
                if (ImGui::Checkbox(showAssistsLabel.c_str(), &showAssists)) {
                    Config::Update([showAssists](ConfigData& c) { c.show_session_assists = showAssists; });
                }
                bool showGoalParticipation = ctx.config.show_session_goal_participation;
                std::string showGoalParticipationLabel = "Show Goal Participation##" + idSuffix;
                if (ImGui::Checkbox(showGoalParticipationLabel.c_str(), &showGoalParticipation)) {
                    Config::Update([showGoalParticipation](ConfigData& c) { c.show_session_goal_participation = showGoalParticipation; });
                }
                bool showMmr = ctx.config.show_session_mmr_change;
                std::string showMmrLabel = "Show MMR Change##" + idSuffix;
                if (ImGui::Checkbox(showMmrLabel.c_str(), &showMmr)) {
                    Config::Update([showMmr](ConfigData& c) { c.show_session_mmr_change = showMmr; });
                }
            }
            if (section == 2) {

                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Overlay Components");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Spacing();

                bool useRankIcons = ctx.config.use_rank_icons;
                std::string useRankIconsLabel = "Use Rank & Playlist Icons##" + idSuffix;
                if (ImGui::Checkbox(useRankIconsLabel.c_str(), &useRankIcons)) {
                    Config::Update([useRankIcons](ConfigData& c) { c.use_rank_icons = useRankIcons; });
                }

                ImGui::Spacing();

                bool showLobbyRanks = ctx.config.show_lobby_ranks_overlay;
                std::string showLobbyRanksLabel = "Show Lobby Ranks Overlay##" + idSuffix;
                if (ImGui::Checkbox(showLobbyRanksLabel.c_str(), &showLobbyRanks)) {
                    Config::Update([showLobbyRanks](ConfigData& c) {
                        c.show_lobby_ranks_overlay = showLobbyRanks;
                        for (auto& w : c.dashboard_layout.widgets) {
                            if (w.id == DashboardLayout::WidgetId::LobbyRanks) {
                                w.zone = showLobbyRanks ? DashboardLayout::Zone::Left : DashboardLayout::Zone::Hidden;
                            }
                        }
                    });
                }
            }

            if (section == 3) {
                ImGui::TextWrapped("Choose the playlists shown in the Lobby Ranks card. Enable that card under Visible cards.");
                ImGui::BeginDisabled(!ctx.config.show_lobby_ranks_overlay);

                bool showLobby1v1 = ctx.config.show_lobby_rank_1v1;
                std::string showLobby1v1Label = "Show 1v1 Playlist Rank##" + idSuffix;
                if (ImGui::Checkbox(showLobby1v1Label.c_str(), &showLobby1v1)) {
                    Config::Update([showLobby1v1](ConfigData& c) { c.show_lobby_rank_1v1 = showLobby1v1; });
                }

                bool showLobby2v2 = ctx.config.show_lobby_rank_2v2;
                std::string showLobby2v2Label = "Show 2v2 Playlist Rank##" + idSuffix;
                if (ImGui::Checkbox(showLobby2v2Label.c_str(), &showLobby2v2)) {
                    Config::Update([showLobby2v2](ConfigData& c) { c.show_lobby_rank_2v2 = showLobby2v2; });
                }

                bool showLobby3v3 = ctx.config.show_lobby_rank_3v3;
                std::string showLobby3v3Label = "Show 3v3 Playlist Rank##" + idSuffix;
                if (ImGui::Checkbox(showLobby3v3Label.c_str(), &showLobby3v3)) {
                    Config::Update([showLobby3v3](ConfigData& c) { c.show_lobby_rank_3v3 = showLobby3v3; });
                }

                bool showLobbyCasual = ctx.config.show_lobby_rank_casual;
                std::string showLobbyCasualLabel = "Show Casual Playlist Rank##" + idSuffix;
                if (ImGui::Checkbox(showLobbyCasualLabel.c_str(), &showLobbyCasual)) {
                    Config::Update([showLobbyCasual](ConfigData& c) { c.show_lobby_rank_casual = showLobbyCasual; });
                }

                bool showLobbyTourny = ctx.config.show_lobby_rank_tourny;
                std::string showLobbyTournyLabel = "Show Tournament Playlist Rank##" + idSuffix;
                if (ImGui::Checkbox(showLobbyTournyLabel.c_str(), &showLobbyTourny)) {
                    Config::Update([showLobbyTourny](ConfigData& c) { c.show_lobby_rank_tourny = showLobbyTourny; });
                }

                if (ctx.config.show_extra_playlists) {
                    bool showLobbyHoops = ctx.config.show_lobby_rank_hoops;
                    std::string showLobbyHoopsLabel = "Show Hoops Playlist Rank##" + idSuffix;
                    if (ImGui::Checkbox(showLobbyHoopsLabel.c_str(), &showLobbyHoops)) {
                        Config::Update([showLobbyHoops](ConfigData& c) { c.show_lobby_rank_hoops = showLobbyHoops; });
                    }

                    bool showLobbyRumble = ctx.config.show_lobby_rank_rumble;
                    std::string showLobbyRumbleLabel = "Show Rumble Playlist Rank##" + idSuffix;
                    if (ImGui::Checkbox(showLobbyRumbleLabel.c_str(), &showLobbyRumble)) {
                        Config::Update([showLobbyRumble](ConfigData& c) { c.show_lobby_rank_rumble = showLobbyRumble; });
                    }

                    bool showLobbyDropshot = ctx.config.show_lobby_rank_dropshot;
                    std::string showLobbyDropshotLabel = "Show Dropshot Playlist Rank##" + idSuffix;
                    if (ImGui::Checkbox(showLobbyDropshotLabel.c_str(), &showLobbyDropshot)) {
                        Config::Update([showLobbyDropshot](ConfigData& c) { c.show_lobby_rank_dropshot = showLobbyDropshot; });
                    }

                    bool showLobbySnowDay = ctx.config.show_lobby_rank_snowday;
                    std::string showLobbySnowDayLabel = "Show Snow Day Playlist Rank##" + idSuffix;
                    if (ImGui::Checkbox(showLobbySnowDayLabel.c_str(), &showLobbySnowDay)) {
                        Config::Update([showLobbySnowDay](ConfigData& c) { c.show_lobby_rank_snowday = showLobbySnowDay; });
                    }

                    bool showLobbyHeatseeker = ctx.config.show_lobby_rank_heatseeker;
                    std::string showLobbyHeatseekerLabel = "Show Heatseeker Playlist Rank##" + idSuffix;
                    if (ImGui::Checkbox(showLobbyHeatseekerLabel.c_str(), &showLobbyHeatseeker)) {
                        Config::Update([showLobbyHeatseeker](ConfigData& c) { c.show_lobby_rank_heatseeker = showLobbyHeatseeker; });
                    }
                }

                ImGui::EndDisabled();
            }

            if (section == 2) {
                bool showAccountWins = ctx.config.show_account_wins_overlay;
                std::string showAccountWinsLabel = "Show Account Wins##" + idSuffix;
                if (ImGui::Checkbox(showAccountWinsLabel.c_str(), &showAccountWins)) {
                    Config::Update([showAccountWins](ConfigData& c) { c.show_account_wins_overlay = showAccountWins; });
                }

                bool showDemoTracker = ctx.config.show_demo_tracker_overlay;
                std::string showDemoTrackerLabel = "Show Demolition Tracker##" + idSuffix;
                if (ImGui::Checkbox(showDemoTrackerLabel.c_str(), &showDemoTracker)) {
                    Config::Update([showDemoTracker](ConfigData& c) {
                        c.show_demo_tracker_overlay = showDemoTracker;
                        for (auto& w : c.dashboard_layout.widgets) {
                            if (w.id == DashboardLayout::WidgetId::DemoTracker) {
                                w.zone = showDemoTracker ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
                            }
                        }
                    });
                }

                bool showPreviousGames = ctx.config.show_previous_games_summary;
                std::string showPreviousGamesLabel = "Show Previous Games Summary##" + idSuffix;
                if (ImGui::Checkbox(showPreviousGamesLabel.c_str(), &showPreviousGames)) {
                    Config::Update([showPreviousGames](ConfigData& c) {
                        c.show_previous_games_summary = showPreviousGames;
                        for (auto& w : c.dashboard_layout.widgets) {
                            if (w.id == DashboardLayout::WidgetId::PreviousGames) {
                                w.zone = showPreviousGames ? DashboardLayout::Zone::Top : DashboardLayout::Zone::Hidden;
                            }
                        }
                    });
                }
            }
            if (section == 4) {
                ImGui::TextWrapped("Options for Previous Games, Streaks, and Gamemode Breakdown. Enable these cards under Visible cards.");
                ImGui::BeginDisabled(!ctx.config.show_previous_games_summary);
                static const int matchCounts[] = {10, 20, 30, 40, 50};
                const int matchCountCount = static_cast<int>(sizeof(matchCounts) / sizeof(matchCounts[0]));
                int currentMatchCountIdx = 1;
                for (int i = 0; i < matchCountCount; ++i) {
                    if (ctx.config.previous_games_limit == matchCounts[i]) {
                        currentMatchCountIdx = i;
                        break;
                    }
                }
                std::string previousGamesLimitLabel = "Matches to Show##" + idSuffix;
                const char* matchCountLabels[] = {"10", "20", "30", "40", "50"};
                if (ImGui::Combo(previousGamesLimitLabel.c_str(), &currentMatchCountIdx, matchCountLabels, matchCountCount)) {
                    int newLimit = matchCounts[currentMatchCountIdx];
                    Config::Update([newLimit](ConfigData& c) { c.previous_games_limit = newLimit; });
                    if (ctx.db) {
                        std::string effectivePrimary = ctx.snap->myPrimaryId.empty() ? ctx.config.last_primary_id : ctx.snap->myPrimaryId;
                        ctx.db->AsyncGetRecentMatchHistory(effectivePrimary, newLimit);
                    }
                }
                ImGui::EndDisabled();
            }

            if (section == 2) {
                bool showStreaks = ctx.config.show_streaks_stats;
                std::string showStreaksLabel = "Show Streaks & Stats##" + idSuffix;
                if (ImGui::Checkbox(showStreaksLabel.c_str(), &showStreaks)) {
                    Config::Update([showStreaks](ConfigData& c) {
                        c.show_streaks_stats = showStreaks;
                        for (auto& w : c.dashboard_layout.widgets) {
                            if (w.id == DashboardLayout::WidgetId::StreaksStats) {
                                w.zone = showStreaks ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
                            }
                        }
                    });
                }
            }
            if (section == 4) {
                ImGui::BeginDisabled(!ctx.config.show_streaks_stats);
                bool showLongestLoss = ctx.config.show_longest_loss_streak;
                std::string showLongestLossLabel = "Show Longest Loss Streak##" + idSuffix;
                if (ImGui::Checkbox(showLongestLossLabel.c_str(), &showLongestLoss)) {
                    Config::Update([showLongestLoss](ConfigData& c) { c.show_longest_loss_streak = showLongestLoss; });
                }
                ImGui::EndDisabled();
            }
            if (section == 2) {
                bool showGamemode = ctx.config.show_gamemode_breakdown;
                std::string showGamemodeLabel = "Show Gamemode Breakdown##" + idSuffix;
                if (ImGui::Checkbox(showGamemodeLabel.c_str(), &showGamemode)) {
                    Config::Update([showGamemode](ConfigData& c) {
                        c.show_gamemode_breakdown = showGamemode;
                        for (auto& w : c.dashboard_layout.widgets) {
                            if (w.id == DashboardLayout::WidgetId::GamemodeBreakdown) {
                                w.zone = showGamemode ? DashboardLayout::Zone::Right : DashboardLayout::Zone::Hidden;
                            }
                        }
                    });
                }
            }

            if (section == 4) {
                ImGui::BeginDisabled(!ctx.config.show_gamemode_breakdown);
                bool show1v1 = ctx.config.show_gamemode_record_1v1;
                std::string show1v1Label = "Include 1v1 Records##" + idSuffix;
                if (ImGui::Checkbox(show1v1Label.c_str(), &show1v1)) {
                    Config::Update([show1v1](ConfigData& c) { c.show_gamemode_record_1v1 = show1v1; });
                }
                bool show2v2 = ctx.config.show_gamemode_record_2v2;
                std::string show2v2Label = "Include 2v2 Records##" + idSuffix;
                if (ImGui::Checkbox(show2v2Label.c_str(), &show2v2)) {
                    Config::Update([show2v2](ConfigData& c) { c.show_gamemode_record_2v2 = show2v2; });
                }
                bool show3v3 = ctx.config.show_gamemode_record_3v3;
                std::string show3v3Label = "Include 3v3 Records##" + idSuffix;
                if (ImGui::Checkbox(show3v3Label.c_str(), &show3v3)) {
                    Config::Update([show3v3](ConfigData& c) { c.show_gamemode_record_3v3 = show3v3; });
                }

                static const char* scopes[] = {"Current Session", "All-Time"};
                GamemodeBreakdownScope currentScope = ScopeFromConfigString(ctx.config.gamemode_breakdown_scope);
                int currentScopeIdx = (currentScope == GamemodeBreakdownScope::AllTime) ? 1 : 0;

                std::string scopeLabel = "Scope##" + idSuffix;
                ImGui::SetNextItemWidth(150.0f * ctx.dpiScale);
                if (ImGui::Combo(scopeLabel.c_str(), &currentScopeIdx, scopes, 2)) {
                    std::string newScopeStr = (currentScopeIdx == 1) ? "all_time" : "current_session";
                    Config::Update([&newScopeStr](ConfigData& c) {
                        c.gamemode_breakdown_scope = newScopeStr;
                    });
                }
                ImGui::EndDisabled();
            }

            ImGui::Spacing();
        }

        if (m_page == Page::Shortcuts) {
            const int section = selectSection("Keyboard\0Controller\0Controller diagnostics\0");
            if (section == 0) {
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Keyboard Binds");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("Select a shortcut to change it, then press a key. Esc cancels. Clear removes a shortcut.");
                ImGui::Spacing();

                renderKeyboardBind(BindCaptureTarget::KeyOverlay, "Show Overlay", ctx.config.key_overlay);
                renderKeyboardBind(BindCaptureTarget::KeyCycle, "Cycle MMR", ctx.config.key_cycle);
                renderKeyboardBind(BindCaptureTarget::KeyExpand, "Expand View", ctx.config.key_expand);
                renderKeyboardBind(BindCaptureTarget::KeySession, "Session View", ctx.config.key_session);
                renderKeyboardBind(BindCaptureTarget::KeyMenu, "Toggle Settings", ctx.config.key_menu);
            }
            if (section == 1) {

                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Controller Binds");
                ImGui::Separator();
                ImGui::Spacing();

                renderControllerBind(BindCaptureTarget::GamepadOverlay, "Show Overlay", ctx.config.gamepad_overlay, ctx.config.gamepad_overlay_raw, ctx.config.gamepad_overlay_raw_button);
                renderControllerBind(BindCaptureTarget::GamepadCycle, "Cycle MMR", ctx.config.gamepad_cycle, ctx.config.gamepad_cycle_raw, ctx.config.gamepad_cycle_raw_button);
                renderControllerBind(BindCaptureTarget::GamepadExpand, "Expand View", ctx.config.gamepad_expand, ctx.config.gamepad_expand_raw, ctx.config.gamepad_expand_raw_button);
                renderControllerBind(BindCaptureTarget::GamepadSession, "Session View", ctx.config.gamepad_session, ctx.config.gamepad_session_raw, ctx.config.gamepad_session_raw_button);
                renderControllerBind(BindCaptureTarget::GamepadMenu, "Toggle Settings", ctx.config.gamepad_menu, ctx.config.gamepad_menu_raw, ctx.config.gamepad_menu_raw_button);
            }

            ImGui::Spacing();
            if (section == 2) {

                const bool connected = ctx.state.ui.controllerConnected.load();
                if (connected) {
                    // Controller name
                    std::string debugName;
                    {
                        std::lock_guard<std::mutex> lock(ctx.state.ui.controllerDebugMutex);
                        debugName = ctx.state.ui.controllerDebugName;
                    }
                    ImGui::Text("Detected Controller: %s", debugName.c_str());

                    // SDL GameController status
                    const bool isGameCtrl = ctx.state.ui.controllerIsGameController.load();
                    ImGui::Text("SDL GameController: %s", isGameCtrl ? "yes" : "no (fallback joystick)");

                    // Current overlay bind
                    std::string bindName = GetGamepadBindName(ctx.config.gamepad_overlay, ctx.config.gamepad_overlay_raw, ctx.config.gamepad_overlay_raw_button);
                    ImGui::Text("Current overlay bind: %s", bindName.c_str());

                    // Last pressed button
                    int lastBtn = isGameCtrl ? ctx.state.ui.lastControllerButtonPressed.load() : -1;
                    int lastRawBtn = ctx.state.ui.lastRawControllerButtonPressed.load();
                    if (lastBtn >= 0) {
                        ImGui::Text("Last mapped button: %s (%d)", GetGamepadButtonName(lastBtn) ? GetGamepadButtonName(lastBtn) : "Unknown", lastBtn);
                    } else if (isGameCtrl) {
                        ImGui::TextColored(Format::C(ctx.config.themeDim), "Last mapped button: (none)");
                    }
                    if (lastRawBtn >= 0) {
                        ImGui::Text("Last raw button: %d", lastRawBtn);
                    } else {
                        ImGui::TextColored(Format::C(ctx.config.themeDim), "Last raw button: (none)");
                    }
                } else {
                    ImGui::TextColored(Format::C(ctx.config.themeDim), "No controller detected.");
                }
            }

            ImGui::Spacing();
        }

        if (m_page == Page::Appearance) {
            const int section = selectSection("Colors\0Size & units\0");
            if (section == 1) {
                ImGui::Spacing();
                static constexpr float kScalePresets[] = {1.00f, 1.25f, 1.50f};
                int scaleIdx = 0;
                for (int i = 0; i < 3; ++i) {
                    if (fabsf(ctx.config.ui_scale - kScalePresets[i]) < 0.01f) {
                        scaleIdx = i;
                        break;
                    }
                }
                static const char* scaleLabels[] = {"100%", "125%", "150%"};
                if (ImGui::Combo("App-wide text size", &scaleIdx, scaleLabels, 3)) {
                    Config::Update([scale = kScalePresets[scaleIdx]](ConfigData& c) { c.ui_scale = scale; });
                    ctx.config = Config::Read();
                    ThemeManager::Apply(ctx.config, ctx.dpiScale);
                    styleChanged = true;
                }
                ImGui::TextWrapped("Changes text size throughout the dashboard and overlay, not just settings.");
            }
            if (section == 0) {
                ImGui::Spacing();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Colors");
                ImGui::Separator();
                ImGui::Spacing();

                ColorRGBA tempBg = ctx.config.themeBg;
                ColorRGBA tempSettingsPanel = ctx.config.themeSettingsPanel;
                ColorRGBA tempText = ctx.config.themeText;
                ColorRGBA tempAccent = ctx.config.themeAccent;
                ColorRGBA tempWin = ctx.config.themeWin;
                ColorRGBA tempLoss = ctx.config.themeLoss;
                ColorRGBA tempDim = ctx.config.themeDim;
                ColorRGBA tempMuted = ctx.config.themeMuted;
                ColorRGBA tempGraphLine = ctx.config.themeGraphLine;
                ColorRGBA tempGraphBaseline = ctx.config.themeGraphBaseline;

                auto colorPicker = [&](const char* label, ColorRGBA& color) -> bool {
                    std::string pickerLabel = std::string(label) + "##" + idSuffix;
                    return ImGui::ColorEdit4(pickerLabel.c_str(), &color.r,
                                             ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
                };

                bool changed = false;

                if (ImGui::BeginTable("AppearanceGrid", 2, ImGuiTableFlags_None)) {
                    ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthStretch);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Settings background", tempBg);
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Settings panels", tempSettingsPanel);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Text Color", tempText);
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Accent Color", tempAccent);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Loss Color", tempLoss);
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Dim Text", tempDim);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Muted Info", tempMuted);
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Graph Line", tempGraphLine);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    changed |= colorPicker("Graph Baseline", tempGraphBaseline);

                    ImGui::EndTable();
                }

                if (changed) {
                    Config::Update([&](ConfigData& c) {
                        c.themeBg = tempBg;
                        c.themeSettingsPanel = tempSettingsPanel;
                        c.themeText = tempText;
                        c.themeAccent = tempAccent;
                        c.themeWin = tempWin;
                        c.themeLoss = tempLoss;
                        c.themeDim = tempDim;
                        c.themeMuted = tempMuted;
                        c.themeGraphLine = tempGraphLine;
                        c.themeGraphBaseline = tempGraphBaseline;
                    });
                    ctx.config = Config::Read();
                    ThemeManager::Apply(ctx.config, ctx.dpiScale);
                    styleChanged = true;
                }
            }
            if (section == 1) {

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Units & Formatting");
                ImGui::Dummy(ImVec2(0, 4));

                int speedUnits = ctx.config.imperial_units ? 1 : 0;
                if (ImGui::Combo("Speed units", &speedUnits, "Kilometers per hour\0Miles per hour\0")) {
                    Config::Update([speedUnits](ConfigData& c) { c.imperial_units = speedUnits == 1; });
                }

                static const char* crossbarModes[] = {"Raw Force (UU/s)", "Speed (MPH/KPH)"};
                int currentCrossbarMode = (ctx.config.crossbar_display_mode == "speed") ? 1 : 0;
                std::string crossbarLabel = "Crossbar Hit Display##" + idSuffix;
                ImGui::SetNextItemWidth(180.0f * ctx.dpiScale);
                if (ImGui::Combo(crossbarLabel.c_str(), &currentCrossbarMode, crossbarModes, 2)) {
                    Config::Update([&](ConfigData& c) {
                        c.crossbar_display_mode = (currentCrossbarMode == 1) ? "speed" : "raw";
                    });
                }

                bool useRoman = ctx.config.use_roman_numerals;
                std::string useRomanLabel = "Use Roman Numerals (I, II, III)##" + idSuffix;
                if (ImGui::Checkbox(useRomanLabel.c_str(), &useRoman)) {
                    Config::Update([useRoman](ConfigData& c) { c.use_roman_numerals = useRoman; });
                    styleChanged = true;
                }
            }
        }

        if (m_page == Page::Integrations) {
            const int section = selectSection("Replay uploads\0Replay saving\0External services\0");
            if (section == 0) {
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Ballchasing Uploader");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("Upload saved replay files to your Ballchasing account. Add your API token from ballchasing.com.");

                if (ctx.pendingBallchasingToken && ctx.pendingBallchasingToken->empty() && !ctx.config.ballchasing_token.empty()) {
                    *ctx.pendingBallchasingToken = ctx.config.ballchasing_token;
                }

                char tokenBuf[256] = {0};
                strcpy_s(tokenBuf, sizeof(tokenBuf),
                         (ctx.pendingBallchasingToken ? ctx.pendingBallchasingToken->c_str() : ""));

                std::string tokenInputLabel = "API Token##" + idSuffix;
                if (ImGui::InputText(tokenInputLabel.c_str(), tokenBuf, sizeof(tokenBuf),
                                     m_showReplayToken ? ImGuiInputTextFlags_None : ImGuiInputTextFlags_Password)) {
                    if (ctx.pendingBallchasingToken) {
                        *ctx.pendingBallchasingToken = std::string(tokenBuf);
                        Config::Update([&](ConfigData& c) { c.ballchasing_token = *ctx.pendingBallchasingToken; });
                    }
                }
                ImGui::Checkbox("Show token", &m_showReplayToken);
                if (ctx.config.ballchasing_token.empty()) {
                    ImGui::TextWrapped("An API token is required before replay uploads can succeed.");
                }

                bool autoUpload = ctx.config.auto_upload_replays;
                std::string autoUploadLabel = "Auto-upload new replays##" + idSuffix;
                if (ImGui::Checkbox(autoUploadLabel.c_str(), &autoUpload)) {
                    if (autoUpload && !ctx.config.ballchasing_upload_notice_accepted) {
                        ImGui::OpenPopup("Replay Upload Privacy Warning");
                    } else {
                        Config::Update([autoUpload](ConfigData& c) { c.auto_upload_replays = autoUpload; });
                    }
                }

                static const char* visibilityOptions[] = {"private", "unlisted", "public"};
                int currentVisibility = 1;
                for (int i = 0; i < 3; ++i) {
                    if (ctx.config.ballchasing_visibility == visibilityOptions[i]) {
                        currentVisibility = i;
                        break;
                    }
                }
                std::string visibilityLabel = "Upload Visibility##" + idSuffix;
                ImGui::SetNextItemWidth(140.0f * ctx.dpiScale);
                if (ImGui::Combo(visibilityLabel.c_str(), &currentVisibility, visibilityOptions, 3)) {
                    std::string newVisibility = visibilityOptions[currentVisibility];
                    Config::Update([newVisibility](ConfigData& c) { c.ballchasing_visibility = newVisibility; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Default is unlisted. Public uploads may be discoverable on Ballchasing.");
                }

                const float noticeWidth = fminf(540.0f * ctx.dpiScale, ImGui::GetIO().DisplaySize.x - 40.0f * ctx.dpiScale);
                ImGui::SetNextWindowSizeConstraints(ImVec2(noticeWidth, 0.0f),
                                                    ImVec2(noticeWidth, ImGui::GetIO().DisplaySize.y - 40.0f * ctx.dpiScale));
                if (ImGui::BeginPopupModal("Replay Upload Privacy Warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextColored(Format::C(ctx.config.themeAccent), "Replay uploads send data to Ballchasing.com");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Rocket League replay files can include player names, platform IDs, match timestamps, teams, scores, gameplay events, and other participants in the match.");
                    ImGui::TextWrapped("Only enable this if you understand that replay data leaves your PC and is handled by Ballchasing under its own terms and privacy policy.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Current visibility: %s", visibilityOptions[currentVisibility]);
                    if (ctx.config.ballchasing_token.empty()) {
                        ImGui::TextColored(Format::C(ctx.config.themeLoss), "Add a Ballchasing API token before uploads can succeed.");
                    }
                    ImGui::Spacing();
                    if (ImGui::Button("Enable Replay Uploads")) {
                        std::string newVisibility = visibilityOptions[currentVisibility];
                        Config::Update([newVisibility](ConfigData& c) {
                            c.ballchasing_upload_notice_accepted = true;
                            c.ballchasing_visibility = newVisibility;
                            c.auto_upload_replays = true;
                        });
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(90.0f * ctx.dpiScale, 0.0f))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
            if (section == 1) {

                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Auto-Save Replays (EAC-Friendly)");
                ImGui::Separator();
                ImGui::Spacing();

                bool autoSave = ctx.config.auto_save_replays;
                std::string autoSaveLabel = "Auto-save replays##" + idSuffix;
                if (ImGui::Checkbox(autoSaveLabel.c_str(), &autoSave)) {
                    Config::Update([autoSave](ConfigData& c) { c.auto_save_replays = autoSave; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Automatically triggers your Rocket League Save Replay keybind after a match.");
                }
                if (autoSave) {
                    renderKeyboardBind(BindCaptureTarget::KeySaveReplay, "Save Replay Bind", ctx.config.key_save_replay);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Matches the Save Replay bind in Rocket League.");
                    }
                }
            }
            if (section == 2) {

                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "External Services");
                ImGui::Separator();
                ImGui::Spacing();

                bool discordRpc = ctx.config.discord_rpc_enabled;
                std::string rpcLabel = "Discord Rich Presence##" + idSuffix;
                if (ImGui::Checkbox(rpcLabel.c_str(), &discordRpc)) {
                    Config::Update([discordRpc](ConfigData& c) { c.discord_rpc_enabled = discordRpc; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Changing this may require an app restart to reconnect Discord RPC.");
                }

                bool mmrTracking = ctx.config.enable_mmr_tracking;
                std::string mmrTrackingLabel = "Enable MMR Tracking (Tracker.gg)##" + idSuffix;
                if (ImGui::Checkbox(mmrTrackingLabel.c_str(), &mmrTracking)) {
                    Config::Update([mmrTracking](ConfigData& c) { c.enable_mmr_tracking = mmrTracking; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Sends lobby player names/platform IDs to Tracker.gg for rank lookup.");
                }
            }
        }

        if (m_page == Page::Data) {

            ImGui::TextWrapped("Startup diagnostics are required after accepting the privacy notice. Match data and player names are not included.");
            ImGui::Spacing();
            bool crashReports = ctx.config.crash_reports_enabled;
            std::string crashReportsLabel = "Upload Crash Reports##" + idSuffix;
            if (ImGui::Checkbox(crashReportsLabel.c_str(), &crashReports)) {
                Config::Update([crashReports](ConfigData& c) { c.crash_reports_enabled = crashReports; });
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, pending minidumps are uploaded on next startup for crash diagnosis.");
            }
            ImGui::TextWrapped("Optional. Pending crash dumps are sent on the next startup.");

            ImGui::Spacing();
            ImGui::TextColored(Format::C(ctx.config.themeAccent), "Local Data");
            ImGui::Separator();
            ImGui::Spacing();

            static std::string localDataStatus;
            std::string dataPath = Storage::GetDataDirectory();
            if (ImGui::Button("Open Data Folder")) {
                ShellExecuteA(NULL, "open", dataPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Opens the folder containing config, database, logs, exports, and crash dump files.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Export Local Data")) {
                if (ctx.db) {
                    std::string exportPath;
                    std::string error;
                    if (ctx.db->ExportLocalData(exportPath, error)) {
                        localDataStatus = "Exported local history.";
                        std::string explorerArg = "/select,\"" + exportPath + "matches.json\"";
                        ShellExecuteA(NULL, "open", "explorer.exe", explorerArg.c_str(), NULL, SW_SHOWNORMAL);
                    } else {
                        localDataStatus = "Export failed: " + error;
                    }
                } else {
                    localDataStatus = "Export failed: database is unavailable.";
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Exports match history as JSON and CSV files.");
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextWrapped("Delete local history and your saved account identity. This cannot be undone. Settings and tokens are kept.");
            if (ImGui::Button("Delete History & Identity")) {
                ImGui::OpenPopup("Delete Local History");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Deletes local match history and clears saved local player identity.");
            }

            if (!localDataStatus.empty()) {
                ImGui::TextColored(Format::C(ctx.config.themeDim), "%s", localDataStatus.c_str());
            }

            const float noticeWidth = fminf(540.0f * ctx.dpiScale, ImGui::GetIO().DisplaySize.x - 40.0f * ctx.dpiScale);
            ImGui::SetNextWindowSizeConstraints(ImVec2(noticeWidth, 0.0f),
                                                ImVec2(noticeWidth, ImGui::GetIO().DisplaySize.y - 40.0f * ctx.dpiScale));
            if (ImGui::BeginPopupModal("Delete Local History", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextColored(Format::C(ctx.config.themeLoss), "Delete local match history and saved identity?");
                ImGui::Spacing();
                ImGui::TextWrapped("This removes saved matches, exported history source files, and your selected local account identity. Settings and tokens are kept.");
                ImGui::Spacing();
                if (ImGui::Button("Delete", ImVec2(110.0f * ctx.dpiScale, 0.0f))) {
                    std::string error;
                    bool deleted = ctx.db && ctx.db->DeleteLocalMatchHistory(error);
                    if (deleted) {
                        DeleteFileA((Storage::GetDataDirectory() + "matches.jsonl").c_str());
                        DeleteFileA((Storage::GetDataDirectory() + "mmr_history.jsonl").c_str());
                        {
                            std::unique_lock<std::shared_mutex> gameLock(ctx.state.game.mutex);
                            ctx.state.game.myPrimaryId = "";
                            ctx.state.game.myTeam = -1;
                        }
                        {
                            std::unique_lock<std::shared_mutex> historyLock(ctx.state.history.mutex);
                            ctx.state.history.lifetimeMmrX.clear();
                            ctx.state.history.lifetimeMmrY.clear();
                            ctx.state.history.recentSavedMatches.clear();
                            ctx.state.history.recentSavedMatchesLoaded = true;
                            ctx.state.history.version++;
                        }
                        Config::Update([](ConfigData& c) {
                            c.last_primary_id = "";
                            c.known_primary_ids.clear();
                        });
                        ctx.state.ui.dbStatsDirty.store(true);
                        localDataStatus = "Deleted local history and identity.";
                    } else {
                        localDataStatus = "Delete failed: " + (error.empty() ? "database is unavailable." : error);
                    }
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(90.0f * ctx.dpiScale, 0.0f))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        if (m_page == Page::Troubleshooting) {
            const int section = selectSection("Game connection\0Diagnostic logs\0");
            if (section == 1) {
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Diagnostics & Logs");
                ImGui::Separator();
                ImGui::Spacing();

                bool debugLogging = ctx.config.debug_logging;
                std::string debugLoggingLabel = "Verbose Debug Logging##" + idSuffix;
                if (ImGui::Checkbox(debugLoggingLabel.c_str(), &debugLogging)) {
                    Config::Update([debugLogging](ConfigData& c) { c.debug_logging = debugLogging; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Off by default. When off, logs redact player IDs, replay paths, UUIDs, service responses, and usernames.");
                }

                std::string logPath = Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
                if (ImGui::Button("Show Log File")) {
                    std::string explorerArg = "/select,\"" + logPath + "\"";
                    ShellExecuteA(NULL, "open", "explorer.exe", explorerArg.c_str(), NULL, SW_SHOWNORMAL);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Opens the log folder and selects the current log file.");
                }
            }
            if (section == 0) {

                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Rocket League connection");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("OmniStats reads live stats through Rocket League's local Stats API. Check the configuration if match data is missing.");

                bool checkStatsApiOnStartup = ctx.config.check_stats_api_config_on_startup;
                std::string checkStatsApiLabel = "Check Stats API Config on Startup##" + idSuffix;
                if (ImGui::Checkbox(checkStatsApiLabel.c_str(), &checkStatsApiOnStartup)) {
                    Config::Update([checkStatsApiOnStartup](ConfigData& c) { c.check_stats_api_config_on_startup = checkStatsApiOnStartup; });
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Verify Rocket League's local telemetry Stats API settings at launch.");
                }

                StatsApiConfig::CheckResult result;
                {
                    std::lock_guard<std::mutex> lock(ctx.state.ui.statsApiMutex);
                    result = ctx.state.ui.statsApiResult;
                }

                ImGui::Text("Detected Path: %s", result.path.empty() ? "(none)" : result.path.c_str());

                ImVec4 statusCol = Format::C(ctx.config.themeDim);
                if (result.status == StatsApiConfig::Status::Valid) {
                    statusCol = Format::C(ctx.config.themeWin);
                } else if (result.status != StatsApiConfig::Status::NotFound) {
                    statusCol = Format::C(ctx.config.themeLoss);
                }
                ImGui::Text("Status: ");
                ImGui::SameLine();
                ImGui::TextColored(statusCol, "%s", StatsApiConfig::GetStatusMessage(result.status).c_str());

                if (result.status != StatsApiConfig::Status::Valid && !result.message.empty()) {
                    ImGui::TextWrapped("%s", result.message.c_str());
                }
                if (result.status != StatsApiConfig::Status::Valid && result.rlRunning) {
                    ImGui::TextColored(Format::C(ctx.config.themeAccent), "Restart Rocket League after fixing for changes to take effect.");
                }

                if (ImGui::Button("Check Again##StatsApi")) {
                    std::string path = ctx.config.rocket_league_stats_api_config_path;
                    if (path.empty()) {
                        path = StatsApiConfig::DetectConfigPath();
                    }
                    auto newResult = StatsApiConfig::VerifyConfig(path, ctx.config.port);
                    {
                        std::lock_guard<std::mutex> lock(ctx.state.ui.statsApiMutex);
                        ctx.state.ui.statsApiResult = newResult;
                    }
                }
                if (result.status != StatsApiConfig::Status::Valid && !result.path.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Fix Config##StatsApi")) {
                        const bool repaired = ExternalUpdaterLauncher::RepairStatsApiConfig(result.path, ctx.config.port);
                        auto newResult = StatsApiConfig::VerifyConfig(result.path, ctx.config.port);
                        if (!repaired || newResult.status != StatsApiConfig::Status::Valid) {
                            newResult.message = "Failed to fix automatically. Please approve the updater elevation prompt if shown, or manually edit:\n" + result.path + "\nto set PacketSendRate=30 and Port=" + std::to_string(ctx.config.port);
                        }
                        {
                            std::lock_guard<std::mutex> lock(ctx.state.ui.statsApiMutex);
                            ctx.state.ui.statsApiResult = newResult;
                        }
                    }
                }

                ImGui::Spacing();
                static std::string pathInputError;
                char pathBuf[512] = {0};
                strcpy_s(pathBuf, sizeof(pathBuf), ctx.config.rocket_league_stats_api_config_path.c_str());
                std::string pathFieldLabel = "Manual Config Path##" + idSuffix;
                if (ImGui::InputText(pathFieldLabel.c_str(), pathBuf, sizeof(pathBuf))) {
                    std::string newPath = pathBuf;
                    size_t first = newPath.find_first_not_of(" \t\r\n");
                    size_t last = newPath.find_last_not_of(" \t\r\n");
                    if (first != std::string::npos && last != std::string::npos) {
                        newPath = newPath.substr(first, last - first + 1);
                    } else {
                        newPath = "";
                    }

                    bool validPath = true;
                    if (!newPath.empty()) {
                        std::filesystem::path p(newPath);
                        std::string filename = p.filename().string();
                        std::transform(filename.begin(), filename.end(), filename.begin(), ::tolower);
                        if (filename != "defaultstatsapi.ini") {
                            validPath = false;
                            pathInputError = "Filename must be exactly DefaultStatsAPI.ini";
                        } else if (!std::filesystem::exists(p)) {
                            validPath = false;
                            pathInputError = "File does not exist.";
                        } else if (!std::filesystem::exists(p.parent_path())) {
                            validPath = false;
                            pathInputError = "Parent directory does not exist.";
                        } else {
                            pathInputError = "";
                        }
                    } else {
                        pathInputError = "";
                    }

                    if (validPath) {
                        Config::Update([newPath](ConfigData& c) {
                            c.rocket_league_stats_api_config_path = newPath;
                        });
                        std::string actualPath = newPath.empty() ? StatsApiConfig::DetectConfigPath() : newPath;
                        auto newResult = StatsApiConfig::VerifyConfig(actualPath, ctx.config.port);
                        {
                            std::lock_guard<std::mutex> lock(ctx.state.ui.statsApiMutex);
                            ctx.state.ui.statsApiResult = newResult;
                        }
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Manually point to your DefaultStatsAPI.ini in the Rocket League installation directory if auto-detection fails.");
                }
                if (!pathInputError.empty()) {
                    ImGui::TextColored(Format::C(ctx.config.themeLoss), "%s", pathInputError.c_str());
                }

                ImGui::Spacing();
            }
        }

        if (m_page == Page::Ranks) {
            const int section = selectSection("Player ranks\0Personal graph\0Your ranks\0");
            if (section == 0) {
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Live Match MMR");
                ImGui::TextWrapped("Choose which playlist rank appears next to players in the live overlay.");

                std::vector<std::pair<const char*, MmrCategory>> liveCategories = {
                    {"Best Rank", MmrCategory::Best},
                    {"1v1", MmrCategory::OneVOne},
                    {"2v2", MmrCategory::TwoVTwo},
                    {"3v3", MmrCategory::ThreeVThree},
                    {"Casual", MmrCategory::Casual},
                    {"Tournament", MmrCategory::Tourny}};
                if (ctx.config.show_extra_playlists) {
                    liveCategories.insert(
                        liveCategories.end(),
                        {{"Hoops", MmrCategory::Hoops},
                         {"Rumble", MmrCategory::Rumble},
                         {"Dropshot", MmrCategory::Dropshot},
                         {"Snow Day", MmrCategory::SnowDay},
                         {"Heatseeker", MmrCategory::Heatseeker}});
                }
                std::vector<const char*> liveLabels;
                for (const auto& category : liveCategories) {
                    liveLabels.push_back(category.first);
                }
                int currentLiveCategory = 0;
                const MmrCategory loadedLiveCategory =
                    StringToMmrCategory(ctx.config.mmr_category);
                for (int i = 0; i < static_cast<int>(liveCategories.size()); ++i) {
                    if (liveCategories[i].second == loadedLiveCategory) {
                        currentLiveCategory = i;
                        break;
                    }
                }
                std::string liveCategoryLabel =
                    "Player MMR category##" + idSuffix;
                if (ImGui::Combo(
                        liveCategoryLabel.c_str(),
                        &currentLiveCategory,
                        liveLabels.data(),
                        static_cast<int>(liveLabels.size()))) {
                    const MmrCategory selected =
                        liveCategories[currentLiveCategory].second;
                    Config::Update([selected](ConfigData& c) {
                        c.mmr_category = MmrCategoryToString(selected);
                    });
                    ctx.state.ui.rosterMmrCategory.store(selected);
                }

                bool autoSwitchMmrCategory =
                    ctx.config.auto_switch_mmr_category;
                std::string autoSwitchMmrCategoryLabel =
                    "Automatically follow current playlist##Live" + idSuffix;
                if (ImGui::Checkbox(
                        autoSwitchMmrCategoryLabel.c_str(),
                        &autoSwitchMmrCategory)) {
                    Config::Update([autoSwitchMmrCategory](ConfigData& c) {
                        c.auto_switch_mmr_category = autoSwitchMmrCategory;
                    });
                }
                ImGui::TextWrapped("When enabled, the live ranks switch to the playlist you are playing.");
            }
            if (section == 1) {

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Personal MMR Graph");
                ImGui::TextWrapped("Your session and lifetime graph has its own playlist selection. It does not change the player ranks above.");

                bool graphFollowCurrentPlaylist =
                    ctx.config.graph_follow_current_playlist;
                std::string graphFollowLabel =
                    "Follow current playlist##Graph" + idSuffix;
                if (ImGui::Checkbox(
                        graphFollowLabel.c_str(),
                        &graphFollowCurrentPlaylist)) {
                    Config::Update([graphFollowCurrentPlaylist](ConfigData& c) {
                        c.graph_follow_current_playlist =
                            graphFollowCurrentPlaylist;
                    });
                    if (!graphFollowCurrentPlaylist) {
                        ctx.state.ui.graphMmrCategory.store(
                            StringToMmrCategory(
                                ctx.config.graph_mmr_category));
                    }
                }

                std::vector<std::pair<const char*, MmrCategory>> graphCategories = {
                    {"1v1", MmrCategory::OneVOne},
                    {"2v2", MmrCategory::TwoVTwo},
                    {"3v3", MmrCategory::ThreeVThree},
                    {"Casual", MmrCategory::Casual},
                    {"Tournament", MmrCategory::Tourny}};
                if (ctx.config.show_extra_playlists) {
                    graphCategories.insert(
                        graphCategories.begin() + 3,
                        {{"Hoops", MmrCategory::Hoops},
                         {"Rumble", MmrCategory::Rumble},
                         {"Dropshot", MmrCategory::Dropshot},
                         {"Snow Day", MmrCategory::SnowDay},
                         {"Heatseeker", MmrCategory::Heatseeker}});
                }
                std::vector<const char*> graphLabels;
                for (const auto& category : graphCategories) {
                    graphLabels.push_back(category.first);
                }
                int currentGraphCategory = 1;
                const MmrCategory loadedGraphCategory =
                    StringToMmrCategory(ctx.config.graph_mmr_category);
                for (int i = 0; i < static_cast<int>(graphCategories.size()); ++i) {
                    if (graphCategories[i].second == loadedGraphCategory) {
                        currentGraphCategory = i;
                        break;
                    }
                }
                std::string graphCategoryLabel =
                    "Default graph category##" + idSuffix;
                if (ImGui::Combo(
                        graphCategoryLabel.c_str(),
                        &currentGraphCategory,
                        graphLabels.data(),
                        static_cast<int>(graphLabels.size()))) {
                    const MmrCategory selected =
                        graphCategories[currentGraphCategory].second;
                    Config::Update([selected](ConfigData& c) {
                        c.graph_mmr_category =
                            MmrCategoryToString(selected);
                    });
                    ctx.state.ui.graphMmrCategory.store(selected);
                }
                ImGui::TextWrapped("The default graph playlist is used when no supported playlist is being followed.");
            }
            if (section == 0) {

                bool showExtraPlaylists = ctx.config.show_extra_playlists;
                std::string showExtraPlaylistsLabel = "Show Extra Playlists##" + idSuffix;
                if (ImGui::Checkbox(showExtraPlaylistsLabel.c_str(), &showExtraPlaylists)) {
                    Config::Update([showExtraPlaylists](ConfigData& c) {
                        c.show_extra_playlists = showExtraPlaylists;
                        if (!showExtraPlaylists &&
                            IsExtraMmrCategory(
                                StringToMmrCategory(c.mmr_category))) {
                            c.mmr_category = "best";
                        }
                        if (!showExtraPlaylists &&
                            IsExtraMmrCategory(
                                StringToMmrCategory(
                                    c.graph_mmr_category))) {
                            c.graph_mmr_category = "2v2";
                        }
                    });
                    if (!showExtraPlaylists) {
                        if (IsExtraMmrCategory(ctx.state.ui.rosterMmrCategory.load())) {
                            ctx.state.ui.rosterMmrCategory.store(MmrCategory::Best);
                        }
                        if (IsExtraMmrCategory(ctx.state.ui.graphMmrCategory.load())) {
                            ctx.state.ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
                        }
                    }
                }
                ImGui::TextWrapped("Adds Hoops, Rumble, Dropshot, Snow Day, and Heatseeker to playlist choices.");
            }
            if (section == 2) {
                ImGui::Spacing();
                ImGui::TextColored(Format::C(ctx.config.themeAccent), "Rank Distribution");
                ImGui::Separator();
                ImGui::Spacing();

                if (ctx.snap && !ctx.snap->myPrimaryId.empty() && ctx.snap->roster.count(ctx.snap->myPrimaryId)) {
                    const auto& myPlayer = ctx.snap->roster.at(ctx.snap->myPrimaryId);

                    std::vector<std::string> playlists = {"1v1", "2v2", "3v3", "casual", "t"};
                    if (ctx.config.show_extra_playlists) {
                        playlists.insert(playlists.end(), {"hoops", "rumble", "dropshot", "snowday"});
                    }

                    std::string rankTableLabel = "RankDistTable##" + idSuffix;
                    if (ImGui::BeginTable(rankTableLabel.c_str(), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Playlist", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Rank", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                        ImGui::TableSetupColumn("MMR", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Matches", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableHeadersRow();

                        for (const auto& playlist : playlists) {
                            ImGui::TableNextRow();
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", playlist.c_str());

                            ImGui::TableNextColumn();
                            auto tierIt = myPlayer.playlistTiers.find(playlist);
                            if (tierIt != myPlayer.playlistTiers.end()) {
                                ImVec4 rankColor = Format::RankColor(tierIt->second);
                                std::string formattedTier = Format::RankTier(tierIt->second, ctx.config.use_roman_numerals);
                                ImGui::TextColored(rankColor, "%s", formattedTier.c_str());
                            } else {
                                ImGui::TextColored(Format::FromColor(ctx.config.themeMuted), "Unranked");
                            }

                            ImGui::TableNextColumn();
                            auto mmrIt = myPlayer.playlists.find(playlist);
                            if (mmrIt != myPlayer.playlists.end() && mmrIt->second > 0) {
                                ImGui::Text("%d", mmrIt->second);
                            } else {
                                ImGui::TextColored(Format::C(ctx.config.themeMuted), "-");
                            }

                            ImGui::TableNextColumn();
                            auto matchesIt = myPlayer.playlistMatches.find(playlist);
                            if (matchesIt != myPlayer.playlistMatches.end() && matchesIt->second > 0) {
                                ImGui::Text("%d", matchesIt->second);
                            } else {
                                ImGui::TextColored(Format::C(ctx.config.themeMuted), "-");
                            }
                        }
                        ImGui::EndTable();
                    }
                } else {
                    ImGui::TextWrapped("Join a match to see your ranks. If your account is not detected, select it in General. Rank lookup can be enabled in Replays & services.");
                }
            }

            ImGui::Spacing();
        }

        ImGui::PopItemWidth();
        ImGui::PopTextWrapPos();
    }

    ImGui::EndChild();
    ImGui::PopID();
    settingsStorage->SetInt(categoryId, static_cast<int>(m_page));

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Help / Discord")) {
        ShellExecuteA(NULL, "open", "https://discord.gg/4KBW35ApvF", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    const float closeWidth = 66.0f * ctx.dpiScale;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - closeWidth);
    if (ImGui::Button("Done", ImVec2(closeWidth, 0.0f))) {
        finishBindCapture();
        ctx.state.ui.showMenu = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("You can also press %s to close settings.", GetKeyDisplayName(ctx.config.key_menu).c_str());
    }
    ImGui::PopID();
}
