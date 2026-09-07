#include <gtest/gtest.h>
#include "ui/panels/SettingsPanel.hpp"
#include "ui/Overlay.hpp"
#include "imgui_internal.h"
#include <cstring>
#include <memory>

class OverlaySettingsTest : public ::testing::Test {
  protected:
    void SetUp() override {
        original = Config::Read();
        Config::Update([](ConfigData& c) { c = ConfigData{}; });
        config = Config::Read();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        state = std::make_shared<SessionState>();
        state->ui.showMenu = true;
        RenderContext context{*state, config, nullptr, &snapshot, 1.0f, &token,
                              nullptr, nullptr, nullptr, nullptr, nullptr};
        panel = std::make_unique<SettingsPanel>(context);
        Frame();
        Frame();
    }

    void TearDown() override {
        panel.reset();
        state.reset();
        ImGui::DestroyContext();
        Config::Update([this](ConfigData& c) { c = original; });
    }

    void Frame() {
        config = Config::Read();
        ImGui::NewFrame();
        panel->Render();
        ImGui::Render();
    }

    void Click(ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        Frame();
        io.AddMouseButtonEvent(0, true);
        Frame();
        io.AddMouseButtonEvent(0, false);
        Frame();
        Frame();
    }

    ImGuiWindow* Navigation() {
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::strstr(window->Name, "/SettingsNavigation_")) return window;
        }
        return nullptr;
    }

    void Navigate(int category) {
        auto* navigation = Navigation();
        ASSERT_NE(navigation, nullptr);
        Click(ImVec2(navigation->WorkRect.Min.x + 50.0f, navigation->WorkRect.Min.y + category * 30.0f + 13.0f));
    }

    ImGuiWindow* Content() {
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::strstr(window->Name, "/SettingsPage_")) return window;
        }
        return nullptr;
    }

    void SelectSection(int section) {
        auto* content = Content();
        ASSERT_NE(content, nullptr);
        Click(ImVec2(content->WorkRect.Min.x + 75.0f, content->WorkRect.Min.y + 38.0f));
        ImGuiWindow* popup = nullptr;
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && std::strstr(window->Name, "##Combo_")) popup = window;
        }
        ASSERT_NE(popup, nullptr);
        Click(ImVec2(popup->WorkRect.Min.x + 50.0f, popup->WorkRect.Min.y + section * 19.0f + 7.0f));
        const auto* settings = ImGui::FindWindowByName("Settings");
        ASSERT_NE(settings, nullptr);
        ASSERT_EQ(settings->StateStorage.GetInt(content->GetID("SelectedSection"), 0), section);
    }

    void OpenShortcutsAndCapture() {
        ASSERT_NO_FATAL_FAILURE(Navigate(3));
        auto& context = *ImGui::GetCurrentContext();
        ImGuiTable* firstRow = nullptr;
        for (int i = 0; i < context.Tables.GetMapSize(); ++i) {
            auto* table = context.Tables.TryGetMapData(i);
            if (table && table->LastFrameActive == context.FrameCount && table->ColumnsCount == 2 &&
                (!firstRow || table->OuterRect.Min.y < firstRow->OuterRect.Min.y)) {
                firstRow = table;
            }
        }
        ASSERT_NE(firstRow, nullptr);
        Click(ImVec2(firstRow->Columns[1].WorkMinX + 35.0f, firstRow->OuterRect.Min.y + 11.0f));
        ASSERT_TRUE(state->ui.inputCaptureActive.load());
    }

    ConfigData original;
    ConfigData config;
    RenderSnapshot snapshot;
    std::string token;
    std::shared_ptr<SessionState> state;
    std::unique_ptr<SettingsPanel> panel;
};

TEST_F(OverlaySettingsTest, ChoosingAnotherCategoryCancelsShortcutCapture) {
    const int previousKey = config.key_overlay;
    ASSERT_NO_FATAL_FAILURE(OpenShortcutsAndCapture());
    auto* navigation = Navigation();
    ASSERT_NE(navigation, nullptr);
    Click(ImVec2(navigation->WorkRect.Min.x + 50.0f, navigation->WorkRect.Min.y + 20.0f));
    state->ui.lastKeyboardKeyPressed.store(VK_F10);
    Frame();
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
    EXPECT_EQ(Config::Read().key_overlay, previousKey);
}

TEST_F(OverlaySettingsTest, DoneClosesSettingsWithoutChangingPendingShortcut) {
    const int previousKey = config.key_overlay;
    ASSERT_NO_FATAL_FAILURE(OpenShortcutsAndCapture());
    const auto* window = ImGui::FindWindowByName("Settings");
    ASSERT_NE(window, nullptr);
    Click(ImVec2(window->Pos.x + window->Size.x - 45.0f, window->Pos.y + window->Size.y - 24.0f));
    state->ui.lastKeyboardKeyPressed.store(VK_F10);
    Frame();
    EXPECT_FALSE(state->ui.showMenu.load());
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
    EXPECT_EQ(Config::Read().key_overlay, previousKey);
}

TEST_F(OverlaySettingsTest, CapturedKeyChangesOnlyTheSelectedShortcut) {
    const int menuKey = config.key_menu;
    ASSERT_NO_FATAL_FAILURE(OpenShortcutsAndCapture());
    state->ui.lastKeyboardKeyPressed.store(VK_F10);
    Frame();
    EXPECT_EQ(Config::Read().key_overlay, VK_F10);
    EXPECT_EQ(Config::Read().key_menu, menuKey);
    EXPECT_FALSE(state->ui.inputCaptureActive.load());
}

TEST_F(OverlaySettingsTest, EverySectionFitsWithoutScrollingInBothDisplayModes) {
    const ImGuiStyle originalStyle = ImGui::GetStyle();
    const float originalScale = Config::Read().ui_scale;
    Config::Update([](ConfigData& c) {
        c.show_lobby_ranks_overlay = true;
        c.show_extra_playlists = true;
        c.show_previous_games_summary = true;
        c.show_streaks_stats = true;
        c.show_gamemode_breakdown = true;
        c.auto_save_replays = true;
    });
    state->ui.controllerConnected.store(true);
    state->ui.controllerDebugName = "Xbox Wireless Controller";
    state->ui.statsApiResult.path = "C:\\Program Files\\Epic Games\\rocketleague\\TAGame\\Config\\DefaultStatsAPI.ini";
    state->ui.statsApiResult.status = StatsApiConfig::Status::NotFound;
    state->ui.statsApiResult.message = "Game connection is unavailable. Check the configuration and restart Rocket League.";
    state->ui.statsApiResult.rlRunning = true;
    snapshot.myPrimaryId = "local";
    auto& player = snapshot.roster["local"];
    player.name = "Local player";
    for (const char* playlist : {"1v1", "2v2", "3v3", "casual", "t", "hoops", "rumble", "dropshot", "snowday"}) {
        player.playlistTiers[playlist] = "Grand Champion III";
        player.playlists[playlist] = 1500;
        player.playlistMatches[playlist] = 100;
    }
    ImGui::GetIO().DisplaySize = ImVec2(1100.0f, 820.0f);
    static constexpr int sectionsPerCategory[] = {4, 5, 3, 3, 2, 3, 1, 2};
    for (bool dashboard : {false, true}) {
        Config::Update([dashboard](ConfigData& c) { c.second_monitor_mode = dashboard; });
        Frame();
        Frame();
        for (int category = 0; category < 8; ++category) {
            ASSERT_NO_FATAL_FAILURE(Navigate(category));
            for (int section = 0; section < sectionsPerCategory[category]; ++section) {
                SCOPED_TRACE(::testing::Message() << "dashboard=" << dashboard << " category=" << category << " section=" << section);
                if (sectionsPerCategory[category] > 1) {
                    ASSERT_NO_FATAL_FAILURE(SelectSection(section));
                }
                auto* content = Content();
                ASSERT_NE(content, nullptr);
                EXPECT_LE(content->ContentSize.y, content->InnerRect.GetHeight());
                EXPECT_LE(content->ContentSize.x, content->InnerRect.GetWidth());
                EXPECT_EQ(content->ScrollMax.y, 0.0f);
            }
        }
    }
    EXPECT_EQ(Config::Read().ui_scale, originalScale);
    EXPECT_EQ(ImGui::GetStyle().FramePadding.x, originalStyle.FramePadding.x);
    EXPECT_EQ(ImGui::GetStyle().FramePadding.y, originalStyle.FramePadding.y);
    EXPECT_EQ(ImGui::GetStyle().ItemSpacing.x, originalStyle.ItemSpacing.x);
    EXPECT_EQ(ImGui::GetStyle().ItemSpacing.y, originalStyle.ItemSpacing.y);
}
