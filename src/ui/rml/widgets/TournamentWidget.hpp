#pragma once
#include "core/tournaments/TournamentSchedule.hpp"
#include "database/DatabaseTypes.hpp"
class TelemetryManager;

#include <memory>
#include <string>
#include <optional>

namespace TournamentUi {
    std::string Countdown(int64_t seconds);
    std::string RenderWidget(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, TelemetryManager* telemetry = nullptr);
    std::string RenderTopbar(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, bool standardOnly, int reminderMinutes, TelemetryManager* telemetry = nullptr);
    std::optional<SeasonRecord> CurrentSeason(const std::shared_ptr<const SeasonSummarySnapshot>& snapshot, int64_t now);
    std::string RenderSeasonChip(const SeasonRecord& season, int64_t now);
    std::string SeasonReminderText(const SeasonRecord& season, int64_t now, int rewardLevel, int rewardWins, bool rewardReminder);
    bool ShouldRemindSeason(int64_t endUnix, int64_t now, int reminderDays, bool inMatch,
                            const std::string& today, const std::string& lastReminderDay);
    bool ShouldShowSeasonRecap(int previousSeason, int currentSeason, bool enabled, bool inMatch);
}
