#include <gtest/gtest.h>
#include "network/DiscordManager.hpp"
#include "core/state/SessionState.hpp"
#include <memory>

TEST(DiscordActivityClockTest, ElapsedTimerSurvivesScoreUpdatesAndRestartsOnTransitions) {
    DiscordActivityClock clock;
    DiscordPresenceSnapshot match;
    match.showPresence = true;
    match.inMatch = true;
    match.matchGuid = "guid-a";
    match.arenaName = "DFH Stadium";

    EXPECT_EQ(clock.StartFor(match, 1000), 1000);
    match.score = {1, 0};
    EXPECT_EQ(clock.StartFor(match, 1090), 1000);
    match.score = {1, 1};
    EXPECT_EQ(clock.StartFor(match, 1200), 1000);

    DiscordPresenceSnapshot menus;
    menus.showPresence = true;
    EXPECT_EQ(clock.StartFor(menus, 1400), 1400);
    EXPECT_EQ(clock.StartFor(menus, 1500), 1400);

    match.matchGuid = "guid-b";
    EXPECT_EQ(clock.StartFor(match, 1600), 1600);
    clock.Reset();
    EXPECT_EQ(clock.StartFor(match, 1700), 1700);
}
