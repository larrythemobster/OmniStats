#include <gtest/gtest.h>
#include "core/tournaments/RegionDetector.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "ui/rml/widgets/TournamentWidget.hpp"
#include "network/omninet/OmniNet.hpp"
#include "network/api/AccountClient.hpp"
#include "core/config/Config.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST(TournamentRegion, ReadsRealCacheAndRejectsTruncation) {
    std::ifstream file(std::filesystem::path(OMNISTATS_SOURCE_DIR) / "tests/fixtures/RegionCache.cache", std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    auto detected = Tournaments::ParseRegionCache(bytes);
    ASSERT_TRUE(detected);
    EXPECT_EQ(detected->region, "USW");
    EXPECT_EQ(detected->server, "USW3");
    EXPECT_EQ(detected->pingMs, 61);
    for (size_t length : {size_t(0), size_t(17), bytes.size() - 20})
        EXPECT_FALSE(Tournaments::ParseRegionCache(std::span<const uint8_t>(bytes.data(), length)));
}
TEST(TournamentRegion, IgnoresNonReservationRegions) {
    auto region = Tournaments::ParseMatchRegions("Region=\"EU\"\nReservation=(Region=\"USW\")\nReservation=(Region=\"USE\")\nReservation=(Region=\"USW\")\nReservation=(Region=\"INVALID\")\n");
    ASSERT_TRUE(region);
    EXPECT_EQ(region->region, "USW");
}
TEST(TournamentSchedule, SelectsMainAndSecondChanceInTimeOrder) {
    using namespace Tournaments;
    const std::vector<TournamentSlot> slots{
        {100, false, "Hoops", 2, SecondChance{"Hoops 2nd Chance", 220}},
        {200, true, "Soccar", 3, SecondChance{"Soccar 2nd Chance", 1100}},
    };
    EXPECT_EQ(NextEvent(slots, 50, false).slot, &slots[0]);
    EXPECT_EQ(NextEvent(slots, 50, true).slot, &slots[1]);
    EXPECT_EQ(NextEvent(slots, 100, false).Start(), 200);
    EXPECT_EQ(NextEvent(slots, 200, false).Start(), 220);
    EXPECT_EQ(NextEvent(slots, 200, true).Start(), 1100);
    EXPECT_FALSE(NextEvent(slots, 1100, false));
}

TEST(TournamentSchedule, RemindsOnceForMainAndEligibleSecondChance) {
    using namespace Tournaments;
    const TournamentSlot slot{1000, true, "Soccar", 3, SecondChance{"Soccar 2nd Chance", 1900}};
    const ScheduledEvent main{&slot};
    const ScheduledEvent second{&slot, &*slot.secondChance};
    ReminderTracker tracker;
    EXPECT_FALSE(tracker.ShouldFire(main, 900, 0));
    EXPECT_TRUE(tracker.ShouldFire(main, 900, 10));
    EXPECT_FALSE(tracker.ShouldFire(main, 901, 10));
    EXPECT_FALSE(tracker.ShouldFire(second, 1300, 10, BracketResult::Advanced));
    EXPECT_FALSE(tracker.ShouldFire(second, 1300, 10, BracketResult::Unknown));
    EXPECT_TRUE(tracker.ShouldFire(second, 1301, 10, BracketResult::Lost));
    EXPECT_FALSE(tracker.ShouldFire(second, 1302, 10, BracketResult::Lost));
    EXPECT_FALSE(tracker.ShouldFire(second, 1900, 10, BracketResult::Lost));
}

TEST(TournamentSchedule, SkippedBracketRequiresContinuousObservationAndJoinGrace) {
    using namespace Tournaments;
    const TournamentSlot slot{1000, true, "Soccar", 3, SecondChance{"Soccar 2nd Chance", 1900}};
    const ScheduledEvent second{&slot, &*slot.secondChance};
    ReminderTracker skipped, gap, played;
    for (int64_t now = 990; now <= 1290; now += 30) {
        skipped.Observe(now, true, false, {});
        gap.Observe(now, now != 1110, false, {});
        played.Observe(now, true, now == 1230, "round-one");
    }
    EXPECT_FALSE(skipped.ShouldFire(second, 1299, 15, BracketResult::NoMatch));
    EXPECT_TRUE(skipped.ShouldFire(second, 1300, 15, BracketResult::NoMatch));
    EXPECT_FALSE(gap.ShouldFire(second, 1300, 15, BracketResult::NoMatch));
    EXPECT_FALSE(played.ShouldFire(second, 1300, 15, BracketResult::NoMatch));
    EXPECT_EQ(played.ObservedMatchSince(1000), std::optional<std::string_view>{"round-one"});
}
TEST(TournamentSchedule, ParsesSecondChanceAndSkipsMalformedSlot) {
    Tournaments::ScheduleSnapshot value;
    ASSERT_TRUE(Tournaments::ParseSchedule(R"({"group":"US","slots":[{"start_time":200,"standard":true,"title":"2v2 Soccar","team_size":2,"second_chance":{"title":"2nd Chance","start_time":1100}},{"start_time":100,"standard":false,"title":"Hoops","team_size":2},{"start_time":0,"title":"Invalid"}]})", value));
    ASSERT_EQ(value.slots.size(), 2u);
    EXPECT_EQ(value.slots[0].title, "Hoops");
    ASSERT_TRUE(value.slots[1].secondChance);
    EXPECT_EQ(value.slots[1].secondChance->startUnix, 1100);
    EXPECT_FALSE(Tournaments::ParseSchedule(R"({"group":"BOGUS","slots":[]})", value));
}
TEST(TournamentCountdown, RoundsUpWithoutShowingZeroBeforeStart) {
    EXPECT_EQ(TournamentUi::Countdown(1), "1m");
    EXPECT_EQ(TournamentUi::Countdown(60), "1m");
    EXPECT_EQ(TournamentUi::Countdown(61), "2m");
    EXPECT_EQ(TournamentUi::Countdown(3600), "1h 0m");
}

TEST(SeasonDisplay, FormatsCountdownAndMarksOnlyFinalWeek) {
    constexpr int64_t day = 86400;
    SeasonRecord season{20, 1, 20 * day, "official"};
    const auto normal = TournamentUi::RenderSeasonChip(season, 10 * day);
    EXPECT_NE(normal.find("10d left"), std::string::npos);
    EXPECT_EQ(normal.find("ending-soon"), std::string::npos);
    const auto soon = TournamentUi::RenderSeasonChip(season, 13 * day);
    EXPECT_NE(soon.find("7d left"), std::string::npos);
    EXPECT_NE(soon.find("ending-soon"), std::string::npos);
    EXPECT_NE(TournamentUi::RenderSeasonChip(season, 20 * day - 1).find("1h left"), std::string::npos);
    EXPECT_NE(TournamentUi::RenderSeasonChip(season, 20 * day).find("ended"), std::string::npos);
    EXPECT_NE(TournamentUi::RenderSeasonChip({21, 20 * day, 20 * day, "observed"}, 21 * day).find("end date unknown"), std::string::npos);
}

TEST(SeasonDisplay, ReminderRespectsDayMatchAndRewardProgress) {
    constexpr int64_t day = 86400;
    const SeasonRecord season{20, 1, 10 * day, "official"};
    EXPECT_FALSE(TournamentUi::ShouldRemindSeason(season.endsAt, 2 * day, 7, false, "today", ""));
    EXPECT_FALSE(TournamentUi::ShouldRemindSeason(season.endsAt, 4 * day, 7, true, "today", ""));
    EXPECT_FALSE(TournamentUi::ShouldRemindSeason(season.endsAt, 4 * day, 7, false, "today", "today"));
    EXPECT_TRUE(TournamentUi::ShouldRemindSeason(season.endsAt, 4 * day, 7, false, "today", "yesterday"));
    EXPECT_FALSE(TournamentUi::ShouldRemindSeason(season.endsAt, 10 * day, 7, false, "today", ""));
    EXPECT_NE(TournamentUi::SeasonReminderText(season, 4 * day, 3, 9, true).find("1 win left"), std::string::npos);
    EXPECT_EQ(TournamentUi::SeasonReminderText(season, 4 * day, 8, 9, true).find("win left"), std::string::npos);
    EXPECT_EQ(TournamentUi::SeasonReminderText(season, 4 * day, 3, 9, false).find("win left"), std::string::npos);
    EXPECT_FALSE(TournamentUi::ShouldShowSeasonRecap(0, 20, true, false));
    EXPECT_FALSE(TournamentUi::ShouldShowSeasonRecap(19, 20, true, true));
    EXPECT_FALSE(TournamentUi::ShouldShowSeasonRecap(19, 20, false, false));
    EXPECT_TRUE(TournamentUi::ShouldShowSeasonRecap(19, 20, true, false));
}

TEST(TournamentSchedule, FetchUsesOmniNetAndParsesSchedule) {
    auto account = std::make_shared<AccountClient>();
    Tournaments::TournamentScheduleService tourneyService{false};
    tourneyService.SetAccountClient(account);
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(account->EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "tourney-refresh";
        c.account_signed_in_name = "TourneyUser";
        c.account_device_public_id = keyPair.publicId;
    });
    account->SyncFromConfig();
    account->SetClockForTests([]() { return 1700000000; });
    account->SetCachedAccessTokenForTests("tourney-access-token", 1700000900);

    OmniNet::Request capturedReq;
    bool transportCalled = false;
    OmniNet::SetTransportForTesting([&](const OmniNet::Request& req) {
        transportCalled = true;
        capturedReq = req;
        OmniNet::Response res;
        res.status = 200;
        res.body = R"({"group":"US","slots":[{"start_time":1700003600,"standard":true,"title":"3v3 Soccar","team_size":3}]})";
        return res;
    });

    Tournaments::ScheduleSnapshot snap;
    snap.region = "USE";
    EXPECT_TRUE(tourneyService.FetchForTests(snap));
    EXPECT_TRUE(transportCalled);
    EXPECT_EQ(capturedReq.op, OmniNet::Op::TournamentSchedule);
    EXPECT_EQ(capturedReq.accessToken, "tourney-access-token");
    EXPECT_EQ(capturedReq.devicePublicId, keyPair.publicId);
    EXPECT_EQ(capturedReq.query["region"], "USE");
    ASSERT_EQ(snap.slots.size(), 1u);
    EXPECT_EQ(snap.slots[0].title, "3v3 Soccar");

    // On updateRequired, returns false (Unavailable)
    OmniNet::SetTransportForTesting([](const OmniNet::Request&) {
        OmniNet::Response res;
        res.status = 426;
        res.updateRequired = true;
        return res;
    });
    Tournaments::ScheduleSnapshot snapUpdate;
    snapUpdate.region = "USE";
    EXPECT_FALSE(tourneyService.FetchForTests(snapUpdate));

    OmniNet::SetTransportForTesting({});
    account->ResetForTests();
    tourneyService.SetAccountClient(nullptr);
    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.account_device_public_id.clear();
    });
}
