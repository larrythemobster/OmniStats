#pragma once
#include <cstdint>
#include <optional>
#include <string>

// One discrete, observed telemetry event. Missing telemetry values remain absent.
// All times are in milliseconds; matchClockSeconds is a remaining-game-clock value.
struct MatchJournalEvent {
    std::string type;
    int64_t observedAtUnixMs = 0;
    std::optional<double> matchClockSeconds;
    std::string actorId;
    std::string actorName;
    std::string secondaryId;
    std::string secondaryName;
    std::optional<int> team;
    std::optional<double> value;
    std::optional<double> ballSpeed;
    std::string source;
    std::string eventKey; // Set only for telemetry-provided unique event identifiers.
};
