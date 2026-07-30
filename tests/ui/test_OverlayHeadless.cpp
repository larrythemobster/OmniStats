#include <gtest/gtest.h>
#include "ui/Overlay.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "imgui.h"
#include <memory>
#include <limits>
#include <cmath>
#include <vector>

class HeadlessOverlayTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Initialize headless ImGui
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();

        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
    }

    void TearDown() override {
        ImGui::DestroyContext();
    }

    // GTest macro-generated classes do not inherit friendship.
    // We define helper methods here that can access Overlay private members.
    bool GetFontReloadPending(const Overlay& o) const {
        return o.m_fontReloadPending;
    }

    void SetFontReloadPending(Overlay& o, bool val) {
        o.m_fontReloadPending = val;
    }

    float GetDpiScale(const Overlay& o) const {
        return o.m_dpiScale;
    }

    void SetDpiScale(Overlay& o, float val) {
        o.m_dpiScale = val;
    }

    float GetLoadedFontScale(const Overlay& o) const {
        return o.m_loadedFontScale;
    }

    void SetLoadedFontScale(Overlay& o, float val) {
        o.m_loadedFontScale = val;
    }

    void CallApplyTheme(Overlay& o) {
        o.ApplyTheme();
    }

    void CallHandleDpiChanged(Overlay& o, UINT dpi, const RECT* suggestedRect) {
        o.HandleDpiChanged(dpi, suggestedRect);
    }

    bool CallLoadFonts(Overlay& o) {
        return o.LoadFonts();
    }

    bool CallRebuildFontsForCurrentScale(Overlay& o) {
        return o.RebuildFontsForCurrentScale();
    }

    ImFont* GetFontRegular(const Overlay& o) const {
        return o.fontRegular;
    }
    ImFont* GetFontBold(const Overlay& o) const {
        return o.fontBold;
    }
    ImFont* GetFontSmall(const Overlay& o) const {
        return o.fontSmall;
    }
    ImFont* GetFontSmallBold(const Overlay& o) const {
        return o.fontSmallBold;
    }
    ImFont* GetFontMono(const Overlay& o) const {
        return o.fontMono;
    }
    ImFont* GetLobbyFontSmall(const Overlay& o) const {
        return o.lobbyFontSmall;
    }
    const RenderSnapshot& GetRenderSnapshot(const Overlay& o) const {
        return o.m_snap;
    }
};

TEST_F(HeadlessOverlayTest, RenderOverlayDoesNotCrash) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    // Simulate one frame
    ImGui::NewFrame();
    EXPECT_NO_THROW(overlay.RenderUI());
    ImGui::Render();
}

TEST_F(HeadlessOverlayTest, TrackerCoveredEstimatedMmrPointBecomesExactInRenderSnapshot) {
    auto state = std::make_shared<SessionState>();
    {
        std::unique_lock<std::shared_mutex> lock(state->history.mutex);
        state->history.playlistHistoryY["2v2"] = {1209.0f};
        state->history.playlistMatchPoints["2v2"] = {
            SessionMmrPoint{
                .matchGuid = "owned-match",
                .historyIndex = 0,
                .mmr = 1209,
                .trackerMatchesPlayed = 50,
                .trackerCovered = true,
                .valueEstimated = true}};
        state->history.version++;
    }
    Overlay overlay(state);

    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();
    ASSERT_EQ(GetRenderSnapshot(overlay).playlistHistoryEstimated.at("2v2").size(), 1u);
    EXPECT_TRUE(GetRenderSnapshot(overlay).playlistHistoryEstimated.at("2v2")[0]);

    {
        std::unique_lock<std::shared_mutex> lock(state->history.mutex);
        state->history.playlistMatchPoints["2v2"][0].valueEstimated = false;
        state->history.version++;
    }
    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();
    ASSERT_EQ(GetRenderSnapshot(overlay).playlistHistoryEstimated.at("2v2").size(), 1u);
    EXPECT_FALSE(GetRenderSnapshot(overlay).playlistHistoryEstimated.at("2v2")[0]);
}

TEST_F(HeadlessOverlayTest, HandleDpiChangedSetsReloadPending) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    SetFontReloadPending(overlay, false);
    CallHandleDpiChanged(overlay, 144, nullptr);
    EXPECT_TRUE(GetFontReloadPending(overlay));
    EXPECT_FLOAT_EQ(GetDpiScale(overlay), 1.5f);
}

TEST_F(HeadlessOverlayTest, ApplyThemeSetsReloadPendingOnLargeScaleChange) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    // Baseline scale
    SetDpiScale(overlay, 1.0f);
    SetLoadedFontScale(overlay, 1.0f);
    SetFontReloadPending(overlay, false);

    // Small scale change (below threshold of 0.05f)
    Config::Update([](ConfigData& c) { c.ui_scale = 1.02f; });
    CallApplyTheme(overlay);
    EXPECT_FALSE(GetFontReloadPending(overlay));

    // Large scale change (above threshold of 0.05f)
    Config::Update([](ConfigData& c) { c.ui_scale = 1.50f; });
    CallApplyTheme(overlay);
    EXPECT_TRUE(GetFontReloadPending(overlay));

    // Reset config
    Config::Update([](ConfigData& c) { c.ui_scale = 1.0f; });
}

TEST_F(HeadlessOverlayTest, LoadFontsSucceedsWithSafeFallbacks) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    SetDpiScale(overlay, 1.0f);
    Config::Update([](ConfigData& c) { c.ui_scale = 1.0f; });

    bool result = CallLoadFonts(overlay);
    EXPECT_TRUE(result);
    EXPECT_NE(GetFontRegular(overlay), nullptr);
    EXPECT_NE(GetFontBold(overlay), nullptr);
    EXPECT_NE(GetFontSmall(overlay), nullptr);
    EXPECT_NE(GetFontSmallBold(overlay), nullptr);
    EXPECT_NE(GetFontMono(overlay), nullptr);
    EXPECT_NE(GetLobbyFontSmall(overlay), nullptr);
}

TEST_F(HeadlessOverlayTest, InvalidUiScaleClampsSafely) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    SetDpiScale(overlay, 1.0f);

    // NaN / Infinity / Extremely large / Extremely small scales
    // All should be clamped safely and LoadFonts should succeed.
    std::vector<float> testScales = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -10.0f,
        0.01f,
        100.0f};

    for (float scale : testScales) {
        Config::Update([scale](ConfigData& c) { c.ui_scale = scale; });
        EXPECT_NO_THROW({
            bool res = CallLoadFonts(overlay);
            EXPECT_TRUE(res);
            float loadedScale = GetLoadedFontScale(overlay);
            EXPECT_GE(loadedScale, 0.5f);
            EXPECT_LE(loadedScale, 5.0f);
        });
    }

    // Restore default
    Config::Update([](ConfigData& c) { c.ui_scale = 1.0f; });
}

TEST_F(HeadlessOverlayTest, RebuildFontsForCurrentScaleReturnsFalseIfD3D11Null) {
    auto state = std::make_shared<SessionState>();
    Overlay overlay(state);

    // m_d3d11 is null in this headless context, so it should return false cleanly
    EXPECT_FALSE(CallRebuildFontsForCurrentScale(overlay));
}
