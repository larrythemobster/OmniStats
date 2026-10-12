#include "ui/rml/RmlUiController.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    constexpr int kVisibleGraphMatches = 25;
}

std::string RmlUiController::RenderGraphPanControls() const {
    int total = 0;
    if (m_snap.showLifetimeGraph) {
        total = static_cast<int>(m_snap.lifetimeMmrY.size());
    } else {
        const std::string playlist = MmrCategoryToString(m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo);
        if (auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) total = static_cast<int>(it->second.size());
    }
    if (total <= kVisibleGraphMatches) return {};
    const int offset = m_state ? std::clamp(m_state->ui.graphOffset.load(), 0, total - kVisibleGraphMatches) : 0;
    const int firstIndex = total - kVisibleGraphMatches - offset;
    std::ostringstream out;
    out << "<span class='label mono graph-range'>" << (firstIndex + 1) << '-' << (firstIndex + kVisibleGraphMatches) << " of " << total
        << "</span><div class='graph-zoom'>" << Button("graph-pan-older", "\xE2\x86\x90", "ghost zoom-step")
        << Button("graph-pan-newer", "\xE2\x86\x92", "ghost zoom-step") << "</div>";
    return out.str();
}

std::string RmlUiController::RenderMmrGraph(bool showCategoryBadge, bool showRankProgress, bool showPanControls) {
    const auto category = m_state ? m_state->ui.graphMmrCategory.load() : MmrCategory::TwoVTwo;
    const std::string playlist = MmrCategoryToString(category);
    std::vector<float> series;
    std::vector<bool> seriesEstimated;
    std::vector<float> seriesTimes;
    float baseline = -1.0f;
    if (m_snap.showLifetimeGraph) {
        series = m_snap.lifetimeMmrY;
        seriesTimes = m_snap.lifetimeMmrX;
        if (!series.empty()) baseline = series.front();
        seriesEstimated.assign(series.size(), false);
    } else {
        if (auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) series = it->second;
        if (auto it = m_snap.playlistHistoryEstimated.find(playlist); it != m_snap.playlistHistoryEstimated.end()) seriesEstimated = it->second;
        if (auto it = m_snap.playlistInitialMmr.find(playlist); it != m_snap.playlistInitialMmr.end()) baseline = static_cast<float>(it->second);
    }

    const int total = static_cast<int>(series.size());
    const int maxOffset = std::max(0, total - kVisibleGraphMatches);
    const int offset = m_state ? std::clamp(m_state->ui.graphOffset.load(), 0, maxOffset) : 0;
    const int firstIndex = std::max(0, total - kVisibleGraphMatches - offset);
    const int windowCount = std::min(kVisibleGraphMatches, total - firstIndex);

    // Progression follows the latest rating even while older matches are panned
    // into view. Before a graph point exists, the fetched lobby rating stands in.
    const auto rankTable = MMRFetcher::RankThresholdsForPlaylist(playlist);
    const PlayerData* self = nullptr;
    if (!m_snap.myPrimaryId.empty()) {
        if (auto it = m_snap.roster.find(m_snap.myPrimaryId); it != m_snap.roster.end()) self = &it->second;
    }
    int currentMmr = series.empty() ? 0 : static_cast<int>(std::lround(series.back()));
    if (currentMmr <= 0 && self) {
        if (auto it = self->playlists.find(playlist); it != self->playlists.end()) currentMmr = it->second;
    }
    const std::string rankProgress = showRankProgress ? RenderRankProgress(RankProgressFor(self, playlist, currentMmr)) : std::string{};

    std::vector<float> values;
    std::vector<bool> estimated;
    if (windowCount > 0) {
        values.assign(series.begin() + firstIndex, series.begin() + firstIndex + windowCount);
        if (!seriesEstimated.empty()) {
            const int endIdx = std::min(firstIndex + windowCount, static_cast<int>(seriesEstimated.size()));
            if (firstIndex < endIdx) estimated.assign(seriesEstimated.begin() + firstIndex, seriesEstimated.begin() + endIdx);
        }
    }
    if (m_snap.showLifetimeGraph && !values.empty()) {
        baseline = values.front();
    }

    std::ostringstream header;
    const std::string pan = showPanControls ? RenderGraphPanControls() : std::string{};
    if (showCategoryBadge || !pan.empty()) {
        header << "<div class='row graph-header'>";
        if (showCategoryBadge) header << "<span class='badge'>" << Escape(MmrLabel(category)) << "</span>";
        header << "<div class='grow'></div>" << pan << "</div>";
    }
    header << rankProgress;

    if (values.empty()) {
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
    const bool hasBaseline = baseline > 0.0f;
    // A session graph starts at the rating from before the first match, so a
    // single game still draws a line from where the session began.
    const bool sessionStart = !m_snap.showLifetimeGraph && hasBaseline && firstIndex == 0;
    std::vector<float> plotted;
    plotted.reserve(values.size() + 1);
    if (sessionStart) plotted.push_back(baseline);
    plotted.insert(plotted.end(), values.begin(), values.end());
    const size_t firstMatch = sessionStart ? 1 : 0;

    float minV = *std::min_element(plotted.begin(), plotted.end());
    float maxV = *std::max_element(plotted.begin(), plotted.end());
    if (hasBaseline) {
        minV = std::min(minV, baseline);
        maxV = std::max(maxV, baseline);
    }
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
    boundaryLayout.reserved = {{yMax, 100.0f}, {currentTop - 4.0f, currentTop + 4.0f}};
    const auto boundaries = RankProgress::LayoutGraphBoundaries(rankTable, minV, maxV, plotted.back(), boundaryLayout);
    for (const auto& b : boundaries) {
        const std::string color = CssColor(Format::RankColor(b.boundary.label));
        out << "<div class='graph-rank-line tooltip-host" << (b.boundary.tierStart ? " tier-start" : "") << "' style='left:" << xMin
            << "%;top:" << b.topPct << "%;width:" << (xMax - xMin) << "%'><div class='graph-rank-rule' style='background-color:" << color
            << "'></div><span class='tooltip-bubble'>"
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
        const float compareTo = i > 0 ? plotted[i - 1] : firstIndex > 0         ? series[static_cast<size_t>(firstIndex) - 1]
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
            const int game = firstIndex + static_cast<int>(valueIndex) + 1;
            tip = "Game " + std::to_string(game);
            if (m_snap.showLifetimeGraph && static_cast<size_t>(firstIndex) + valueIndex < seriesTimes.size())
                tip += " \xC2\xB7 " + FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(firstIndex) + valueIndex]));
            tip += " \xC2\xB7 " + std::to_string(mmr) + " MMR";
            if (compareTo > 0.0f) {
                const int delta = mmr - static_cast<int>(std::lround(compareTo));
                cls = delta > 0 ? " win" : delta < 0 ? " loss"
                                                     : " neutral";
                tip += std::string(" (") + (delta >= 0 ? "+" : "") + std::to_string(delta) + ")";
            } else {
                cls = " neutral";
            }
            if (valueIndex < estimated.size() && estimated[valueIndex]) {
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
            << "<div class='graph-hit tooltip-host' style='left:" << x << "%;top:" << y << "%'><span class='tooltip-bubble"
            << (y < 45.0f ? " below" : "") << (x > 55.0f ? " align-right" : x < 25.0f ? " align-left"
                                                                                      : "")
            << "'>" << Escape(tip)
            << "</span></div>";
    }

    // Labels stop short of the plot's right edge so the current point stays clear.
    for (const auto& b : boundaries) {
        if (!b.labeled) continue;
        out << "<div class='graph-rank-label mono' style='right:" << (100.0f - xMax + 2.0f) << "%;top:" << b.labelTopPct
            << "%;color:" << CssColor(Format::RankColor(b.boundary.label)) << "'>" << b.boundary.mmr << "</div>";
    }

    const float axisTop = yMax + 2.0f;
    if (m_snap.showLifetimeGraph && !seriesTimes.empty() && firstIndex < static_cast<int>(seriesTimes.size())) {
        const int lastIndex = std::min(firstIndex + windowCount - 1, static_cast<int>(seriesTimes.size()) - 1);
        out << "<div class='graph-label' style='left:" << xMin << "%;top:" << axisTop << "%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(firstIndex)]))) << "</div>"
            << "<div class='graph-label graph-last' style='right:" << (100.0f - xMax) << "%;top:" << axisTop << "%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(lastIndex)]))) << "</div>";
    } else {
        if (plotted.size() > 1)
            out << "<div class='graph-label' style='left:" << xMin << "%;top:" << axisTop << "%'>"
                << (sessionStart ? std::string("Start") : "Game " + std::to_string(firstIndex + 1)) << "</div>";
        out << "<div class='graph-label graph-last' style='right:" << (100.0f - xMax) << "%;top:" << axisTop << "%'>Game "
            << (firstIndex + windowCount) << "</div>";
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

    if (progress.floor && progress.previous) {
        const std::string floorText = rankName(progress.floor->label) + " (starts at " + std::to_string(progress.floor->mmr) + " MMR)";
        if (progress.aboveFloor >= 0) {
            out << delta("loss", "-" + std::to_string(progress.aboveFloor),
                         "Lose " + std::to_string(progress.aboveFloor) + " MMR and you fall below " + floorText + ". Rocket League's demotion buffer can delay the drop to " +
                             rankName(progress.previous->label) + ".",
                         false);
        } else {
            out << delta("loss", "buffer",
                         std::to_string(-progress.aboveFloor) + " MMR below " + floorText + ". Rocket League's demotion buffer is keeping you in this division.",
                         false);
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
        const int gain = std::max(0, progress.toNext);
        out << delta("win", "+" + std::to_string(gain),
                     gain > 0 ? "Gain " + std::to_string(gain) + " MMR to reach " + nextText + "." : "You have reached " + nextText + ".", true);
    }
    out << "</div></div>";
    return out.str();
}

void RmlUiController::PanGraph(int direction) {
    if (!m_state || direction == 0) return;
    const auto category = m_state->ui.graphMmrCategory.load();
    const std::string playlist = MmrCategoryToString(category);
    int total = 0;
    if (m_snap.showLifetimeGraph) {
        total = static_cast<int>(m_snap.lifetimeMmrY.size());
    } else {
        if (auto it = m_snap.playlistHistoryY.find(playlist); it != m_snap.playlistHistoryY.end()) {
            total = static_cast<int>(it->second.size());
        }
    }
    if (total <= kVisibleGraphMatches) return;
    constexpr int step = 5;
    const int maxOffset = total - kVisibleGraphMatches;
    const int currentOffset = m_state->ui.graphOffset.load();
    const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
    m_state->ui.graphOffset.store(newOffset);
    RebuildVisibleUi(false);
}
