#include "RenderHelpers.hpp"
#include "ui/Overlay.hpp"
#include "core/Config.hpp"
#include "ui/Formatting.hpp"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <shellapi.h>

namespace RenderHelper {

void Record(int winsWith, int lossesWith, int winsAgainst, int lossesAgainst,
            const ConfigData& cfg, const RenderFonts& fonts) {
    bool hasWith = (winsWith > 0 || lossesWith > 0);
    bool hasAgainst = (winsAgainst > 0 || lossesAgainst > 0);

    if (!hasWith && !hasAgainst) {
        ImGui::PushFont(fonts.small);
        ImGui::TextColored(Format::C(cfg.themeMuted), "0W 0L");
        ImGui::PopFont();
        return;
    }

    if (hasWith) {
        ImGui::PushFont(fonts.mono);
        ImGui::TextColored(Format::C(cfg.themeText), "%d", winsWith);
        ImGui::PopFont();
        ImGui::PushFont(fonts.small);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeMuted), "W ");
        ImGui::PopFont();
        ImGui::PushFont(fonts.mono);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeText), "%d", lossesWith);
        ImGui::PopFont();
        ImGui::PushFont(fonts.small);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeMuted), "L with");
        ImGui::PopFont();
    }

    if (hasWith && hasAgainst) {
        ImGui::PushFont(fonts.small);
        ImGui::SameLine(0, 4.0f); ImGui::TextColored(Format::C(cfg.themeMuted), " / ");
        ImGui::PopFont();
    }

    if (hasAgainst) {
        ImGui::PushFont(fonts.mono);
        if (hasWith) ImGui::SameLine(0, 0);
        ImGui::TextColored(Format::C(cfg.themeText), "%d", winsAgainst);
        ImGui::PopFont();
        ImGui::PushFont(fonts.small);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeMuted), "W ");
        ImGui::PopFont();
        ImGui::PushFont(fonts.mono);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeText), "%d", lossesAgainst);
        ImGui::PopFont();
        ImGui::PushFont(fonts.small);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(cfg.themeMuted), "L vs");
        ImGui::PopFont();
    }
}

MmrDeltaResult ComputeMmrDelta(const RenderSnapshot& snap, const std::string& playlist) {
    MmrDeltaResult r;
    if (snap.showLifetimeGraph) {
        if (!snap.lifetimeMmrY.empty()) {
            r.currentMmr = (int)snap.lifetimeMmrY.back();
            r.initialMmr = (int)snap.lifetimeMmrY.front();
        }
    } else {
        auto it = snap.roster.find(snap.myPrimaryId);
        if (it != snap.roster.end()) {
            auto& myPlayer = it->second;
            if (myPlayer.playlists.count(playlist)) {
                r.currentMmr = myPlayer.playlists.at(playlist);
            }
        }
        if (r.currentMmr == 0 && snap.playlistHistoryY.count(playlist) && !snap.playlistHistoryY.at(playlist).empty()) {
            r.currentMmr = (int)snap.playlistHistoryY.at(playlist).back();
        }
        if (snap.playlistInitialMmr.count(playlist)) {
            r.initialMmr = snap.playlistInitialMmr.at(playlist);
        }
    }
    if (r.initialMmr > 0 && r.currentMmr > 0) {
        r.delta = r.currentMmr - r.initialMmr;
    }
    return r;
}

} // namespace RenderHelper
