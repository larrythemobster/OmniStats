#include <gtest/gtest.h>
#include "network/api/RemoteConfig.hpp"
#include "network/api/AccountClient.hpp"
#include "network/mmr/MmrService.hpp"
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "network/omninet/OmniNet.hpp"
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

    class RemoteConfigTestFixture : public ::testing::Test {
      protected:
        fs::path tempDir;
        RemoteConfig remoteConfig{false};

        void SetUp() override {
            remoteConfig.ResetForTests();
            tempDir = fs::temp_directory_path() / "OmniStatsRemoteConfigTests";
            std::error_code ec;
            fs::remove_all(tempDir, ec);
            fs::create_directories(tempDir, ec);
            remoteConfig.SetCacheFilePathForTests((tempDir / "cache.json").string());
            remoteConfig.SetDismissedFilePathForTests((tempDir / "dismissed.json").string());
        }

        void TearDown() override {
            remoteConfig.ResetForTests();
            OmniNet::SetTransportForTesting({});
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
    remoteConfig.SetClockForTests([nowUnix]() { return nowUnix; });

    RemoteConfigData data;
    data.announcements = {
        {"a1", "info", "Active Dismissible", "Body 1", "https://omnistats.org/news", true, "2026-10-09T00:00:00Z"},
        {"a2", "critical", "Non-Dismissible Alert", "Body 2", "https://discord.gg/omnistats", false, ""},
        {"a3", "warning", "Expired Alert", "Body 3", "", true, "2026-10-08T11:59:59Z"}};
    remoteConfig.SetSnapshotForTests(data);

    auto active = remoteConfig.ActiveAnnouncements();
    ASSERT_EQ(active.size(), 2u);
    EXPECT_EQ(active[0].id, "a1");
    EXPECT_EQ(active[1].id, "a2");

    // Dismissing a1 hides it; dismissing non-dismissible a2 does NOT hide a2
    remoteConfig.DismissAnnouncement("a1");
    remoteConfig.DismissAnnouncement("a2");
    EXPECT_TRUE(remoteConfig.IsAnnouncementDismissed("a1"));

    active = remoteConfig.ActiveAnnouncements();
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
    remoteConfig.SetSnapshotForTests(data);
    EXPECT_TRUE(remoteConfig.IsBelowMinSupportedVersion("2.2.15"));
    EXPECT_FALSE(remoteConfig.IsBelowMinSupportedVersion("2.3.0"));

    bool transportCalled = false;
    OmniNet::SetTransportForTesting([&](const OmniNet::Request& req) {
        transportCalled = true;
        EXPECT_EQ(req.op, OmniNet::Op::AuthStart);
        OmniNet::Response res;
        res.status = 200;
        res.body = "{}";
        return res;
    });
    AccountClient account;
    account.StartAuthorization();
    EXPECT_TRUE(transportCalled);
    OmniNet::SetTransportForTesting({});
    Config::Update([](ConfigData& c) { c.enable_mmr_tracking = true; }, false);
    auto state = std::make_shared<SessionState>();
    MmrService fetcher(state, nullptr);
    fetcher.Enqueue("Steam|76561198000000001|0", "TestPlayer");
    EXPECT_EQ(fetcher.PendingRequestCount(), 1u);
}

TEST_F(RemoteConfigTestFixture, CacheFallbackUnder24hAndDefaultResetAfter24h) {
    int64_t currentTime = 1760000000;
    remoteConfig.SetClockForTests([&currentTime]() { return currentTime; });

    OmniNet::SetTransportForTesting([](const OmniNet::Request& req) {
        EXPECT_EQ(req.op, OmniNet::Op::ClientConfig);
        OmniNet::Response res;
        res.status = 200;
        res.body = R"({"schema":1,"ttl_seconds":600,"min_supported_version":"2.3.0"})";
        return res;
    });
    EXPECT_TRUE(remoteConfig.FetchOnce());
    EXPECT_EQ(remoteConfig.GetSnapshot().min_supported_version, "2.3.0");

    currentTime += 2 * 3600;
    OmniNet::SetTransportForTesting([](const OmniNet::Request&) {
        OmniNet::Response res;
        res.transportError = 7;
        return res;
    });
    EXPECT_FALSE(remoteConfig.FetchOnce());
    EXPECT_EQ(remoteConfig.GetSnapshot().min_supported_version, "2.3.0");

    currentTime += 23 * 3600;
    EXPECT_FALSE(remoteConfig.FetchOnce());
    EXPECT_TRUE(remoteConfig.GetSnapshot().min_supported_version.empty());
}

TEST_F(RemoteConfigTestFixture, SeasonConfigParseAndSerializeRoundTripContract) {
    const std::string contractJson = R"({
        "schema": 1,
        "ttl_seconds": 900,
        "season": {
            "current": 24,
            "seasons": [
                {
                    "season": 24,
                    "starts_at": "2026-03-10T15:00:00Z",
                    "ends_at": "2026-06-16T15:00:00Z",
                    "source": "rocket_league"
                }
            ],
            "next": {
                "season": 25,
                "starts_at": "2026-06-16T15:00:00Z",
                "ends_at": "2026-09-15T15:00:00Z"
            }
        }
    })";

    RemoteConfigData parsed;
    std::string err;
    ASSERT_TRUE(RemoteConfig::ParseConfigJson(contractJson, parsed, &err)) << err;
    EXPECT_EQ(parsed.schema, 1);
    EXPECT_EQ(parsed.ttl_seconds, 900);
    ASSERT_TRUE(parsed.season.has_value());
    EXPECT_EQ(parsed.season->current, 24);

    ASSERT_EQ(parsed.season->seasons.size(), 1u);
    EXPECT_EQ(parsed.season->seasons[0].season, 24);
    EXPECT_EQ(parsed.season->seasons[0].starts_at, "2026-03-10T15:00:00Z");
    EXPECT_EQ(parsed.season->seasons[0].ends_at, "2026-06-16T15:00:00Z");
    EXPECT_EQ(parsed.season->seasons[0].source, "rocket_league");

    ASSERT_TRUE(parsed.season->next.has_value());
    EXPECT_EQ(parsed.season->next->season, 25);
    EXPECT_EQ(parsed.season->next->starts_at, "2026-06-16T15:00:00Z");
    EXPECT_EQ(parsed.season->next->ends_at, "2026-09-15T15:00:00Z");
    EXPECT_EQ(parsed.season->next->source, "rocket_league");

    const nlohmann::json serialized = RemoteConfig::SerializeConfigJson(parsed);
    ASSERT_TRUE(serialized.contains("season"));
    EXPECT_EQ(serialized["season"]["current"], 24);
    ASSERT_TRUE(serialized["season"]["seasons"].is_array());
    ASSERT_EQ(serialized["season"]["seasons"].size(), 1u);
    EXPECT_EQ(serialized["season"]["seasons"][0]["season"], 24);
    EXPECT_EQ(serialized["season"]["seasons"][0]["source"], "rocket_league");

    ASSERT_TRUE(serialized["season"].contains("next"));
    EXPECT_EQ(serialized["season"]["next"]["season"], 25);
    EXPECT_FALSE(serialized["season"]["next"].contains("source"));

    RemoteConfigData roundTrip;
    std::string roundTripErr;
    ASSERT_TRUE(RemoteConfig::ParseConfigJson(serialized.dump(), roundTrip, &roundTripErr)) << roundTripErr;
    EXPECT_EQ(roundTrip, parsed);
    EXPECT_EQ(RemoteConfig::SerializeConfigJson(roundTrip), serialized);
}

TEST_F(RemoteConfigTestFixture, InvalidSeasonObjectIgnoredWhileRestValid) {
    auto assertSeasonIgnored = [](const nlohmann::json& invalidSeason) {
        const nlohmann::json payload = {
            {"schema", 1},
            {"ttl_seconds", 1200},
            {"min_supported_version", "2.2.0"},
            {"min_version_message", "Please update to continue."},
            {"season", invalidSeason}};
        RemoteConfigData parsed;
        std::string err;
        ASSERT_TRUE(RemoteConfig::ParseConfigJson(payload.dump(), parsed, &err)) << err;
        EXPECT_EQ(parsed.schema, 1);
        EXPECT_EQ(parsed.ttl_seconds, 1200);
        EXPECT_EQ(parsed.min_supported_version, "2.2.0");
        EXPECT_EQ(parsed.min_version_message, "Please update to continue.");
        EXPECT_FALSE(parsed.season.has_value());
    };

    auto makeSeasonsArray = [](std::initializer_list<nlohmann::json> entries) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& entry : entries) {
            arr.push_back(entry);
        }
        return arr;
    };

    // 1. Invalid timestamps in seasons array or next object
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "not-a-timestamp"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-02-31T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 25}, {"starts_at", "invalid-rfc3339"}, {"ends_at", "2026-09-15T15:00:00Z"}}}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 25}, {"starts_at", "2026-06-16T15:00:00Z"}, {"ends_at", "bad-date-format"}}}});

    // 2. start >= end
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-03-10T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-06-16T15:00:00Z"}, {"ends_at", "2026-03-10T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 25}, {"starts_at", "2026-09-15T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}}}});

    // 3. Season numbers outside 1..1000
    assertSeasonIgnored({{"current", 0},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 1001},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 0}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 1001}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 0}, {"starts_at", "2026-06-16T15:00:00Z"}, {"ends_at", "2026-09-15T15:00:00Z"}}}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 1001}, {"starts_at", "2026-06-16T15:00:00Z"}, {"ends_at", "2026-09-15T15:00:00Z"}}}});

    // 4. Too many entries (> 8 in seasons array)
    {
        nlohmann::json nineSeasons = nlohmann::json::array();
        for (int s = 1; s <= 9; ++s) {
            nineSeasons.push_back({{"season", s},
                                   {"starts_at", "2026-01-01T00:00:00Z"},
                                   {"ends_at", "2026-02-01T00:00:00Z"},
                                   {"source", "rocket_league"}});
        }
        assertSeasonIgnored({{"current", 1},
                             {"seasons", nineSeasons}});
    }

    // 5. Invalid source
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "unsupported_source"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}}})}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", "rocket_league"}}})},
                         {"next", {{"season", 25}, {"starts_at", "2026-06-16T15:00:00Z"}, {"ends_at", "2026-09-15T15:00:00Z"}, {"source", "invalid_source"}}}});
    assertSeasonIgnored({{"current", 24},
                         {"seasons", makeSeasonsArray({{{"season", 24}, {"starts_at", "2026-03-10T15:00:00Z"}, {"ends_at", "2026-06-16T15:00:00Z"}, {"source", 42}}})}});
}
