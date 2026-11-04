#include <ctime>
#include <algorithm>
#include <sstream>
#include "ui/rml/widgets/TournamentWidget.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/telemetry/TelemetryManager.hpp"

namespace TournamentUi {
    namespace {
        std::string LocalTime(int64_t unixTime, bool date = false) {
            const auto time = static_cast<time_t>(unixTime);
            tm local{};
            if (localtime_s(&local, &time) != 0) return {};
            char buffer[64]{};
            std::strftime(buffer, sizeof(buffer), date ? "%a %I:%M %p" : "%I:%M %p", &local);
            std::string value(buffer);
            if (!date && !value.empty() && value[0] == '0') value.erase(0, 1);
            return value;
        }
    }
    std::string Countdown(int64_t seconds) {
        if (seconds <= 0) return "now";
        const auto minutes = (seconds + 59) / 60;
        if (minutes < 60) return std::to_string(minutes) + "m";
        return std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
    }
    std::string RenderWidget(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, TelemetryManager* telemetry) {
        using RmlUiDetail::Escape;
        using Tournaments::ScheduleStatus;
        switch (snapshot.status) {
        case ScheduleStatus::SignedOut:
            return "<span class='label'>Sign in to OmniStats to see tournaments.</span>";
        case ScheduleStatus::Disabled:
            return {};
        case ScheduleStatus::NoRegion:
            return "<span class='label'>Tournament region not detected.</span>";
        case ScheduleStatus::Loading:
            if (snapshot.slots.empty()) return "<span class='label'>Loading tournament...</span>";
            break;
        case ScheduleStatus::Unavailable:
            return "<span class='label'>Tournament schedule unavailable.</span>";
        case ScheduleStatus::Ready:
            break;
        }
        const auto* next = Tournaments::NextSlot(snapshot.slots, now, false);
        if (!next) return "<span class='label'>No upcoming tournaments.</span>";
        if (telemetry) telemetry->RecordMilestone(TelemetryManager::Milestone::TournamentsView);
        return "<div class='row tournament-countdown'><span class='tournament-name grow'>" + Escape(next->title) +
               "</span><span class='tournament-region label'>" + Escape(snapshot.group) +
               "</span><span class='mono accent tournament-remaining'>" + Countdown(next->startUnix - now) + "</span></div>";
    }

    std::string RenderTopbar(const Tournaments::ScheduleSnapshot& snapshot, int64_t now, bool standardOnly, int reminderMinutes, TelemetryManager* telemetry) {
        using RmlUiDetail::Escape;
        if (snapshot.status != Tournaments::ScheduleStatus::Ready && snapshot.status != Tournaments::ScheduleStatus::Loading) return {};
        const auto* next = Tournaments::NextSlot(snapshot.slots, now, standardOnly);
        if (!next) return {};
        if (telemetry) telemetry->RecordMilestone(TelemetryManager::Milestone::TournamentsView);
        const bool soon = reminderMinutes > 0 && next->startUnix - now <= static_cast<int64_t>(reminderMinutes) * 60;
        std::ostringstream out;
        out << "<div class='tournament-topbar tooltip-host" << (soon ? " starting-soon" : "") << "'>"
            << "<span class='tournament-topbar-title'>" << Escape(next->title) << "</span>"
            << "<span class='mono accent tournament-remaining'>" << Countdown(next->startUnix - now) << "</span><span class='tooltip-bubble'>";
        size_t count = 0;
        for (const auto& slot : snapshot.slots) {
            if (slot.startUnix <= now) continue;
            if (count++ == 5) break;
            out << Escape(LocalTime(slot.startUnix, true)) << " · " << Escape(slot.title) << "<br/>";
        }
        out << "</span></div>";
        return out.str();
    }
    std::optional<SeasonRecord> CurrentSeason(const std::shared_ptr<const SeasonSummarySnapshot>& snapshot, int64_t now) {
        if (!snapshot || !snapshot->loaded) return std::nullopt;
        for (const auto& summary : snapshot->seasons) {
            if (summary.season.startsAt <= now) return summary.season;
        }
        return std::nullopt;
    }

    std::string RenderSeasonChip(const SeasonRecord& season, int64_t now) {
        if (season.season <= 0 || season.startsAt > now) return {};
        const bool knownEnd = season.endsAt > season.startsAt;
        const int64_t remaining = season.endsAt - now;
        const bool soon = knownEnd && remaining > 0 && remaining <= 7 * 86400;
        std::string label = "Season " + std::to_string(season.season) + " · ";
        if (!knownEnd)
            label += "end date unknown";
        else if (remaining <= 0)
            label += "ended";
        else if (remaining < 86400)
            label += std::to_string((remaining + 3599) / 3600) + "h left";
        else
            label += std::to_string((remaining + 86399) / 86400) + "d left";
        return "<span class='season-chip" + std::string(soon ? " ending-soon" : "") + "'>" + label + "</span>";
    }

    bool ShouldRemindSeason(int64_t endUnix, int64_t now, int reminderDays, bool inMatch,
                            const std::string& today, const std::string& lastReminderDay) {
        return reminderDays > 0 && !inMatch && !today.empty() && today != lastReminderDay &&
               endUnix > now && endUnix - now <= static_cast<int64_t>(reminderDays) * 86400;
    }

    std::string SeasonReminderText(const SeasonRecord& season, int64_t now, int rewardLevel, int rewardWins, bool rewardReminder) {
        const int64_t remaining = season.endsAt - now;
        std::string text = "Season " + std::to_string(season.season) + " ends in ";
        if (remaining < 86400)
            text += std::to_string(std::max<int64_t>(1, (remaining + 3599) / 3600)) + "h";
        else
            text += std::to_string((remaining + 86399) / 86400) + "d";
        if (rewardReminder && rewardLevel >= 0 && rewardLevel < 8 && rewardWins >= 0 && rewardWins < 10) {
            const int wins = 10 - rewardWins;
            text += ". " + std::to_string(wins) + (wins == 1 ? " win" : " wins") + " left for your next reward.";
        } else {
            text += ".";
        }
        return text;
    }

    bool ShouldShowSeasonRecap(int previousSeason, int currentSeason, bool enabled, bool inMatch) {
        return enabled && !inMatch && previousSeason > 0 && currentSeason > previousSeason;
    }
}
