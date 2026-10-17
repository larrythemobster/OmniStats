#pragma once
#include <string>
#include <string_view>

namespace ArenaNames {
    // Rocket League map package name (e.g. "Stadium_Foggy_P") to its in-game display name.
    // Unknown packages fall back to a readable form of the package name.
    std::string DisplayName(std::string_view asset);
    // Corrects a stored Matches.arena value written by older clients: raw package
    // names and the display names produced by the previous asset table.
    std::string RepairStoredName(std::string_view stored);
}
