#include <gtest/gtest.h>
#include "network/RemoteConfig.hpp"
#include "network/AccountClient.hpp"
#include "network/MMRFetcher.hpp"
#include "core/Config.hpp"
#include "core/SessionState.hpp"
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

    class RemoteConfigTestFixture : public ::testing::Test {
      protected:
        fs::path tempDir;

        void SetUp() override {
            RemoteConfig::Instance().ResetForTests();
            AccountClient::Instance().ResetForTests();
            tempDir = fs::temp_directory_path() / "OmniStatsRemoteConfigTests";
            std::error_code ec;
            fs::remove_all(tempDir, ec);
            fs::create_directories(tempDir, ec);
            RemoteConfig::Instance().SetCacheFilePathForTests((tempDir / "cache.json").string());
            RemoteConfig::Instance().SetDismissedFilePathForTests((tempDir / "dismissed.json").string());
        }

        void TearDown() override {
            RemoteConfig::Instance().ResetForTests();
            AccountClient::Instance().ResetForTests();
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        }
    };

} // namespace

TEST_F(RemoteConfigTestFixture, ParseValidConfigAndIgnoreUnknownKeys) {
    const std::string json = R"({
        "schema": 1,
        "ttl_seconds": 1200,
        "unknown_top_level": {"nested": true},
        "announcements": [
            {
                "id": "ann-1",
                "severity": "warning",
                "title": "Maintenance Window",
                "body": "Rank lookups may be intermittent for 15 minutes.",
                "link_url": "https://omnistats.org/status",
                "dismissible": true,
                "ends_at": "2026-12-31T23:59:59Z",
                "extra_ann_key": 42
            }
        ],
        "min_supported_version": "2.2.0",
        "min_version_message": "Please update to continue.",
        "min_version_enforcement": "notice"
    })";

    RemoteConfigData parsed;
    std::string err;
    ASSERT_TRUE(RemoteConfig::ParseConfigJson(json, parsed, &err)) << err;
    EXPECT_EQ(parsed.schema, 1);
    EXPECT_EQ(parsed.ttl_seconds, 1200);
    ASSERT_EQ(parsed.announcements.size(), 1u);
    EXPECT_EQ(parsed.announcements[0].id, "ann-1");
    EXPECT_EQ(parsed.announcements[0].severity, "warning");
    EXPECT_EQ(parsed.announcements[0].title, "Maintenance Window");
    EXPECT_EQ(parsed.announcements[0].link_url, "https://omnistats.org/status");
    EXPECT_TRUE(parsed.announcements[0].dismissible);
    EXPECT_EQ(parsed.min_supported_version, "2.2.0");
    EXPECT_EQ(parsed.min_version_message, "Please update to continue.");
}

TEST_F(RemoteConfigTestFixture, ClampsTtlBounds) {
    RemoteConfigData parsed;
    ASSERT_TRUE(RemoteConfig::ParseConfigJson(R"({"schema":1,"ttl_seconds":10})", parsed));
    EXPECT_EQ(parsed.ttl_seconds, 300);

    ASSERT_TRUE(RemoteConfig::ParseConfigJson(R"({"schema":1,"ttl_seconds":99999})", parsed));
    EXPECT_EQ(parsed.ttl_seconds, 3600);
}

TEST_F(RemoteConfigTestFixture, RejectsOversizedAndWrongTypePayloads) {
    RemoteConfigData parsed;

    // Empty and oversized payloads
    EXPECT_FALSE(RemoteConfig::ParseConfigJson("", parsed));
    const std::string huge(RemoteConfig::kMaxConfigPayloadBytes + 10, ' ');
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(huge, parsed));

    // Wrong schema or type
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(R"({"schema":2})", parsed));
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(R"({"schema":"1"})", parsed));
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(R"({"schema":true})", parsed));

    // Oversized title (>80) or body (>500)
    const std::string longTitle(81, 'A');
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(
        R"({"schema":1,"announcements":[{"id":"a1","severity":"info","title":")" + longTitle + R"(","body":"ok"}]})",
        parsed));

    const std::string longBody(501, 'B');
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(
        R"({"schema":1,"announcements":[{"id":"a1","severity":"info","title":"ok","body":")" + longBody + R"("}]})",
        parsed));

    // Invalid severity or disallowed link_url
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(
        R"({"schema":1,"announcements":[{"id":"a1","severity":"urgent","title":"ok","body":"ok"}]})",
        parsed));
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(
        R"({"schema":1,"announcements":[{"id":"a1","severity":"info","title":"ok","body":"ok","link_url":"https://evil.example/phish"}]})",
        parsed));

    // Oversized min_version_message (>300)
    const std::string longMsg(301, 'M');
    EXPECT_FALSE(RemoteConfig::ParseConfigJson(
        R"({"schema":1,"min_supported_version":"2.2.0","min_version_message":")" + longMsg + R"("})",
        parsed));
}

TEST_F(RemoteConfigTestFixture, AnnouncementFilteringAndDismissalPersistence) {
    int64_t nowUnix = 0;
    ASSERT_TRUE(RemoteConfig::ParseRfc3339Unix("2026-10-08T12:00:00Z", nowUnix));
    RemoteConfig::Instance().SetClockForTests([nowUnix]() { return nowUnix; });

    RemoteConfigData data;
    data.announcements = {
        {"a1", "info", "Active Dismissible", "Body 1", "https://omnistats.org/news", true, "2026-10-09T00:00:00Z"},
        {"a2", "critical", "Non-Dismissible Alert", "Body 2", "https://discord.gg/omnistats", false, ""},
        {"a3", "warning", "Expired Alert", "Body 3", "", true, "2026-10-08T11:59:59Z"}};
    RemoteConfig::Instance().SetSnapshotForTests(data);

    auto active = RemoteConfig::Instance().ActiveAnnouncements();
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0].id, "a1");
    EXPECT_EQ(active[1].id, "a2");

    // Dismissing a1 hides it; dismissing non-dismissible a2 does NOT hide a2
    RemoteConfig::Instance().DismissAnnouncement("a1");
    RemoteConfig::Instance().DismissAnnouncement("a2");
    EXPECT_TRUE(RemoteConfig::Instance().IsAnnouncementDismissed("a1"));

    active = RemoteConfig::Instance().ActiveAnnouncements();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].id, "a2");
}

TEST_F(RemoteConfigTestFixture, MinVersionIsWarningOnly) {
    EXPECT_TRUE(RemoteConfig::IsVersionBelowMinimum("2.1.9", "2.2.0"));
    EXPECT_TRUE(RemoteConfig::IsVersionBelowMinimum("2.2.0", "2.2.1"));
    EXPECT_TRUE(RemoteConfig::IsVersionBelowMinimum("1.9.99", "2.0.0"));
    EXPECT_FALSE(RemoteConfig::IsVersionBelowMinimum("2.2.0", "2.2.0"));
    EXPECT_FALSE(RemoteConfig::IsVersionBelowMinimum("2.2.15", "2.2.2"));
    EXPECT_FALSE(RemoteConfig::IsVersionBelowMinimum("2.2.0", ""));

    RemoteConfigData data;
    data.min_supported_version = "2.3.0";
    data.min_version_message = "Update recommended.";
    RemoteConfig::Instance().SetSnapshotForTests(data);
    EXPECT_TRUE(RemoteConfig::Instance().IsBelowMinSupportedVersion("2.2.15"));
    EXPECT_FALSE(RemoteConfig::Instance().IsBelowMinSupportedVersion("2.3.0"));

    bool transportCalled = false;
    AccountClient::Instance().SetTransportForTests([&](const std::string&, const std::string&) {
        transportCalled = true;
        return AccountClient::HttpResponse{0, 200, "{}"};
    });
    AccountClient::Instance().StartAuthorization();
    EXPECT_TRUE(transportCalled);

    Config::Update([](ConfigData& c) { c.enable_mmr_tracking = true; }, false);
    auto state = std::make_shared<SessionState>();
    MMRFetcher fetcher(state, nullptr);
    fetcher.Enqueue("Steam|76561198000000001|0", "TestPlayer");
    EXPECT_EQ(fetcher.PendingRequestCountForTests(), 1u);
}

TEST_F(RemoteConfigTestFixture, CacheFallbackUnder24hAndDefaultResetAfter24h) {
    int64_t currentTime = 1760000000;
    RemoteConfig::Instance().SetClockForTests([&currentTime]() { return currentTime; });

    RemoteConfig::Instance().SetTransportForTests([](const std::string& url, const std::string&) {
        EXPECT_EQ(url, "https://api.omnistats.org/api/v1/client/config");
        return RemoteConfig::HttpResponse{
            0, 200, R"({"schema":1,"ttl_seconds":600,"min_supported_version":"2.3.0"})"};
    });
    EXPECT_TRUE(RemoteConfig::Instance().FetchOnce());
    EXPECT_EQ(RemoteConfig::Instance().GetSnapshot().min_supported_version, "2.3.0");

    currentTime += 2 * 3600;
    RemoteConfig::Instance().SetTransportForTests([](const std::string&, const std::string&) {
        return RemoteConfig::HttpResponse{7, 0, ""};
    });
    EXPECT_FALSE(RemoteConfig::Instance().FetchOnce());
    EXPECT_EQ(RemoteConfig::Instance().GetSnapshot().min_supported_version, "2.3.0");

    currentTime += 23 * 3600;
    EXPECT_FALSE(RemoteConfig::Instance().FetchOnce());
    EXPECT_TRUE(RemoteConfig::Instance().GetSnapshot().min_supported_version.empty());
}
