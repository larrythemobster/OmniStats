#include "core/RankProgress.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace RankProgress {
    namespace {
        std::string Label(const Threshold& t) {
            std::string label = t.tier ? t.tier : "";
            if (t.division && t.division[0] != '\0') label += std::string(" ") + t.division;
            return label;
        }

        Boundary MakeBoundary(std::span<const Threshold> table, size_t index) {
            const Threshold& t = table[index];
            Boundary b;
            b.mmr = t.minMmr;
            b.label = Label(t);
            b.tierStart = !t.division || t.division[0] == '\0' || std::string_view(t.division) == "Div I";
            b.index = index;
            return b;
        }

        // Division containing `mmr` per the table; 0 below the table.
        size_t TableIndex(std::span<const Threshold> table, float mmr) {
            size_t index = 0;
            for (size_t i = 0; i < table.size(); ++i) {
                if (static_cast<float>(table[i].minMmr) > mmr) break;
                index = i;
            }
            return index;
        }

        std::string_view Trim(std::string_view text) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
                text.remove_prefix(1);
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
                text.remove_suffix(1);
            return text;
        }

        bool Overlaps(float top, float bottom, const std::pair<float, float>& span) {
            return top < span.second && span.first < bottom;
        }
    }

    Progress Compute(std::span<const Threshold> table, int mmr, std::string_view reportedTier) {
        Progress progress;
        progress.mmr = mmr;
        if (table.empty() || mmr <= 0) return progress;

        reportedTier = Trim(reportedTier);
        if (reportedTier.rfind("Unranked", 0) == 0) {
            progress.status = Status::Placement;
            progress.rank = "Unranked";
            return progress;
        }

        // The reported division wins over the table: Rocket League's demotion
        // buffer can keep a player in a division below its threshold.
        std::optional<size_t> index;
        if (!reportedTier.empty()) {
            for (size_t i = 0; i < table.size(); ++i) {
                if (Label(table[i]) == reportedTier) {
                    index = i;
                    break;
                }
            }
        }
        if (!index) index = TableIndex(table, static_cast<float>(mmr));

        progress.status = Status::Ranked;
        progress.rank = Label(table[*index]);
        if (*index > 0) {
            progress.floor = MakeBoundary(table, *index);
            progress.previous = MakeBoundary(table, *index - 1);
            progress.aboveFloor = mmr - progress.floor->mmr;
        }
        if (*index + 1 < table.size()) {
            progress.next = MakeBoundary(table, *index + 1);
            progress.toNext = progress.next->mmr - mmr;
        }
        return progress;
    }

    void ExpandRangeForBoundaries(std::span<const Threshold> table, float current, float maxExtension, float& minV, float& maxV) {
        if (table.empty() || !std::isfinite(current) || !(maxV > minV)) return;
        const size_t index = TableIndex(table, current);
        constexpr float kEdgeMargin = 3.0f;
        if (index > 0) {
            const float below = static_cast<float>(table[index].minMmr);
            if (below < minV && minV - below <= maxExtension) minV = below - kEdgeMargin;
        }
        if (index + 1 < table.size()) {
            const float above = static_cast<float>(table[index + 1].minMmr);
            if (above > maxV && above - maxV <= maxExtension) maxV = above + kEdgeMargin;
        }
    }

    std::vector<GraphBoundary> LayoutGraphBoundaries(std::span<const Threshold> table, float minV, float maxV, float current,
                                                     const GraphLayoutOptions& options) {
        std::vector<GraphBoundary> out;
        if (table.size() < 2 || !(maxV > minV) || !std::isfinite(minV) || !std::isfinite(maxV)) return out;

        // Index 0 is the bottom of the ladder, not a boundary between two ranks.
        std::vector<Boundary> visible;
        for (size_t i = 1; i < table.size(); ++i) {
            const float value = static_cast<float>(table[i].minMmr);
            if (value >= minV && value <= maxV) visible.push_back(MakeBoundary(table, i));
        }

        const size_t currentIndex = TableIndex(table, current);
        const auto distance = [&](const Boundary& b) { return std::fabs(static_cast<float>(b.mmr) - current); };
        if (visible.size() > options.maxLines) {
            std::erase_if(visible, [&](const Boundary& b) {
                return !b.tierStart && b.index != currentIndex && b.index != currentIndex + 1;
            });
            if (visible.size() > options.maxLines) {
                std::stable_sort(visible.begin(), visible.end(), [&](const Boundary& a, const Boundary& b) { return distance(a) < distance(b); });
                visible.resize(options.maxLines);
            }
        }
        std::sort(visible.begin(), visible.end(), [](const Boundary& a, const Boundary& b) { return a.mmr > b.mmr; });

        const float span = options.plotBottomPct - options.plotTopPct;
        out.reserve(visible.size());
        for (auto& boundary : visible) {
            GraphBoundary g;
            g.topPct = options.plotBottomPct - (static_cast<float>(boundary.mmr) - minV) / (maxV - minV) * span;
            g.boundary = std::move(boundary);
            out.push_back(std::move(g));
        }

        std::vector<size_t> order(out.size());
        for (size_t i = 0; i < order.size(); ++i)
            order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return distance(out[a].boundary) < distance(out[b].boundary); });

        std::vector<std::pair<float, float>> taken = options.reserved;
        size_t labels = 0;
        for (size_t i : order) {
            if (labels >= options.maxLabels) break;
            GraphBoundary& g = out[i];
            for (const float top : {g.topPct - options.labelHeightPct - 0.5f, g.topPct + 0.5f}) {
                const float bottom = top + options.labelHeightPct;
                if (top < 0.0f || bottom > 100.0f) continue;
                if (std::any_of(taken.begin(), taken.end(), [&](const auto& s) { return Overlaps(top, bottom, s); })) continue;
                g.labeled = true;
                g.labelTopPct = top;
                taken.emplace_back(top, bottom);
                ++labels;
                break;
            }
        }
        return out;
    }

    std::vector<GraphBand> LayoutGraphBands(std::span<const Threshold> table, float minV, float maxV, float plotTopPct, float plotBottomPct) {
        std::vector<GraphBand> out;
        if (table.empty() || !(maxV > minV) || !std::isfinite(minV) || !std::isfinite(maxV)) return out;
        const float span = plotBottomPct - plotTopPct;
        const auto pct = [&](float value) { return plotBottomPct - (value - minV) / (maxV - minV) * span; };
        for (size_t i = table.size(); i-- > 0;) {
            // The lowest division also covers ratings below the table's floor.
            const float low = i == 0 ? minV : std::max(static_cast<float>(table[i].minMmr), minV);
            const float high = i + 1 < table.size() ? std::min(static_cast<float>(table[i + 1].minMmr), maxV) : maxV;
            if (!(high > low)) continue;
            GraphBand band;
            band.label = Label(table[i]);
            band.index = i;
            band.topPct = pct(high);
            band.bottomPct = pct(low);
            out.push_back(std::move(band));
        }
        return out;
    }

} // namespace RankProgress
