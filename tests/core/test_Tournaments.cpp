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
TEST(TournamentSchedule, SelectsFutureStandardSlotsAndRemindsOnce) {
    std::vector<Tournaments::TournamentSlot> slots{{100, false, "Extra", 2, {}}, {200, true, "Soccar", 3, {}}};
    EXPECT_EQ(Tournaments::NextSlot(slots, 50, false), &slots[0]);
    EXPECT_EQ(Tournaments::NextSlot(slots, 50, true), &slots[1]);
    EXPECT_EQ(Tournaments::NextSlot(slots, 100, false), &slots[1]);
    EXPECT_EQ(Tournaments::NextSlot(slots, 200, false), nullptr);
    Tournaments::ReminderTracker tracker;
    EXPECT_FALSE(tracker.ShouldFire(slots[0], 50, 10, true));
    EXPECT_FALSE(tracker.ShouldFire(slots[1], 50, 0, true));
    EXPECT_TRUE(tracker.ShouldFire(slots[1], 50, 10, true));
    EXPECT_FALSE(tracker.ShouldFire(slots[1], 60, 10, true));
    EXPECT_FALSE(tracker.ShouldFire(slots[1], 200, 10, true));
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

TEST(TournamentSchedule, FetchUsesOmniNetAndParsesSchedule) {
    auto& account = AccountClient::Instance();
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(account.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "tourney-refresh";
        c.account_signed_in_name = "TourneyUser";
        c.account_device_public_id = keyPair.publicId;
    });
    account.SyncFromConfig();
    account.SetClockForTests([]() { return 1700000000; });
    account.SetCachedAccessTokenForTests("tourney-access-token", 1700000900);

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

    Tournaments::TournamentScheduleService tourneyService;
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
    account.ResetForTests();
    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.account_device_public_id.clear();
    });
}
