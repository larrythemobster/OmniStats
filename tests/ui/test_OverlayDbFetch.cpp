#include <gtest/gtest.h>
#include "ui/Overlay.hpp"
#include "database/DatabaseManager.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include "imgui.h"
#include <memory>

class OverlayDbFetchTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.DisplaySize = ImVec2(1920.0f, 1080.0f);
        io.DeltaTime = 1.0f / 60.0f;
        // Clear config
        Config::Update([](ConfigData& c) {
            c.last_primary_id = "";
            c.graph_mmr_category = "2v2";
        },
                       true);
    }

    void TearDown() override {
        ImGui::DestroyContext();
        Config::Update([](ConfigData& c) { c.last_primary_id = ""; }, true);
    }
};

TEST_F(OverlayDbFetchTest, TriggersSingleDbFetchWhenEffectivePrimaryChanges) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);

    // Ensure counters start at zero
    DatabaseManager::s_test_async_get_lifetime_calls.store(0);
    DatabaseManager::s_test_async_refresh_calls.store(0);

    // Set persisted last_primary_id but leave state->game.myPrimaryId empty
    Config::Update([](ConfigData& c) { c.last_primary_id = "Steam|TEST123"; }, true);

    Overlay overlay(state, db);

    // First render should trigger AsyncGetLifetimeMmrHistory and AsyncRefreshDbStats once
    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();

    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), 1);
    EXPECT_EQ(DatabaseManager::s_test_async_refresh_calls.load(), 1);
    {
        std::lock_guard<std::mutex> lk(DatabaseManager::s_test_mutex);
        EXPECT_EQ(DatabaseManager::s_test_last_get_lifetime_primary, "Steam|TEST123");
        EXPECT_EQ(DatabaseManager::s_test_last_refresh_primary, "Steam|TEST123");
    }

    int beforeGet = DatabaseManager::s_test_async_get_lifetime_calls.load();
    int beforeRefresh = DatabaseManager::s_test_async_refresh_calls.load();

    // Second render should NOT enqueue additional async fetches (deduped by overlay)
    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();

    EXPECT_EQ(DatabaseManager::s_test_async_get_lifetime_calls.load(), beforeGet);
    EXPECT_EQ(DatabaseManager::s_test_async_refresh_calls.load(), beforeRefresh);

    // cleanup
    Config::Update([](ConfigData& c) { c.last_primary_id = ""; }, true);
}

TEST_F(OverlayDbFetchTest, GraphCategoryChangeFetchesLifetimeHistoryOnce) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    {
        std::unique_lock<std::shared_mutex> lock(
            state->history.mutex);
        state->history.lifetimeMmrX = {1.0f};
        state->history.lifetimeMmrY = {1200.0f};
        state->history.version++;
    }
    Config::Update([](ConfigData& c) {
        c.last_primary_id = "Steam|GRAPH";
    },
                   true);
    DatabaseManager::s_test_async_get_lifetime_calls.store(0);

    Overlay overlay(state, db);
    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();
    EXPECT_EQ(
        DatabaseManager::s_test_async_get_lifetime_calls.load(),
        1);

    state->ui.graphMmrCategory.store(MmrCategory::OneVOne);
    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();
    EXPECT_EQ(
        DatabaseManager::s_test_async_get_lifetime_calls.load(),
        2);
    {
        std::lock_guard<std::mutex> lock(
            DatabaseManager::s_test_mutex);
        EXPECT_EQ(
            DatabaseManager::s_test_last_get_lifetime_playlist,
            "1v1");
    }
    {
        std::shared_lock<std::shared_mutex> lock(
            state->history.mutex);
        EXPECT_TRUE(state->history.lifetimeMmrX.empty());
        EXPECT_TRUE(state->history.lifetimeMmrY.empty());
    }

    ImGui::NewFrame();
    overlay.RenderUI();
    ImGui::Render();
    EXPECT_EQ(
        DatabaseManager::s_test_async_get_lifetime_calls.load(),
        2);
}
