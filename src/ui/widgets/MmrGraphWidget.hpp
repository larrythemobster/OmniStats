#pragma once
#include <vector>
#include <string>
#include "imgui.h"

namespace Widgets {
    struct MmrGraphParams {
        const std::vector<float>& history;
        int currentMmr;
        int initialMmr;
        float plotHeight = 150.0f;
        
        ImColor colorWin;
        ImColor colorLoss;
        ImColor colorMuted;
        ImColor colorDim;
        ImColor colorText;
        ImColor colorGraphLine;
        ImColor colorGraphBaseline;
        
        ImFont* fontSmall = nullptr;
        ImFont* fontSmallBold = nullptr;
        ImFont* fontBold = nullptr;
    };

    // Renders the MMR history plot line and markers inside the current ImGui container context
    void RenderMmrGraph(const MmrGraphParams& p);

    // Renders the stylized MMR change badge (e.g. +12 / -8 / +0)
    void RenderMmrDeltaBadge(ImVec2 badgePos, int delta, ImColor colorWin, ImColor colorLoss,
                             ImColor colorMuted, ImFont* fontSmallBold);
}
