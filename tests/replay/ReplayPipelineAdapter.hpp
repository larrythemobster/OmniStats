#pragma once

#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "core/state/SessionState.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/effects/SideEffectExecutor.hpp"
#include "core/effects/SideEffects.hpp"
#include "database/DatabaseManager.hpp"
#include "network/mmr/MMRFetcher.hpp"

namespace omnistats::replay {

    struct ReplayPipelineContext {
        std::shared_ptr<SessionState> state;
        std::shared_ptr<DatabaseManager> dbManager;
        std::shared_ptr<MMRFetcher> mmrFetcher;
        std::unique_ptr<TelemetryReducer> reducer;
        std::unique_ptr<SideEffectExecutor> executor;
        int64_t currentClockMs = 0;
        int64_t lastMatchEndedClockMs = 0;
    };

    struct StepSideEffectsSummary {
        bool hasEffects = false;
        std::string eventName;
        nlohmann::json details = nlohmann::json::object();
    };

    // ============================================================================
    // SINGLE PIPELINE ADAPTER FUNCTION
    // ============================================================================
    // Translates a raw Stats API wire line into event parsing, reducer invocation,
    // and side effect execution. When the livematch slice changes the reducer API
    // (e.g. typed event structs or pure reducer signatures), ONLY this function
    // needs to be updated.
    // Location: tests/replay/ReplayPipelineAdapter.cpp: DispatchWireLine()
    // ============================================================================
    StepSideEffectsSummary DispatchWireLine(ReplayPipelineContext& ctx, const std::string& rawLine);

} // namespace omnistats::replay
