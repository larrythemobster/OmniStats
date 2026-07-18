#include <gtest/gtest.h>
#include "network/TelemetryManager.hpp"
#include "core/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include <memory>
#include <regex>

TEST(TelemetryManagerTest, GenerateUUIDv4) {
    std::string uuid = TelemetryManager::GenerateUUIDv4();
    EXPECT_EQ(uuid.length(), 36);

    // Format: 8-4-4-4-12
    std::regex uuid_regex("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    EXPECT_TRUE(std::regex_match(uuid, uuid_regex));
}

TEST(TelemetryManagerTest, InitializeDoesNotCrash) {
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    db->Initialize(":memory:");

    EXPECT_NO_THROW(TelemetryManager::Initialize(db));
}
