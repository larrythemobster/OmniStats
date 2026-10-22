#include "ui/rml/RmlUiPch.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include "core/config/AppVersion.hpp"
#include "utils/Storage.hpp"
#include "core/config/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/window/KeyNames.hpp"
#include "ui/rml/backend/RmlInputWin32.hpp"
#include "ui/rml/widgets/RmlMmrGraphLines.hpp"

#include "ui/rml/controller/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    constexpr int kVisibleGraphMatches = 25;
}

RmlUiController::GraphSeriesData RmlUiController::ResolveGraphSeries() const {
    GraphSeriesData data;
    if (m_snap.showLifetimeGraph) {
        data.playlist = MmrCategoryToString(m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo);
        data.ratings = m_snap.lifetimeMmrY;
        data.times = m_snap.lifetimeMmrX;
        if (!data.ratings.empty()) data.baseline = data.ratings.front();
    } else {
        const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
        data.playlist = MmrCategoryToString(category);
        if (auto it = m_snap.playlistHistoryY.find(data.playlist); it != m_snap.playlistHistoryY.end()) {
            data.ratings = it->second;
        }
        if (auto it = m_snap.playlistHistoryEstimated.find(data.playlist); it != m_snap.playlistHistoryEstimated.end()) {
            data.estimated = &it->second;
        }
        if (auto it = m_snap.playlistInitialMmr.find(data.playlist); it != m_snap.playlistInitialMmr.end()) {
            data.baseline = static_cast<float>(it->second);
        }
    }
    return data;
}

std::string RmlUiController::RenderGraphPanControls() const {
    const auto series = ResolveGraphSeries();
    const int offset = m_state ? m_state->ui.graphOffset.load() : 0;
    const auto window = ComputeGraphWindow(static_cast<int>(series.ratings.size()), offset, kVisibleGraphMatches);
    return RenderGraphPanControls(window);
}

std::string RmlUiController::RenderGraphPanControls(const GraphWindow& window) const {
    if (window.total <= kVisibleGraphMatches) return {};
    std::ostringstream out;
    out << "<span class='label mono graph-range'>" << (window.firstIndex + 1) << '-' << (window.firstIndex + window.count) << " of " << window.total
        << "</span><div class='graph-zoom'>"
        << Button("graph-pan-older", "\xE2\x86\x90", "ghost zoom-step", !window.canPanOlder)
        << Button("graph-pan-newer", "\xE2\x86\x92", "ghost zoom-step", !window.canPanNewer)
        << "</div>";
    return out.str();
}

std::string RmlUiController::RenderMmrGraph(bool showCategoryBadge, bool showPanControls) {
    const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const auto series = ResolveGraphSeries();
    const int offset = m_state ? m_state->ui.graphOffset.load() : 0;
    const auto window = ComputeGraphWindow(static_cast<int>(series.ratings.size()), offset, kVisibleGraphMatches);
    const auto rankTable = MMRFetcher::RankThresholdsForPlaylist(series.playlist);

    std::ostringstream header;
    const std::string pan = showPanControls ? RenderGraphPanControls(window) : std::string{};
    if (showCategoryBadge || !pan.empty()) {
        header << "<div class='row graph-header'>";
        if (showCategoryBadge) header << "<span class='badge'>" << Escape(MmrLabel(category)) << "</span>";
        header << "<div class='grow'></div>" << pan << "</div>";
    }

    if (window.count == 0) {
        std::ostringstream empty;
        empty << header.str();
        if (m_snap.showLifetimeGraph) {
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
    if (m_snap.showLifetimeGraph && !windowRatings.empty()) {
        baseline = windowRatings.front();
    }
    const bool hasBaseline = baseline > 0.0f;
    // A session graph starts at the rating from before the first match, so a
    // single game still draws a line from where the session began.
    const bool sessionStart = !m_snap.showLifetimeGraph && hasBaseline && window.firstIndex == 0;
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
                << Escape(Format::RankTier(band.label, m_config.use_roman_numerals)) << "</div>";
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
            << Escape(Format::RankTier(b.boundary.label, m_config.use_roman_numerals) + " starts at " + std::to_string(b.boundary.mmr) + " MMR")
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
        const float compareTo = i > 0                      ? plotted[i - 1]
                                : window.firstIndex > 0    ? series.ratings[static_cast<size_t>(window.firstIndex) - 1]
                                : m_snap.showLifetimeGraph ? 0.0f
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
            if (m_snap.showLifetimeGraph && timeIdx < series.times.size())
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
    if (m_snap.showLifetimeGraph && !series.times.empty() && window.firstIndex < static_cast<int>(series.times.size())) {
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

RankProgress::Progress RmlUiController::RankProgressFor(const PlayerData* player, const std::string& playlist, int mmr) const {
    // A reported division only describes the rating it was fetched with. The
    // tournament tier is itself derived from the table, so it is not evidence.
    std::string reportedTier;
    if (player && playlist != "t") {
        const auto mmrIt = player->playlists.find(playlist);
        const auto tierIt = player->playlistTiers.find(playlist);
        if (mmrIt != player->playlists.end() && tierIt != player->playlistTiers.end() && mmrIt->second == mmr) reportedTier = tierIt->second;
    }
    return RankProgress::Compute(MMRFetcher::RankThresholdsForPlaylist(playlist), mmr, reportedTier);
}

std::string RmlUiController::RenderRankProgress(const RankProgress::Progress& progress) const {
    using RankProgress::Status;
    if (progress.status == Status::Unavailable) return {};

    const auto rankName = [&](const std::string& label) { return Format::RankTier(label, m_config.use_roman_numerals); };
    const auto coloredName = [&](const std::string& label) {
        return "<span class='rank-progress-tier' style='color:" + CssColor(Format::RankColor(label)) + "'>" + Escape(rankName(label)) + "</span>";
    };
    const auto delta = [&](const char* tone, const std::string& value, const std::string& tip, bool alignRight) {
        return std::string("<span class='rank-progress-delta mono tooltip-host ") + tone + "'>" + Escape(value) + "<span class='tooltip-bubble" +
               (alignRight ? " align-right" : "") + "'>" + Escape(tip) + "</span></span>";
    };

    std::ostringstream out;
    out << "<div class='rank-progress'><div class='row rank-progress-names'>" << coloredName(progress.rank);
    if (progress.status == Status::Placement) {
        out << "<span class='label'>placement</span></div>"
            << "<div class='label rank-progress-note'>Division progress appears once Rocket League assigns a rank.</div></div>";
        return out.str();
    }
    out << "<div class='grow'></div>";
    if (progress.next)
        out << coloredName(progress.next->label);
    else
        out << "<span class='label'>Top rank</span>";
    out << "</div><div class='row rank-progress-bar'>";

    std::string note;
    if (progress.floor && progress.previous) {
        const std::string floorText = rankName(progress.floor->label) + " (starts at " + std::to_string(progress.floor->mmr) + " MMR)";
        if (progress.aboveFloor > 0) {
            out << delta("loss", "-" + std::to_string(progress.aboveFloor),
                         "Lose " + std::to_string(progress.aboveFloor) + " MMR to reach lower boundary of " + floorText + ". Rocket League's demotion buffer can delay the drop to " +
                             rankName(progress.previous->label) + ".",
                         false);
        } else if (progress.aboveFloor == 0) {
            out << delta("loss", "0",
                         "At lower boundary of " + floorText + ". Rocket League's demotion buffer can delay the drop to " +
                             rankName(progress.previous->label) + ".",
                         false);
        } else {
            out << delta("neutral", "Grace",
                         "Rocket League gives you a grace period before demoting you, so falling below the MMR threshold does not immediately remove your rank.",
                         false);
            note = std::to_string(-progress.aboveFloor) + " MMR below the " + rankName(progress.floor->label) + " threshold";
        }
    }

    if (progress.floor && progress.next) {
        const float span = static_cast<float>(progress.next->mmr - progress.floor->mmr);
        const float position = span > 0.0f ? std::clamp(static_cast<float>(progress.mmr - progress.floor->mmr) / span, 0.0f, 1.0f) * 100.0f : 0.0f;
        ColorRGBA tint = Format::RankColor(progress.rank);
        const std::string solid = CssColor(tint);
        tint.a = 0.18f;
        out << "<div class='rank-progress-track' style='background-color:" << CssColor(tint) << "'><div class='rank-progress-fill' style='width:"
            << position << "%;background-color:" << solid << "'></div><div class='rank-progress-marker' style='left:" << position << "%'></div></div>";
    } else {
        out << "<div class='grow'></div>";
    }

    if (progress.next) {
        const std::string nextText = rankName(progress.next->label) + " (starts at " + std::to_string(progress.next->mmr) + " MMR)";
        if (progress.toNext <= 0) {
            out << delta("neutral", "Rank unchanged",
                         "You have reached the " + rankName(progress.next->label) + " MMR threshold, but your reported division has not changed.",
                         true);
            note = progress.toNext == 0
                       ? "At the " + rankName(progress.next->label) + " MMR threshold"
                       : std::to_string(progress.mmr - progress.next->mmr) + " MMR above the " + rankName(progress.next->label) + " threshold";
        } else {
            out << delta("win", "+" + std::to_string(progress.toNext),
                         "Gain " + std::to_string(progress.toNext) + " MMR to reach " + nextText + ".", true);
        }
    }
    out << "</div>";
    if (!note.empty()) {
        out << "<div class='label rank-progress-note'>" << Escape(note) << "</div>";
    }
    out << "</div>";
    return out.str();
}

void RmlUiController::PanGraph(int direction) {
    if (!m_state || direction == 0) return;
    const auto series = ResolveGraphSeries();
    const int total = static_cast<int>(series.ratings.size());
    if (total <= kVisibleGraphMatches) return;
    constexpr int step = 5;
    const int maxOffset = total - kVisibleGraphMatches;
    const int currentOffset = m_state->ui.graphOffset.load();
    const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
    if (newOffset == currentOffset) return;
    m_state->ui.graphOffset.store(newOffset);
    RebuildVisibleUi(false);
}
