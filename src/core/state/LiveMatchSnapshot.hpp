#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/game/GamemodeUtils.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/state/MatchSaveSnapshot.hpp"
#include "core/state/MatchState.hpp"

struct LiveMatchSnapshot : MatchSnapshotData {
    LiveMatchSnapshot() = default;
    explicit LiveMatchSnapshot(const MatchState& state) : MatchSnapshotData(state) {}
};
