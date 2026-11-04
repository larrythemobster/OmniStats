#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct RemoteSeasonConfig;

struct SeasonRecord {
    int season = 0;
    int64_t startsAt = 0;
    int64_t endsAt = 0;
    std::string source;
    bool operator==(const SeasonRecord&) const = default;
};

class SeasonCalendar {
  public:
    enum class State { Active,
                       EndingSoon,
                       EndedAwaitingNext };
    struct Status {
        SeasonRecord season;
        State state = State::Active;
        int64_t daysLeft = 0;
        int64_t hoursLeft = 0;
    };

    explicit SeasonCalendar(std::vector<SeasonRecord> seasons = {});
    static std::vector<SeasonRecord> Merge(std::vector<SeasonRecord> stored, const RemoteSeasonConfig& remote);
    std::optional<SeasonRecord> SeasonForTimestamp(int64_t timestamp) const;
    std::optional<Status> Current(int64_t now) const;
    const std::vector<SeasonRecord>& Seasons() const noexcept {
        return m_seasons;
    }

  private:
    std::vector<SeasonRecord> m_seasons;
};
