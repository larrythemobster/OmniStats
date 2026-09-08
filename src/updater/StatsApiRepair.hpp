#pragma once

#include <string>

namespace UpdaterStatsApiRepair {

    // Return codes are intentionally stable because OmniStats maps them back to
    // user-facing StatsApiConfig::Status values.
    // 0 = success
    // 2 = invalid/missing DefaultStatsAPI.ini
    // 3 = backup/write preparation failed
    // 4 = read failed
    // 5 = final write failed
    int FixConfigStrictHeadless(const std::string& filePath, int expectedPort);

} // namespace UpdaterStatsApiRepair
