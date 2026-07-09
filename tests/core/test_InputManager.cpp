#include <gtest/gtest.h>
#include "core/InputManager.hpp"
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
