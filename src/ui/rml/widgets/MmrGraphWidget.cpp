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

std::string RmlUiController::RenderMmrGraph(bool showCategoryBadge) {
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

    constexpr int kVisibleMatches = 25;
    const int total = static_cast<int>(series.size());
    const int maxOffset = std::max(0, total - kVisibleMatches);
    const int offset = m_state ? std::clamp(m_state->ui.graphOffset.load(), 0, maxOffset) : 0;
    const int firstIndex = std::max(0, total - kVisibleMatches - offset);
    const int windowCount = std::min(kVisibleMatches, total - firstIndex);

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
    // A reported division only describes the rating it was fetched with. The
    // tournament tier is itself derived from the table, so it is not evidence.
    std::string reportedTier;
    if (self && playlist != "t") {
        const auto mmrIt = self->playlists.find(playlist);
        const auto tierIt = self->playlistTiers.find(playlist);
        if (mmrIt != self->playlists.end() && tierIt != self->playlistTiers.end() && mmrIt->second == currentMmr) reportedTier = tierIt->second;
    }
    const std::string rankProgress = RenderRankProgress(RankProgress::Compute(rankTable, currentMmr, reportedTier));

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
    header << "<div class='row graph-header'>";
    if (showCategoryBadge) {
        header << "<span class='badge'>" << Escape(MmrLabel(category)) << "</span>";
    }
    header << "<div class='grow'></div>";
    if (total > kVisibleMatches) {
        header << "<div class='graph-zoom'>"
               << Button("graph-pan-older", "\xE2\x86\x90", "ghost zoom-step")
               << Button("graph-pan-newer", "\xE2\x86\x92", "ghost zoom-step")
               << "</div>";
    }
    header << "</div>" << rankProgress;

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

    float minV = values.front();
    float maxV = values.front();
    for (float value : values) {
        minV = std::min(minV, value);
        maxV = std::max(maxV, value);
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
    if (!rankTable.empty()) RankProgress::ExpandRangeForBoundaries(rankTable, values.back(), kBoundaryReach, minV, maxV);

    constexpr float xMin = 4.0f;
    constexpr float xMax = 80.0f;
    constexpr float yMin = 10.0f;
    constexpr float yMax = 88.0f;
    auto yPct = [&](float value) { return yMax - ((value - minV) / (maxV - minV)) * (yMax - yMin); };
    auto xPct = [&](size_t index) { return values.size() <= 1 ? (xMin + xMax) * 0.5f : xMin + static_cast<float>(index) / static_cast<float>(values.size() - 1) * (xMax - xMin); };

    std::ostringstream out;
    out << header.str() << "<div class='graph-wrap'>";
    const float midV = (minV + maxV) * 0.5f;
    out << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMin << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-gridline graph-boundary' style='left:" << xMin << "%;top:" << yMax << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-gridline' style='left:" << xMin << "%;top:" << yPct(midV) << "%;width:" << (xMax - xMin) << "%'></div>"
        << "<div class='graph-label graph-max' style='left:" << xMin << "%;top:1%'>" << static_cast<int>(maxV) << "</div>"
        << "<div class='graph-label' style='left:" << xMin << "%;top:" << (yPct(midV) + 1.0f) << "%'>" << static_cast<int>(midV) << "</div>"
        << "<div class='graph-label graph-min' style='left:" << xMin << "%;top:89%'>" << static_cast<int>(minV) << "</div>";

    RankProgress::GraphLayoutOptions boundaryLayout;
    boundaryLayout.plotTopPct = yMin;
    boundaryLayout.plotBottomPct = yMax;
    // Right-edge labels already in use: the match count (top), the last match
    // time (bottom), and the current point.
    const float currentTop = yPct(values.back());
    boundaryLayout.reserved = {{0.0f, 8.0f}, {87.0f, 100.0f}, {currentTop - 4.0f, currentTop + 4.0f}};
    const auto boundaries = RankProgress::LayoutGraphBoundaries(rankTable, minV, maxV, values.back(), boundaryLayout);
    for (const auto& b : boundaries) {
        const std::string color = CssColor(Format::RankColor(b.boundary.label));
        out << "<div class='graph-rank-line tooltip-host" << (b.boundary.tierStart ? " tier-start" : "") << "' style='left:" << xMin
            << "%;top:" << b.topPct << "%;width:" << (xMax - xMin) << "%'><div class='graph-rank-rule' style='background-color:" << color
            << "'></div><span class='tooltip-bubble'>"
            << Escape(Format::RankTier(b.boundary.label, m_config.use_roman_numerals) + " starts near " + std::to_string(b.boundary.mmr) + " MMR (estimate)")
            << "</span></div>";
    }

    out << "<div class='graph-label graph-last' style='right:20%;top:1%'>";
    if (total > kVisibleMatches) {
        out << (firstIndex + 1) << '-' << (firstIndex + windowCount) << " of " << total;
    } else {
        out << "last " << total;
    }
    out << "</div>";

    if (!seriesTimes.empty() && firstIndex < static_cast<int>(seriesTimes.size())) {
        const int lastIndex = std::min(firstIndex + windowCount - 1, static_cast<int>(seriesTimes.size()) - 1);
        out << "<div class='graph-label' style='left:" << (xMin + 7.0f) << "%;top:89%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(firstIndex)]))) << "</div>"
            << "<div class='graph-label graph-last' style='right:20%;top:89%'>"
            << Escape(FormatClock(static_cast<int64_t>(seriesTimes[static_cast<size_t>(lastIndex)]))) << "</div>";
    }

    if (hasBaseline && baseline >= minV && baseline <= maxV) {
        const float baseY = yPct(baseline);
        constexpr int dashCount = 13;
        constexpr float dashWidth = 3.6f;
        const float step = (xMax - xMin) / static_cast<float>(dashCount);
        for (int i = 0; i < dashCount; ++i) {
            const float left = xMin + static_cast<float>(i) * step;
            out << "<div class='graph-line baseline' style='left:" << left << "%;top:" << baseY << "%;width:" << dashWidth << "%'></div>";
        }
    }

    out << "<mmrgraphlines class='graph-polyline' points='";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ';';
        out << (xPct(i) / 100.0f) << ',' << (yPct(values[i]) / 100.0f);
    }
    out << "'></mmrgraphlines>";

    for (size_t i = 0; i < values.size(); ++i) {
        std::string cls;
        if (i == 0) {
            if (baseline > 0 && values[i] > baseline)
                cls = " win";
            else if (baseline > 0 && values[i] < baseline)
                cls = " loss";
            else
                cls = " neutral";
        } else if (values[i] > values[i - 1])
            cls = " win";
        else if (values[i] < values[i - 1])
            cls = " loss";
        else
            cls = " neutral";
        const bool isEstimated = i < estimated.size() && estimated[i];
        if (isEstimated) cls += " estimated";
        const bool isCurrent = i + 1 == values.size();
        if (isCurrent) {
            out << "<div class='graph-point-halo" << cls << "' style='left:" << xPct(i) << "%;top:" << yPct(values[i]) << "%'></div>";
            cls += " current";
        }
        out << "<div class='graph-point" << cls << "' style='left:" << xPct(i) << "%;top:" << yPct(values[i]) << "%'></div>";
    }

    // Labels stop short of the plot's right edge so the current point stays clear.
    for (const auto& b : boundaries) {
        if (!b.labeled) continue;
        out << "<div class='graph-rank-label' style='right:" << (100.0f - xMax + 2.0f) << "%;top:" << b.labelTopPct
            << "%;color:" << CssColor(Format::RankColor(b.boundary.label)) << "'>" << Escape(Format::AbbreviateRank(b.boundary.label))
            << " <span class='mono'>" << b.boundary.mmr << "</span></div>";
    }

    const int current = static_cast<int>(std::lround(values.back()));
    out << "<div class='graph-current' style='left:83%;top:" << (yPct(values.back()) - 4.0f) << "%;'>" << current;
    if (hasBaseline) {
        const int change = static_cast<int>(std::lround(values.back() - baseline));
        out << " <span class='" << (change >= 0 ? "win" : "loss") << "'>";
        if (change >= 0) out << '+';
        out << change << "</span>";
    }
    out << "</div></div>";
    return out.str();
}

std::string RmlUiController::RenderRankProgress(const RankProgress::Progress& progress) const {
    using RankProgress::Status;
    if (progress.status == Status::Unavailable) return {};

    std::ostringstream out;
    out << "<div class='rank-progress'><div class='row rank-progress-head'><span class='rank-progress-tier' style='color:"
        << CssColor(Format::RankColor(progress.rank)) << "'>" << Escape(Format::RankTier(progress.rank, m_config.use_roman_numerals)) << "</span>";
    if (progress.status == Status::Placement) {
        out << "<span class='label'>placement or unranked</span>";
    } else if (!progress.rankVerified) {
        out << "<span class='label rank-progress-est tooltip-host'>est.<span class='tooltip-bubble'>"
            << "No reported rank for this rating, so the division is estimated from the rank table.</span></span>";
    }
    out << "<div class='grow'></div><span class='rank-progress-mmr mono'>" << progress.mmr << "</span></div>";

    if (progress.status == Status::Placement) {
        out << "<div class='label rank-progress-note'>Rank distances appear once Rocket League assigns a rank.</div></div>";
        return out.str();
    }

    const auto rankName = [&](const RankProgress::Boundary& b) { return Format::RankTier(b.label, m_config.use_roman_numerals); };
    if (progress.floor && progress.next) {
        const float span = static_cast<float>(progress.next->mmr - progress.floor->mmr);
        const float position = span > 0.0f ? std::clamp(static_cast<float>(progress.mmr - progress.floor->mmr) / span, 0.0f, 1.0f) * 100.0f : 0.0f;
        out << "<div class='rank-progress-track'><div class='rank-progress-fill' style='width:" << position
            << "%'></div><div class='rank-progress-marker' style='left:" << position << "%'></div></div>";
    }

    out << "<div class='row rank-progress-ends'>";
    if (progress.floor && progress.previous) {
        std::string value;
        std::string tip;
        const std::string floorText = rankName(*progress.floor) + " (about " + std::to_string(progress.floor->mmr) + " MMR)";
        if (progress.aboveFloor >= 0) {
            value = "\xE2\x89\x88-" + std::to_string(progress.aboveFloor);
            tip = "About " + std::to_string(progress.aboveFloor) + " MMR above the estimated start of " + floorText +
                  ". Falling below it can drop you to " + rankName(*progress.previous) +
                  ". Rocket League also applies a demotion buffer it does not publish.";
        } else {
            value = "below floor";
            tip = std::to_string(-progress.aboveFloor) + " MMR below the estimated start of " + floorText +
                  ". You are still reported in this division, so the demotion buffer is likely holding you there.";
        }
        out << "<span class='rank-progress-end tooltip-host'><span class='loss mono'>" << Escape(value) << "</span> <span class='label'>"
            << Escape(Format::AbbreviateRank(progress.previous->label)) << "</span><span class='tooltip-bubble'>" << Escape(tip) << "</span></span>";
    } else {
        out << "<span class='label'>Lowest division</span>";
    }
    out << "<div class='grow'></div>";
    if (progress.next) {
        const std::string nextText = rankName(*progress.next) + " (about " + std::to_string(progress.next->mmr) + " MMR)";
        std::string value;
        std::string tip;
        if (progress.toNext > 0) {
            value = "\xE2\x89\x88+" + std::to_string(progress.toNext);
            tip = "About " + std::to_string(progress.toNext) + " MMR to the estimated start of " + nextText +
                  ". Estimates come from Tracker Network's rating distribution and shift during a season.";
        } else {
            value = "at boundary";
            tip = "Your rating has reached the estimated start of " + nextText +
                  ". Rocket League does not publish exact promotion points.";
        }
        out << "<span class='rank-progress-end tooltip-host'><span class='label'>" << Escape(Format::AbbreviateRank(progress.next->label))
            << "</span> <span class='win mono'>" << Escape(value) << "</span><span class='tooltip-bubble align-right'>" << Escape(tip)
            << "</span></span>";
    } else {
        out << "<span class='label'>Top rank</span>";
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
    constexpr int kVisibleMatches = 25;
    if (total <= kVisibleMatches) return;
    constexpr int step = 5;
    const int maxOffset = total - kVisibleMatches;
    const int currentOffset = m_state->ui.graphOffset.load();
    const int newOffset = std::clamp(currentOffset - direction * step, 0, maxOffset);
    m_state->ui.graphOffset.store(newOffset);
    RebuildVisibleUi(false);
}
