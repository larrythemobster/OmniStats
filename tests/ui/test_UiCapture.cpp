#include <gtest/gtest.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "ui/rml/backend/RmlCapture.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "utils/Storage.hpp"

namespace {
    struct WarpCaptureDevice {
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

    void SeedMatch(MatchState& game) {
        game.myPrimaryId = "Steam|capture_self";
        game.myTeam = 0;
        game.inMatch = true;

        PlayerData self;
        self.primaryId = "Steam|capture_self";
        self.name = "SelfPlayer";
        self.team = 0;
        self.mmr = 1250;
        self.rankTier = "Champion II Div II";
        self.playlists["2v2"] = 1250;
        self.playlistTiers["2v2"] = "Champion II Div II";
        self.fetched = true;
        game.roster[self.primaryId] = self;

        PlayerData mate;
        mate.primaryId = "Epic|mate_01";
        mate.name = "Teammate";
        mate.team = 0;
        mate.mmr = 1230;
        mate.rankTier = "Champion II Div I";
        mate.playlists["2v2"] = 1230;
        mate.playlistTiers["2v2"] = "Champion II Div I";
        mate.fetched = true;
        game.roster[mate.primaryId] = mate;

        PlayerData opp1;
        opp1.primaryId = "Epic|opp_01";
        opp1.name = "OpponentA";
        opp1.team = 1;
        opp1.mmr = 1260;
        opp1.rankTier = "Champion II Div III";
        opp1.playlists["2v2"] = 1260;
        opp1.playlistTiers["2v2"] = "Champion II Div III";
        opp1.fetched = true;
        game.roster[opp1.primaryId] = opp1;

        PlayerData opp2;
        opp2.primaryId = "Steam|opp_02";
        opp2.name = "OpponentB";
        opp2.team = 1;
        opp2.mmr = 1240;
        opp2.rankTier = "Champion II Div II";
        opp2.playlists["2v2"] = 1240;
        opp2.playlistTiers["2v2"] = "Champion II Div II";
        opp2.fetched = true;
        game.roster[opp2.primaryId] = opp2;

        game.sessionTotals.wins = 5;
        game.sessionTotals.losses = 2;
        game.sessionTotals.goals = 12;
        game.sessionTotals.saves = 8;
        game.sessionTotals.assists = 6;
        game.sessionTotals.demos = 4;
        game.sessionGamemodes["2v2"] = {5, 2, 7};
    }

    void SeedCaptureFixtureState(SessionState& state) {
        state.liveMatch->UpdateMatchStateForTests(SeedMatch);
        std::unique_lock lock(state.history.mutex);
        state.history.playlistHistoryY["2v2"] = {1200.0f, 1215.0f, 1205.0f, 1220.0f, 1235.0f, 1250.0f};
        state.history.playlistInitialMmr["2v2"] = 1200;
        state.history.version++;
    }
}

TEST(UiCapture, EmitOverlayAndDashboardScreenshots) {
    const char* envDir = std::getenv("OMNISTATS_CAPTURE_DIR");
    std::filesystem::path outDir;
    if (envDir && *envDir) {
        outDir = envDir;
    } else {
        outDir = std::filesystem::temp_directory_path() / "OmniStatsUiCaptures";
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    WarpCaptureDevice warp;
    if (!warp.Create(1920, 1080)) {
        GTEST_SKIP() << "WARP not available.";
    }

    Storage::InitializeEnvironment();
    const ConfigData original = Config::Read();
    Config::Update([](ConfigData& c) {
        c.last_primary_id.clear();
        c.graph_mmr_category = "2v2";
        c.onboarding_completed = true;
        c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
        c.terms_accepted_version = Config::CurrentTermsVersion;
        c.account_link_prompt_shown = true;
    },
                   true);

    auto state = std::make_shared<SessionState>();
    SeedCaptureFixtureState(*state);

    // 1. Capture Overlay view
    {
        ConfigData config = Config::Read();
        config.second_monitor_mode = false;
        config.show_running_indicator = false;
        state->ui.showOverlay.store(true);

        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1920, 1080, 1.0f));
        controller.Update(config, true);
        controller.Render();

        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        warp.context->OMSetRenderTargets(1, warp.target.GetAddressOf(), nullptr);
        warp.context->ClearRenderTargetView(warp.target.Get(), clear);
        controller.Render();

        std::string err;
        const auto overlayPath = outDir / "overlay.png";
        EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1920, 1080, 0.0f, overlayPath.wstring(), err)) << err;
    }

    // 2. Capture Dashboard view
    {
        ConfigData config = Config::Read();
        config.second_monitor_mode = true;
        config.show_running_indicator = false;
        state->ui.showOverlay.store(false);

        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1920, 1080, 1.0f));
        controller.Update(config, true);
        controller.Render();

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        warp.context->OMSetRenderTargets(1, warp.target.GetAddressOf(), nullptr);
        warp.context->ClearRenderTargetView(warp.target.Get(), clear);
        controller.Render();

        std::string err;
        const auto dashboardPath = outDir / "dashboard.png";
        EXPECT_TRUE(RmlCapture::SaveBoundRenderTargetRegion(warp.context.Get(), 0, 0, 1920, 1080, 0.0f, dashboardPath.wstring(), err)) << err;
    }
    Config::Update([&](ConfigData& c) { c = original; }, true);
}
