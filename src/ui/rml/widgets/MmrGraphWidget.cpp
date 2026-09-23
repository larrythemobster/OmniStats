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
    header << "</div>";

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

    const int current = static_cast<int>(std::lround(values.back()));
    const int change = hasBaseline ? static_cast<int>(std::lround(values.back() - baseline)) : 0;
    out << "<div class='graph-current' style='left:83%;top:" << (yPct(values.back()) - 4.0f) << "%;'>" << current
        << " <span class='" << (change >= 0 ? "win" : "loss") << "'>";
    if (change >= 0) out << '+';
    out << change << "</span></div></div>";
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
