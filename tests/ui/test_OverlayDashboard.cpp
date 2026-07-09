#include <gtest/gtest.h>
#include "ui/Overlay.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "imgui.h"
#include <memory>

class OverlayDashboardTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels; int w, h;
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

// -- Basic rendering stability --

TEST_F(OverlayDashboardTest, RenderDashboard_DefaultState_NoCrash) {
    m_state->ui.showOverlay = true;
    m_state->ui.h2hExpanded = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlayDashboardTest, RenderDashboard_SecondMonitorMode_NoCrash) {
    Config::Update([](ConfigData& c) { c.second_monitor_mode = true; });
    m_state->ui.showOverlay = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([](ConfigData& c) { c.second_monitor_mode = false; });
}

// -- Empty roster boundary --

TEST_F(OverlayDashboardTest, EmptyRoster_NoCrash) {
    {
        std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
        m_state->game.roster.clear();
        m_state->game.inMatch = true;
    }
    m_state->ui.showOverlay = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

// -- Malformed UTF-8 player names --

TEST_F(OverlayDashboardTest, MalformedUtf8PlayerNames_NoCrash) {
    {
        std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
        m_state->game.roster.clear();
        m_state->game.inMatch = true;
        m_state->game.myTeam = 0;
        m_state->game.myPrimaryId = "steam|111";

        PlayerData p1;
        p1.primaryId = "steam|111";
        // Invalid UTF-8: continuation bytes without start byte
        p1.name = "\x80\x81\x82\xFE\xFF";
        p1.team = 0;
        p1.mmr = 1200;
        m_state->game.roster["steam|111"] = p1;

        PlayerData p2;
        p2.primaryId = "steam|222";
        // Truncated multi-byte sequence
        p2.name = "Player\xC0\xAF\xE0\x80";
        p2.team = 1;
        p2.mmr = 1100;
        m_state->game.roster["steam|222"] = p2;
    }
    m_state->ui.showOverlay = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

// -- Populated roster with valid data --

TEST_F(OverlayDashboardTest, PopulatedRoster_RendersDashboard_NoCrash) {
    {
        std::unique_lock<std::shared_mutex> lk(m_state->game.mutex);
        m_state->game.inMatch = true;
        m_state->game.arenaName = "stadium_p";
        m_state->game.myTeam = 0;
        m_state->game.myPrimaryId = "steam|player1";
        m_state->game.score[0] = 2;
        m_state->game.score[1] = 1;

        PlayerData p;
        p.primaryId = "steam|player1";
        p.name = "TestPlayer";
        p.team = 0;
        p.mmr = 1500;
        p.rankTier = "Diamond III";
        m_state->game.roster["steam|player1"] = p;

        PlayerData opp;
        opp.primaryId = "epic|opp1";
        opp.name = "Opponent";
        opp.team = 1;
        opp.mmr = 1450;
        m_state->game.roster["epic|opp1"] = opp;
    }
    m_state->ui.showOverlay = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

// -- Session view and graph view toggles --

TEST_F(OverlayDashboardTest, SessionView_NoCrash) {
    m_state->ui.showOverlay = true;
    m_state->ui.showSessionView = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

TEST_F(OverlayDashboardTest, GraphView_NoCrash) {
    m_state->ui.showOverlay = true;
    m_state->ui.showSessionView = true;
    m_state->ui.showGraphView = true;
    EXPECT_NO_THROW(RenderOneFrame());
}

// -- All dashboard panels toggled off --

TEST_F(OverlayDashboardTest, AllPanelsOff_NoCrash) {
    Config::Update([](ConfigData& c) {
        c.second_monitor_show_roster = false;
        c.second_monitor_show_session = false;
        c.second_monitor_mode = true;
    });
    m_state->ui.showOverlay = true;
    EXPECT_NO_THROW(RenderOneFrame());
    Config::Update([](ConfigData& c) {
        c.second_monitor_show_roster = true;
        c.second_monitor_show_session = true;
        c.second_monitor_mode = false;
    });
}

// -- Multiple consecutive frames --

TEST_F(OverlayDashboardTest, MultipleFrames_NoAccumulationCrash) {
    m_state->ui.showOverlay = true;
    for (int i = 0; i < 10; ++i) EXPECT_NO_THROW(RenderOneFrame());
}

// -- Container minimum size clamping --

TEST_F(OverlayDashboardTest, ContainerSizeClampingEnforced) {
    // 1. Manually set a container size to be very small in Config
    Config::Update([](ConfigData& c) {
        for (auto& cont : c.overlay_layout.containers) {
            if (cont.id == "main_stack") {
                cont.w = 50.0f;
                cont.h = 50.0f;
                break;
            }
        }
    }, true);

    // 2. Render a frame. This will run the layout manager and should clamp the container size upward.
    m_state->ui.showOverlay = true;
    RenderOneFrame();

    // 3. Read the config again and assert that it has been clamped to at least the minimum size
    ConfigData finalConf = Config::Read();
    for (const auto& cont : finalConf.overlay_layout.containers) {
        if (cont.id == "main_stack") {
            // main_stack contains LiveRoster, which has minimum width of 410
            EXPECT_GE(cont.w, 410.0f);
            EXPECT_GE(cont.h, 120.0f);
            break;
        }
    }
}

