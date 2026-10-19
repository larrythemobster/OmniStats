#include <gtest/gtest.h>

#include "core/state/MatchCardUrl.hpp"
#include "core/game/MatchAnalytics.hpp"

namespace {

    using namespace OmniStats::MatchCard;

    TEST(MatchCardUrlTest, Base64UrlRoundTrip) {
        EXPECT_EQ(Base64UrlEncode(""), "");
        EXPECT_EQ(Base64UrlDecode(""), "");

        const std::vector<std::string> testCases = {
            "a",
            "ab",
            "abc",
            "abcd",
            "Hello World!",
            "{\"v\":1,\"res\":\"W\",\"sc\":[3,2]}",
            "\x00\xFF\xFE\x01\x7F\x80\xAA",
        };

        for (const auto& tc : testCases) {
            const std::string enc = Base64UrlEncode(tc);
            EXPECT_EQ(enc.find('='), std::string::npos);
            EXPECT_EQ(enc.find('+'), std::string::npos);
            EXPECT_EQ(enc.find('/'), std::string::npos);
            const std::string dec = Base64UrlDecode(enc);
            EXPECT_EQ(dec, tc);
        }
    }

    TEST(MatchCardUrlTest, BuildAndParseMatchCard) {
        MatchDetail detail;
        detail.found = true;
        detail.matchId = 12345;
        detail.playlist = "Ranked Doubles 2v2";
        detail.ourScore = 4;
        detail.theirScore = 2;
        detail.win = true;
        detail.durationSeconds = 300.0f;
        detail.overtimeSeconds = 0.0f;
        detail.timestampUnix = 1729344000;
        detail.accountPrimaryId = "steam_12345";
        detail.mmrBefore = 1042;
        detail.mmrAfter = 1051;

        MatchDetailPlayer me;
        me.primaryId = "steam_12345";
        me.name = "AeroPulse";
        me.isMe = true;
        me.hasStats = true;
        me.tier = "Diamond II Div 3";
        me.goals = 3;
        me.assists = 1;
        me.saves = 2;
        me.shots = 5;
        me.score = 680;
        me.demos = 1;
        detail.ourTeam.push_back(me);

        MatchAnalytics analytics;
        analytics.playSeconds = 300.0f;
        PlayerAnalytics pa;
        pa.primaryId = "steam_12345";
        pa.name = "AeroPulse";
        CarTelemetry car;
        car.sampledSeconds = 290.0f;
        car.boostUsed = 2100;
        car.supersonicSeconds = 58.0f;
        car.airSeconds = 72.5f;
        pa.car = car;
        analytics.players.push_back(pa);
        detail.analytics = analytics;

        const nlohmann::json json = BuildMatchCardJson(detail);
        EXPECT_EQ(json["v"], 1);
        EXPECT_EQ(json["res"], "W");
        EXPECT_EQ(json["sc"][0], 4);
        EXPECT_EQ(json["sc"][1], 2);
        EXPECT_EQ(json["pl"], "Ranked Doubles 2v2");
        EXPECT_EQ(json["dur"], 300);
        EXPECT_EQ(json["name"], "AeroPulse");
        EXPECT_EQ(json["st"]["g"], 3);
        EXPECT_EQ(json["st"]["a"], 1);
        EXPECT_EQ(json["st"]["sv"], 2);
        EXPECT_EQ(json["st"]["sh"], 5);
        EXPECT_EQ(json["st"]["sc"], 680);
        EXPECT_EQ(json["st"]["dm"], 1);
        EXPECT_EQ(json["mmr"]["b"], 1042);
        EXPECT_EQ(json["mmr"]["a"], 1051);
        EXPECT_EQ(json["mmr"]["rank"], "Diamond II Div 3");
        EXPECT_EQ(json["ss"], 20);  // 58 / 290 = 20%
        EXPECT_EQ(json["air"], 25); // 72.5 / 290 = 25%

        const std::string url = BuildMatchCardUrl(detail);
        EXPECT_TRUE(url.starts_with("https://omnistats.org/card#1."));

        const auto parsed = ParseMatchCardUrl(url);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ((*parsed)["v"], 1);
        EXPECT_EQ((*parsed)["res"], "W");
        EXPECT_EQ((*parsed)["sc"][0], 4);
        EXPECT_EQ((*parsed)["sc"][1], 2);
        EXPECT_EQ((*parsed)["name"], "AeroPulse");
    }

    TEST(MatchCardUrlTest, ParseInvalidUrl) {
        EXPECT_FALSE(ParseMatchCardUrl("").has_value());
        EXPECT_FALSE(ParseMatchCardUrl("https://omnistats.org/card").has_value());
        EXPECT_FALSE(ParseMatchCardUrl("https://omnistats.org/card#2.invalid").has_value());
        EXPECT_FALSE(ParseMatchCardUrl("https://omnistats.org/card#1.not_json!!!").has_value());
    }

} // namespace
