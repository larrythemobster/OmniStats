#pragma once

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "core/state/SessionState.hpp"
#include "core/effects/SideEffects.hpp"
#include "database/DatabaseManager.hpp"
#include "network/mmr/MMRFetcher.hpp"

namespace omnistats::replay {

    struct ReplayPipelineContext {
        std::shared_ptr<SessionState> state;
        std::shared_ptr<DatabaseManager> dbManager;
        std::shared_ptr<MMRFetcher> mmrFetcher;
        int64_t currentClockMs = 0;
        int64_t lastMatchEndedClockMs = 0;
    };

    struct StepSideEffectsSummary {
        bool hasEffects = false;
        std::string eventName;
        nlohmann::json details = nlohmann::json::object();
    };

    // The only coupling to pipeline internals: feeds one recorded wire line through the live match store
    // and effect router the way StatsClient does, on the recording's clock.
    StepSideEffectsSummary DispatchWireLine(ReplayPipelineContext& ctx, const std::string& rawLine);

} // namespace omnistats::replay
