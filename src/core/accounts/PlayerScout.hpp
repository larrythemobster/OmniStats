#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/game/Insights.hpp"

// Only recorded matches. No inferred skills, public reputation or extrapolated results.
struct PlayerScoutEncounter {
    int64_t matchId = 0;
    int64_t playedAtUnix = 0;
    std::string playlist;
    bool opponent = false;
    bool win = false; // From the selected local account's perspective.
};

struct PlayerScoutProfile {
    PersonRecord record;
    std::string mostFrequentPlaylist;
    std::vector<PlayerScoutEncounter> recent;
};
