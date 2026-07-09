#pragma once
#include <string>
#include <vector>

namespace DashboardLayout {

constexpr int kLayoutVersion = 3;

enum class WidgetId : int {
    LiveRoster = 0,
    LiveMatchStats = 1,
    SessionStats = 2,
    MmrGraph = 3,
    StreaksStats = 4,
    GamemodeBreakdown = 5,
    LobbyRanks = 6,
    DemoTracker = 7,
    PreviousGames = 8
};

enum class Zone : int {
    Left = 0,
    Right = 1,
    Bottom = 2,
    Hidden = 3,
    Top = 4
};

struct WidgetPlacement {
    WidgetId id = WidgetId::LiveRoster;
    Zone zone = Zone::Left;
    int order = 0;
    float height = 0.0f; // 0 means auto
    bool collapsed = false;
};

struct LayoutConfig {
    int version = kLayoutVersion;
    float leftColumnWeight = 0.48f;
    std::vector<WidgetPlacement> widgets;
};

LayoutConfig DefaultLayout();
void Sanitize(LayoutConfig& layout);

const char* ToConfigString(WidgetId id);
const char* ToConfigString(Zone zone);
WidgetId WidgetIdFromConfigString(const std::string& value);
Zone ZoneFromConfigString(const std::string& value);

const char* GetWidgetDisplayName(WidgetId id);

} // namespace DashboardLayout
