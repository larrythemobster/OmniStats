#pragma once
#include <string>
#include <array>

struct DiscordPresenceSnapshot {
    bool showPresence = false;
    bool inMatch = false;
    std::string arenaName;
    std::array<int, 2> score{};
    int myTeam = -1;
    int sessionWins = 0;
    int sessionLosses = 0;
};
