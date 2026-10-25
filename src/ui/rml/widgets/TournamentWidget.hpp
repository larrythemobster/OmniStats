#pragma once
#include "core/tournaments/TournamentSchedule.hpp"
class TelemetryManager;

#include <string>

namespace TournamentUi {
    std::string Countdown(int64_t seconds);
    std::string RenderWidget(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, TelemetryManager* telemetry = nullptr);
    std::string RenderTopbar(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, bool standardOnly, int reminderMinutes, TelemetryManager* telemetry = nullptr);
}
