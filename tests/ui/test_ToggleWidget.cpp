#include <gtest/gtest.h>
#include "ui/widgets/ToggleWidget.hpp"
#include "core/Config.hpp"
#include "imgui.h"
#include "imgui_internal.h"

class ToggleWidgetTest : public ::testing::Test {
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
    }
    void TearDown() override {
        ImGui::DestroyContext();
    }
};

TEST_F(ToggleWidgetTest, RenderToggle_NoCrash) {
    bool value = false;
    ImGui::NewFrame();
    ImGui::Begin("TestWindow");
    EXPECT_NO_THROW(Widgets::ToggleButton("TestToggle", &value));
    ImGui::End();
    ImGui::Render();
}

TEST_F(ToggleWidgetTest, RenderToggle_TrueState_NoCrash) {
    bool value = true;
    ImGui::NewFrame();
    ImGui::Begin("TestWindow");
    EXPECT_NO_THROW(Widgets::ToggleButton("ActiveToggle", &value));
    ImGui::End();
    ImGui::Render();
}

TEST_F(ToggleWidgetTest, MultipleToggles_SameFrame_NoCrash) {
    bool a = false, b = true, c = false;
    ImGui::NewFrame();
    ImGui::Begin("TestWindow");
    Widgets::ToggleButton("Toggle_A", &a);
    Widgets::ToggleButton("Toggle_B", &b);
    Widgets::ToggleButton("Toggle_C", &c);
    ImGui::End();
    ImGui::Render();
}

TEST_F(ToggleWidgetTest, ConfigBoolToggle_ViaToggleWidget_NoCrash) {
    bool showSummary = Config::Read().show_match_summary;
    ImGui::NewFrame();
    ImGui::Begin("ConfigTest");
    Widgets::ToggleButton("MatchSummary", &showSummary);
    ImGui::End();
    ImGui::Render();
}

TEST_F(ToggleWidgetTest, MultipleFrames_ToggleRender_NoCrash) {
    bool value = false;
    for (int i = 0; i < 10; ++i) {
        ImGui::NewFrame();
        ImGui::Begin("LoopTest");
        Widgets::ToggleButton("RepeatedToggle", &value);
        ImGui::End();
        ImGui::Render();
    }
}
