#pragma once

// Distance to the neighbouring rank divisions and the division boundaries drawn
// on the MMR graph, from Tracker Network's per-playlist division table.

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace RankProgress {

    // Lowest MMR of one division. `division` is empty for Supersonic Legend.
    struct Threshold {
        int minMmr;
        const char* tier;
        const char* division;
    };

    struct Boundary {
        int mmr = 0;
        std::string label; // "Champion II Div III"
        bool tierStart = false;
        size_t index = 0;
    };

    enum class Status {
        Unavailable, // no rating, or no rank table for the playlist
        Placement,   // the rank source reports Unranked for a rated playlist
        Ranked,
    };

    struct Progress {
        Status status = Status::Unavailable;
        int mmr = 0;
        std::string rank;
        std::optional<Boundary> floor;    // start of the current division; absent in the lowest one
        std::optional<Boundary> next;     // next division up; absent at the top rank
        std::optional<Boundary> previous; // division a demotion would drop to
        int toNext = 0;                   // next->mmr - mmr; <= 0 once the threshold is reached
        int aboveFloor = 0;               // mmr - floor->mmr; < 0 when the demotion buffer holds the division
    };

    // `reportedTier` is the source's rank label for this rating ("" when unknown),
    // e.g. "Champion II Div III" or "Unranked".
    Progress Compute(std::span<const Threshold> table, int mmr, std::string_view reportedTier);

    // Widens [minV, maxV] so the boundaries directly above and below `current`
    // are on the graph when they lie within `maxExtension` MMR of the range.
    void ExpandRangeForBoundaries(std::span<const Threshold> table, float current, float maxExtension, float& minV, float& maxV);

    struct GraphLayoutOptions {
        float plotTopPct = 10.0f;
        float plotBottomPct = 88.0f;
        float labelHeightPct = 8.0f;
        size_t maxLines = 8;
        size_t maxLabels = 4;
        // Vertical spans (top%, bottom%) already occupied by other labels.
        std::vector<std::pair<float, float>> reserved;
    };

    struct GraphBoundary {
        Boundary boundary;
        float topPct = 0.0f;
        bool labeled = false;
        float labelTopPct = 0.0f;
    };

    // Boundaries inside [minV, maxV], thinned to tier starts plus the divisions
    // adjacent to `current` when crowded, with non-overlapping labels assigned
    // nearest-first.
    std::vector<GraphBoundary> LayoutGraphBoundaries(std::span<const Threshold> table, float minV, float maxV, float current,
                                                     const GraphLayoutOptions& options);

    // The part of one division visible on the graph, as plot percentages
    // (0 = top). `index` is the division's position in the table.
    struct GraphBand {
        std::string label;
        size_t index = 0;
        float topPct = 0.0f;
        float bottomPct = 0.0f;
    };

    // Every division overlapping [minV, maxV], highest first, clipped to the plot.
    std::vector<GraphBand> LayoutGraphBands(std::span<const Threshold> table, float minV, float maxV, float plotTopPct, float plotBottomPct);

} // namespace RankProgress
