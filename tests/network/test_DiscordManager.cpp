#include <gtest/gtest.h>
#include "network/DiscordManager.hpp"
#include "core/SessionState.hpp"
#include <memory>

TEST(DiscordManagerTest, LifecycleAndPush) {
    auto state = std::make_shared<SessionState>();
    DiscordManager discord(state);
    
    EXPECT_NO_THROW(discord.Initialize());
    
    DiscordPresenceSnapshot snap;
    snap.showPresence = true;
    snap.inMatch = true;
    snap.arenaName = "Test Arena";
    
    EXPECT_NO_THROW(discord.PushPresenceUpdate(snap));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    EXPECT_NO_THROW(discord.Shutdown());
}
