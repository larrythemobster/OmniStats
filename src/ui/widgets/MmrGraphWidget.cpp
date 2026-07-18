#include "MmrGraphWidget.hpp"
#include <string>
#include <algorithm>

namespace Widgets {
    void RenderMmrGraph(const MmrGraphParams& p) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        if (p.history.empty()) {
            return;
        }

        int numPoints = (int)p.history.size();

        // Find min and max MMR
        float minMmr = p.history[0];
        float maxMmr = p.history[0];
        for (float val : p.history) {
            if (val < minMmr) minMmr = val;
            if (val > maxMmr) maxMmr = val;
        }

        // Add vertical padding so points aren't clipped
        if (maxMmr == minMmr) {
            maxMmr += 15.0f;
            minMmr -= 15.0f;
        } else {
            float diff = maxMmr - minMmr;
            maxMmr += diff * 0.20f;
            minMmr -= diff * 0.20f;
        }

        ImGui::Dummy(ImVec2(0.0f, 12.0f)); // Spacer
        ImVec2 plotStart = ImGui::GetCursorScreenPos();
        float plotWidth = ImGui::GetContentRegionAvail().x;
        float plotHeight = p.plotHeight;
        ImVec2 plotEnd = ImVec2(plotStart.x + plotWidth, plotStart.y + plotHeight);

        // Reserve space in layout
        ImGui::Dummy(ImVec2(plotWidth, plotHeight));

        float x_min = plotStart.x + 8.0f;
        float x_max = plotStart.x + plotWidth - 55.0f; // Leave space on right for current MMR text
        float y_min = plotStart.y + 15.0f;
        float y_max = plotStart.y + plotHeight - 15.0f;

        // Horizontal Boundary Lines
        drawList->AddLine(ImVec2(x_min, y_min), ImVec2(x_max, y_min), ImColor(p.colorMuted.Value.x, p.colorMuted.Value.y, p.colorMuted.Value.z, 0.3f), 1.0f);
        drawList->AddLine(ImVec2(x_min, y_max), ImVec2(x_max, y_max), ImColor(p.colorMuted.Value.x, p.colorMuted.Value.y, p.colorMuted.Value.z, 0.3f), 1.0f);

        // Max and Min labels
        std::string maxStr = std::to_string((int)maxMmr);
        std::string minStr = std::to_string((int)minMmr);

        if (p.fontSmall) {
            drawList->AddText(p.fontSmall, 12.0f, ImVec2(x_min, y_min - 16.0f), p.colorDim, maxStr.c_str());
            drawList->AddText(p.fontSmall, 12.0f, ImVec2(x_min, y_max + 2.0f), p.colorDim, minStr.c_str());
        }

        // Right side: "last N"
        std::string lastStr = "last " + std::to_string(numPoints);
        if (p.fontSmall) {
            ImVec2 lastSize = p.fontSmall->CalcTextSizeA(12.0f, FLT_MAX, -1.0f, lastStr.c_str());
            drawList->AddText(p.fontSmall, 12.0f, ImVec2(x_max - lastSize.x, y_min - 16.0f), p.colorDim, lastStr.c_str());
        }

        // Horizontal Baseline (Accent dashed line representing initial session/lifetime MMR)
        if (p.initialMmr > 0 && p.initialMmr >= minMmr && p.initialMmr <= maxMmr) {
            float tY = (p.initialMmr - minMmr) / (maxMmr - minMmr);
            float baselineY = y_max - tY * (y_max - y_min);

            float dashLen = 6.0f;
            float gapLen = 4.0f;
            for (float dx = x_min; dx < x_max; dx += (dashLen + gapLen)) {
                float nextDx = dx + dashLen;
                if (nextDx > x_max) nextDx = x_max;
                drawList->AddLine(ImVec2(dx, baselineY), ImVec2(nextDx, baselineY), ImColor(p.colorGraphBaseline.Value.x, p.colorGraphBaseline.Value.y, p.colorGraphBaseline.Value.z, 0.45f), 1.0f);
            }
        }

        // Coordinate generation using thread-local scratch buffer to prevent per-frame allocations
        thread_local std::vector<ImVec2> points;
        points.clear();
        points.reserve(numPoints);
        for (int i = 0; i < numPoints; ++i) {
            float tX = (numPoints > 1) ? (float)i / (numPoints - 1) : 0.5f;
            float tY = (p.history[i] - minMmr) / (maxMmr - minMmr);

            float px = x_min + tX * (x_max - x_min);
            float py = y_max - tY * (y_max - y_min);
            points.push_back(ImVec2(px, py));
        }

        // Connection Lines
        for (size_t i = 0; i < points.size() - 1; ++i) {
            drawList->AddLine(points[i], points[i + 1], ImColor(p.colorGraphLine.Value.x, p.colorGraphLine.Value.y, p.colorGraphLine.Value.z, 0.85f), 2.5f);
        }

        // Dot Markers
        for (size_t i = 0; i < points.size(); ++i) {
            ImColor dotColor;
            if (i == 0) {
                dotColor = p.colorMuted; // Neutral starting point
            } else {
                if (p.history[i] > p.history[i - 1]) {
                    dotColor = p.colorWin; // Theme Win Color
                } else if (p.history[i] < p.history[i - 1]) {
                    dotColor = p.colorLoss; // Theme Loss Color
                } else {
                    dotColor = p.colorMuted; // Neutral unchanged point
                }
            }

            float radius = (i == points.size() - 1) ? 6.0f : 4.0f;

            if (i == points.size() - 1) {
                drawList->AddCircleFilled(points[i], radius + 4.0f, ImColor(dotColor.Value.x, dotColor.Value.y, dotColor.Value.z, 0.25f));

                std::string currStr = std::to_string(p.currentMmr);
                if (p.fontBold) {
                    ImVec2 textSz = p.fontBold->CalcTextSizeA(15.0f, FLT_MAX, -1.0f, currStr.c_str());
                    drawList->AddText(p.fontBold, 15.0f, ImVec2(x_max + 8.0f, points[i].y - textSz.y * 0.5f), p.colorText, currStr.c_str());
                }
            }

            drawList->AddCircleFilled(points[i], radius, dotColor);
        }
    }

    void RenderMmrDeltaBadge(ImVec2 badgePos, int delta, ImColor colorWin, ImColor colorLoss,
                             ImColor colorMuted, ImFont* fontSmallBold) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 badgeSize = ImVec2(44.0f, 22.0f);
        ImVec2 badgePosEnd = ImVec2(badgePos.x + badgeSize.x, badgePos.y + badgeSize.y);

        ImColor badgeBgColor;
        ImColor badgeTextColor;
        std::string badgeText;

        if (delta > 0) {
            badgeBgColor = colorWin;
            badgeTextColor = ImColor(15, 15, 15, 255);
            badgeText = "+" + std::to_string(delta);
        } else if (delta < 0) {
            badgeBgColor = colorLoss;
            badgeTextColor = ImColor(255, 255, 255, 255);
            badgeText = std::to_string(delta);
        } else {
            badgeBgColor = ImColor(45, 45, 45, 255);
            badgeTextColor = colorMuted;
            badgeText = "+0";
        }

        drawList->AddRectFilled(badgePos, badgePosEnd, badgeBgColor, 6.0f);

        if (fontSmallBold) {
            ImVec2 textSize = fontSmallBold->CalcTextSizeA(12.0f, FLT_MAX, -1.0f, badgeText.c_str());
            ImVec2 textPos = ImVec2(badgePos.x + (badgeSize.x - textSize.x) * 0.5f, badgePos.y + (badgeSize.y - textSize.y) * 0.5f);
            drawList->AddText(fontSmallBold, 12.0f, textPos, badgeTextColor, badgeText.c_str());
        }
    }
}
