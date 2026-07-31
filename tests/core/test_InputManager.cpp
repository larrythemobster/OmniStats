#include <gtest/gtest.h>
#include "core/InputManager.hpp"
#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include <memory>

TEST(InputManagerTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    InputManager manager(state);

    // Start/stop should leave the manager in a usable state
    EXPECT_NO_THROW(manager.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NO_THROW(manager.Stop());
}

namespace {
    class InputManagerActionTest : public ::testing::Test {
      protected:
        void TearDown() override {
            Config::Update(
                [this](ConfigData& config) { config = m_originalConfig; },
                false);
        }

        void Dispatch(
            InputManager::HotKeyId hotKeyId,
            bool showExtraPlaylists = true) {
            InputManager::HandleHotKeyAction(
                *m_state,
                hotKeyId,
                showExtraPlaylists);
        }

        ConfigData m_originalConfig = Config::Read();
        std::shared_ptr<SessionState> m_state =
            std::make_shared<SessionState>();
    };
} // namespace

TEST_F(InputManagerActionTest, SessionPanelTogglesWithoutOverlayActivation) {
    m_state->ui.showOverlay.store(false);

    Dispatch(InputManager::HotKeySession);
    EXPECT_TRUE(m_state->ui.showSessionView.load());
    EXPECT_FALSE(m_state->ui.showMenu.load());

    Dispatch(InputManager::HotKeySession);
    EXPECT_FALSE(m_state->ui.showSessionView.load());
}

TEST_F(InputManagerActionTest, ClosingSessionPanelAlsoClosesGraphSubview) {
    m_state->ui.showOverlay.store(false);
    m_state->ui.showSessionView.store(true);
    m_state->ui.showGraphView.store(true);

    Dispatch(InputManager::HotKeySession);
    EXPECT_FALSE(m_state->ui.showSessionView.load());
    EXPECT_FALSE(m_state->ui.showGraphView.load());
}

TEST_F(InputManagerActionTest, OverlayReleaseDoesNotCloseSessionPanel) {
    m_state->ui.showOverlay.store(true);

    Dispatch(InputManager::HotKeySession);
    m_state->ui.showOverlay.store(false);

    EXPECT_TRUE(m_state->ui.showSessionView.load());
}

TEST_F(InputManagerActionTest, OneSessionHotkeyActionTogglesExactlyOnce) {
    EXPECT_FALSE(m_state->ui.showSessionView.load());

    Dispatch(InputManager::HotKeySession);

    EXPECT_TRUE(m_state->ui.showSessionView.load());
}

TEST_F(InputManagerActionTest, HiddenSessionPanelCanToggleGraphSubview) {
    m_state->ui.showOverlay.store(false);
    m_state->ui.showSessionView.store(true);

    Dispatch(InputManager::HotKeyExpand);

    EXPECT_TRUE(m_state->ui.showGraphView.load());
}

TEST_F(InputManagerActionTest, WindowHotkeyMessagesAreNotAnInputPath) {
    InputManager manager(m_state);

    EXPECT_FALSE(InputManager::HandleWindowMessage(
        WM_HOTKEY,
        InputManager::HotKeySession,
        0));
    EXPECT_FALSE(m_state->ui.showSessionView.load());
}

TEST_F(InputManagerActionTest, F6OnlyCyclesRosterWhenGraphFollowsPlaylist) {
    Config::Update([](ConfigData& config) {
        config.mmr_category = "best";
        config.auto_switch_mmr_category = true;
        config.graph_mmr_category = "2v2";
        config.graph_follow_current_playlist = true;
    },
                   false);
    m_state->ui.showOverlay.store(false);
    m_state->ui.showSessionView.store(true);
    m_state->ui.rosterMmrCategory.store(MmrCategory::Best);
    m_state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);

    Dispatch(InputManager::HotKeyCycle);

    EXPECT_EQ(
        m_state->ui.rosterMmrCategory.load(),
        MmrCategory::OneVOne);
    EXPECT_EQ(
        m_state->ui.graphMmrCategory.load(),
        MmrCategory::TwoVTwo);
    const ConfigData changed = Config::Read();
    EXPECT_EQ(changed.mmr_category, "1v1");
    EXPECT_EQ(changed.graph_mmr_category, "2v2");
    EXPECT_TRUE(changed.graph_follow_current_playlist);
    EXPECT_TRUE(changed.auto_switch_mmr_category);
}

TEST_F(InputManagerActionTest, F6CyclesRosterAndManualGraphWhenFollowIsOff) {
    Config::Update([](ConfigData& config) {
        config.mmr_category = "best";
        config.auto_switch_mmr_category = true;
        config.graph_mmr_category = "2v2";
        config.graph_follow_current_playlist = false;
    },
                   false);
    m_state->ui.showOverlay.store(false);
    m_state->ui.showSessionView.store(false);
    m_state->ui.rosterMmrCategory.store(MmrCategory::Best);
    m_state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);

    Dispatch(InputManager::HotKeyCycle);

    EXPECT_EQ(
        m_state->ui.rosterMmrCategory.load(),
        MmrCategory::OneVOne);
    EXPECT_EQ(
        m_state->ui.graphMmrCategory.load(),
        MmrCategory::ThreeVThree);
    const ConfigData changed = Config::Read();
    EXPECT_EQ(changed.mmr_category, "1v1");
    EXPECT_EQ(changed.graph_mmr_category, "3v3");
    EXPECT_FALSE(changed.graph_follow_current_playlist);
    EXPECT_TRUE(changed.auto_switch_mmr_category);
}

TEST_F(InputManagerActionTest, F6PreservesHiddenExtraPlaylistRules) {
    Config::Update([](ConfigData& config) {
        config.mmr_category = "3v3";
        config.graph_mmr_category = "3v3";
        config.graph_follow_current_playlist = false;
    },
                   false);
    m_state->ui.rosterMmrCategory.store(MmrCategory::ThreeVThree);
    m_state->ui.graphMmrCategory.store(MmrCategory::ThreeVThree);

    Dispatch(InputManager::HotKeyCycle, false);

    EXPECT_EQ(
        m_state->ui.rosterMmrCategory.load(),
        MmrCategory::Casual);
    EXPECT_EQ(
        m_state->ui.graphMmrCategory.load(),
        MmrCategory::Casual);
}
