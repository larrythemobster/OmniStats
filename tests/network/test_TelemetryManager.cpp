#include <gtest/gtest.h>
#include "network/telemetry/TelemetryManager.hpp"
#include "core/config/AppVersion.hpp"
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include "network/omninet/OmniNet.hpp"
#include <memory>
#include <nlohmann/json.hpp>
#include <regex>
#include <thread>
namespace {
    nlohmann::json PendingAfterDrain(DatabaseManager& db, const std::string& barrier) {
        db.AsyncSetSetting(barrier, "done");
        for (int i = 0; i < 400 && db.GetSetting(barrier, "") != "done"; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return nlohmann::json::parse(db.GetSetting("milestones_pending", "[]"));
    }
}

TEST(TelemetryManagerTest, GenerateUUIDv4) {
    std::string uuid = TelemetryManager::GenerateUUIDv4();
    EXPECT_EQ(uuid.length(), 36);

    // Format: 8-4-4-4-12
    std::regex uuid_regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    EXPECT_TRUE(std::regex_match(uuid, uuid_regex));
}

TEST(TelemetryManagerTest, UninitializedShutdownDoesNotSendHeartbeat) {
    const auto original = Config::Read().client_uuid;
    Config::Update([](ConfigData& c) { c.client_uuid = "existing-client"; }, false);
    int requests = 0;
    OmniNet::SetTransportForTesting([&](const OmniNet::Request&) {
        ++requests;
        return OmniNet::Response{};
    });
    {
        TelemetryManager tm;
        tm.Shutdown();
    }
    OmniNet::SetTransportForTesting({});
    Config::Update([&](ConfigData& c) { c.client_uuid = original; }, false);
    EXPECT_EQ(requests, 0);
}

TEST(TelemetryManagerTest, MilestoneDeduplicationAndValidation) {
    Config::Update([](ConfigData& c) { c.client_uuid.clear(); }, false);
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    TelemetryManager tm(db);

    auto j = PendingAfterDrain(*db, "milestone_barrier_1");
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["kind"], "onboarding");
    EXPECT_EQ(j[0]["name"], "first_launch");

    tm.RecordMilestone(TelemetryManager::Milestone::OverlayToggle);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_2").size(), 2u);

    tm.RecordMilestone(TelemetryManager::Milestone::OverlayToggle);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_3").size(), 2u);

    tm.RecordMilestone(TelemetryManager::Milestone::OnboardingCompleted);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_4").size(), 3u);

    tm.Shutdown();
}
TEST(TelemetryManagerTest, FirstLaunchDecisionFreshVsExistingUUID) {
    {
        Config::Update([](ConfigData& c) { c.client_uuid.clear(); }, false);
        auto state = std::make_shared<SessionState>();
        auto db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));

        TelemetryManager tm(db);

        auto j = PendingAfterDrain(*db, "barrier_fresh");
        ASSERT_TRUE(j.is_array());
        ASSERT_EQ(j.size(), 1u);
        EXPECT_EQ(j[0]["kind"], "onboarding");
        EXPECT_EQ(j[0]["name"], "first_launch");
        EXPECT_EQ(db->GetSetting("last_run_version"), AppVersion::Current);
        EXPECT_FALSE(db->GetSetting("client_uuid").empty());

        tm.Shutdown();
    }

    {
        Config::Update([](ConfigData& c) { c.client_uuid.clear(); }, false);
        auto state = std::make_shared<SessionState>();
        auto db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
        const std::string existingUuid = "11111111-2222-4333-8444-555555555555";
        ASSERT_TRUE(db->SetSetting("client_uuid", existingUuid));

        TelemetryManager tm(db);

        auto j = PendingAfterDrain(*db, "barrier_existing_db");
        ASSERT_TRUE(j.is_array());
        EXPECT_TRUE(j.empty());
        EXPECT_EQ(db->GetSetting("last_run_version"), AppVersion::Current);
        EXPECT_EQ(db->GetSetting("client_uuid"), existingUuid);

        tm.Shutdown();
    }

    {
        const std::string existingUuid = "22222222-3333-4444-8555-666666666666";
        Config::Update([&](ConfigData& c) { c.client_uuid = existingUuid; }, false);
        auto state = std::make_shared<SessionState>();
        auto db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));

        TelemetryManager tm(db);

        auto j = PendingAfterDrain(*db, "barrier_existing_conf");
        ASSERT_TRUE(j.is_array());
        EXPECT_TRUE(j.empty());
        EXPECT_EQ(db->GetSetting("last_run_version"), AppVersion::Current);
        EXPECT_EQ(db->GetSetting("client_uuid"), existingUuid);

        tm.Shutdown();
    }
}

TEST(TelemetryManagerTest, OpenMinutesAccounting) {
    using TelemetryDetail::CalculateHeartbeatMinutes;
    using TelemetryDetail::k100nsPerMinute;

    {
        const auto res = CalculateHeartbeatMinutes(0, 0);
        EXPECT_EQ(res.open_minutes, 0);
        EXPECT_EQ(res.remainder_100ns, 0u);
    }

    {
        const uint64_t fortyFiveSeconds = 45ULL * 10'000'000ULL;
        const auto res = CalculateHeartbeatMinutes(fortyFiveSeconds, 0);
        EXPECT_EQ(res.open_minutes, 0);
        EXPECT_EQ(res.remainder_100ns, fortyFiveSeconds);
    }

    {
        const uint64_t fortySeconds = 40ULL * 10'000'000ULL;
        const auto step1 = CalculateHeartbeatMinutes(fortySeconds, 0);
        EXPECT_EQ(step1.open_minutes, 0);
        EXPECT_EQ(step1.remainder_100ns, fortySeconds);

        const uint64_t thirtyFiveSeconds = 35ULL * 10'000'000ULL;
        const auto step2 = CalculateHeartbeatMinutes(thirtyFiveSeconds, step1.remainder_100ns);
        EXPECT_EQ(step2.open_minutes, 1);
        EXPECT_EQ(step2.remainder_100ns, 15ULL * 10'000'000ULL);

        const uint64_t fortyFiveSeconds = 45ULL * 10'000'000ULL;
        const auto step3 = CalculateHeartbeatMinutes(fortyFiveSeconds, step2.remainder_100ns);
        EXPECT_EQ(step3.open_minutes, 1);
        EXPECT_EQ(step3.remainder_100ns, 0u);
    }

    {
        const auto res = CalculateHeartbeatMinutes(60ULL * k100nsPerMinute, 0);
        EXPECT_EQ(res.open_minutes, 60);
        EXPECT_EQ(res.remainder_100ns, 0u);
    }

    {
        const uint64_t twentySeconds = 20ULL * 10'000'000ULL;
        const auto res1439 = CalculateHeartbeatMinutes(1439ULL * k100nsPerMinute + twentySeconds, 0);
        EXPECT_EQ(res1439.open_minutes, 1439);
        EXPECT_EQ(res1439.remainder_100ns, twentySeconds);

        const auto res1440 = CalculateHeartbeatMinutes(1440ULL * k100nsPerMinute, 0);
        EXPECT_EQ(res1440.open_minutes, 1440);
        EXPECT_EQ(res1440.remainder_100ns, 0u);

        const auto res1441 = CalculateHeartbeatMinutes(1441ULL * k100nsPerMinute + twentySeconds, 0);
        EXPECT_EQ(res1441.open_minutes, 1440);
        EXPECT_EQ(res1441.remainder_100ns, twentySeconds);

        const auto resMax = CalculateHeartbeatMinutes(5000ULL * k100nsPerMinute + 42ULL, 0);
        EXPECT_EQ(resMax.open_minutes, 1440);
        EXPECT_EQ(resMax.remainder_100ns, 42u);
    }
}
