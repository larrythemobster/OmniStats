#include <gtest/gtest.h>
#include "network/telemetry/TelemetryManager.hpp"
#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"
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

TEST(TelemetryManagerTest, MilestoneDeduplicationAndValidation) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    TelemetryManager::Initialize(db);

    auto j = PendingAfterDrain(*db, "milestone_barrier_1");
    ASSERT_TRUE(j.is_array());
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0]["kind"], "onboarding");
    EXPECT_EQ(j[0]["name"], "first_launch");

    TelemetryManager::RecordMilestone(TelemetryManager::Milestone::OverlayToggle);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_2").size(), 2u);

    TelemetryManager::RecordMilestone(TelemetryManager::Milestone::OverlayToggle);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_3").size(), 2u);

    TelemetryManager::RecordMilestone(TelemetryManager::Milestone::OnboardingCompleted);
    EXPECT_EQ(PendingAfterDrain(*db, "milestone_barrier_4").size(), 3u);

    TelemetryManager::Shutdown();
}
