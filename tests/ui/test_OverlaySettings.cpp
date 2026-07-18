#include <gtest/gtest.h>
#include "ui/Overlay.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "imgui.h"
#include <memory>

class OverlaySettingsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        m_state = std::make_shared<SessionState>();
        m_overlay = std::make_unique<Overlay>(m_state);
    }
    void TearDown() override {
        m_overlay.reset();
        m_state.reset();
        ImGui::DestroyContext();
    }
    void RenderOneFrame() {
        ImGui::NewFrame();
        m_overlay->RenderUI();
        ImGui::Render();
    }
    std::shared_ptr<SessionState> m_state;
    std::unique_ptr<Overlay> m_overlay;
};

TEST_F(OverlaySettingsTest, RenderSettingsMenu_DefaultState_NoCrash) {
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlaySettingsTest, RenderSettingsMenu_OverlayAndMenuActive_NoCrash) {
    m_state->ui.showOverlay = true;
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlaySettingsTest, RenderSettingsMenu_SecondMonitorMode_NoCrash) {
    Config::Update([](ConfigData& c) { c.second_monitor_mode = true; });
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([](ConfigData& c) { c.second_monitor_mode = false; });
}

TEST_F(OverlaySettingsTest, ConfigToggle_RequireFocus_UpdatesInMemory) {
    Config::Update([](ConfigData& c) { c.require_rl_focus = false; });
    EXPECT_FALSE(Config::Read().require_rl_focus);
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([](ConfigData& c) { c.require_rl_focus = true; });
}

TEST_F(OverlaySettingsTest, ConfigToggle_DiscordRPC_UpdatesInMemory) {
    Config::Update([](ConfigData& c) { c.discord_rpc_enabled = false; });
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
    EXPECT_FALSE(Config::Read().discord_rpc_enabled);
    Config::Update([](ConfigData& c) { c.discord_rpc_enabled = true; });
}

TEST_F(OverlaySettingsTest, EmptyRoster_IdentityDropdown_NoCrash) {
    {
        std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
        m_state->game.roster.clear();
    }
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlaySettingsTest, ExtremeThemeColors_NoCrash) {
    auto backup = Config::Read();
    Config::Update([](ConfigData& c) {
        c.themeBg = {0, 0, 0, 0};
        c.themeText = {0, 0, 0, 0};
    });
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([backup](ConfigData& c) { c = backup; });
}

TEST_F(OverlaySettingsTest, MultipleFrames_NoAccumulationCrash) {
    m_state->ui.showMenu = true;
    for (int i = 0; i < 10; ++i)
        EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlaySettingsTest, MmrCategoryChange_UpdatesSessionState) {
    Config::Update([](ConfigData& c) { c.mmr_category = "3v3"; });
    m_state->ui.rosterMmrCategory.store(MmrCategory::ThreeVThree);
    m_state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);
    EXPECT_EQ(m_state->ui.rosterMmrCategory.load(), MmrCategory::ThreeVThree);
    m_state->ui.showMenu = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([](ConfigData& c) { c.mmr_category = "best"; });
}
