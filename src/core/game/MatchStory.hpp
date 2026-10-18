#pragma once

#include <string>
#include <vector>

#include "core/state/SessionState.hpp"

namespace MatchStory {

    // Generates 0-5 short factual story lines ranked by interest from a match's details.
    // Lines only appear when thresholds for notable events are met.
    std::vector<std::string> BuildMatchStory(const MatchDetail& detail);

} // namespace MatchStory
