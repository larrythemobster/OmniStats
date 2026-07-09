#include "Overlay.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/RankIconAssets.hpp"
#include "ui/RenderHelpers.hpp"
#include "ui/ImGuiGuards.hpp"
#include "ui/widgets/ToggleWidget.hpp"
#include "ui/widgets/MmrGraphWidget.hpp"
#include <imgui.h>
#include <implot.h>
#include <iostream>
#include <algorithm>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cstdint>
#include <utility>

static float CalcGoalParticipationPercent(const SessionTotals& s) {
    if (s.teamGoals <= 0) return 0.0f;
    return (static_cast<float>(s.goalParticipations) / static_cast<float>(s.teamGoals)) * 100.0f;
}

static std::string FormatDemoKd(int demos, int demoed) {
    if (demoed <= 0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << static_cast<float>(demos);
        return oss.str();
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << (static_cast<float>(demos) / static_cast<float>(demoed));
    return oss.str();
}

static std::string FormatRelativeMatchTime(int64_t endedAtUnix) {
    if (endedAtUnix <= 0) return "--";

    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t diff = now - endedAtUnix;
    if (diff < 0) diff = 0;

    if (diff < 60) return "now";
    if (diff < 3600) return std::to_string(diff / 60) + "m ago";
    if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
    return std::to_string(diff / 86400) + "d ago";
}

static void RenderInlineIcon(ID3D11ShaderResourceView* texture, const ImVec2& size, const char* tooltip = nullptr) {
    if (!texture) {
        return;
    }
    
    float reserveHeight = ImGui::GetTextLineHeight();
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float iconY = cursor.y + (reserveHeight - size.y) * 0.5f;

    ImVec2 clipMin(cursor.x, iconY);
    ImVec2 clipMax(cursor.x + size.x, iconY + size.y);

    // Disable clipping for this icon so it can overflow table/window boundaries gracefully
    ImGui::GetWindowDrawList()->PushClipRect(clipMin, clipMax, false);
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(texture),
        clipMin,
        clipMax
    );
    ImGui::GetWindowDrawList()->PopClipRect();

    ImGui::Dummy(ImVec2(size.x, reserveHeight));

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

static void RenderDrawListIcon(ID3D11ShaderResourceView* texture, const ImVec2& min, const ImVec2& size) {
    ImGui::GetWindowDrawList()->AddImage(
        reinterpret_cast<ImTextureID>(texture),
        min,
        ImVec2(min.x + size.x, min.y + size.y)
    );
}

static void RenderDivisionStack(RankIconAssets* icons, const std::string& tier, const ImVec2& min, float dpiScale) {
    int division = RankIconAssets::DivisionLevel(tier);
    if (!icons || division <= 0) {
        return;
    }

    ImVec2 pillSize(22.0f * dpiScale, 4.0f * dpiScale);
    float gap = 0.5f * dpiScale;
    float y = min.y;
    for (int i = 4; i >= 1; --i) {
        ID3D11ShaderResourceView* texture = icons->DivisionTexture(tier, i <= division);
        if (texture) {
            RenderDrawListIcon(texture, ImVec2(min.x, y), pillSize);
        }
        y += pillSize.y + gap;
    }
}

static bool RenderRankBadgeInline(RankIconAssets* icons, const std::string& tier, const ImVec2& iconSize, float dpiScale, const char* tooltip = nullptr) {
    if (!icons) {
        return false;
    }

    ID3D11ShaderResourceView* tierIcon = icons->TierTexture(tier);
    if (!tierIcon) {
        return false;
    }

    int division = RankIconAssets::DivisionLevel(tier);
    ImVec2 pillSize(22.0f * dpiScale, 4.0f * dpiScale);
    float pillGap = 0.5f * dpiScale;
    float pillStackHeight = pillSize.y * 4.0f + pillGap * 3.0f;
    float divisionGap = division > 0 ? 2.0f * dpiScale : 0.0f;
    float divisionWidth = division > 0 ? pillSize.x : 0.0f;
    float reserveHeight = 18.0f * dpiScale;
    if (reserveHeight < ImGui::GetTextLineHeight()) {
        reserveHeight = ImGui::GetTextLineHeight();
    }

    ImVec2 cursor = ImGui::GetCursorScreenPos();
    float iconY = cursor.y + (reserveHeight - iconSize.y) * 0.5f;
    ImVec2 iconMin(cursor.x, iconY);
    RenderDrawListIcon(tierIcon, iconMin, iconSize);

    if (division > 0) {
        float stackY = iconY + (iconSize.y - pillStackHeight) * 0.5f;
        RenderDivisionStack(icons, tier, ImVec2(iconMin.x + iconSize.x + divisionGap, stackY), dpiScale);
    }

    ImGui::Dummy(ImVec2(iconSize.x + divisionGap + divisionWidth, reserveHeight));
    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return true;
}

static std::string PlaylistKeyFromLabel(std::string label) {
    std::transform(label.begin(), label.end(), label.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (label == "t") {
        return "t";
    }
    return label;
}

void Overlay::RenderRecord(int winsWith, int lossesWith, int winsAgainst, int lossesAgainst, RecordFormat fmt) const {
    bool hasWith = (winsWith > 0 || lossesWith > 0);
    bool hasAgainst = (winsAgainst > 0 || lossesAgainst > 0);

    if (!hasWith && !hasAgainst) {
        ImGui::PushFont(fontSmall);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "0W 0L");
        ImGui::PopFont();
        return;
    }

    ImGui::BeginGroup();

    if (fmt == RecordFormat::Short) {
        int totalWins = (hasWith ? winsWith : 0) + (hasAgainst ? winsAgainst : 0);
        int totalLosses = (hasWith ? lossesWith : 0) + (hasAgainst ? lossesAgainst : 0);
        ImGui::PushFont(fontMono);
        ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", totalWins);
        ImGui::PopFont();
        ImGui::PushFont(fontSmall);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "W ");
        ImGui::PopFont();
        ImGui::PushFont(fontMono);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", totalLosses);
        ImGui::PopFont();
        ImGui::PushFont(fontSmall);
        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "L");
        ImGui::PopFont();
    } else {
        if (hasWith) {
            ImGui::PushFont(fontMono);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", winsWith);
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "W ");
            ImGui::PopFont();
            ImGui::PushFont(fontMono);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", lossesWith);
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), fmt == RecordFormat::Full ? "L with" : "L");
            ImGui::PopFont();
        }

        if (hasWith && hasAgainst) {
            ImGui::PushFont(fontSmall);
            ImGui::SameLine(0, 4.0f); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), " / ");
            ImGui::PopFont();
        }

        if (hasAgainst) {
            ImGui::PushFont(fontMono);
            if (hasWith) ImGui::SameLine(0, 0);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", winsAgainst);
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "W ");
            ImGui::PopFont();
            ImGui::PushFont(fontMono);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", lossesAgainst);
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), fmt == RecordFormat::Full ? "L vs" : "L");
            ImGui::PopFont();
        }
    }

    ImGui::EndGroup();

    if (fmt != RecordFormat::Full && ImGui::IsItemHovered()) {
        std::string tooltipText = "Lifetime H2H: ";
        if (hasWith) {
            tooltipText += std::to_string(winsWith) + "W " + std::to_string(lossesWith) + "L with";
        }
        if (hasAgainst) {
            if (hasWith) {
                tooltipText += " / ";
            }
            tooltipText += std::to_string(winsAgainst) + "W " + std::to_string(lossesAgainst) + "L vs";
        }
        ImGui::SetTooltip("%s", tooltipText.c_str());
    }
}

static std::string ClipUtf8Text(const std::string& text, float maxWidth, ImFont* font) {
    if (text.empty() || maxWidth <= 0.0f) {
        return "";
    }

    ImGui::PushFont(font);
    float totalWidth = ImGui::CalcTextSize(text.c_str()).x;
    if (totalWidth <= maxWidth) {
        ImGui::PopFont();
        return text;
    }

    float suffixWidth = ImGui::CalcTextSize("..").x;
    float allowedWidth = maxWidth - suffixWidth;
    if (allowedWidth < 0.0f) {
        allowedWidth = 0.0f;
    }

    std::vector<size_t> charIndices;
    for (size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            charIndices.push_back(i);
        }
    }
    charIndices.push_back(text.size());

    size_t bestCharCount = 0;
    for (size_t i = 1; i < charIndices.size(); ++i) {
        size_t byteLength = charIndices[i];
        std::string sub = text.substr(0, byteLength);
        float w = ImGui::CalcTextSize(sub.c_str()).x;
        if (w <= allowedWidth) {
            bestCharCount = i;
        } else {
            break;
        }
    }
    ImGui::PopFont();

    if (bestCharCount == 0) {
        return "..";
    }

    return text.substr(0, charIndices[bestCharCount]) + "..";
}

void Overlay::RenderPlayerRoster(int teamNum, const char* label, ImColor color, const char* tableId) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + 16), color);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    ImGui::PushFont(fontBold);
    ImGui::TextColored(color, "%s", label);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 4));

    std::string activeCat = MmrCategoryToString(m_state->ui.rosterMmrCategory.load());
    bool drawRankIcons = m_frameConfig.use_rank_icons && m_rankIcons && m_rankIcons->IsLoaded();

    // Determine record format based on available region width dynamically
    float availWidth = ImGui::GetContentRegionAvail().x;
    RecordFormat recordFormat = RecordFormat::Full;
    if (availWidth < 340.0f * m_dpiScale) {
        recordFormat = RecordFormat::Short;
    } else if (availWidth < 390.0f * m_dpiScale) {
        recordFormat = RecordFormat::Abbreviated;
    }

    // Gather players for this team to calculate dynamic column width and sort them later
    std::vector<const PlayerData*> teamPlayers;
    for (const auto& [id, pData] : m_snap.roster) {
        if (pData.team == teamNum) {
            teamPlayers.push_back(&pData);
        }
    }

    auto GetRecordWidth = [&](const PlayerData& pData) -> float {
        if (pData.primaryId == m_snap.myPrimaryId) {
            ImGui::PushFont(fontSmallBold);
            float w = ImGui::CalcTextSize("YOU").x;
            ImGui::PopFont();
            return w;
        }
        bool hasRecord = pData.hasLifetimeData &&
            (pData.lifetimeWinsWith > 0 || pData.lifetimeLossesWith > 0 ||
             pData.lifetimeWinsAgainst > 0 || pData.lifetimeLossesAgainst > 0);
        if (!hasRecord) {
            ImGui::PushFont(fontSmallBold);
            float w = ImGui::CalcTextSize("NEW").x;
            ImGui::PopFont();
            return w;
        }

        float width = 0.0f;
        int winsWith = pData.lifetimeWinsWith;
        int lossesWith = pData.lifetimeLossesWith;
        int winsAgainst = pData.lifetimeWinsAgainst;
        int lossesAgainst = pData.lifetimeLossesAgainst;

        bool hasWith = (winsWith > 0 || lossesWith > 0);
        bool hasAgainst = (winsAgainst > 0 || lossesAgainst > 0);

        if (recordFormat == RecordFormat::Short) {
            int totalWins = (hasWith ? winsWith : 0) + (hasAgainst ? winsAgainst : 0);
            int totalLosses = (hasWith ? lossesWith : 0) + (hasAgainst ? lossesAgainst : 0);
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(totalWins).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize("W ").x;
            ImGui::PopFont();
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(totalLosses).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize("L").x;
            ImGui::PopFont();
            return width;
        }

        if (hasWith) {
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(winsWith).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize("W ").x;
            ImGui::PopFont();
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(lossesWith).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize(recordFormat == RecordFormat::Full ? "L with" : "L").x;
            ImGui::PopFont();
        }

        if (hasWith && hasAgainst) {
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize(" / ").x + 4.0f;
            ImGui::PopFont();
        }

        if (hasAgainst) {
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(winsAgainst).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize("W ").x;
            ImGui::PopFont();
            ImGui::PushFont(fontMono);
            width += ImGui::CalcTextSize(std::to_string(lossesAgainst).c_str()).x;
            ImGui::PopFont();
            ImGui::PushFont(fontSmall);
            width += ImGui::CalcTextSize(recordFormat == RecordFormat::Full ? "L vs" : "L").x;
            ImGui::PopFont();
        }

        return width;
    };

    float rightColWidth = 120.0f * m_dpiScale;
    for (const auto* pDataPtr : teamPlayers) {
        float w = GetRecordWidth(*pDataPtr);
        rightColWidth = (std::max)(rightColWidth, w);
    }
    rightColWidth = std::clamp(rightColWidth, 120.0f * m_dpiScale, 240.0f * m_dpiScale);

    if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed, rightColWidth);

        std::sort(teamPlayers.begin(), teamPlayers.end(), [&](const PlayerData* a, const PlayerData* b) {
            int mmrA = 0;
            if (a->playlists.count(activeCat)) {
                mmrA = a->playlists.at(activeCat);
            }
            if (mmrA <= 0) {
                mmrA = a->mmr;
            }

            int mmrB = 0;
            if (b->playlists.count(activeCat)) {
                mmrB = b->playlists.at(activeCat);
            }
            if (mmrB <= 0) {
                mmrB = b->mmr;
            }

            if (mmrA != mmrB) {
                return mmrA > mmrB; // Highest MMR first
            }
            return a->name < b->name; // Alphabetical fallback
        });

        for (const auto* pDataPtr : teamPlayers) {
            const auto& pData = *pDataPtr;
            ImGui::TableNextRow();

            // Left Column: Player Name & MMR Info
            ImGui::TableNextColumn();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
            ImGui::BeginGroup();

            std::string platform = "";
            size_t delim = pData.primaryId.find('|');
            if (delim != std::string::npos) {
                platform = pData.primaryId.substr(0, delim);
            }

            std::string displayPlat = platform;
            ImColor platColor = Format::C(m_frameConfig.themeMuted);
            float badgesWidth = 0.0f;

            if (!platform.empty()) {
                if (platform == "Steam") {
                    displayPlat = "STEAM";
                    platColor = ImColor(102, 192, 244); // Steam Blue
                } else if (platform == "Epic") {
                    displayPlat = "EPIC";
                    platColor = ImColor(180, 180, 180); // Epic Gray
                } else if (platform == "Unknown") {
                    displayPlat = "BOT";
                    platColor = ImColor(150, 150, 150); // Grey BOT label
                } else if (platform.rfind("PS", 0) == 0) {
                    displayPlat = "PSN";
                    platColor = ImColor(46, 109, 217); // PSN Blue
                } else if (platform.rfind("Xbox", 0) == 0) {
                    displayPlat = "XBOX";
                    platColor = ImColor(16, 124, 16); // Xbox Green
                } else if (platform == "Switch") {
                    displayPlat = "SWITCH";
                    platColor = ImColor(230, 0, 18); // Nintendo Red
                } else {
                    std::transform(displayPlat.begin(), displayPlat.end(), displayPlat.begin(), ::toupper);
                }

                ImGui::PushFont(fontSmallBold);
                ImVec2 textSize = ImGui::CalcTextSize(displayPlat.c_str());
                ImGui::PopFont();

                float uiScale = m_dpiScale * m_frameConfig.ui_scale;
                float padX = 6.0f * uiScale;
                float badgeW = textSize.x + padX * 2.0f;
                badgesWidth = (6.0f * m_dpiScale) + badgeW;
            }

            float availWidth = ImGui::GetContentRegionAvail().x;
            float maxNameWidth = availWidth - badgesWidth - 24.0f * m_dpiScale;
            std::string clippedName = ClipUtf8Text(pData.name, maxNameWidth, fontBold);

            ImGui::PushFont(fontBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", clippedName.c_str());
            ImGui::PopFont();

            // Interactive click-to-tracker.gg link
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                ImVec2 min = ImGui::GetItemRectMin();
                ImVec2 max = ImGui::GetItemRectMax();
                ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), ImGui::GetColorU32(Format::C(m_frameConfig.themeAccent)), 1.5f);

                if (ImGui::IsMouseClicked(0)) {
                    std::string trnPlat = "";
                    if (!platform.empty()) {
                        std::string rawPlatLower = platform;
                        std::transform(rawPlatLower.begin(), rawPlatLower.end(), rawPlatLower.begin(), ::tolower);

                        if (rawPlatLower == "epic" || rawPlatLower == "epicgames") trnPlat = "epic";
                        else if (rawPlatLower == "steam") trnPlat = "steam";
                        else if (rawPlatLower == "ps4" || rawPlatLower == "psn" || rawPlatLower == "playstation") trnPlat = "psn";
                        else if (rawPlatLower == "xboxone" || rawPlatLower == "xbox" || rawPlatLower == "xbl") trnPlat = "xbl";
                        else if (rawPlatLower == "switch" || rawPlatLower == "nintendo") trnPlat = "switch";
                    } else {
                        std::cout << "[Overlay] WARNING: No delimiter found in primaryId: " << PrivacyLog::Sensitive(pData.primaryId, "player ID") << "\n";
                    }

                    if (!trnPlat.empty()) {
                        std::string ident = "";
                        if (trnPlat == "steam") {
                            std::string sub = pData.primaryId.substr(delim + 1);
                            size_t secondDelim = sub.find('|');
                            if (secondDelim != std::string::npos) {
                                ident = sub.substr(0, secondDelim);
                            } else {
                                ident = sub;
                            }
                        } else {
                            ident = pData.name;
                        }
                        std::string trackerUrl = "https://rocketleague.tracker.network/rocket-league/profile/" + trnPlat + "/" + Format::SimpleUrlEncode(ident) + "/overview";
                        ShellExecuteA(NULL, "open", trackerUrl.c_str(), NULL, NULL, SW_SHOWNORMAL);
                    } else {
                        std::cout << "[Overlay] WARNING: trnPlat is empty (platform not supported or not recognized), cannot open URL.\n";
                    }
                }
            }

            if (!platform.empty()) {
                ImGui::SameLine(0, 6.0f * m_dpiScale);

                ImGui::PushFont(fontSmallBold);
                ImVec2 textSize = ImGui::CalcTextSize(displayPlat.c_str());
                ImGui::PopFont();

                float uiScale = m_dpiScale * m_frameConfig.ui_scale;
                float padX = 6.0f * uiScale;
                float padY = 2.0f * uiScale;
                float badgeW = textSize.x + padX * 2.0f;
                float badgeH = textSize.y + padY * 2.0f;

                ImVec2 curPos = ImGui::GetCursorScreenPos();

                // Center the badge vertically with the bold player name
                float lineH = ImGui::GetTextLineHeight();
                float badgeFontSize = 0.0f;
#if IMGUI_VERSION_NUM >= 19200
                if (fontBold) lineH = fontBold->LegacySize;
                if (fontSmallBold) badgeFontSize = fontSmallBold->LegacySize;
#else
                if (fontBold) lineH = fontBold->FontSize;
                if (fontSmallBold) badgeFontSize = fontSmallBold->FontSize;
#endif
                float offset = (lineH - badgeH) * 0.5f;
                if (offset < 0.0f) offset = 0.0f;

                ImVec2 minPos = ImVec2(curPos.x, curPos.y + offset);
                ImVec2 maxPos = ImVec2(curPos.x + badgeW, curPos.y + offset + badgeH);

                // Reserve layout space in ImGui on the current line
                ImGui::Dummy(ImVec2(badgeW, badgeH));

                // Draw background pill (15% opacity)
                ImColor bgCol = platColor;
                bgCol.Value.w = 0.15f;
                ImGui::GetWindowDrawList()->AddRectFilled(minPos, maxPos, bgCol, 4.0f * m_dpiScale);

                // Draw border pill (40% opacity)
                ImColor borderCol = platColor;
                borderCol.Value.w = 0.40f;
                ImGui::GetWindowDrawList()->AddRect(minPos, maxPos, borderCol, 4.0f * m_dpiScale, 0, 1.0f);

                // Draw the text inside the badge
                ImVec2 textPos = ImVec2(
                    minPos.x + (badgeW - textSize.x) * 0.5f,
                    minPos.y + (badgeH - textSize.y) * 0.5f);
                ImGui::GetWindowDrawList()->AddText(fontSmallBold, badgeFontSize, textPos, ImGui::GetColorU32(platColor.Value), displayPlat.c_str());
            }

            if (pData.playlists.count(activeCat) && pData.playlists.at(activeCat) > 0) {
                std::string displayTier = pData.playlistTiers.count(activeCat) ? pData.playlistTiers.at(activeCat) : "Unranked";
                std::string formattedTier = Format::RankTier(displayTier, m_frameConfig.use_roman_numerals);
                if (drawRankIcons) {
                    if (!RenderRankBadgeInline(m_rankIcons.get(), displayTier, ImVec2(42.0f * m_dpiScale, 28.0f * m_dpiScale), m_dpiScale, formattedTier.c_str())) {
                        ImGui::PushFont(fontSmallBold);
                        ImGui::TextColored(Format::RankColor(displayTier), "%s", formattedTier.c_str());
                        ImGui::PopFont();
                    }
                } else {
                    ImGui::PushFont(fontSmallBold);
                    ImGui::TextColored(Format::RankColor(displayTier), "%s", formattedTier.c_str());
                    ImGui::PopFont();
                }

                ImGui::PushFont(fontSmall);
                ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), " \xC2\xB7 ");
                ImGui::PopFont();

                ImGui::PushFont(fontMono);
                ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeDim), "%d", pData.playlists.at(activeCat));
                ImGui::PopFont();

                std::string displayMode = "";
                if (activeCat == "best") {
                    int bestMmr = pData.playlists.at("best");
                    if (pData.playlists.count("2v2") && pData.playlists.at("2v2") == bestMmr) displayMode = "2V2";
                    else if (pData.playlists.count("3v3") && pData.playlists.at("3v3") == bestMmr) displayMode = "3V3";
                    else if (pData.playlists.count("1v1") && pData.playlists.at("1v1") == bestMmr) displayMode = "1V1";
                    else if (m_frameConfig.show_extra_playlists &&
                             pData.playlists.count("hoops") && pData.playlists.at("hoops") == bestMmr) displayMode = "HOOPS";
                    else if (m_frameConfig.show_extra_playlists &&
                             pData.playlists.count("rumble") && pData.playlists.at("rumble") == bestMmr) displayMode = "RUMBLE";
                    else if (m_frameConfig.show_extra_playlists &&
                             pData.playlists.count("dropshot") && pData.playlists.at("dropshot") == bestMmr) displayMode = "DROPSHOT";
                    else if (m_frameConfig.show_extra_playlists &&
                             pData.playlists.count("snowday") && pData.playlists.at("snowday") == bestMmr) displayMode = "SNOW DAY";
                    else displayMode = "BEST";
                } else {
                    displayMode = activeCat;
                    std::transform(displayMode.begin(), displayMode.end(), displayMode.begin(), ::toupper);
                    if (displayMode == "TOURNY") {
                        displayMode = "T";
                    }
                }

                if (!displayMode.empty()) {
                    ImGui::PushFont(fontSmall);
                    ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), " \xC2\xB7 ");
                    ImGui::PopFont();
                    if (drawRankIcons) {
                        ID3D11ShaderResourceView* playlistIcon = m_rankIcons->PlaylistTexture(PlaylistKeyFromLabel(displayMode));
                        if (playlistIcon) {
                            ImGui::SameLine(0, 0);
                            RenderInlineIcon(playlistIcon, ImVec2(28.0f * m_dpiScale, 28.0f * m_dpiScale), displayMode.c_str());
                        } else {
                            ImGui::PushFont(fontSmallBold);
                            ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", displayMode.c_str());
                            ImGui::PopFont();
                        }
                    } else {
                        ImGui::PushFont(fontSmallBold);
                        ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", displayMode.c_str());
                        ImGui::PopFont();
                    }
                }

                if (pData.playlistMatches.count(activeCat) && pData.playlistMatches.at(activeCat) > 0) {
                    int matches = pData.playlistMatches.at(activeCat);
                    ImGui::PushFont(fontSmall);
                    ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), " \xC2\xB7 ");
                    ImGui::PopFont();
                    ImGui::PushFont(fontSmall);
                    ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeDim), "%d matches", matches);
                    ImGui::PopFont();
                }
            } else if (pData.fetched) {
                if (drawRankIcons) {
                    if (!RenderRankBadgeInline(m_rankIcons.get(), "Unranked", ImVec2(42.0f * m_dpiScale, 28.0f * m_dpiScale), m_dpiScale, "Unranked")) {
                        ImGui::PushFont(fontSmall);
                        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "\xE2\x80\x94"); // em-dash —
                        ImGui::PopFont();
                    }
                } else {
                    ImGui::PushFont(fontSmall);
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "\xE2\x80\x94"); // em-dash —
                    ImGui::PopFont();
                }
            } else {
                if (drawRankIcons) {
                    ID3D11ShaderResourceView* tierIcon = m_rankIcons->UnsyncedTierTexture();
                    if (tierIcon) {
                        RenderInlineIcon(tierIcon, ImVec2(42.0f * m_dpiScale, 28.0f * m_dpiScale), "Fetching rank...");
                    } else {
                        ImGui::PushFont(fontSmall);
                        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "\xE2\x80\xA6"); // ellipsis …
                        ImGui::PopFont();
                    }
                } else {
                    ImGui::PushFont(fontSmall);
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "\xE2\x80\xA6"); // ellipsis …
                    ImGui::PopFont();
                }
            }

            if (m_frameConfig.show_account_wins_overlay && pData.totalWins >= 0) {
                ImVec4 winsColor = Format::C(m_frameConfig.themeDim);
                if (pData.totalWins < 100) {
                    winsColor = Format::C(m_frameConfig.themeLoss);
                } else if (pData.totalWins < 500) {
                    winsColor = ImVec4(1.0f, 0.72f, 0.20f, 1.0f);
                }

                ImGui::PushFont(fontSmall);
                ImGui::SameLine(0, 0); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), " \xC2\xB7 ");
                ImGui::PopFont();
                ImGui::PushFont(fontSmall);
                ImGui::SameLine(0, 0); ImGui::TextColored(winsColor, "%d wins", pData.totalWins);
                ImGui::PopFont();
            }
            ImGui::EndGroup();

            // Right Column: Record / Status
            ImGui::TableNextColumn();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);

            float width = GetRecordWidth(pData);
            float availX = ImGui::GetContentRegionAvail().x;
            if (availX > width) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availX - width);
            }

            if (pData.primaryId == m_snap.myPrimaryId) {
                ImGui::PushFont(fontSmallBold);
                ImGui::TextColored(Format::C(m_frameConfig.themeWin), "YOU");
                ImGui::PopFont();
            } else {
                bool hasRecord = pData.hasLifetimeData &&
                    (pData.lifetimeWinsWith > 0 || pData.lifetimeLossesWith > 0 ||
                     pData.lifetimeWinsAgainst > 0 || pData.lifetimeLossesAgainst > 0);

                if (hasRecord) {
                    RenderRecord(pData.lifetimeWinsWith, pData.lifetimeLossesWith, pData.lifetimeWinsAgainst, pData.lifetimeLossesAgainst, recordFormat);
                } else {
                    ImGui::PushFont(fontSmallBold);
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "NEW");
                    ImGui::PopFont();
                }
            }
        }
        ImGui::EndTable();
    }
}

void Overlay::RenderStatSection(const std::string& title, const std::string& tableId, const std::vector<std::pair<std::string, std::string>>& rows, bool compact) {
    if (rows.empty()) return;
    if (!compact) {
        ImGui::Spacing();
    }
    ImGui::PushFont(fontSmallBold);
    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", title.c_str());
    ImGui::PopFont();
    ImGui::Separator();
    if (!compact) {
        ImGui::Spacing();
    }

    if (ImGui::BeginTable(tableId.c_str(), 4, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("LV", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        ImGui::TableSetupColumn("SEP", ImGuiTableColumnFlags_WidthFixed, 10.0f);
        ImGui::TableSetupColumn("RV", ImGuiTableColumnFlags_WidthFixed, 55.0f);
        for (const auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(fontSmall);
            ImGui::TextColored(Format::C(m_frameConfig.themeDim), "%s", r.first.c_str());
            ImGui::PopFont();

            std::string val = r.second;
            size_t pos = val.find(" | ");
            if (pos != std::string::npos) {
                std::string left_val = val.substr(0, pos);
                std::string right_val = val.substr(pos + 3);

                // Column 2: Left Value (Right-aligned)
                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmall);
                float posX = ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(left_val.c_str()).x;
                if (posX > ImGui::GetCursorPosX()) {
                    ImGui::SetCursorPosX(posX);
                }
                ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", left_val.c_str());
                ImGui::PopFont();

                // Column 3: Separator (Center-aligned)
                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmall);
                float posSep = ImGui::GetCursorPosX() + (ImGui::GetColumnWidth() - ImGui::CalcTextSize("|").x) * 0.5f;
                if (posSep > ImGui::GetCursorPosX()) {
                    ImGui::SetCursorPosX(posSep);
                }
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "|");
                ImGui::PopFont();

                // Column 4: Right Value (Left-aligned)
                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmall);
                ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", right_val.c_str());
                ImGui::PopFont();
            } else {
                // Single value (Right-aligned under left values)
                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmall);
                float posX = ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(val.c_str()).x;
                if (posX > ImGui::GetCursorPosX()) {
                    ImGui::SetCursorPosX(posX);
                }
                ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", val.c_str());
                ImGui::PopFont();

                // Empty separator and right columns
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
            }
        }
        ImGui::EndTable();
    }
}

void Overlay::RenderLiveMatchStats(const char* tableIdPrefix) {
    const auto& cm = m_snap.currentMatch;
    std::vector<std::pair<std::string, std::string>> playRows;
    playRows.push_back({"Saves", Format::PairCount(cm.saves, cm.savesSelf)});
    playRows.push_back({"Shots", Format::PairCount(cm.shots, cm.shotsSelf)});
    playRows.push_back({"Assists", Format::PairCount(cm.assists, cm.assistsSelf)});
    playRows.push_back({"Demos", Format::PairCount(cm.demos, cm.demosSelf)});
    if (cm.demoedSelf > 0) playRows.push_back({"Demoed", std::to_string(cm.demoedSelf)});
    playRows.push_back({"Crossbars", Format::PairCount(cm.crossbars, cm.crossbarsSelf)});

    std::vector<std::pair<std::string, std::string>> funRows;
    funRows.push_back({"Max goal speed", Format::PairSpeed(cm.maxGoalSpeed, cm.maxGoalSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
    funRows.push_back({"Max ball speed", Format::PairSpeed(cm.maxBallSpeed, cm.maxBallSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
    if (m_frameConfig.crossbar_display_mode == "speed") {
        funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(cm.maxImpactForce * 0.036f, cm.maxImpactForceSelf * 0.036f, true, " kph", m_frameConfig.imperial_units)});
    } else {
        funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(cm.maxImpactForce, cm.maxImpactForceSelf, true, "", m_frameConfig.imperial_units)});
    }
    funRows.push_back({"Fastest goal", Format::PairFastest(cm.fastestGoalTime, cm.fastestGoalTimeSelf)});
    if (cm.ownGoals > 0) funRows.push_back({"Own goals", Format::PairCount(cm.ownGoals, cm.ownGoalsSelf)});

    RenderStatSection("PLAY", std::string("PLAY") + tableIdPrefix, playRows, true);
    ImGui::Dummy(ImVec2(0.0f, 4.0f * m_dpiScale));
    RenderStatSection("FUN", std::string("FUN") + tableIdPrefix, funRows, true);
}

void Overlay::RenderStreaksStatsTable(const char* tableId) {
    if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);

        int currentWins = 0;
        int currentLosses = 0;
        int longestWins = 0;
        int longestLosses = 0;
        {
            std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
            currentWins = m_state->ui.cachedDbStats.currentWins;
            currentLosses = m_state->ui.cachedDbStats.currentLosses;
            longestWins = m_state->ui.cachedDbStats.longestWins;
            longestLosses = m_state->ui.cachedDbStats.longestLosses;
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Current Streak");
        ImGui::TableNextColumn();
        if (currentWins > 0) {
            ImGui::TextColored(Format::C(m_frameConfig.themeWin), "+%d", currentWins);
        } else if (currentLosses > 0) {
            ImGui::TextColored(Format::C(m_frameConfig.themeLoss), "-%d", currentLosses);
        } else {
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "+0");
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Longest Win Streak");
        ImGui::TableNextColumn();
        ImGui::TextColored(Format::C(m_frameConfig.themeText), "%d", longestWins);

        if (m_frameConfig.show_longest_loss_streak) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Longest Loss Streak");
            ImGui::TableNextColumn();
            ImGui::TextColored(Format::C(m_frameConfig.themeLoss), "%d", longestLosses);
        }

        ImGui::EndTable();
    }
}

void Overlay::RenderGamemodeBreakdownTable(const char* tableId, GamemodeBreakdownScope scope) {
    bool anyEnabled = m_frameConfig.show_gamemode_record_1v1 ||
                      m_frameConfig.show_gamemode_record_2v2 ||
                      m_frameConfig.show_gamemode_record_3v3;

    if (!anyEnabled) {
        ImGui::PushFont(fontSmall);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "No gamemodes selected.");
        ImGui::PopFont();
        return;
    }

    if (ImGui::BeginTable(tableId, 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Record", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Win %", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        std::vector<std::string> gamemodes = {"1v1", "2v2", "3v3"};
        for (const auto& gamemode : gamemodes) {
            if (gamemode == "1v1" && !m_frameConfig.show_gamemode_record_1v1) continue;
            if (gamemode == "2v2" && !m_frameConfig.show_gamemode_record_2v2) continue;
            if (gamemode == "3v3" && !m_frameConfig.show_gamemode_record_3v3) continue;

            int wins = 0, losses = 0, total = 0;

            if (scope == GamemodeBreakdownScope::CurrentSession) {
                if (m_snap.sessionGamemodes.count(gamemode)) {
                    const auto& gm = m_snap.sessionGamemodes.at(gamemode);
                    wins = gm.wins; losses = gm.losses; total = gm.total;
                }
            } else {
                std::lock_guard<std::mutex> lock(m_state->ui.dbStatsMutex);
                if (m_state->ui.cachedDbStats.gamemodes.count(gamemode)) {
                    const auto& gm = m_state->ui.cachedDbStats.gamemodes.at(gamemode);
                    wins = gm.wins; losses = gm.losses; total = gm.total;
                }
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", gamemode.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d-%d", wins, losses);

            ImGui::TableNextColumn();
            if (total > 0) {
                float winPct = (float)wins / total * 100.0f;
                ImGui::Text("%.1f%%", winPct);
            } else {
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "-");
            }

            ImGui::TableNextColumn();
            ImGui::Text("%d", total);
        }
        ImGui::EndTable();
    }
}

void Overlay::RenderSessionStatsTable(const char* tableId, bool isDashboard, bool showStreak) {
    if (isDashboard) {
        if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);

            if (m_frameConfig.show_session_record) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Record"); ImGui::TableNextColumn(); ImGui::Text("%d - %d", m_snap.sessionTotals.wins, m_snap.sessionTotals.losses);
            }
            if (m_frameConfig.show_session_goals) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Goals"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.goals);
            }
            if (m_frameConfig.show_session_saves) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Saves"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.saves);
            }
            if (m_frameConfig.show_session_assists) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Assists"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.assists);
            }
            if (m_frameConfig.show_session_goal_participation) {
                const float gp = CalcGoalParticipationPercent(m_snap.sessionTotals);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Goal Participation"); ImGui::TableNextColumn();
                if (m_snap.sessionTotals.teamGoals > 0) {
                    ImGui::Text("%.0f%%", gp);
                } else {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "--");
                }
            }
            if (m_frameConfig.show_session_demos) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Demos"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.demos);
            }
            if (m_frameConfig.show_session_boost) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Boost Picked Up"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.boostPickedUp);
            }
            if (m_frameConfig.show_session_mmr_change) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("MMR Change"); ImGui::TableNextColumn();
                float deltaVal = m_snap.sessionTotals.totalMmrChange;
                if (deltaVal > 0) ImGui::TextColored(Format::C(m_frameConfig.themeWin), "+%.0f", deltaVal);
                else if (deltaVal < 0) ImGui::TextColored(Format::C(m_frameConfig.themeLoss), "%.0f", deltaVal);
                else ImGui::TextColored(Format::C(m_frameConfig.themeDim), "+0");
            }
            if (showStreak && m_frameConfig.show_streaks_stats) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Streak"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.wins - m_snap.sessionTotals.losses);
            }

            ImGui::EndTable();
        }
    } else {
        // Full Session View
        if (ImGui::BeginTable(tableId, 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);

            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Record"); ImGui::TableNextColumn(); ImGui::Text("%d - %d", m_snap.sessionTotals.wins, m_snap.sessionTotals.losses);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Goals"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.goals);

            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Goal Participation"); ImGui::TableNextColumn();
            const float gp = CalcGoalParticipationPercent(m_snap.sessionTotals);
            if (m_snap.sessionTotals.teamGoals > 0) {
                ImGui::Text("%.0f%%", gp);
            } else {
                ImGui::TextColored(Format::C(m_frameConfig.themeDim), "--");
            }

            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("MMR Change"); ImGui::TableNextColumn();
            float deltaVal = m_snap.sessionTotals.totalMmrChange;
            if (deltaVal > 0) ImGui::TextColored(Format::C(m_frameConfig.themeWin), "+%.0f", deltaVal);
            else if (deltaVal < 0) ImGui::TextColored(Format::C(m_frameConfig.themeLoss), "%.0f", deltaVal);
            else ImGui::TextColored(Format::C(m_frameConfig.themeDim), "+0");

            if (showStreak) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("Streak"); ImGui::TableNextColumn(); ImGui::Text("%d", m_snap.sessionTotals.wins - m_snap.sessionTotals.losses);
            }

            ImGui::EndTable();
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushFont(fontSmall);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Left = Lobby Total  |  Right = You");
        ImGui::PopFont();
        ImGui::Spacing();

        const auto& s = m_snap.sessionTotals;

        std::vector<std::pair<std::string, std::string>> playRows;
        playRows.push_back({"Saves", Format::PairCount(s.savesTotal, s.saves)});
        playRows.push_back({"Shots", Format::PairCount(s.shotsTotal, s.shots)});
        playRows.push_back({"Assists", Format::PairCount(s.assistsTotal, s.assists)});

        std::string gpStr = "--";
        if (s.teamGoals > 0) {
            std::ostringstream oss;
            oss << s.goalParticipations << " / " << s.teamGoals << " (" << std::fixed << std::setprecision(0) << CalcGoalParticipationPercent(s) << "%)";
            gpStr = oss.str();
        }
        playRows.push_back({"Goal participation", gpStr});

        playRows.push_back({"Demos", Format::PairCount(s.demosTotal, s.demos)});
        playRows.push_back({"Crossbars", Format::PairCount(s.crossbarsTotal, s.crossbars)});

        std::vector<std::pair<std::string, std::string>> funRows;
        funRows.push_back({"Max goal speed", Format::PairSpeed(s.maxGoalSpeed, s.maxGoalSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
        funRows.push_back({"Max ball speed", Format::PairSpeed(s.maxBallSpeed, s.maxBallSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
        if (m_frameConfig.crossbar_display_mode == "speed") {
            funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(s.maxImpactForce * 0.036f, s.maxImpactForceSelf * 0.036f, true, " kph", m_frameConfig.imperial_units)});
        } else {
            funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(s.maxImpactForce, s.maxImpactForceSelf, true, "", m_frameConfig.imperial_units)});
        }
        funRows.push_back({"Fastest goal", Format::PairFastest(s.fastestGoalTime, s.fastestGoalTimeSelf)});
        if (s.ownGoals > 0) {
            funRows.push_back({"Own goals", Format::PairCount(s.ownGoals, s.ownGoalsSelf)});
        }

        RenderStatSection("PLAY", std::string("SessionPLAY_") + tableId, playRows);
        RenderStatSection("FUN", std::string("SessionFUN_") + tableId, funRows);
    }
}

void Overlay::RenderSessionCard() {
    // Position: Top-left with 24px margin
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize;

    bool hasVisibleItems = m_frameConfig.show_session_record ||
                          m_frameConfig.show_session_goals ||
                          m_frameConfig.show_session_saves ||
                          m_frameConfig.show_session_assists ||
                          m_frameConfig.show_session_goal_participation ||
                          m_frameConfig.show_session_demos ||
                          m_frameConfig.show_session_mmr_change ||
                          m_frameConfig.show_session_boost;

    if (hasVisibleItems && ImGui::Begin("SessionSummary", nullptr, flags)) {
        ImGui::TextColored(Format::C(m_frameConfig.themeAccent), "SESSION");
        ImGui::Separator();

        RenderSessionStatsTable("SessionCardTbl", true, false);

        ImGui::End();
    }
}

void Overlay::RenderPreviousGamesOverlay() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 90.0f * m_dpiScale), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    const auto& matches = m_snap.recentSavedMatches;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * m_dpiScale, 10.0f * m_dpiScale));
    if (ImGui::Begin("PreviousGames###PreviousGamesOverlay", nullptr, flags)) {
        ImGui::PushFont(fontSmallBold);
        ImGui::TextColored(Format::C(m_frameConfig.themeAccent), "PREVIOUS GAMES");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::PushFont(fontSmall);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "last %d saved games", m_frameConfig.previous_games_limit);
        ImGui::PopFont();
        ImGui::Separator();

        if (!m_snap.recentSavedMatchesLoaded) {
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Loading saved match history...");
        } else if (matches.empty()) {
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "No saved games in local history.");
        } else {
            const size_t displayCount = matches.size();
            const bool twoColumns = io.DisplaySize.x >= 860.0f * m_dpiScale && displayCount > 10;
            const int columnSets = twoColumns ? 2 : 1;
            const int rowsPerColumn = twoColumns ? static_cast<int>((displayCount + 1) / 2) : static_cast<int>(displayCount);
            const int tableColumns = columnSets * 4 + (columnSets - 1);

            if (ImGui::BeginTable("PreviousGamesOverlayTable", tableColumns, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                for (int set = 0; set < columnSets; ++set) {
                    if (set > 0) ImGui::TableSetupColumn("##Gap", ImGuiTableColumnFlags_WidthFixed, 14.0f * m_dpiScale);
                    ImGui::TableSetupColumn((std::string("Playlist##Overlay") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 180.0f * m_dpiScale);
                    ImGui::TableSetupColumn((std::string("Score##Overlay") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 52.0f * m_dpiScale);
                    ImGui::TableSetupColumn((std::string("MMR##Overlay") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 56.0f * m_dpiScale);
                    ImGui::TableSetupColumn((std::string("Time##Overlay") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 78.0f * m_dpiScale);
                }
                ImGui::TableHeadersRow();

                ImGui::PushFont(fontMono);
                for (int row = 0; row < rowsPerColumn; ++row) {
                    ImGui::TableNextRow();
                    for (int set = 0; set < columnSets; ++set) {
                        if (set > 0) {
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted("");
                        }

                        const size_t matchIndex = static_cast<size_t>(set * rowsPerColumn + row);
                        if (matchIndex >= displayCount) {
                            ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                            ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                            ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                            ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                            continue;
                        }

                        const auto& match = matches[matchIndex];
                        const ImVec4 rowColor = Format::C(match.win ? m_frameConfig.themeWin : m_frameConfig.themeLoss);
                        const std::string playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
                        const std::string score = std::to_string(match.ourScore) + "-" + std::to_string(match.theirScore);
                        const std::string time = FormatRelativeMatchTime(match.endedAtUnix);

                        ImGui::TableNextColumn();
                        ImGui::TextColored(rowColor, "%s", playlist.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextColored(rowColor, "%s", score.c_str());
                        ImGui::TableNextColumn();
                        if (match.mmr > 0) ImGui::TextColored(rowColor, "%d", match.mmr);
                        else ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "--");
                        ImGui::TableNextColumn();
                        ImGui::TextColored(rowColor, "%s", time.c_str());
                    }
                }
                ImGui::PopFont();
                ImGui::EndTable();
            }
        }

        ImGui::Dummy(ImVec2(0, 4.0f * m_dpiScale));
        ImGui::PushFont(fontSmallBold);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Current session: W:%d L:%d", m_snap.sessionTotals.wins, m_snap.sessionTotals.losses);
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::RenderDemoTrackerTable(const char* tableId) {
    const auto& cm = m_snap.currentMatch;
    const auto& s = m_snap.sessionTotals;
    const int gameDemos = cm.demosSelf;
    const int gameDemoed = cm.demoedSelf;
    const int sessionDemos = s.demos + gameDemos;
    const int sessionDemoed = s.demoed + gameDemoed;

    if (ImGui::BeginTable(tableId, 3, ImGuiTableFlags_None)) {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 92.0f * m_dpiScale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 58.0f * m_dpiScale);
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, 42.0f * m_dpiScale);

        auto renderRow = [&](const char* label, const std::string& value, const std::string& count, bool positive) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(fontSmallBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", label);
            ImGui::PopFont();

            ImGui::TableNextColumn();
            ImGui::PushFont(fontMono);
            ImVec4 valueColor = positive ? Format::C(m_frameConfig.themeWin) : Format::C(m_frameConfig.themeLoss);
            if (value == "--") valueColor = Format::C(m_frameConfig.themeMuted);
            ImGui::TextColored(valueColor, "%s", value.c_str());
            ImGui::PopFont();

            ImGui::TableNextColumn();
            ImGui::PushFont(fontMono);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", count.c_str());
            ImGui::PopFont();
        };

        renderRow("GAME K/D", FormatDemoKd(gameDemos, gameDemoed), std::to_string(gameDemos) + ":" + std::to_string(gameDemoed), gameDemos >= gameDemoed);
        renderRow("SESSION K/D", FormatDemoKd(sessionDemos, sessionDemoed), std::to_string(sessionDemos) + ":" + std::to_string(sessionDemoed), sessionDemos >= sessionDemoed);

        ImGui::EndTable();
    }
}

void Overlay::RenderDemoTrackerOverlay() {

    ImGui::SetNextWindowPos(ImVec2(24.0f, 130.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * m_dpiScale, 10.0f * m_dpiScale));
    if (ImGui::Begin("DemoTracker###DemoTrackerOverlay", nullptr, flags)) {
        ImGui::PushFont(fontSmallBold);
        ImGui::TextColored(Format::C(m_frameConfig.themeAccent), "DEMOLITION TRACKER");
        ImGui::PopFont();
        ImGui::Separator();

        RenderDemoTrackerTable("DemoTrackerStats");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::RenderMatchSummary() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize;

    Widgets::StyleVarGuard styleGuard(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    // Keep the summary width consistent with the other popups.
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f * m_dpiScale, -1.0f), ImVec2(280.0f * m_dpiScale, -1.0f));

    if (ImGui::Begin("Match Summary", nullptr, flags)) {
        // 1. Result & Score Header (Using a 2-column table to align score right and prevent runaway width scaling)
        if (ImGui::BeginTable("MatchSummaryHeaderTbl", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const int summaryMyTeam = m_snap.matchSummaryMyTeam;
            const int summaryScore0 = m_snap.matchSummaryScore[0];
            const int summaryScore1 = m_snap.matchSummaryScore[1];

            int winner = m_snap.matchSummaryWinnerTeam;
            if (winner != 0 && winner != 1) {
                winner = -1;
                if (summaryScore0 > summaryScore1) winner = 0;
                else if (summaryScore1 > summaryScore0) winner = 1;
            }

            std::string resultStr = "DRAW";
            ImColor resultColor = Format::C(m_frameConfig.themeMuted);

            if (m_snap.lastMatchWasVoid) {
                resultStr = "VOID";
                resultColor = Format::C(m_frameConfig.themeMuted);
            } else if ((summaryMyTeam == 0 || summaryMyTeam == 1) && winner != -1) {
                if (winner == summaryMyTeam) {
                    resultStr = "WIN";
                    resultColor = Format::C(m_frameConfig.themeWin);
                } else {
                    resultStr = "LOSS";
                    resultColor = Format::C(m_frameConfig.themeLoss);
                }
            }

            {
                Widgets::FontGuard fontGuard(fontBold);
                ImGui::TextColored(resultColor, "%s", resultStr.c_str());
            }

            ImGui::TableNextColumn();

            int myScore = (summaryMyTeam == 1) ? summaryScore1 : summaryScore0;
            int opScore = (summaryMyTeam == 1) ? summaryScore0 : summaryScore1;
            std::string scoreStr = std::to_string(myScore) + "-" + std::to_string(opScore);

            {
                Widgets::FontGuard fontGuard(fontBold);
                ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", scoreStr.c_str());
            }

            ImGui::EndTable();
        }

        if (m_snap.lastMatchWasVoid && !m_snap.lastMatchVoidReason.empty()) {
            ImGui::Spacing();
            ImGui::PushFont(fontSmall);
            ImGui::PushStyleColor(ImGuiCol_Text, Format::C(m_frameConfig.themeMuted));
            ImGui::TextWrapped("Not counted: %s", Format::FriendlyVoidReason(m_snap.lastMatchVoidReason).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }

        ImGui::Dummy(ImVec2(0, 4.0f * m_dpiScale));
        ImGui::Separator();

        // 2. Statistics Gather
        const auto& cm = m_snap.currentMatch;
        std::vector<std::pair<std::string, std::string>> playRows;
        if (cm.saves > 0) playRows.push_back({"Saves", Format::PairCount(cm.saves, cm.savesSelf)});
        if (cm.shots > 0) playRows.push_back({"Shots", Format::PairCount(cm.shots, cm.shotsSelf)});
        if (cm.assists > 0) playRows.push_back({"Assists", Format::PairCount(cm.assists, cm.assistsSelf)});
        if (cm.demos > 0) playRows.push_back({"Demos", Format::PairCount(cm.demos, cm.demosSelf)});
        if (cm.demoedSelf > 0) playRows.push_back({"Demoed", std::to_string(cm.demoedSelf)});
        if (cm.crossbars > 0) playRows.push_back({"Crossbars", Format::PairCount(cm.crossbars, cm.crossbarsSelf)});
        if (cm.boostPickedUp > 0) playRows.push_back({"Boost", Format::PairCount(cm.boostPickedUp, cm.boostPickedUpSelf)});

        std::vector<std::pair<std::string, std::string>> funRows;
        if (cm.maxGoalSpeed > 0) funRows.push_back({"Max goal speed", Format::PairSpeed(cm.maxGoalSpeed, cm.maxGoalSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
        if (cm.maxBallSpeed > 0) funRows.push_back({"Max ball speed", Format::PairSpeed(cm.maxBallSpeed, cm.maxBallSpeedSelf, true, " kph", m_frameConfig.imperial_units)});
        if (cm.maxImpactForce > 0) {
            if (m_frameConfig.crossbar_display_mode == "speed") {
                funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(cm.maxImpactForce * 0.036f, cm.maxImpactForceSelf * 0.036f, true, " kph", m_frameConfig.imperial_units)});
            } else {
                funRows.push_back({"Hardest crossbar hit", Format::PairSpeed(cm.maxImpactForce, cm.maxImpactForceSelf, true, "", m_frameConfig.imperial_units)});
            }
        }
        if (cm.fastestGoalTime > 0) funRows.push_back({"Fastest goal", Format::PairFastest(cm.fastestGoalTime, cm.fastestGoalTimeSelf)});
        if (cm.ownGoals > 0) funRows.push_back({"Own goals", Format::PairCount(cm.ownGoals, cm.ownGoalsSelf)});

        RenderStatSection("PLAY", "SummaryPLAYTbl", playRows);
        RenderStatSection("FUN", "SummaryFUNTbl", funRows);
    }
    ImGui::End();
}

void Overlay::RenderSessionView() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);
    ImGui::SetNextWindowSizeConstraints(ImVec2(450.0f * m_dpiScale, -1.0f), ImVec2(450.0f * m_dpiScale, -1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 20));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("Session View", nullptr, flags)) {
        std::string playlist = MmrCategoryToString(m_state->ui.graphMmrCategory.load()); // "1v1", "2v2", "3v3", "best", "casual"
        std::string playlistUpper = playlist;
        std::transform(playlistUpper.begin(), playlistUpper.end(), playlistUpper.begin(), ::toupper);

        // Dynamic Database Refresh for Lifetime graph
        static MmrCategory lastCategory = MmrCategory::Best;
        static bool lastShowLifetime = false;
        static std::string lastLifetimePrimaryId = "";
        MmrCategory curCat = m_state->ui.graphMmrCategory.load();
        if (curCat != lastCategory || m_snap.showLifetimeGraph != lastShowLifetime || m_snap.myPrimaryId != lastLifetimePrimaryId) {
            lastCategory = curCat;
            lastShowLifetime = m_snap.showLifetimeGraph;
            lastLifetimePrimaryId = m_snap.myPrimaryId;
            if (m_snap.showLifetimeGraph && m_dbManager && !m_snap.myPrimaryId.empty()) {
                m_dbManager->AsyncGetLifetimeMmrHistory(m_snap.myPrimaryId, playlist);
            }
        }

        if (m_state->ui.showGraphView) {
            // Theme colors mapped to ImColor.
            auto& t = m_frameConfig;
            ImColor colorBg     = Format::C(t.themeBg);
            ImColor colorText   = Format::C(t.themeText);
            ImColor colorAccent = Format::C(t.themeAccent);
            ImColor colorDim    = Format::C(t.themeDim);
            ImColor colorMuted  = Format::C(t.themeMuted);
            ImColor colorWin    = Format::C(t.themeWin);
            ImColor colorLoss   = Format::C(t.themeLoss);
            ImColor colorGraphLine = Format::C(t.themeGraphLine);
            ImColor colorGraphBaseline = Format::C(t.themeGraphBaseline);

            auto [currentMmr, initialMmr, delta] = RenderHelper::ComputeMmrDelta(m_snap, playlist);

            // Title: "MMR · 2V2" or "MMR · LIFETIME · 2V2"
            std::string displayPlaylist = m_snap.showLifetimeGraph ? "LIFETIME \xC2\xB7 " + playlistUpper : playlistUpper;
            ImGui::PushFont(fontBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "MMR \xC2\xB7 %s", displayPlaylist.c_str());
            ImGui::PopFont();

            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130.0f);
            bool showLifetime = m_snap.showLifetimeGraph;
            if (ImGui::Checkbox("Lifetime", &showLifetime)) {
                m_state->history.showLifetimeGraph = showLifetime;
            }

            // Session/Lifetime Change Badge (Top-Right)
            ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
            ImVec2 badgePos = ImVec2(cursorScreen.x + ImGui::GetContentRegionAvail().x - 44.0f, cursorScreen.y - 24.0f);
            Widgets::RenderMmrDeltaBadge(badgePos, delta, colorWin, colorLoss, colorMuted, fontSmallBold);

            // Render Plot Area
            const auto& history = m_snap.showLifetimeGraph ? m_snap.lifetimeMmrY :
                                 (m_snap.playlistHistoryY.count(playlist) ? m_snap.playlistHistoryY.at(playlist) : std::vector<float>{});
            if (history.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 25.0f));
                ImGui::PushFont(fontRegular);
                if (m_snap.showLifetimeGraph) {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "No lifetime MMR history in database yet.");
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Play matches to populate database records!");
                } else {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "No MMR data for %s this session.", playlistUpper.c_str());
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Play a match to start plotting!");
                }
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 105.0f));
            } else {
                Widgets::MmrGraphParams params{
                    .history = history,
                    .currentMmr = currentMmr,
                    .initialMmr = initialMmr,
                    .plotHeight = 150.0f,
                    .colorWin = colorWin,
                    .colorLoss = colorLoss,
                    .colorMuted = colorMuted,
                    .colorDim = colorDim,
                    .colorText = colorText,
                    .colorGraphLine = colorGraphLine,
                    .colorGraphBaseline = colorGraphBaseline,
                    .fontSmall = fontSmall,
                    .fontSmallBold = fontSmallBold,
                    .fontBold = fontBold
                };
                Widgets::RenderMmrGraph(params);
            }
        } else {
            ImGui::TextColored(Format::C(m_frameConfig.themeAccent), "SESSION STATS");
            ImGui::Separator();

            RenderSessionStatsTable("SessionStatsTbl", true, true);
        }

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 2.0f));

        ImGui::PushFont(fontSmall);
        ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "F7 session \xC2\xB7 F6 playlist");
        ImGui::PopFont();

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(playlistUpper.c_str()).x);

        ImGui::PushFont(fontSmallBold);
        ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", playlistUpper.c_str());
        ImGui::PopFont();
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Overlay::RenderLobbyRanksTable(const char* tableId) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f * m_dpiScale, 1.0f * m_dpiScale));
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(255, 255, 255, 12));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, IM_COL32(255, 255, 255, 12));

    std::vector<std::pair<std::string, std::string>> playlists;
    if (m_frameConfig.show_lobby_rank_1v1) playlists.push_back({"1v1", "1v1"});
    if (m_frameConfig.show_lobby_rank_2v2) playlists.push_back({"2v2", "2v2"});
    if (m_frameConfig.show_lobby_rank_3v3) playlists.push_back({"3v3", "3v3"});
    if (m_frameConfig.show_lobby_rank_casual) playlists.push_back({"casual", "Cas"});
    if (m_frameConfig.show_lobby_rank_tourny) playlists.push_back({"t", "T"});

    if (m_frameConfig.show_extra_playlists) {
        if (m_frameConfig.show_lobby_rank_hoops) playlists.push_back({"hoops", "HP"});
        if (m_frameConfig.show_lobby_rank_rumble) playlists.push_back({"rumble", "R"});
        if (m_frameConfig.show_lobby_rank_dropshot) playlists.push_back({"dropshot", "D"});
        if (m_frameConfig.show_lobby_rank_snowday) playlists.push_back({"snowday", "S"});
        if (m_frameConfig.show_lobby_rank_heatseeker) playlists.push_back({"heatseeker", "HS"});
    }

    if (ImGui::BeginTable(tableId, static_cast<int>(playlists.size()) + 1, ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 110.0f * m_dpiScale);
            for (const auto& playlist : playlists) {
                ImGui::TableSetupColumn(playlist.second.c_str(), ImGuiTableColumnFlags_WidthFixed, 54.0f * m_dpiScale);
            }
            
            // Header Row background derived from theme background
            ImVec4 baseColor = Format::C(m_frameConfig.themeBg);
            float alpha = 0.8f;
            ImU32 bgHeader = IM_COL32(
                (int)(baseColor.x * 0.70f * 255),
                (int)(baseColor.y * 0.70f * 255),
                (int)(baseColor.z * 0.70f * 255),
                (int)(alpha * 255)
            );
            float rEven = baseColor.x * 1.15f; if (rEven > 1.0f) rEven = 1.0f;
            float gEven = baseColor.y * 1.15f; if (gEven > 1.0f) gEven = 1.0f;
            float bEven = baseColor.z * 1.15f; if (bEven > 1.0f) bEven = 1.0f;
            ImU32 bgEven = IM_COL32(
                (int)(rEven * 255),
                (int)(gEven * 255),
                (int)(bEven * 255),
                (int)(alpha * 255)
            );
            ImU32 bgOdd = IM_COL32(
                (int)(baseColor.x * 0.85f * 255),
                (int)(baseColor.y * 0.85f * 255),
                (int)(baseColor.z * 0.85f * 255),
                (int)(alpha * 255)
            );
 
            // Header Row
            ImGui::TableNextRow();
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, bgHeader);
            
            auto CenterText = [](const char* text, ImColor color, ImFont* font) {
                ImGui::PushFont(font);
                float posX = ImGui::GetCursorPosX() + (ImGui::GetColumnWidth() - ImGui::CalcTextSize(text).x) * 0.5f;
                if (posX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(posX);
                ImGui::TextColored(color, "%s", text);
                ImGui::PopFont();
            };
 
            ImGui::TableNextColumn();
            ImGui::PushFont(lobbyFontSmall);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * m_dpiScale);
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Name");
            ImGui::PopFont();
            
            for (const auto& playlist : playlists) {
                ImGui::TableNextColumn(); CenterText(playlist.second.c_str(), Format::C(m_frameConfig.themeMuted), lobbyFontSmall);
            }
 
            std::vector<const PlayerData*> allPlayers;
            for (const auto& [id, pData] : m_snap.roster) {
                allPlayers.push_back(&pData);
            }
 
            // Sort players: Blue first (0), Orange (1), then MMR in 2v2
            std::sort(allPlayers.begin(), allPlayers.end(), [&](const PlayerData* a, const PlayerData* b) {
                if (a->team != b->team) return a->team < b->team;
                int mmrA = a->playlists.count("2v2") ? a->playlists.at("2v2") : a->mmr;
                int mmrB = b->playlists.count("2v2") ? b->playlists.at("2v2") : b->mmr;
                if (mmrA != mmrB) return mmrA > mmrB;
                return a->name < b->name;
            });
 
            bool even = false;
            for (const auto* pDataPtr : allPlayers) {
                const auto& pData = *pDataPtr;
 
                ImU32 rowBgColor = even ? bgEven : bgOdd;
                even = !even;
 
                // Color player name: White if it's the local player, else team colored
                ImColor nameColor;
                if (pData.primaryId == m_snap.myPrimaryId) {
                    nameColor = ImColor(255, 255, 255, 255); // White
                } else if (pData.team == 0) {
                    nameColor = ImColor(59, 158, 255, 255); // Blue
                } else if (pData.team == 1) {
                    nameColor = ImColor(235, 140, 36, 255); // Orange
                } else {
                    nameColor = Format::C(m_frameConfig.themeText);
                }
                
                ImColor platformColor = Format::C(m_frameConfig.themeMuted);
                std::string platform = "";
                size_t delim = pData.primaryId.find('|');
                if (delim != std::string::npos) {
                    platform = pData.primaryId.substr(0, delim);
                }
                std::string displayPlat = platform;
                if (platform == "Steam") {
                    displayPlat = "Steam";
                    platformColor = ImColor(110, 115, 128); // Grey-blue
                } else if (platform == "Epic") {
                    displayPlat = "Epic";
                    platformColor = ImColor(164, 164, 164); // Light grey
                } else if (platform.rfind("PS", 0) == 0) {
                    displayPlat = "PlayStation";
                    platformColor = ImColor(21, 80, 150); // Darker blue
                } else if (platform.rfind("Xbox", 0) == 0) {
                    displayPlat = "Xbox";
                    platformColor = ImColor(60, 120, 60); // Green
                } else if (platform == "Switch" || platform == "Nintendo") {
                    displayPlat = "Nintendo";
                    platformColor = ImColor(200, 40, 30); // Red
                }

                // Truncate player name if too long for compact layout
                std::string displayName = pData.name;
                if (displayName.length() > 14) {
                    displayName = displayName.substr(0, 12) + "..";
                }
 
                ImGui::TableNextRow();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowBgColor);
                
                ImGui::TableNextColumn();
                ImGui::PushFont(lobbyFontSmall);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * m_dpiScale);
                ImGui::TextColored(nameColor, "%s", displayName.c_str());
                ImGui::PopFont();
                
                for (const auto& playlist : playlists) {
                    const std::string& pl = playlist.first;
                    ImGui::TableNextColumn();
                    if (pData.playlists.count(pl) && pData.playlists.at(pl) > 0) {
                        std::string tier = pData.playlistTiers.count(pl) ? pData.playlistTiers.at(pl) : "Unranked";
                        std::string abbr = Format::AbbreviateRank(tier);
                        CenterText(abbr.c_str(), Format::RankColor(tier), lobbyFontSmall);
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            int matches = pData.playlistMatches.count(pl) ? pData.playlistMatches.at(pl) : 0;
                            ImGui::Text("%s", tier.c_str());
                            ImGui::Text("MMR: %d", pData.playlists.at(pl));
                            ImGui::Text("Matches: %d", matches);
                            ImGui::EndTooltip();
                        }
                    } else if (pData.fetched) {
                        CenterText("-", Format::C(m_frameConfig.themeMuted), lobbyFontSmall);
                    } else {
                        CenterText("...", Format::C(m_frameConfig.themeMuted), lobbyFontSmall);
                    }
                }
 
                ImGui::TableNextRow();
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowBgColor);
                
                ImGui::TableNextColumn();
                ImGui::PushFont(lobbyFontSmall);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f * m_dpiScale);
                if (!displayPlat.empty()) {
                    ImGui::TextColored(platformColor, "%s", displayPlat.c_str());
                } else {
                    ImGui::TextDisabled(" ");
                }
                ImGui::PopFont();
                
                for (const auto& playlist : playlists) {
                    const std::string& pl = playlist.first;
                    ImGui::TableNextColumn();
                    if (pData.playlists.count(pl) && pData.playlists.at(pl) > 0) {
                        int mmr = pData.playlists.at(pl);
                        int matches = pData.playlistMatches.count(pl) ? pData.playlistMatches.at(pl) : 0;
                        std::string tier = pData.playlistTiers.count(pl) ? pData.playlistTiers.at(pl) : "Unranked";
                        std::string text = std::to_string(mmr);
                        if (matches > 0) {
                            text += " (" + std::to_string(matches) + ")";
                        }
                        CenterText(text.c_str(), Format::RankColor(tier), lobbyFontSmall);
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            int matches = pData.playlistMatches.count(pl) ? pData.playlistMatches.at(pl) : 0;
                            ImGui::Text("%s", tier.c_str());
                            ImGui::Text("MMR: %d", mmr);
                            ImGui::Text("Matches: %d", matches);
                            ImGui::EndTooltip();
                        }
                    } else if (pData.fetched) {
                        CenterText("-", Format::C(m_frameConfig.themeMuted), lobbyFontSmall);
                    } else {
                        CenterText("...", Format::C(m_frameConfig.themeMuted), lobbyFontSmall);
                    }
                }
            }
            ImGui::EndTable();
        }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void Overlay::RenderLobbyRanksOverlay() {
    ImGuiIO& io = ImGui::GetIO();

    static ImVec2 lastSize = ImVec2(0, 0);
    static ImVec2 lastPos = ImVec2(0, 0);
    static bool wasNearBottom = false;
    static float bottomDist = 0.0f;
    static bool wasDragging = false;

    // If it was near the bottom of the screen, and the user is not dragging it, we anchor the bottom position
    if (wasNearBottom && lastSize.y > 0 && !wasDragging) {
        float targetY = io.DisplaySize.y - bottomDist - lastSize.y;
        ImGui::SetNextWindowPos(ImVec2(lastPos.x, targetY), ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24.0f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    }

    // No window background, we will color the table rows
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (ImGui::Begin("Lobby Ranks Overlay", nullptr, flags)) {
        RenderLobbyRanksTable("LobbyRanksTable");

        lastPos = ImGui::GetWindowPos();
        lastSize = ImGui::GetWindowSize();
        wasDragging = ImGui::IsWindowHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);

        float currentBottom = lastPos.y + lastSize.y;
        float distToBottom = io.DisplaySize.y - currentBottom;
        // If the bottom is within 60 pixels of the screen bottom, treat it as snapped/anchored to the bottom
        if (distToBottom < 60.0f && lastSize.y > 0) {
            wasNearBottom = true;
            bottomDist = distToBottom;
            if (bottomDist < 0.0f) {
                bottomDist = 0.0f;
            }
        } else {
            wasNearBottom = false;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Overlay::RenderUI() {
    // Take a rapid double-lock snapshot of SessionState telemetry (TS-2)
    {
        std::shared_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.version.load() != m_lastGameVersion) {
            m_lastGameVersion = m_state->game.version.load();
            m_snap.matchGuid = m_state->game.matchGuid;
            m_snap.arenaName = m_state->game.arenaName;
            m_snap.score[0] = m_state->game.score[0];
            m_snap.score[1] = m_state->game.score[1];
            m_snap.inMatch = m_state->game.inMatch;
            m_snap.inReplay = m_state->game.inReplay;
            m_snap.maxPlayersSeen = m_state->game.maxPlayersSeen;
            m_snap.myPrimaryId = m_state->game.myPrimaryId;
            m_snap.myTeam = m_state->game.myTeam;
            m_snap.currentMatch = m_state->game.currentMatch;
            m_snap.sessionTotals = m_state->game.sessionTotals;
            m_snap.roster = m_state->game.roster;
            m_snap.sessionGamemodes = m_state->game.sessionGamemodes;
            m_snap.lastMatchWasVoid = m_state->game.lastMatchWasVoid;
            m_snap.lastMatchVoidReason = m_state->game.lastMatchVoidReason;
            m_snap.matchSummaryScore[0] = m_state->game.matchSummaryScore[0];
            m_snap.matchSummaryScore[1] = m_state->game.matchSummaryScore[1];
            m_snap.matchSummaryMyTeam = m_state->game.matchSummaryMyTeam;
            m_snap.matchSummaryWinnerTeam = m_state->game.matchSummaryWinnerTeam;
        }
    }
    {
        std::shared_lock<std::shared_mutex> historyLock(m_state->history.mutex);
        m_snap.showLifetimeGraph = m_state->history.showLifetimeGraph.load();
        if (m_state->history.version.load() != m_lastHistoryVersion) {
            m_lastHistoryVersion = m_state->history.version.load();
            m_snap.initialMmr = m_state->history.initialMmr;
            m_snap.mmrHistoryX = m_state->history.mmrHistoryX;
            m_snap.mmrHistoryY = m_state->history.mmrHistoryY;
            m_snap.playlistInitialMmr = m_state->history.playlistInitialMmr;
            m_snap.playlistHistoryY = m_state->history.playlistHistoryY;
            m_snap.lifetimeMmrX = m_state->history.lifetimeMmrX;
            m_snap.lifetimeMmrY = m_state->history.lifetimeMmrY;
            m_snap.recentSavedMatches = m_state->history.recentSavedMatches;
            m_snap.recentSavedMatchesLoaded = m_state->history.recentSavedMatchesLoaded;
        }
    }

    // If we don't currently have an identified myPrimaryId in the snapshot, prefer
    // the persisted last_primary_id as a UI fallback so lifetime graphs can show while
    // the identity process catches up. This should not override an actively detected
    // myPrimaryId (set from telemetry).
    if (m_snap.myPrimaryId.empty()) {
        ConfigData conf = Config::Read();
        if (!conf.last_primary_id.empty()) {
            m_snap.myPrimaryId = conf.last_primary_id;
        }
    }

    // Trigger DB fetches (lifetime history + cached DB stats) when the effective
    // primary id for the UI changes. We use m_lastDbFetchPrimaryId to avoid
    // repeatedly enqueueing async DB jobs every frame.
    {
        std::string effectivePrimary = m_snap.myPrimaryId;
        if (effectivePrimary.empty()) {
            ConfigData conf = Config::Read();
            effectivePrimary = conf.last_primary_id;
        }
        if (!effectivePrimary.empty() && m_dbManager) {
            if (effectivePrimary != m_lastDbFetchPrimaryId) {
                std::string playlist = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                m_dbManager->AsyncGetLifetimeMmrHistory(effectivePrimary, playlist);
                m_dbManager->AsyncRefreshDbStats(effectivePrimary);
                m_lastDbFetchPrimaryId = effectivePrimary;
            }
        }
    }

    if (m_dbManager) {
        std::string effectivePrimary = m_snap.myPrimaryId;
        if (effectivePrimary.empty()) {
            ConfigData conf = Config::Read();
            effectivePrimary = conf.last_primary_id;
        }
        int recentLimit = std::clamp(m_frameConfig.previous_games_limit, 10, kPreviousGamesMaxLimit);
        if (effectivePrimary != m_lastRecentMatchHistoryPrimaryId || recentLimit != m_lastRecentMatchHistoryLimit) {
            m_dbManager->AsyncGetRecentMatchHistory(effectivePrimary, recentLimit);
            m_lastRecentMatchHistoryPrimaryId = effectivePrimary;
            m_lastRecentMatchHistoryLimit = recentLimit;
        }
    }

    if (m_frameConfig.show_running_indicator && !m_frameConfig.second_monitor_mode) {
        ImGuiWindowFlags indicatorFlags = ImGuiWindowFlags_NoDecoration |
                                          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                          ImGuiWindowFlags_AlwaysAutoResize;
        ImGui::SetNextWindowBgAlpha(m_frameConfig.themeBg.a);
        ImGui::SetNextWindowPos(ImVec2(12.0f * m_dpiScale, 12.0f * m_dpiScale), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * m_dpiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * m_dpiScale, 8.0f * m_dpiScale));
        if (ImGui::Begin("RunningIndicator", nullptr, indicatorFlags)) {
            // Draw a small green status dot
            ImVec2 p = ImGui::GetCursorScreenPos();
            float radius = 4.0f * m_dpiScale;
            p.x += radius;
            p.y += ImGui::GetTextLineHeight() * 0.5f;
            ImGui::GetWindowDrawList()->AddCircleFilled(p, radius, IM_COL32(40, 220, 110, 255));

            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + radius * 2.0f + 6.0f * m_dpiScale);
            ImGui::PushFont(fontSmallBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "OmniStats");
            ImGui::PopFont();
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    if (m_frameConfig.second_monitor_mode) {
        if (m_dashboardPanel) m_dashboardPanel->Render();
        if (m_state->ui.showMenu && m_settingsPanel) {
            SettingsResult result = m_settingsPanel->Render();
            if (result.windowChanged) {
                UpdateWindowStyle();
                UpdateWindowPosition();
            }
            if (result.styleChanged) {
                ApplyTheme();
            }
        }
        return;
    }

    // Render Match Summary Popup if active
    if (m_state->ui.showMatchSummary && m_frameConfig.show_match_summary) {
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t elapsedMs = nowMs - m_state->ui.matchSummaryStartMs.load();
        if (elapsedMs < 30000) { // 30 seconds
            RenderMatchSummary();
        } else {
            m_state->ui.showMatchSummary = false;
        }
    }

    // Render session view (F8 page) if requested
    if (m_state->ui.showSessionView) {
        RenderSessionView();
    }

    // Render modular containers
    if (!m_layoutManager) {
        m_layoutManager = std::make_unique<OverlayLayout::LayoutManager>([this](DashboardLayout::WidgetId id, const char* suffix) {
            this->RenderWidgetContent(id, suffix);
        });
    }
    bool overlayEditMode = m_state->ui.showMenu && m_state->ui.dashboardLayoutEditMode.load();
    ID3D11ShaderResourceView* logoTexture = m_rankIcons ? m_rankIcons->LogoTexture() : nullptr;
    m_layoutManager->Render(m_state->ui.showMenu, overlayEditMode, m_state->ui.showOverlay, m_snap.inMatch, m_state->ui.h2hExpanded.load(), m_dpiScale, fontBold, fontSmall, fontSmallBold, fontMono, logoTexture);

    // Render Settings Menu last so edit-mode background drop targets cannot cover it
    if (m_state->ui.showMenu && m_settingsPanel) {
        SettingsResult result = m_settingsPanel->Render();
        if (result.windowChanged) {
            UpdateWindowStyle();
            UpdateWindowPosition();
        }
        if (result.styleChanged) {
            ApplyTheme();
        }
    }
}

void Overlay::RenderWidgetContent(DashboardLayout::WidgetId id, const char* suffix) {
    switch (id) {
        case DashboardLayout::WidgetId::LiveRoster: {
            std::string suffixStr = suffix ? suffix : "";
            std::string headerId = "HeaderTbl_" + suffixStr;
            if (ImGui::BeginTable(headerId.c_str(), 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoClip);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushFont(fontBold);
                ImGui::TextColored(Format::C(m_frameConfig.themeText), "O M N I S T A T S");
                ImGui::PopFont();

                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmallBold);
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "MMR \xC2\xB7 ");
                ImGui::SameLine(0, 0);
                std::string cat = MmrCategoryToString(m_state->ui.rosterMmrCategory.load());
                std::string catUpper = cat;
                std::transform(catUpper.begin(), catUpper.end(), catUpper.begin(), ::toupper);
                if (m_frameConfig.use_rank_icons && m_rankIcons && m_rankIcons->IsLoaded()) {
                    ID3D11ShaderResourceView* playlistIcon = m_rankIcons->PlaylistTexture(cat);
                    if (playlistIcon) {
                        RenderInlineIcon(playlistIcon, ImVec2(24.0f * m_dpiScale, 24.0f * m_dpiScale), catUpper.c_str());
                    } else {
                        ImGui::TextColored(Format::C(m_frameConfig.themeDim), "%s", catUpper.c_str());
                    }
                } else {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "%s", catUpper.c_str());
                }
                ImGui::PopFont();

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::PushFont(fontSmall);
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", m_snap.arenaName.c_str());
                ImGui::PopFont();

                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Spacing();

            if (m_snap.roster.empty()) {
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "No active match connected.");
            } else {
                RenderPlayerRoster(0, "BLUE", ImColor(59, 158, 255), (std::string("BLUE_") + suffixStr).c_str());
                ImGui::Dummy(ImVec2(0, 8.0f * m_dpiScale));
                RenderPlayerRoster(1, "ORANGE", ImColor(255, 122, 41), (std::string("ORANGE_") + suffixStr).c_str());
            }

            ImGui::Dummy(ImVec2(0, 4.0f * m_dpiScale));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4.0f * m_dpiScale));

            std::string footerId = "FooterTbl_" + suffixStr;
            if (ImGui::BeginTable(footerId.c_str(), 2, ImGuiTableFlags_None)) {
                ImGui::TableSetupColumn("L", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoClip);
                ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoClip);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::string keyCycleName = GetKeyDisplayName(m_frameConfig.key_cycle);
                ImGui::PushFont(fontSmallBold); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", keyCycleName.c_str()); ImGui::PopFont();
                ImGui::SameLine(0, 4.0f); ImGui::PushFont(fontSmall); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "cycle MMR"); ImGui::PopFont();

                ImGui::TableNextColumn();
                std::string footCat = MmrCategoryToString(m_state->ui.rosterMmrCategory.load());
                std::string footCatUpper = footCat;
                std::transform(footCatUpper.begin(), footCatUpper.end(), footCatUpper.begin(), ::toupper);
                if (m_frameConfig.use_rank_icons && m_rankIcons && m_rankIcons->IsLoaded()) {
                    ID3D11ShaderResourceView* playlistIcon = m_rankIcons->PlaylistTexture(footCat);
                    if (playlistIcon) {
                        RenderInlineIcon(playlistIcon, ImVec2(22.0f * m_dpiScale, 22.0f * m_dpiScale), footCatUpper.c_str());
                    } else {
                        ImGui::PushFont(fontSmallBold); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", footCatUpper.c_str()); ImGui::PopFont();
                    }
                } else {
                    ImGui::PushFont(fontSmallBold); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", footCatUpper.c_str()); ImGui::PopFont();
                }

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                std::string keyExpandName = GetKeyDisplayName(m_frameConfig.key_expand);
                const char* expandLabel = m_state->ui.h2hExpanded.load() ? "shrink" : "expand";
                ImGui::PushFont(fontSmallBold); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", keyExpandName.c_str()); ImGui::PopFont();
                ImGui::SameLine(0, 4.0f); ImGui::PushFont(fontSmall); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", expandLabel); ImGui::PopFont();

                ImGui::TableNextColumn();
                std::string keySessionName = GetKeyDisplayName(m_frameConfig.key_session);
                ImGui::PushFont(fontSmallBold); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "%s", keySessionName.c_str()); ImGui::PopFont();
                ImGui::SameLine(0, 4.0f); ImGui::PushFont(fontSmall); ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "session"); ImGui::PopFont();

                ImGui::EndTable();
            }
            break;
        }
        case DashboardLayout::WidgetId::LiveMatchStats: {
            RenderLiveMatchStats((std::string("Live_") + suffix).c_str());
            break;
        }
        case DashboardLayout::WidgetId::SessionStats: {
            RenderSessionStatsTable((std::string("Session_") + suffix).c_str(), true, true);
            break;
        }
        case DashboardLayout::WidgetId::MmrGraph: {
            std::string playlist = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
            std::string playlistUpper = playlist;
            std::transform(playlistUpper.begin(), playlistUpper.end(), playlistUpper.begin(), ::toupper);

            auto& t = m_frameConfig;
            ImColor colorBg     = Format::C(t.themeBg);
            ImColor colorText   = Format::C(t.themeText);
            ImColor colorAccent = Format::C(t.themeAccent);
            ImColor colorDim    = Format::C(t.themeDim);
            ImColor colorMuted  = Format::C(t.themeMuted);
            ImColor colorWin    = Format::C(t.themeWin);
            ImColor colorLoss   = Format::C(t.themeLoss);
            ImColor colorGraphLine = Format::C(t.themeGraphLine);
            ImColor colorGraphBaseline = Format::C(t.themeGraphBaseline);

            auto [currentMmr, initialMmr, delta] = RenderHelper::ComputeMmrDelta(m_snap, playlist);

            std::string displayPlaylist = m_snap.showLifetimeGraph ? "LIFETIME \xC2\xB7 " + playlistUpper : playlistUpper;

            std::string titleStr = std::string("MMR \xC2\xB7 ") + displayPlaylist;
            ImGui::PushFont(fontBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeText), "%s", titleStr.c_str());
            ImGui::PopFont();

            ImGui::SameLine();
            float avail = ImGui::GetContentRegionAvail().x;

            float baseComboW_graph = 120.0f * m_dpiScale;
            float checkboxLabelW = ImGui::CalcTextSize("Lifetime").x;
            float checkboxBoxW = ImGui::GetFrameHeight();
            float baseCheckboxTotal = checkboxBoxW + 6.0f * m_dpiScale + checkboxLabelW;
            float badgeReserve = 44.0f * m_dpiScale;
            float gap = 8.0f * m_dpiScale;

            float controlsWanted = baseComboW_graph + gap + baseCheckboxTotal;
            float maxAvailable = avail - badgeReserve - gap;
            float scale = 1.0f;
            const float minScale = 0.5f;
            if (controlsWanted > maxAvailable && maxAvailable > 0.0f) {
                scale = (std::max)(minScale, maxAvailable / controlsWanted);
            }

            bool pushedSmallGraph = (scale < 1.0f - 1e-6f);
            if (pushedSmallGraph) {
                ImGui::PushFont(fontSmall);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * m_dpiScale * scale, 2.0f * m_dpiScale * scale));
            }

            float comboW_graph = baseComboW_graph * scale;
            float checkboxTotal = baseCheckboxTotal * scale;

            float controlsTotal = comboW_graph + gap + checkboxTotal;
            float posX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - (badgeReserve + controlsTotal);
            float minXAllowed = ImGui::GetCursorPosX() + 8.0f * m_dpiScale;
            if (posX < minXAllowed) posX = minXAllowed;

            ImGui::SetCursorPosX(posX);
            ImGui::SetNextItemWidth(comboW_graph);

            std::vector<std::pair<const char*, MmrCategory>> graphCategories = {
                {"1v1", MmrCategory::OneVOne}, {"2v2", MmrCategory::TwoVTwo}, {"3v3", MmrCategory::ThreeVThree},
                {"Casual", MmrCategory::Casual}, {"Tournament", MmrCategory::Tourny}
            };
            if (m_frameConfig.show_extra_playlists) {
                graphCategories.insert(graphCategories.begin() + 3, {
                    {"Hoops", MmrCategory::Hoops}, {"Rumble", MmrCategory::Rumble},
                    {"Dropshot", MmrCategory::Dropshot}, {"Snow Day", MmrCategory::SnowDay},
                    {"Heatseeker", MmrCategory::Heatseeker}
                });
            }
            std::vector<const char*> graphLabels;
            for (const auto& category : graphCategories) graphLabels.push_back(category.first);
            int currentGraphCat = 1;
            MmrCategory loadedGraphCat = m_state->ui.graphMmrCategory.load();
            for (int i = 0; i < static_cast<int>(graphCategories.size()); ++i) {
                if (graphCategories[i].second == loadedGraphCat) {
                    currentGraphCat = i;
                    break;
                }
            }

            if (ImGui::Combo((std::string("##GraphCatCombo_") + suffix).c_str(), &currentGraphCat, graphLabels.data(), static_cast<int>(graphLabels.size()))) {
                MmrCategory selectedGraphCat = graphCategories[currentGraphCat].second;
                m_state->ui.graphMmrCategory.store(selectedGraphCat);
                if (m_state->history.showLifetimeGraph.load() && m_dbManager && !m_snap.myPrimaryId.empty()) {
                    std::string playlist = MmrCategoryToString(selectedGraphCat);
                    m_dbManager->AsyncGetLifetimeMmrHistory(m_snap.myPrimaryId, playlist);
                }
            }

            ImGui::SameLine(0.0f, gap);
            bool showLifetime = m_snap.showLifetimeGraph;
            if (ImGui::Checkbox((std::string("Lifetime##") + suffix).c_str(), &showLifetime)) {
                m_state->history.showLifetimeGraph = showLifetime;
            }

            if (pushedSmallGraph) {
                ImGui::PopStyleVar();
                ImGui::PopFont();
            }

            ImVec2 cursorScreen = ImGui::GetCursorScreenPos();
            ImVec2 badgePos = ImVec2(cursorScreen.x + ImGui::GetContentRegionAvail().x - 44.0f * m_dpiScale, cursorScreen.y - 24.0f * m_dpiScale);
            Widgets::RenderMmrDeltaBadge(badgePos, delta, colorWin, colorLoss, colorMuted, fontSmallBold);

            const auto& history = m_snap.showLifetimeGraph ? m_snap.lifetimeMmrY :
                                 (m_snap.playlistHistoryY.count(playlist) ? m_snap.playlistHistoryY.at(playlist) : std::vector<float>{});
            if (history.empty()) {
                ImGui::Dummy(ImVec2(0.0f, 25.0f));
                ImGui::PushFont(fontRegular);
                if (m_snap.showLifetimeGraph) {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "No lifetime MMR history in database yet.");
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Play matches to populate database records!");
                } else {
                    ImGui::TextColored(Format::C(m_frameConfig.themeDim), "No MMR data for %s this session.", playlistUpper.c_str());
                    ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Play a match to start plotting!");
                }
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, 105.0f));
            } else {
                Widgets::MmrGraphParams params{
                    .history = history,
                    .currentMmr = currentMmr,
                    .initialMmr = initialMmr,
                    .plotHeight = 150.0f,
                    .colorWin = colorWin,
                    .colorLoss = colorLoss,
                    .colorMuted = colorMuted,
                    .colorDim = colorDim,
                    .colorText = colorText,
                    .colorGraphLine = colorGraphLine,
                    .colorGraphBaseline = colorGraphBaseline,
                    .fontSmall = fontSmall,
                    .fontSmallBold = fontSmallBold,
                    .fontBold = fontBold
                };
                Widgets::RenderMmrGraph(params);
            }
            break;
        }
        case DashboardLayout::WidgetId::StreaksStats: {
            RenderStreaksStatsTable((std::string("Streak_") + suffix).c_str());
            break;
        }
        case DashboardLayout::WidgetId::GamemodeBreakdown: {
            RenderGamemodeBreakdownTable((std::string("Gamemode_") + suffix).c_str(), ScopeFromConfigString(m_frameConfig.gamemode_breakdown_scope));
            break;
        }
        case DashboardLayout::WidgetId::LobbyRanks: {
            RenderLobbyRanksTable((std::string("LobbyRanks_") + suffix).c_str());
            break;
        }
        case DashboardLayout::WidgetId::DemoTracker: {
            RenderDemoTrackerTable((std::string("DemoTracker_") + suffix).c_str());
            break;
        }
        case DashboardLayout::WidgetId::PreviousGames: {
            ImGui::PushFont(fontSmallBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeAccent), "PREVIOUS GAMES");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::PushFont(fontSmall);
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "last %d saved games", m_frameConfig.previous_games_limit);
            ImGui::PopFont();
            ImGui::Separator();
            ImGui::Spacing();

            const auto& matches = m_snap.recentSavedMatches;
            if (!m_snap.recentSavedMatchesLoaded) {
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Loading saved match history...");
                ImGui::Dummy(ImVec2(0, 6.0f * m_dpiScale));
            } else if (matches.empty()) {
                ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "No saved games in local history.");
                ImGui::Dummy(ImVec2(0, 6.0f * m_dpiScale));
            } else {
                const size_t displayCount = matches.size();
                const bool twoColumns = ImGui::GetContentRegionAvail().x >= 720.0f * m_dpiScale && displayCount > 10;
                const int columnSets = twoColumns ? 2 : 1;
                const int rowsPerColumn = twoColumns ? static_cast<int>((displayCount + 1) / 2) : static_cast<int>(displayCount);
                const int tableColumns = columnSets * 4 + (columnSets - 1);
                std::string tableId = std::string("PreviousGames_") + suffix;

                if (ImGui::BeginTable(tableId.c_str(), tableColumns, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
                    for (int set = 0; set < columnSets; ++set) {
                        if (set > 0) ImGui::TableSetupColumn("##Gap", ImGuiTableColumnFlags_WidthFixed, 14.0f * m_dpiScale);
                        ImGui::TableSetupColumn((std::string("Playlist##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn((std::string("Score##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 52.0f * m_dpiScale);
                        ImGui::TableSetupColumn((std::string("MMR##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 56.0f * m_dpiScale);
                        ImGui::TableSetupColumn((std::string("Time##") + std::to_string(set)).c_str(), ImGuiTableColumnFlags_WidthFixed, 78.0f * m_dpiScale);
                    }
                    ImGui::TableHeadersRow();

                    ImGui::PushFont(fontMono);
                    for (int row = 0; row < rowsPerColumn; ++row) {
                        ImGui::TableNextRow();
                        for (int set = 0; set < columnSets; ++set) {
                            if (set > 0) {
                                ImGui::TableNextColumn();
                                ImGui::TextUnformatted("");
                            }

                            const size_t matchIndex = static_cast<size_t>(set * rowsPerColumn + row);
                            if (matchIndex >= displayCount) {
                                ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                                ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                                ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                                ImGui::TableNextColumn(); ImGui::TextUnformatted("");
                                continue;
                            }

                            const auto& match = matches[matchIndex];
                            const ImVec4 rowColor = Format::C(match.win ? m_frameConfig.themeWin : m_frameConfig.themeLoss);
                            const std::string playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
                            const std::string score = std::to_string(match.ourScore) + "-" + std::to_string(match.theirScore);
                            const std::string time = FormatRelativeMatchTime(match.endedAtUnix);

                            ImGui::TableNextColumn();
                            ImGui::TextColored(rowColor, "%s", playlist.c_str());
                            ImGui::TableNextColumn();
                            ImGui::TextColored(rowColor, "%s", score.c_str());
                            ImGui::TableNextColumn();
                            if (match.mmr > 0) ImGui::TextColored(rowColor, "%d", match.mmr);
                            else ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "--");
                            ImGui::TableNextColumn();
                            ImGui::TextColored(rowColor, "%s", time.c_str());
                        }
                    }
                    ImGui::PopFont();
                    ImGui::EndTable();
                }
            }

            ImGui::Dummy(ImVec2(0, 4.0f * m_dpiScale));
            ImGui::PushFont(fontSmallBold);
            ImGui::TextColored(Format::C(m_frameConfig.themeMuted), "Current session: W:%d L:%d", m_snap.sessionTotals.wins, m_snap.sessionTotals.losses);
            ImGui::PopFont();
            break;
        }
    }
}
