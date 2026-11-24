#include <gtest/gtest.h>

#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/MatchState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

TEST(TelemetryReducerDiscord, PresenceCarriesScoreFromTheSameUpdate) {
    MatchState state;
    ReducerContext ctx;
    ctx.nowMs = 1000;
    ctx.nowSeconds = 1.0;
    ctx.nowUnix = 1700000000;

    TelemetryEvents::UpdateStateEvent update;
    update.arena = "Stadium_P";
    update.teamScores = {2, 1};
    update.hasTeamScores = true;

    const SideEffects effects = TelemetryReducer::ReduceInPlace(state, update, ctx);

    ASSERT_TRUE(effects.pushDiscord);
    EXPECT_TRUE(effects.discordSnapshot.inMatch);
    EXPECT_EQ(effects.discordSnapshot.score[0], 2);
    EXPECT_EQ(effects.discordSnapshot.score[1], 1);
}
