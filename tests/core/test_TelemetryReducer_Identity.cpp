#include <gtest/gtest.h>

#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/SessionState.hpp"
#include "network/telemetry/TelemetryEvents.hpp"
#include <nlohmann/json.hpp>
#include "utils/Storage.hpp"

namespace {
    nlohmann::json LiveState(bool hasTarget) {
        // Mirrors a recorded online 2v2: you and your teammate both carry car state.
        nlohmann::json game{{"Arena", "Stadium_P"}, {"PlaylistId", 11}, {"TimeSeconds", 250}, {"bReplay", false}, {"bOvertime", false}, {"bHasTarget", hasTarget}};
        if (hasTarget) game["Target"] = {{"Name", "Me"}, {"Shortcut", 6}, {"TeamNum", 1}};
        return {{"Game", game},
                {"Players",
                 {{{"PrimaryId", "Epic|me|0"}, {"Name", "Me"}, {"TeamNum", 1}, {"Shortcut", 6}, {"bHasCar", true}, {"Boost", 40}},
                  {{"PrimaryId", "Steam|mate|0"}, {"Name", "Mate"}, {"TeamNum", 1}, {"Shortcut", 5}, {"bHasCar", true}, {"Boost", 70}},
                  {{"PrimaryId", "Epic|opp|0"}, {"Name", "Opp"}, {"TeamNum", 0}, {"Shortcut", 1}}}}};
    }

    struct Fixture {
        TelemetryReducer reducer;
        UIState ui;
        ReducerContext ctx;
        Fixture() {
            Storage::InitializeEnvironment();
            Config::Update([](ConfigData& c) { c.last_primary_id.clear(); });
            reducer.State().myPrimaryId.clear();
            ctx = MakeReducerContext(ui);
        }
        void SendUpdate(const nlohmann::json& json) {
            auto ev = TelemetryEvents::ParseEventFromJson(Constants::EVT_UPDATE_STATE, json.dump());
            if (ev) {
                reducer.Reduce(*ev, ctx);
            }
        }
        std::string MyId() const {
            return reducer.State().myPrimaryId;
        }
    };
}

TEST(TelemetryReducerIdentity, CameraTargetIdentifiesYouEvenWhenTeammatesSendCarState) {
    Fixture f;
    f.SendUpdate(LiveState(true));
    EXPECT_EQ(f.MyId(), "Epic|me|0");
}

TEST(TelemetryReducerIdentity, NoCameraTargetLeavesIdentityUndecided) {
    Fixture f;
    for (int i = 0; i < 5; ++i)
        f.SendUpdate(LiveState(false));
    EXPECT_EQ(f.MyId(), "");
    f.SendUpdate(LiveState(true));
    EXPECT_EQ(f.MyId(), "Epic|me|0");
}
