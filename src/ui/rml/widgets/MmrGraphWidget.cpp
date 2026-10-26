#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>
#include <span>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MmrService.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    constexpr int kVisibleGraphMatches = 25;

    struct GraphSeriesData {
        std::string playlist;
        std::span<const float> ratings;
        std::span<const int64_t> times;
        const std::vector<bool>* estimated = nullptr;
        float baseline = -1.0f;
    };

    GraphSeriesData ResolveGraphSeries(const RmlRenderSnapshot& snap, const std::shared_ptr<SessionState>& state) {
        GraphSeriesData data;
        if (snap.showLifetimeGraph) {
            data.playlist = MmrCategoryToString(state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo);
            data.ratings = snap.lifetimeMmrY;
            data.times = snap.lifetimeMmrX;
            if (!data.ratings.empty()) data.baseline = data.ratings.front();
        } else {
            const auto category = state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
            data.playlist = MmrCategoryToString(category);
            if (auto it = snap.playlistHistoryY.find(data.playlist); it != snap.playlistHistoryY.end()) {
                data.ratings = it->second;
            }
            if (auto it = snap.playlistHistoryEstimated.find(data.playlist); it != snap.playlistHistoryEstimated.end()) {
                data.estimated = &it->second;
            }
            if (auto it = snap.playlistInitialMmr.find(data.playlist); it != snap.playlistInitialMmr.end()) {
                data.baseline = static_cast<float>(it->second);
            }
        }
        return data;
    }

    std::string RenderGraphPanControlsImpl(const GraphWindow& window) {
        if (window.total <= kVisibleGraphMatches) return {};
        std::ostringstream out;
        out << "<span class='label mono graph-range'>" << (window.firstIndex + 1) << '-' << (window.firstIndex + window.count) << " of " << window.total
            << "</span><div class='graph-zoom'>"
            << Button("graph-pan-older", "\xE2\x86\x90", "ghost zoom-step", !window.canPanOlder)
            << Button("graph-pan-newer", "\xE2\x86\x92", "ghost zoom-step", !window.canPanNewer)
            << "</div>";
        return out.str();
    }
}

std::string RenderMmrGraphImpl(bool showCategoryBadge, bool showPanControls, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state) {
    const auto category = state ? state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const auto series = ResolveGraphSeries(snap, state);
    const int offset = state ? state->ui.graphOffset.load() : 0;
    const auto window = ComputeGraphWindow(static_cast<int>(series.ratings.size()), offset, kVisibleGraphMatches);
    const auto rankTable = MmrService::RankThresholdsForPlaylist(series.playlist);

    std::ostringstream header;
    const std::string pan = showPanControls ? RenderGraphPanControlsImpl(window) : std::string{};
    if (showCategoryBadge || !pan.empty()) {
        header << "<div class='row graph-header'>";
        if (showCategoryBadge) header << "<span class='badge'>" << Escape(MmrLabel(category)) << "</span>";
        header << "<div class='grow'></div>" << pan << "</div>";
    }

    if (window.count == 0) {
        std::ostringstream empty;
        empty << header.str();
        if (snap.showLifetimeGraph) {
            empty << "<div class='graph-empty'><div class='muted'>No lifetime MMR history in database yet.</div>"
                  << "<div class='label'>Play matches to populate database records!</div></div>";
        } else {
            empty << "<div class='graph-empty'><div class='muted'>No MMR data for " << Escape(MmrLabel(category))
                  << " this session.</div><div class='label'>Play a match to start plotting!</div></div>";
        }
        return empty.str();
    }

    const auto windowRatings = series.ratings.subspan(window.firstIndex, window.count);
    float baseline = series.baseline;
    if (snap.showLifetimeGraph && !windowRatings.empty()) {
        baseline = windowRatings.front();
    }
    const bool hasBaseline = baseline > 0.0f;
    // A session graph starts at the rating from before the first match, so a
    // single game still draws a line from where the session began.
    const bool sessionStart = !snap.showLifetimeGraph && hasBaseline && window.firstIndex == 0;
    std::array<float, kVisibleGraphMatches + 1> plotStorage;
    const size_t firstMatch = sessionStart ? 1 : 0;
    if (sessionStart) plotStorage[0] = baseline;
    std::copy(windowRatings.begin(), windowRatings.end(), plotStorage.begin() + firstMatch);
    const std::span<const float> plotted(plotStorage.data(), windowRatings.size() + firstMatch);

    float minV = *std::min_element(plotted.begin(), plotted.end());
    float maxV = *std::max_element(plotted.begin(), plotted.end());
    if (maxV == minV) {
        maxV += 15.0f;
        minV -= 15.0f;
    } else {
        const float padding = (maxV - minV) * 0.20f;
        maxV += padding;
        minV -= padding;
    }
    constexpr float kBoundaryReach = 60.0f;
    if (!rankTable.empty()) RankProgress::ExpandRangeForBoundaries(rankTable, plotted.back(), kBoundaryReach, minV, maxV);

    constexpr float xMin = 4.0f;
    constexpr float xMax = 95.0f;
    constexpr float yMin = 6.0f;
    constexpr float yMax = 86.0f;
    auto yPct = [&](float value) { return yMax - ((value - minV) / (maxV - minV)) * (yMax - yMin); };
    auto xPct = [&](size_t index) { return plotted.size() <= 1 ? xMax : xMin + static_cast<float>(index) / static_cast<float>(plotted.size() - 1) * (xMax - xMin); };

    std::ostringstream out;
    out << header.str() << "<div class='graph-wrap'>";

    // Division bands are the y-axis when the playlist has a rank table; their
    // boundary values replace generic gridlines.
    const auto bands = RankProgress::LayoutGraphBands(rankTable, minV, maxV, yMin, yMax);
    constexpr float kBandLabelMinPct = 8.0f;
    for (const auto& band : bands) {
        ColorRGBA tint = Format::RankColor(band.label);
        tint.a = band.index % 2 == 0 ? 0.10f : 0.17f;
        out << "<div class='graph-band' style='left:" << xMin << "%;width:" << (xMax - xMin) << "%;top:" << band.topPct
            << "%;height:" << (band.bottomPct - band.topPct) << "%;background-color:" << CssColor(tint) << "'></div>";
        if (band.bottomPct - band.topPct >= kBandLabelMinPct)
            out << "<div class='graph-band-label' style='left:" << (xMin + 1.0f) << "%;top:" << (band.topPct + 1.0f)
                << "%;color:" << CssColor(Format::RankColor(band.label)) << "'>"
                << Escape(Format::RankTier(band.label, config.use_roman_numerals)) << "</div>";
    }
    if (bands.empty()) {
        const float midV = (minV + maxV) * 0.5f;
        out << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMin << "%;width:" << (xMax - xMin) << "%'></div>"
            << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMax << "%;width:" << (xMax - xMin) << "%'></div>"
            << "<div class='graph-gridline' style='left:" << xMin << "%;top:" << yPct(midV) << "%;width:" << (xMax - xMin) << "%'></div>"
            << "<div class='graph-label' style='right:" << (100.0f - xMax + 1.0f) << "%;top:" << (yMin + 0.5f) << "%'>" << static_cast<int>(maxV) << "</div>"
            << "<div class='graph-label' style='right:" << (100.0f - xMax + 1.0f) << "%;top:" << (yPct(midV) + 0.5f) << "%'>" << static_cast<int>(midV)
            << "</div>"
            << "<div class='graph-label' style='right:" << (100.0f - xMax + 1.0f) << "%;top:" << (yMax - 8.0f) << "%'>" << static_cast<int>(minV) << "</div>";
    }

    RankProgress::GraphLayoutOptions boundaryLayout;
    boundaryLayout.plotTopPct = yMin;
    boundaryLayout.plotBottomPct = yMax;
    const float currentTop = yPct(plotted.back());
    const float currentReserveAbove = currentTop >= 45.0f ? 16.0f : 8.0f;
    const float currentReserveBelow = currentTop < 45.0f ? 16.0f : 8.0f;
    boundaryLayout.reserved = {
        {0.0f, yMin},
        {yMax, 100.0f},
        {std::max(0.0f, currentTop - currentReserveAbove), std::min(100.0f, currentTop + currentReserveBelow)}};
    const auto boundaries = RankProgress::LayoutGraphBoundaries(rankTable, minV, maxV, plotted.back(), boundaryLayout);
    for (const auto& b : boundaries) {
        const std::string color = CssColor(Format::RankColor(b.boundary.label));
        out << "<div class='graph-rank-line tooltip-host" << (b.boundary.tierStart ? " tier-start" : "") << "' style='left:" << xMin
            << "%;top:" << b.topPct << "%;width:" << (xMax - xMin) << "%'><div class='graph-rank-rule' style='background-color:" << color
            << "'></div><span class='tooltip-bubble" << (b.topPct < 25.0f ? " below" : "") << "'>"
            << Escape(Format::RankTier(b.boundary.label, config.use_roman_numerals) + " starts at " + std::to_string(b.boundary.mmr) + " MMR")
            << "</span></div>";
    }

    if (hasBaseline && baseline >= minV && baseline <= maxV) {
        const float baseY = yPct(baseline);
        constexpr int dashCount = 19;
        constexpr float dashWidth = 2.0f;
        const float step = (xMax - xMin) / static_cast<float>(dashCount);
        for (int i = 0; i < dashCount; ++i) {
            const float left = xMin + static_cast<float>(i) * step;
            out << "<div class='graph-line baseline' style='left:" << left << "%;top:" << baseY << "%;width:" << dashWidth << "%'></div>";
        }
    }

    out << "<mmrgraphlines class='graph-polyline' fill-to='" << (yMax / 100.0f) << "' points='";
    for (size_t i = 0; i < plotted.size(); ++i) {
        if (i) out << ';';
        out << (xPct(i) / 100.0f) << ',' << (yPct(plotted[i]) / 100.0f);
    }
    out << "'></mmrgraphlines>";

    for (size_t i = 0; i < plotted.size(); ++i) {
        const float compareTo = i > 0                    ? plotted[i - 1]
                                : window.firstIndex > 0  ? series.ratings[static_cast<size_t>(window.firstIndex) - 1]
                                : snap.showLifetimeGraph ? 0.0f
                                                         : baseline;
        const int mmr = static_cast<int>(std::lround(plotted[i]));
        const bool isStart = sessionStart && i == 0;
        std::string cls;
        std::string tip;
        if (isStart) {
            cls = " start";
            tip = "Session start \xC2\xB7 " + std::to_string(mmr) + " MMR";
        } else {
            const size_t valueIndex = i - firstMatch;
            const int game = window.firstIndex + static_cast<int>(valueIndex) + 1;
            tip = "Game " + std::to_string(game);
            const size_t timeIdx = static_cast<size_t>(window.firstIndex) + valueIndex;
            if (snap.showLifetimeGraph && timeIdx < series.times.size())
                tip += " \xC2\xB7 " + FormatClock(static_cast<int64_t>(series.times[timeIdx]));
            tip += " \xC2\xB7 " + std::to_string(mmr) + " MMR";
            if (compareTo > 0.0f) {
                const int delta = mmr - static_cast<int>(std::lround(compareTo));
                cls = delta > 0 ? " win" : delta < 0 ? " loss"
                                                     : " neutral";
                tip += std::string(" (") + (delta >= 0 ? "+" : "") + std::to_string(delta) + ")";
            } else {
                cls = " neutral";
            }
            const size_t estIdx = static_cast<size_t>(window.firstIndex) + valueIndex;
            if (series.estimated && estIdx < series.estimated->size() && (*series.estimated)[estIdx]) {
                cls += " estimated";
                tip += " \xC2\xB7 pending Tracker confirmation";
            }
        }
        const float x = xPct(i);
        const float y = yPct(plotted[i]);
        const bool isCurrent = i + 1 == plotted.size();
        if (isCurrent) {
            out << "<div class='graph-point-halo" << cls << "' style='left:" << x << "%;top:" << y << "%'></div>";
            cls += " current";
        }
        out << "<div class='graph-point" << cls << "' style='left:" << x << "%;top:" << y << "%'></div>"
            << "<div class='graph-hover-row' style='top:" << y << "%'>";
        if (plotted.size() <= 2) {
            out << "<div class='graph-hit' style='left:" << x << "%'></div>";
        } else {
            const float xSpacing = (xMax - xMin) / static_cast<float>(plotted.size() - 1);
            const float hitWidth = xSpacing * 0.96f;
            out << "<div class='graph-hit' style='left:" << x
                << "%;width:" << hitWidth << "%;margin-left:-" << (hitWidth * 0.5f) << "%'></div>";
        }
        const float tooltipLeft = std::clamp(x - 27.0f, 4.0f, 38.0f);
        out << "<span class='tooltip-bubble" << (y < 45.0f ? " below" : "")
            << "' style='left:" << tooltipLeft << "%'>" << Escape(tip)
            << "</span></div>";
    }

    // Labels stop short of the plot's right edge so the current point stays clear.
    for (const auto& b : boundaries) {
        if (!b.labeled) continue;
        out << "<div class='graph-rank-label mono' style='right:" << (100.0f - xMax + 2.0f) << "%;top:" << b.labelTopPct
            << "%;color:" << CssColor(Format::RankColor(b.boundary.label)) << "'>" << b.boundary.mmr << "</div>";
    }

    const float axisTop = yMax + 2.0f;
    if (snap.showLifetimeGraph && !series.times.empty() && window.firstIndex < static_cast<int>(series.times.size())) {
        const int lastIndex = std::min(window.firstIndex + window.count - 1, static_cast<int>(series.times.size()) - 1);
        out << "<div class='graph-label' style='left:" << xMin << "%;top:" << axisTop << "%'>"
            << Escape(FormatClock(static_cast<int64_t>(series.times[static_cast<size_t>(window.firstIndex)]))) << "</div>"
            << "<div class='graph-label graph-last' style='right:" << (100.0f - xMax) << "%;top:" << axisTop << "%'>"
            << Escape(FormatClock(static_cast<int64_t>(series.times[static_cast<size_t>(lastIndex)]))) << "</div>";
    } else {
        if (plotted.size() > 1)
            out << "<div class='graph-label' style='left:" << xMin << "%;top:" << axisTop << "%'>"
                << (sessionStart ? std::string("Start") : "Game " + std::to_string(window.firstIndex + 1)) << "</div>";
        out << "<div class='graph-label graph-last' style='right:" << (100.0f - xMax) << "%;top:" << axisTop << "%'>Game "
            << (window.firstIndex + window.count) << "</div>";
    }

    out << "</div>";
    return out.str();
}

std::string OverlayPresenter::RenderMmrGraph(bool showCategoryBadge, bool showPanControls, const RmlRenderSnapshot& snap, const ConfigData& config, const std::shared_ptr<SessionState>& state) {
    return RenderMmrGraphImpl(showCategoryBadge, showPanControls, snap, config, state);
}

std::string OverlayPresenter::RenderGraphPanControls(const RmlRenderSnapshot& snap, const std::shared_ptr<SessionState>& state) {
    const auto series = ResolveGraphSeries(snap, state);
    const int offset = state ? state->ui.graphOffset.load() : 0;
    return RenderGraphPanControlsImpl(ComputeGraphWindow(static_cast<int>(series.ratings.size()), offset, kVisibleGraphMatches));
}

std::string RmlUiController::RenderGraphPanControls() const {
    return m_overlayPresenter->RenderGraphPanControls(m_snap, m_state);
}

std::string RmlUiController::RenderMmrGraph(bool showCategoryBadge, bool showPanControls) {
    return m_overlayPresenter->RenderMmrGraph(showCategoryBadge, showPanControls, m_snap, m_config, m_state);
}

void RmlUiController::PanGraph(int direction) {
    if (!m_state || direction == 0) return;
    size_t sampleCount = m_snap.showLifetimeGraph ? m_snap.lifetimeMmrY.size() : 0;
    if (!m_snap.showLifetimeGraph) {
        const auto playlist = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
        if (const auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) sampleCount = it->second.size();
    }
    const int total = static_cast<int>(sampleCount);
    if (total <= kVisibleGraphMatches) return;
    constexpr int step = 5;
    const int maxOffset = total - kVisibleGraphMatches;
    const int currentOffset = m_state->ui.graphOffset.load();
    const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
    if (newOffset == currentOffset) return;
    m_state->ui.graphOffset.store(newOffset);
    RebuildVisibleUi(false);
}
