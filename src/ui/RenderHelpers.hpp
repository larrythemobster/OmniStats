#pragma once
#include <string>
#include <vector>
#include <memory>
#include "imgui.h"

struct ConfigData;
struct RenderSnapshot;
namespace std { class mutex; }
struct CachedDbStats;

struct MmrDeltaResult {
    int currentMmr = 0;
    int initialMmr = 0;
    int delta = 0;
};

struct RenderFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* small = nullptr;
    ImFont* smallBold = nullptr;
    ImFont* mono = nullptr;
};

namespace RenderHelper {

void Record(int winsWith, int lossesWith, int winsAgainst, int lossesAgainst,
            const ConfigData& cfg, const RenderFonts& fonts);

void PlayerRoster(int teamNum, const char* label, ImColor color, const char* tableId,
                  const RenderSnapshot& snap, const ConfigData& cfg,
                  const std::string& activeMmrCategory, float dpiScale,
                  const RenderFonts& fonts);

void StatSection(const std::string& title, const std::string& tableId,
                 const std::vector<std::pair<std::string, std::string>>& rows,
                 const ConfigData& cfg, const RenderFonts& fonts);

void LiveMatchStats(const char* tableIdPrefix,
                    const RenderSnapshot& snap, const ConfigData& cfg,
                    const RenderFonts& fonts);

void StreaksStatsTable(const char* tableId, const CachedDbStats& dbStats,
                       const ConfigData& cfg, const RenderFonts& fonts);

void GamemodeBreakdownTable(const char* tableId, const CachedDbStats& dbStats,
                            const ConfigData& cfg);

void SessionStatsTable(const char* tableId, bool isDashboard, bool showStreak,
                       const RenderSnapshot& snap, const ConfigData& cfg,
                       const RenderFonts& fonts);

MmrDeltaResult ComputeMmrDelta(const RenderSnapshot& snap, const std::string& playlist);

} // namespace RenderHelper


