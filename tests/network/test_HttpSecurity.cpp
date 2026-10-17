#include <gtest/gtest.h>
#include "network/http/HttpSecurity.hpp"

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace HttpSecurity;

TEST(HttpSecurityTest, ParsesUrlsCorrectly) {
    auto parsed = ParseUrl("https://api.omnistats.org/v1/ranks?scope=global#frag");
    EXPECT_TRUE(parsed.valid);
    EXPECT_EQ(parsed.scheme, "https");
    EXPECT_EQ(parsed.host, "api.omnistats.org");
    EXPECT_EQ(parsed.port, 0);
    EXPECT_EQ(parsed.EffectivePort(), 443);
    EXPECT_EQ(parsed.path, "/v1/ranks");
    EXPECT_EQ(parsed.query, "scope=global");

    auto withPort = ParseUrl("http://127.0.0.1:8080/test");
    EXPECT_TRUE(withPort.valid);
    EXPECT_EQ(withPort.scheme, "http");
    EXPECT_EQ(withPort.host, "127.0.0.1");
    EXPECT_EQ(withPort.port, 8080);
    EXPECT_EQ(withPort.EffectivePort(), 8080);
    EXPECT_EQ(withPort.path, "/test");

    auto ipv6 = ParseUrl("https://[::1]:9443/auth");
    EXPECT_TRUE(ipv6.valid);
    EXPECT_EQ(ipv6.scheme, "https");
    EXPECT_EQ(ipv6.host, "[::1]");
    EXPECT_EQ(ipv6.port, 9443);
    EXPECT_EQ(ipv6.path, "/auth");

    EXPECT_FALSE(ParseUrl("").valid);
    EXPECT_FALSE(ParseUrl("not-a-url").valid);
    EXPECT_FALSE(ParseUrl("://missing-scheme").valid);
}

TEST(HttpSecurityTest, LoopbackHostDetection) {
    EXPECT_TRUE(IsLoopbackHost("127.0.0.1"));
    EXPECT_TRUE(IsLoopbackHost("127.0.0.2"));
    EXPECT_TRUE(IsLoopbackHost("localhost"));
    EXPECT_TRUE(IsLoopbackHost("::1"));
    EXPECT_TRUE(IsLoopbackHost("[::1]"));

    EXPECT_FALSE(IsLoopbackHost("api.omnistats.org"));
    EXPECT_FALSE(IsLoopbackHost("192.168.1.100"));
    EXPECT_FALSE(IsLoopbackHost("example.com"));
}

TEST(HttpSecurityTest, SensitiveUrlPolicyEnforcement) {
    std::string err;
    auto httpsUrl = ParseUrl("https://api.omnistats.org/v1/ranks");
    EXPECT_TRUE(IsAllowedSensitiveUrl(httpsUrl, &err));

    auto loopbackHttp = ParseUrl("http://127.0.0.1:49123/v1/ranks");
    EXPECT_TRUE(IsAllowedSensitiveUrl(loopbackHttp, &err));

    auto localhostHttp = ParseUrl("http://localhost:8080/v1/ranks");
    EXPECT_TRUE(IsAllowedSensitiveUrl(localhostHttp, &err));

    auto remoteHttp = ParseUrl("http://api.omnistats.org/v1/ranks");
    EXPECT_FALSE(IsAllowedSensitiveUrl(remoteHttp, &err));
    EXPECT_FALSE(err.empty());

    auto ftpUrl = ParseUrl("ftp://api.omnistats.org/v1/ranks");
    EXPECT_FALSE(IsAllowedSensitiveUrl(ftpUrl, &err));
}

TEST(HttpSecurityTest, SameOriginComparison) {
    auto u1 = ParseUrl("https://api.omnistats.org/v1/ranks");
    auto u2 = ParseUrl("https://api.omnistats.org/v2/ranks");
    auto u3 = ParseUrl("https://api.omnistats.org:443/v1/ranks");
    auto uDiffHost = ParseUrl("https://evil.omnistats.org/v1/ranks");
    auto uDiffScheme = ParseUrl("http://api.omnistats.org/v1/ranks");
    auto uDiffPort = ParseUrl("https://api.omnistats.org:8443/v1/ranks");

    EXPECT_TRUE(IsSameOrigin(u1, u2));
    EXPECT_TRUE(IsSameOrigin(u1, u3));
    EXPECT_FALSE(IsSameOrigin(u1, uDiffHost));
    EXPECT_FALSE(IsSameOrigin(u1, uDiffScheme));
    EXPECT_FALSE(IsSameOrigin(u1, uDiffPort));
}

TEST(HttpSecurityTest, ResolvesRedirectLocations) {
    const std::string base = "https://api.omnistats.org/v1/ranks";
    EXPECT_EQ(ResolveRedirectUrl(base, "/v2/ranks"), "https://api.omnistats.org/v2/ranks");
    EXPECT_EQ(ResolveRedirectUrl(base, "relative"), "https://api.omnistats.org/v1/relative");
    EXPECT_EQ(ResolveRedirectUrl(base, "//other.org/path"), "https://other.org/path");
    EXPECT_EQ(ResolveRedirectUrl(base, "https://api.omnistats.org/new"), "https://api.omnistats.org/new");
}

TEST(HttpSecurityTest, ValidatesRedirectSafety) {
    const std::string current = "https://api.omnistats.org/v1/ranks";

    // Same origin redirect allowed
    auto rSame = ValidateRedirect(current, "/v1/ranks-v2", true);
    EXPECT_TRUE(rSame.allowed);
    EXPECT_EQ(rSame.resolvedUrl, "https://api.omnistats.org/v1/ranks-v2");

    // Cross-origin redirect rejected for sensitive requests
    auto rCross = ValidateRedirect(current, "https://attacker.com/steal", true);
    EXPECT_FALSE(rCross.allowed);
    EXPECT_FALSE(rCross.reason.empty());

    // HTTPS to HTTP downgrade rejected
    auto rDowngrade = ValidateRedirect(current, "http://api.omnistats.org/v1/ranks", true);
    EXPECT_FALSE(rDowngrade.allowed);
    EXPECT_FALSE(rDowngrade.reason.empty());

    // Unauthenticated request allows cross-origin
    auto rUnauthCross = ValidateRedirect(current, "https://other.org/data", false);
    EXPECT_TRUE(rUnauthCross.allowed);
}
