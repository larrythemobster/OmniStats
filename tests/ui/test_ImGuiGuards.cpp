#include <gtest/gtest.h>
#include "ui/ImGuiGuards.hpp"
#include "imgui.h"
#include "imgui_internal.h"

class ImGuiGuardsTest : public ::testing::Test {
protected:
    void SetUp() override {
        IMGUI_CHECKVERSION();
        m_ctx = ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels; int w, h;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
    }
    void TearDown() override {
        ImGui::DestroyContext(m_ctx);
        m_ctx = nullptr;
    }
    ImGuiContext* m_ctx = nullptr;
};

TEST_F(ImGuiGuardsTest, StyleVarGuard_SinglePush_RestoresStack) {
    ImGui::NewFrame();
    int stackBefore = m_ctx->StyleVarStack.Size;
    {
        Widgets::StyleVarGuard guard(ImGuiStyleVar_Alpha, 0.5f);
        EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore + 1);
    }
    EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, StyleVarGuard_MultiplePushes_RestoresStack) {
    ImGui::NewFrame();
    int stackBefore = m_ctx->StyleVarStack.Size;
    {
        Widgets::StyleVarGuard guard(ImGuiStyleVar_Alpha, 0.8f);
        guard.Push(ImGuiStyleVar_WindowRounding, 5.0f);
        guard.Push(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 4.0f));
        EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore + 3);
    }
    EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, StyleVarGuard_Vec2Constructor_RestoresStack) {
    ImGui::NewFrame();
    int stackBefore = m_ctx->StyleVarStack.Size;
    {
        Widgets::StyleVarGuard guard(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
        EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore + 1);
    }
    EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, FontGuard_ValidFont_RestoresStack) {
    ImGui::NewFrame();
    int fontStackBefore = m_ctx->FontStack.Size;
    ImFont* defaultFont = ImGui::GetFont();
    {
        Widgets::FontGuard guard(defaultFont);
        EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore + 1);
    }
    EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, FontGuard_NullFont_NoOp) {
    ImGui::NewFrame();
    int fontStackBefore = m_ctx->FontStack.Size;
    {
        Widgets::FontGuard guard(nullptr);
        EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore);
    }
    EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, NestedGuards_UnwindCorrectly) {
    ImGui::NewFrame();
    int styleStackBefore = m_ctx->StyleVarStack.Size;
    int fontStackBefore = m_ctx->FontStack.Size;
    {
        Widgets::StyleVarGuard outer(ImGuiStyleVar_Alpha, 0.9f);
        EXPECT_EQ(m_ctx->StyleVarStack.Size, styleStackBefore + 1);
        {
            Widgets::StyleVarGuard inner(ImGuiStyleVar_WindowRounding, 8.0f);
            Widgets::FontGuard fontGuard(ImGui::GetFont());
            EXPECT_EQ(m_ctx->StyleVarStack.Size, styleStackBefore + 2);
            EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore + 1);
        }
        EXPECT_EQ(m_ctx->StyleVarStack.Size, styleStackBefore + 1);
        EXPECT_EQ(m_ctx->FontStack.Size, fontStackBefore);
    }
    EXPECT_EQ(m_ctx->StyleVarStack.Size, styleStackBefore);
    ImGui::Render();
}

TEST_F(ImGuiGuardsTest, RepeatedCycles_NoStackAccumulation) {
    ImGui::NewFrame();
    int stackBefore = m_ctx->StyleVarStack.Size;
    for (int i = 0; i < 100; ++i) {
        Widgets::StyleVarGuard guard(ImGuiStyleVar_Alpha, 0.5f);
        guard.Push(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
    }
    EXPECT_EQ(m_ctx->StyleVarStack.Size, stackBefore);
    ImGui::Render();
}
