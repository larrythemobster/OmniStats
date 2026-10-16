#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Tournaments {
    struct DetectedRegion {
        std::string region;
        std::string source;
        std::string server;
        int pingMs = 0;
    };
    std::string RegionGroup(std::string_view region);
    std::optional<DetectedRegion> ParseRegionCache(std::span<const uint8_t> bytes);
    std::optional<DetectedRegion> ParseMatchRegions(std::string_view log);
    std::optional<DetectedRegion> DetectRegion();
}
