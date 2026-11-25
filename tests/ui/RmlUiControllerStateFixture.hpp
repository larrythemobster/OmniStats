#pragma once

#include "ui/rml/widgets/TournamentWidget.hpp"
#include "core/state/LiveMatchStore.hpp"
#include <gtest/gtest.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/StringUtilities.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

#include "core/config/Config.hpp"
#include "core/accounts/AccountManager.hpp"
#include "network/omninet/OmniNet.hpp"
#include "ui/config/WidgetSettings.hpp"
#include "core/state/SessionState.hpp"
#include "utils/Storage.hpp"
#include "platform/InputManager.hpp"
#include "database/DatabaseManager.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/backend/RmlCapture.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/api/AccountClient.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"

namespace {
    std::vector<unsigned char> DecodePngBgra(const std::filesystem::path& path, UINT& width, UINT& height) {
        using Microsoft::WRL::ComPtr;
        std::vector<unsigned char> pixels;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICBitmapDecoder> decoder;
            ComPtr<IWICBitmapFrameDecode> frame;
            ComPtr<IWICFormatConverter> converter;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
                SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
                SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
                SUCCEEDED(converter->GetSize(&width, &height))) {
                pixels.resize(static_cast<size_t>(width) * height * 4);
                if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) pixels.clear();
            }
        }
        if (init == S_OK || init == S_FALSE) CoUninitialize();
        return pixels;
    }
}

class RmlUiControllerStateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        OmniNet::ResetUpdateRequiredForTesting();
        OmniNet::SetTransportForTesting({});
        original = Config::Read();
        Config::Update([](ConfigData& c) {
            c.last_primary_id.clear();
            c.graph_mmr_category = "2v2";
            c.graph_default_lifetime = false;
            c.show_running_indicator = false;
            c.second_monitor_mode = false;
            c.ballchasing_token.clear();
            c.onboarding_completed = true;
            c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
            c.terms_accepted_version = Config::CurrentTermsVersion;
            c.account_link_prompt_shown = true;
        },
                       true);
    }

    void TearDown() override {
        Config::Update([this](ConfigData& c) { c = original; }, true);
        OmniNet::ResetUpdateRequiredForTesting();
        OmniNet::SetTransportForTesting({});
        ExternalUpdaterLauncher::ShutdownBackgroundTasks();
    }

    const RmlRenderSnapshot& Snapshot(const RmlUiController& controller) const {
        return controller.m_snap;
    }
    float DpiScale(const RmlUiController& controller) const {
        return controller.m_dpiScale;
    }
    std::string EscapeText(std::string_view text) const {
        return RmlUiDetail::Escape(text);
    }
    std::string RenderGraph(RmlUiController& controller) {
        return controller.RenderMmrGraph(true, true);
    }
    std::string RenderCompactSession(RmlUiController& controller) {
        return controller.RenderSessionStats(true, false);
    }
    std::string RenderSessionWidget(RmlUiController& controller, bool dashboard) {
        return controller.RenderWidget(DashboardLayout::WidgetId::SessionStats, dashboard);
    }
    std::string RenderGamemodeWidget(RmlUiController& controller, bool dashboard) {
        return controller.RenderWidget(DashboardLayout::WidgetId::GamemodeBreakdown, dashboard);
    }
    std::string RenderFullSession(RmlUiController& controller) {
        return controller.RenderSessionStats(false, true);
    }
    std::string RenderSessionView(RmlUiController& controller) {
        return controller.RenderSessionView();
    }
    std::string RenderCardSettings(RmlUiController& controller) {
        controller.m_showCardDetails = true;
        return controller.RenderSettingsCards();
    }
    std::vector<std::string> IdentityValues(RmlUiController& controller) {
        std::vector<std::string> values;
        for (const auto& option : controller.IdentityOptions())
            values.push_back(option.value);
        return values;
    }
    std::vector<RmlUiDetail::SelectOption> IdentityChoices(RmlUiController& controller) {
        return controller.IdentityOptions();
    }
    Rml::ElementDocument* AccountsDocument(RmlUiController& controller) {
        return controller.m_accountsDoc;
    }
    std::string RenderIntegrationSettings(RmlUiController& controller) {
        return controller.RenderSettingsIntegrations();
    }
    std::string RenderDataSettings(RmlUiController& controller) {
        return controller.RenderSettingsData();
    }
    void BeginSaveReplayKeyCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::KeySaveReplay);
    }
    void CaptureMenuKey(RmlUiController& controller, const std::shared_ptr<SessionState>& state, int key) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::KeyMenu);
        state->ui.lastKeyboardKeyPressed.store(key);
        controller.UpdateInputCapture();
    }
    int ConfiguredMenuKey(const RmlUiController& controller) const {
        return controller.m_config.key_menu;
    }
    std::string SaveReplayBindTarget() const {
        return std::to_string(static_cast<int>(RmlUiController::BindCaptureTarget::KeySaveReplay));
    }
    std::string RenderRoster(RmlUiController& controller, int team = 0) {
        return controller.RenderPlayerRoster(team, team == 0 ? "BLUE" : "ORANGE");
    }
    std::string RenderRosterCard(RmlUiController& controller) {
        return controller.RenderWidget(DashboardLayout::WidgetId::LiveRoster, false);
    }
    std::string RenderWidgetHtml(RmlUiController& controller, DashboardLayout::WidgetId id, bool dashboard) {
        return controller.RenderWidget(id, dashboard);
    }
    std::string RenderLobbyRanks(RmlUiController& controller) {
        return controller.RenderLobbyRanks();
    }
    std::string RenderPreviousGames(RmlUiController& controller) {
        return controller.RenderPreviousGames();
    }
    std::string RenderDemoTracker(RmlUiController& controller) {
        return controller.RenderDemoTracker();
    }
    std::string RenderOverlayContainer(RmlUiController& controller, const OverlayLayout::ContainerConfig& container, bool editMode = false) {
        return controller.RenderOverlayContainer(container, editMode);
    }
    bool ValidateStatsPath(std::string input, std::string& normalized, std::string& error) {
        return RmlUiController::ValidateStatsApiPath(std::move(input), normalized, error);
    }
    void BeginOverlayKeyCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::KeyOverlay);
    }
    void BeginCycleControllerCapture(RmlUiController& controller) {
        controller.BeginBindCapture(RmlUiController::BindCaptureTarget::GamepadCycle);
    }
    void SetSettingsTransientState(RmlUiController& controller) {
        controller.m_showBallchasingToken = true;
        controller.m_confirmReplayUploads = true;
        controller.m_confirmDeleteHistory = true;
        controller.m_themeEditor.m_editColorKey = "theme_accent";
    }
    bool HasSettingsTransientState(const RmlUiController& controller) const {
        return controller.m_showBallchasingToken || controller.m_confirmReplayUploads ||
               controller.m_confirmDeleteHistory || !controller.m_themeEditor.m_editColorKey.empty();
    }
    void SetSettingsPage(RmlUiController& controller, int page) {
        controller.m_settingsPage = static_cast<RmlUiController::SettingsPage>(page);
    }
    Rml::Element* OverlayRoot(RmlUiController& controller) {
        return controller.Root("overlay-root");
    }
    Rml::Element* PromptRoot(RmlUiController& controller) {
        return controller.Root("prompt-root");
    }
    Rml::Element* SettingsRoot(RmlUiController& controller) {
        return controller.Root("settings-root");
    }
    Rml::Element* DashboardRoot(RmlUiController& controller) {
        return controller.Root("dashboard-root");
    }
    Rml::ElementDocument* Document(RmlUiController& controller) {
        return controller.m_document;
    }
    Rml::Context* RmlContext(RmlUiController& controller) {
        return controller.m_context;
    }
    void RefreshSettings(RmlUiController& controller) {
        controller.RebuildSettings();
        controller.Render();
    }
    void SimulateOverlayDragStart(RmlUiController& controller, const std::string& containerId, float x, float y) {
        controller.m_layoutDrag.m_drag = {};
        controller.m_layoutDrag.m_drag.kind = RmlUiController::DragKind::OverlayMove;
        controller.m_layoutDrag.m_drag.containerId = containerId;
        controller.m_layoutDrag.m_drag.startX = x;
        controller.m_layoutDrag.m_drag.startY = y;
        for (auto& c : controller.m_config.overlay_layout.containers) {
            if (c.id == containerId) {
                c.x = x;
                c.y = y;
                break;
            }
        }
    }
    void ClearOverlayDrag(RmlUiController& controller) {
        controller.m_layoutDrag.m_drag = {};
    }
    bool HasDeferredSelectRebuild(const RmlUiController& controller) const {
        return controller.m_deferredSelectRebuild;
    }
    void SetVisibilityWakeMs(RmlUiController& controller, std::optional<int64_t> wakeMs) {
        controller.m_nextVisibilityWakeMs = wakeMs;
    }
    std::pair<float, float> GetContainerPos(const RmlUiController& controller, const std::string& containerId) const {
        for (const auto& c : controller.m_config.overlay_layout.containers) {
            if (c.id == containerId) return {c.x, c.y};
        }
        return {-1.0f, -1.0f};
    }
    float GetConfigUiScale(const RmlUiController& controller) const {
        return controller.m_config.ui_scale;
    }
    void SeedExpiredToast(RmlUiController& controller, std::string message = "Valid") {
        controller.m_statusMessage = std::move(message);
        controller.m_statusUntilMs = 1;
    }
    void RebuildVisibleUi(RmlUiController& controller) {
        controller.RebuildVisibleUi(false, false);
    }
    bool HasToastMessage(const RmlUiController& controller) const {
        return !controller.m_statusMessage.empty();
    }
    int64_t ToastDeadline(const RmlUiController& controller) const {
        return controller.m_statusUntilMs;
    }
    Rml::ElementDocument* OnboardingDocument(RmlUiController& controller) {
        return controller.m_onboardingDoc;
    }
    OnboardingView& OnboardingModel(RmlUiController& controller) {
        return controller.m_onboarding;
    }
    Rml::ElementDocument* InsightsDocument(RmlUiController& controller) {
        return controller.m_insightsDoc;
    }
    Rml::ElementDocument* HistoryDocument(RmlUiController& controller) {
        return controller.m_historyDoc;
    }
    HistoryView& HistoryModel(RmlUiController& controller) {
        return controller.m_history;
    }
    bool IsHistoryVisible(const RmlUiController& controller) const {
        return controller.m_historyVisible;
    }
    void Click(RmlUiController& controller, Rml::Element* element) {
        controller.HandleClick(element);
    }
    void ThemeInput(RmlUiController& controller, const std::string& key, const std::string& value) {
        controller.m_themeEditor.HandleInput(key, value);
    }
    Themes::Palette PreviewTheme(const RmlUiController& controller) {
        return Themes::Read(controller.m_themeEditor.PreviewConfig());
    }
    PlayerNoteView& NoteModel(RmlUiController& controller) {
        return controller.m_playerNote;
    }
    bool IsNoteVisible(const RmlUiController& controller) const {
        return controller.m_playerNoteVisible;
    }
    Rml::ElementDocument* NoteDocument(RmlUiController& controller) {
        return controller.m_playerNoteDoc;
    }
    void OpenNote(RmlUiController& controller, const std::string& primaryId, const std::string& name) {
        controller.ShowPlayerNote(primaryId, name, "");
    }
    bool ViewAction(RmlUiController& controller, const std::string& action) {
        return controller.HandleViewAction(action, nullptr);
    }
    std::string RenderLiveMatch(RmlUiController& controller) {
        return controller.RenderLiveMatchStats();
    }
    void ReloadUi(RmlUiController& controller) {
        controller.ReloadUiResources();
    }
    void ShowRecap(RmlUiController& controller) {
        controller.ShowInsights(InsightsView::Tab::Recap, false);
    }
    void ShowSeason(RmlUiController& controller, const std::vector<SeasonSummary>& summaries,
                    const TrackedAccounts::SeasonReward& reward, int64_t now, int selectedSeason = 0) {
        controller.ShowInsights(InsightsView::Tab::Season, false);
        controller.m_insights.SetSeason(summaries, reward, now, selectedSeason);
    }
    bool InsightsShowEndedSession(const RmlUiController& controller) const {
        return controller.m_insightsShowsEndedSession;
    }
    std::optional<int64_t> RecapDismissAt(const RmlUiController& controller) const {
        return controller.m_sessionRecapDismissAtMs;
    }
    void SetRecapDismissAt(RmlUiController& controller, int64_t deadline) {
        controller.m_sessionRecapDismissAtMs = deadline;
    }
    void RunStatsApiCheck(RmlUiController& controller, bool repair) {
        controller.CheckStatsApi(repair, false);
    }
    bool StatsApiRepairRunning(const RmlUiController& controller) const {
        return controller.m_backgroundTaskRunning.load();
    }

    ConfigData original;
};

namespace {
    struct WarpDevice {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;

        bool Create(UINT width, UINT height) {
            D3D_FEATURE_LEVEL level;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context)))
                return false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
            if (FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) return false;
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
            context->ClearRenderTargetView(target.Get(), clear);
            return true;
        }
    };

    void CaptureKickoffView(WarpDevice& warp, RmlUiController& controller, const char* filename) {
        if (const char* dir = std::getenv("OMNISTATS_CAPTURE_DIR"); dir && *dir) {
            std::filesystem::create_directories(dir);
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            warp.context->ClearRenderTargetView(warp.target.Get(), clear);
            controller.Render();
            std::string error;
            EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1280, 900, 0.0f,
                                                                (std::filesystem::path(dir) / filename).wstring(), error))
                << error;
        }
    }
}

namespace {
    const std::string kSelfId = "Steam|76561198000000001|0";
    const std::string kRivalId = "Epic|0123456789abcdef0123456789abcdef|0";
}

namespace {
    void SeedGraph(SessionState& state, const std::string& playlist, std::vector<float> points, int rosterMmr, const std::string& rosterTier) {
        state.liveMatch->Update([&](MatchState& ms) {
            ms.myPrimaryId = kSelfId;
            PlayerData self;
            self.primaryId = kSelfId;
            self.name = "Me";
            self.team = 0;
            self.fetched = true;
            if (rosterMmr > 0) self.playlists[playlist] = rosterMmr;
            if (!rosterTier.empty()) self.playlistTiers[playlist] = rosterTier;
            ms.roster[kSelfId] = self;
        });
        state.history.Mutate([&](auto& history) {
            if (!points.empty()) history.playlistHistoryY[playlist] = std::move(points);
        });
    }
}
