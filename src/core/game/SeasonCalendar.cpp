#include "core/game/SeasonCalendar.hpp"

#include "network/api/RemoteConfig.hpp"
#include <algorithm>
#include <utility>

SeasonCalendar::SeasonCalendar(std::vector<SeasonRecord> seasons) : m_seasons(std::move(seasons)) {
    std::sort(m_seasons.begin(), m_seasons.end(), [](const auto& a, const auto& b) { return a.startsAt > b.startsAt; });
}

std::vector<SeasonRecord> SeasonCalendar::Merge(std::vector<SeasonRecord> stored, const RemoteSeasonConfig& remote) {
    auto merge = [&stored](const RemoteSeasonEntry& entry) {
        SeasonRecord record{entry.season, 0, 0, entry.source};
        if (!RemoteConfig::ParseRfc3339Unix(entry.starts_at, record.startsAt) ||
            !RemoteConfig::ParseRfc3339Unix(entry.ends_at, record.endsAt) || record.startsAt >= record.endsAt) return;
        const auto it = std::find_if(stored.begin(), stored.end(), [&](const auto& existing) { return existing.season == record.season; });
        if (it == stored.end())
            stored.push_back(std::move(record));
        else
            *it = std::move(record);
    };
    for (const auto& entry : remote.seasons)
        merge(entry);
    if (remote.next) merge(*remote.next);
    std::sort(stored.begin(), stored.end(), [](const auto& a, const auto& b) { return a.startsAt > b.startsAt; });
    return stored;
}

std::optional<SeasonRecord> SeasonCalendar::SeasonForTimestamp(int64_t timestamp) const {
    for (size_t i = 0; i < m_seasons.size(); ++i) {
        const auto& season = m_seasons[i];
        if (timestamp < season.startsAt) continue;
        if (i == 0 ? timestamp <= season.endsAt : timestamp < m_seasons[i - 1].startsAt) return season;
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<SeasonCalendar::Status> SeasonCalendar::Current(int64_t now) const {
    for (const auto& season : m_seasons) {
        if (season.startsAt > now) continue;
        Status status;
        status.season = season;
        if (now >= season.endsAt) {
            status.state = State::EndedAwaitingNext;
        } else {
            const int64_t remaining = season.endsAt - now;
            status.daysLeft = remaining / 86400;
            status.hoursLeft = (remaining % 86400) / 3600;
            status.state = remaining <= 7 * 86400 ? State::EndingSoon : State::Active;
        }
        return status;
    }
    return std::nullopt;
}
