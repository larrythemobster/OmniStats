#include <gtest/gtest.h>

#include "core/game/SeasonCalendar.hpp"
#include "network/api/RemoteConfig.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

    int64_t ParseRfc3339(std::string_view rfc3339) {
        int64_t unixTime = 0;
        const bool ok = RemoteConfig::ParseRfc3339Unix(rfc3339, unixTime);
        EXPECT_TRUE(ok) << "Expected valid RFC3339 string: " << rfc3339;
        return unixTime;
    }

    SeasonRecord MakeSeason(int season, std::string_view startStr, std::string_view endStr, std::string source = "rocket_league") {
        return SeasonRecord{
            season,
            ParseRfc3339(startStr),
            ParseRfc3339(endStr),
            std::move(source),
        };
    }

} // namespace

TEST(SeasonCalendarTest, SeasonForTimestampStartInclusive) {
    const auto s22 = MakeSeason(22, "2025-06-01T00:00:00Z", "2025-09-01T00:00:00Z");
    const auto s23 = MakeSeason(23, "2025-09-01T00:00:00Z", "2025-12-01T00:00:00Z");
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T00:00:00Z");

    const SeasonCalendar calendar({s22, s23, s24});

    const auto atS22Start = calendar.SeasonForTimestamp(s22.startsAt);
    ASSERT_TRUE(atS22Start.has_value());
    EXPECT_EQ(atS22Start->season, 22);

    const auto atS23Start = calendar.SeasonForTimestamp(s23.startsAt);
    ASSERT_TRUE(atS23Start.has_value());
    EXPECT_EQ(atS23Start->season, 23);

    const auto atS24Start = calendar.SeasonForTimestamp(s24.startsAt);
    ASSERT_TRUE(atS24Start.has_value());
    EXPECT_EQ(atS24Start->season, 24);
}

TEST(SeasonCalendarTest, SeasonForTimestampNextStartExclusive) {
    const auto s22 = MakeSeason(22, "2025-06-01T00:00:00Z", "2025-09-01T00:00:00Z");
    const auto s23 = MakeSeason(23, "2025-09-01T00:00:00Z", "2025-12-01T00:00:00Z");
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T00:00:00Z");

    const SeasonCalendar calendar({s22, s23, s24});

    const auto justBeforeS23 = calendar.SeasonForTimestamp(s23.startsAt - 1);
    ASSERT_TRUE(justBeforeS23.has_value());
    EXPECT_EQ(justBeforeS23->season, 22);

    const auto atS23 = calendar.SeasonForTimestamp(s23.startsAt);
    ASSERT_TRUE(atS23.has_value());
    EXPECT_EQ(atS23->season, 23);

    const auto inInterSeasonGap = calendar.SeasonForTimestamp(s24.startsAt - 1);
    ASSERT_TRUE(inInterSeasonGap.has_value());
    EXPECT_EQ(inInterSeasonGap->season, 23);

    const auto atS24 = calendar.SeasonForTimestamp(s24.startsAt);
    ASSERT_TRUE(atS24.has_value());
    EXPECT_EQ(atS24->season, 24);
}

TEST(SeasonCalendarTest, SeasonForTimestampFinalEndInclusive) {
    const auto s22 = MakeSeason(22, "2025-06-01T00:00:00Z", "2025-09-01T00:00:00Z");
    const auto s23 = MakeSeason(23, "2025-09-01T00:00:00Z", "2025-12-01T00:00:00Z");
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T00:00:00Z");

    const SeasonCalendar calendar({s22, s23, s24});

    const auto atFinalEnd = calendar.SeasonForTimestamp(s24.endsAt);
    ASSERT_TRUE(atFinalEnd.has_value());
    EXPECT_EQ(atFinalEnd->season, 24);

    const auto pastFinalEnd = calendar.SeasonForTimestamp(s24.endsAt + 1);
    EXPECT_FALSE(pastFinalEnd.has_value());
}

TEST(SeasonCalendarTest, SeasonForTimestampBeforeAndAfterUnknown) {
    const auto s22 = MakeSeason(22, "2025-06-01T00:00:00Z", "2025-09-01T00:00:00Z");
    const auto s23 = MakeSeason(23, "2025-09-01T00:00:00Z", "2025-12-01T00:00:00Z");
    const SeasonCalendar calendar({s22, s23});

    EXPECT_FALSE(calendar.SeasonForTimestamp(s22.startsAt - 1).has_value());
    EXPECT_FALSE(calendar.SeasonForTimestamp(s22.startsAt - 86400).has_value());

    EXPECT_FALSE(calendar.SeasonForTimestamp(s23.endsAt + 1).has_value());
    EXPECT_FALSE(calendar.SeasonForTimestamp(s23.endsAt + 86400).has_value());

    const SeasonCalendar emptyCalendar;
    EXPECT_FALSE(emptyCalendar.SeasonForTimestamp(s22.startsAt).has_value());
}

TEST(SeasonCalendarTest, CurrentActiveState) {
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T12:00:00Z");
    const SeasonCalendar calendar({s24});

    const int64_t now = ParseRfc3339("2026-03-01T00:00:00Z");
    const auto status = calendar.Current(now);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->season.season, 24);
    EXPECT_EQ(status->state, SeasonCalendar::State::Active);
    EXPECT_EQ(status->daysLeft, 31);
    EXPECT_EQ(status->hoursLeft, 12);

    const int64_t nowBoundary = s24.endsAt - (7 * 86400 + 1);
    const auto boundaryStatus = calendar.Current(nowBoundary);
    ASSERT_TRUE(boundaryStatus.has_value());
    EXPECT_EQ(boundaryStatus->state, SeasonCalendar::State::Active);
    EXPECT_EQ(boundaryStatus->daysLeft, 7);
    EXPECT_EQ(boundaryStatus->hoursLeft, 0);
}

TEST(SeasonCalendarTest, CurrentEndingSoonState) {
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T12:00:00Z");
    const SeasonCalendar calendar({s24});

    const int64_t exactlySevenDaysLeft = s24.endsAt - (7 * 86400);
    const auto status7d = calendar.Current(exactlySevenDaysLeft);
    ASSERT_TRUE(status7d.has_value());
    EXPECT_EQ(status7d->season.season, 24);
    EXPECT_EQ(status7d->state, SeasonCalendar::State::EndingSoon);
    EXPECT_EQ(status7d->daysLeft, 7);
    EXPECT_EQ(status7d->hoursLeft, 0);

    const int64_t partialDaysLeft = s24.endsAt - (2 * 86400 + 5 * 3600);
    const auto status2d = calendar.Current(partialDaysLeft);
    ASSERT_TRUE(status2d.has_value());
    EXPECT_EQ(status2d->state, SeasonCalendar::State::EndingSoon);
    EXPECT_EQ(status2d->daysLeft, 2);
    EXPECT_EQ(status2d->hoursLeft, 5);

    const int64_t oneSecondLeft = s24.endsAt - 1;
    const auto status1s = calendar.Current(oneSecondLeft);
    ASSERT_TRUE(status1s.has_value());
    EXPECT_EQ(status1s->state, SeasonCalendar::State::EndingSoon);
    EXPECT_EQ(status1s->daysLeft, 0);
    EXPECT_EQ(status1s->hoursLeft, 0);
}

TEST(SeasonCalendarTest, CurrentEndedAwaitingNextState) {
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T12:00:00Z");
    const SeasonCalendar calendar({s24});

    const auto atEnd = calendar.Current(s24.endsAt);
    ASSERT_TRUE(atEnd.has_value());
    EXPECT_EQ(atEnd->season.season, 24);
    EXPECT_EQ(atEnd->state, SeasonCalendar::State::EndedAwaitingNext);

    const auto afterEnd = calendar.Current(s24.endsAt + 3600);
    ASSERT_TRUE(afterEnd.has_value());
    EXPECT_EQ(afterEnd->season.season, 24);
    EXPECT_EQ(afterEnd->state, SeasonCalendar::State::EndedAwaitingNext);
}

TEST(SeasonCalendarTest, CurrentFutureNextSeason) {
    const auto s24 = MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T00:00:00Z");
    const auto s25 = MakeSeason(25, "2026-04-10T00:00:00Z", "2026-07-01T00:00:00Z");
    const SeasonCalendar calendar({s24, s25});

    const int64_t betweenSeasons = ParseRfc3339("2026-04-05T00:00:00Z");
    const auto betweenStatus = calendar.Current(betweenSeasons);
    ASSERT_TRUE(betweenStatus.has_value());
    EXPECT_EQ(betweenStatus->season.season, 24);
    EXPECT_EQ(betweenStatus->state, SeasonCalendar::State::EndedAwaitingNext);

    const int64_t beforeAll = ParseRfc3339("2025-12-31T23:59:59Z");
    const auto beforeStatus = calendar.Current(beforeAll);
    EXPECT_FALSE(beforeStatus.has_value());

    const SeasonCalendar futureOnly({s25});
    EXPECT_FALSE(futureOnly.Current(betweenSeasons).has_value());

    const auto atS25Start = calendar.Current(s25.startsAt);
    ASSERT_TRUE(atS25Start.has_value());
    EXPECT_EQ(atS25Start->season.season, 25);
    EXPECT_EQ(atS25Start->state, SeasonCalendar::State::Active);
}

TEST(SeasonCalendarTest, MergeRetainsOldSeasonsAndOverwritesMatching) {
    const std::vector<SeasonRecord> stored = {
        MakeSeason(22, "2025-06-01T00:00:00Z", "2025-09-01T00:00:00Z", "database"),
        MakeSeason(23, "2025-09-01T00:00:00Z", "2025-12-01T00:00:00Z", "database"),
        MakeSeason(24, "2026-01-01T00:00:00Z", "2026-03-31T00:00:00Z", "local_cache"),
    };

    RemoteSeasonConfig remote;
    remote.current = 24;
    remote.seasons = {
        RemoteSeasonEntry{
            24,
            "2026-01-01T00:00:00Z",
            "2026-04-08T15:00:00Z",
            "rocket_league",
        },
    };
    remote.next = RemoteSeasonEntry{
        25,
        "2026-04-08T15:00:00Z",
        "2026-07-01T00:00:00Z",
        "rocket_league",
    };

    const auto merged = SeasonCalendar::Merge(stored, remote);
    ASSERT_EQ(merged.size(), 4u);

    EXPECT_EQ(merged[0].season, 25);
    EXPECT_EQ(merged[0].startsAt, ParseRfc3339("2026-04-08T15:00:00Z"));
    EXPECT_EQ(merged[0].endsAt, ParseRfc3339("2026-07-01T00:00:00Z"));
    EXPECT_EQ(merged[0].source, "rocket_league");

    EXPECT_EQ(merged[1].season, 24);
    EXPECT_EQ(merged[1].startsAt, ParseRfc3339("2026-01-01T00:00:00Z"));
    EXPECT_EQ(merged[1].endsAt, ParseRfc3339("2026-04-08T15:00:00Z"));
    EXPECT_EQ(merged[1].source, "rocket_league");

    EXPECT_EQ(merged[2].season, 23);
    EXPECT_EQ(merged[2].startsAt, ParseRfc3339("2025-09-01T00:00:00Z"));
    EXPECT_EQ(merged[2].endsAt, ParseRfc3339("2025-12-01T00:00:00Z"));
    EXPECT_EQ(merged[2].source, "database");

    EXPECT_EQ(merged[3].season, 22);
    EXPECT_EQ(merged[3].startsAt, ParseRfc3339("2025-06-01T00:00:00Z"));
    EXPECT_EQ(merged[3].endsAt, ParseRfc3339("2025-09-01T00:00:00Z"));
    EXPECT_EQ(merged[3].source, "database");
}

TEST(SeasonCalendarTest, MergeIgnoresInvalidRemoteEntries) {
    const std::vector<SeasonRecord> stored = {
        MakeSeason(24, "2026-01-01T00:00:00Z", "2026-04-01T00:00:00Z", "database"),
    };

    RemoteSeasonConfig remote;
    remote.seasons = {
        RemoteSeasonEntry{24, "invalid-start", "2026-04-01T00:00:00Z", "remote"},
        RemoteSeasonEntry{25, "2026-05-01T00:00:00Z", "2026-04-01T00:00:00Z", "remote"}, // startsAt >= endsAt
    };
    remote.next = RemoteSeasonEntry{26, "2026-06-01T00:00:00Z", "not-a-date", "remote"};

    const auto merged = SeasonCalendar::Merge(stored, remote);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].season, 24);
    EXPECT_EQ(merged[0].startsAt, ParseRfc3339("2026-01-01T00:00:00Z"));
    EXPECT_EQ(merged[0].endsAt, ParseRfc3339("2026-04-01T00:00:00Z"));
    EXPECT_EQ(merged[0].source, "database");
}
