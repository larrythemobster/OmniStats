#include <gtest/gtest.h>
#include "platform/InputManager.hpp"
#include "platform/KeyPressSimulator.hpp"
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include <memory>

TEST(KeyPressSimulatorTest, ForegroundGuardRequiresRocketLeagueProcessNotTitle) {
    EXPECT_TRUE(IsRocketLeagueImagePath(L"C:\\Games\\rocketleague\\Binaries\\Win64\\RocketLeague.exe"));
    EXPECT_TRUE(IsRocketLeagueImagePath(L"D:/Epic/rocketleague.EXE"));
    EXPECT_FALSE(IsRocketLeagueImagePath(L"C:\\Program Files\\Browser\\browser.exe"));
    EXPECT_FALSE(IsRocketLeagueImagePath(L"C:\\Tools\\NotRocketLeague.exe"));
    EXPECT_FALSE(IsRocketLeagueWindow(nullptr));

    HWND impostor = CreateWindowExW(0, L"STATIC", L"Rocket League (64-bit, DX11)", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(impostor, nullptr);
    EXPECT_FALSE(IsRocketLeagueWindow(impostor));
    DestroyWindow(impostor);
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
                showExtraPlaylists,
                nullptr);
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

TEST_F(InputManagerActionTest, F7CyclesSessionGraphLifetimeGraphAndStats) {
    m_state->ui.showOverlay.store(false);
    m_state->ui.showSessionView.store(true);

    // Step 1: Session Graph
    Dispatch(InputManager::HotKeyExpand);
    EXPECT_TRUE(m_state->ui.showGraphView.load());
    EXPECT_FALSE(m_state->history.GetSnapshot()->showLifetimeGraph);

    // Step 2: Lifetime Graph
    Dispatch(InputManager::HotKeyExpand);
    EXPECT_TRUE(m_state->ui.showGraphView.load());
    EXPECT_TRUE(m_state->history.GetSnapshot()->showLifetimeGraph);

    // Step 3: Back to Session Stats
    Dispatch(InputManager::HotKeyExpand);
    EXPECT_FALSE(m_state->ui.showGraphView.load());
    EXPECT_FALSE(m_state->history.GetSnapshot()->showLifetimeGraph);
}

TEST_F(InputManagerActionTest, LifetimeHotkeySwitchesTheGraphWithoutOpeningCards) {
    m_state->ui.graphOffset.store(10);

    Dispatch(InputManager::HotKeyLifetimeGraph);
    EXPECT_TRUE(m_state->history.GetSnapshot()->showLifetimeGraph);
    EXPECT_FALSE(m_state->ui.showSessionView.load());
    EXPECT_FALSE(m_state->ui.showGraphView.load());
    EXPECT_EQ(m_state->ui.graphOffset.load(), 0);

    // An open graph card stays open while it switches back.
    m_state->ui.showSessionView.store(true);
    m_state->ui.showGraphView.store(true);
    m_state->ui.graphOffset.store(5);
    Dispatch(InputManager::HotKeyLifetimeGraph);
    EXPECT_FALSE(m_state->history.GetSnapshot()->showLifetimeGraph);
    EXPECT_TRUE(m_state->ui.showSessionView.load());
    EXPECT_TRUE(m_state->ui.showGraphView.load());
    EXPECT_EQ(m_state->ui.graphOffset.load(), 0);
}

TEST_F(InputManagerActionTest, PanHotkeysScrollGraphWindow) {
    m_state->ui.showSessionView.store(true);
    m_state->ui.showGraphView.store(true);
    m_state->history.Mutate([](auto& history) {
        history.showLifetimeGraph = true;
        for (int i = 0; i < 40; ++i) {
            history.lifetimeMmrY.push_back(1000.0f + static_cast<float>(i));
        }
    });
    EXPECT_EQ(m_state->ui.graphOffset.load(), 0);

    // Pan older (left)
    Dispatch(InputManager::HotKeyPanLeft);
    EXPECT_EQ(m_state->ui.graphOffset.load(), 5);

    Dispatch(InputManager::HotKeyPanLeft);
    EXPECT_EQ(m_state->ui.graphOffset.load(), 10);

    // Pan newer (right)
    Dispatch(InputManager::HotKeyPanRight);
    EXPECT_EQ(m_state->ui.graphOffset.load(), 5);
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
