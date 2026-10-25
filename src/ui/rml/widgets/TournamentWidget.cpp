#include <ctime>
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
}
