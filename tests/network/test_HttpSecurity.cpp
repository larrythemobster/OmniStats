#include <gtest/gtest.h>
#include "network/HttpSecurity.hpp"
#include "network/AccountClient.hpp"

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

// Deterministic in-process mock HTTP server to test redirect behavior end-to-end
namespace {
    class MockHttpServer {
      public:
        explicit MockHttpServer(std::function<void(const std::string& request, std::string& response)> handler)
            : m_handler(std::move(handler)),
              m_acceptor(m_io, asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0)) {
            m_port = m_acceptor.local_endpoint().port();
            m_thread = std::jthread([this]() { Run(); });
        }

        ~MockHttpServer() {
            m_running.store(false);
            asio::error_code ec;
            m_acceptor.close(ec);
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        uint16_t Port() const {
            return m_port;
        }
        std::string BaseUrl() const {
            return "http://127.0.0.1:" + std::to_string(m_port);
        }

      private:
        void Run() {
            while (m_running.load()) {
                asio::ip::tcp::socket socket(m_io);
                asio::error_code ec;
                m_acceptor.accept(socket, ec);
                if (ec) {
                    if (!m_running.load()) break;
                    continue;
                }

                std::array<char, 4096> buf{};
                size_t bytes = socket.read_some(asio::buffer(buf), ec);
                if (!ec && bytes > 0) {
                    std::string req(buf.data(), bytes);
                    std::string resp;
                    m_handler(req, resp);
                    asio::write(socket, asio::buffer(resp), ec);
                }
                socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            }
        }

        std::function<void(const std::string& request, std::string& response)> m_handler;
        asio::io_context m_io;
        asio::ip::tcp::acceptor m_acceptor;
        uint16_t m_port = 0;
        std::atomic<bool> m_running{true};
        std::jthread m_thread;
    };
} // namespace

TEST(HttpSecurityTest, AccountClientFollowsSameOriginRedirectAndPreservesBody) {
    std::atomic<bool> hitTarget{false};
    std::string receivedBody;

    MockHttpServer server([&](const std::string& req, std::string& resp) {
        if (req.find("POST /initial") != std::string::npos) {
            resp = "HTTP/1.1 307 Temporary Redirect\r\n"
                   "Location: /final\r\n"
                   "Content-Length: 0\r\n\r\n";
        } else if (req.find("POST /final") != std::string::npos) {
            hitTarget.store(true);
            const size_t bodyPos = req.find("\r\n\r\n");
            if (bodyPos != std::string::npos) {
                receivedBody = req.substr(bodyPos + 4);
            }
            resp = "HTTP/1.1 200 OK\r\n"
                   "Content-Type: application/json\r\n"
                   "Content-Length: 15\r\n\r\n"
                   "{\"status\":\"ok\"}";
        } else {
            resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        }
    });

    AccountClient client;
    const std::string postData = "{\"secret_token\":\"sample_secret_123\"}";
    const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/initial", postData);

    EXPECT_EQ(resp.statusCode, 200);
    EXPECT_TRUE(hitTarget.load());
    EXPECT_NE(receivedBody.find("sample_secret_123"), std::string::npos);
}

TEST(HttpSecurityTest, AccountClientRejectsCrossOriginRedirectAndNeverSendsCredentials) {
    std::atomic<bool> attackerHit{false};

    MockHttpServer attacker([&](const std::string& req, std::string& resp) {
        attackerHit.store(true);
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    });

    MockHttpServer server([&](const std::string& req, std::string& resp) {
        if (req.find("POST /login") != std::string::npos) {
            resp = "HTTP/1.1 307 Temporary Redirect\r\n"
                   "Location: " +
                   attacker.BaseUrl() + "/steal\r\n"
                                        "Content-Length: 0\r\n\r\n";
        } else {
            resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        }
    });

    AccountClient client;
    const std::string secretPayload = "{\"refresh_token\":\"ultra_secret_refresh_token_999\"}";
    const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/login", secretPayload);

    // Redirect to different port (cross-origin) must be rejected
    EXPECT_NE(resp.statusCode, 200);
    EXPECT_FALSE(attackerHit.load()); // Attacker server must never be reached!
    EXPECT_NE(resp.body.find("unsafe_redirect"), std::string::npos);
}

TEST(HttpSecurityTest, AccountClientRejectsRedirectDowngradeAttempt) {
    std::atomic<bool> insecureTargetHit{false};

    MockHttpServer insecureServer([&](const std::string& req, std::string& resp) {
        insecureTargetHit.store(true);
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    });

    // Test ValidateRedirect rejection directly for downgrade
    const auto val = ValidateRedirect("https://api.omnistats.org/v1/auth", "http://api.omnistats.org/v1/auth", true);
    EXPECT_FALSE(val.allowed);
    EXPECT_NE(val.reason.find("downgrade"), std::string::npos);
}
TEST(HttpSecurityTest, NormalAuthenticatedRequestSucceeds) {
    std::atomic<bool> hit{false};
    std::string receivedAuth;

    MockHttpServer server([&](const std::string& req, std::string& resp) {
        hit.store(true);
        resp = "HTTP/1.1 200 OK\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: 17\r\n\r\n"
               "{\"auth\":\"passed\"}";
    });

    AccountClient client;
    const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/auth", "{\"token\":\"tok_123\"}");
    EXPECT_EQ(resp.statusCode, 200);
    EXPECT_TRUE(hit.load());
}

TEST(HttpSecurityTest, RedirectStatusCodes301_302_308_HandledCorrectly) {
    // 301 Permanent Redirect
    {
        MockHttpServer server([&](const std::string& req, std::string& resp) {
            if (req.find("POST /from301") != std::string::npos) {
                resp = "HTTP/1.1 301 Moved Permanently\r\nLocation: /to301\r\nContent-Length: 0\r\n\r\n";
            } else if (req.find("POST /to301") != std::string::npos) {
                resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
            }
        });
        AccountClient client;
        const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/from301", "{}");
        EXPECT_EQ(resp.statusCode, 200);
    }

    // 302 Found
    {
        MockHttpServer server([&](const std::string& req, std::string& resp) {
            if (req.find("POST /from302") != std::string::npos) {
                resp = "HTTP/1.1 302 Found\r\nLocation: /to302\r\nContent-Length: 0\r\n\r\n";
            } else if (req.find("POST /to302") != std::string::npos) {
                resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
            }
        });
        AccountClient client;
        const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/from302", "{}");
        EXPECT_EQ(resp.statusCode, 200);
    }

    // 308 Permanent Redirect (preserves POST body)
    {
        std::string bodyAtDestination;
        MockHttpServer server([&](const std::string& req, std::string& resp) {
            if (req.find("POST /from308") != std::string::npos) {
                resp = "HTTP/1.1 308 Permanent Redirect\r\nLocation: /to308\r\nContent-Length: 0\r\n\r\n";
            } else if (req.find("POST /to308") != std::string::npos) {
                const size_t bpos = req.find("\r\n\r\n");
                if (bpos != std::string::npos) bodyAtDestination = req.substr(bpos + 4);
                resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
            }
        });
        AccountClient client;
        const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/from308", "{\"p\":308}");
        EXPECT_EQ(resp.statusCode, 200);
        EXPECT_NE(bodyAtDestination.find("308"), std::string::npos);
    }

    // 303 See Other is rejected for credential POST endpoints
    {
        MockHttpServer server([&](const std::string& req, std::string& resp) {
            resp = "HTTP/1.1 303 See Other\r\nLocation: /to303\r\nContent-Length: 0\r\n\r\n";
        });
        AccountClient client;
        const auto resp = client.PerformHttpPostForTests(server.BaseUrl() + "/from303", "{}");
        EXPECT_NE(resp.statusCode, 200);
        EXPECT_NE(resp.body.find("unsupported_redirect"), std::string::npos);
    }
}

TEST(HttpSecurityTest, ReusedRequestConfigurationsDoNotLeakState) {
    MockHttpServer server([&](const std::string& req, std::string& resp) {
        resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    });

    AccountClient client;
    // Sequential requests with different endpoints and bodies
    const auto r1 = client.PerformHttpPostForTests(server.BaseUrl() + "/req1", "{\"req\":1}");
    EXPECT_EQ(r1.statusCode, 200);

    const auto r2 = client.PerformHttpPostForTests(server.BaseUrl() + "/req2", "{\"req\":2}");
    EXPECT_EQ(r2.statusCode, 200);

    // Third request to invalid port fails cleanly without lingering state
    const auto r3 = client.PerformHttpPostForTests("http://127.0.0.1:1/fail", "{}");
    EXPECT_NE(r3.statusCode, 200);

    // Fourth request to valid server still succeeds cleanly
    const auto r4 = client.PerformHttpPostForTests(server.BaseUrl() + "/req4", "{\"req\":4}");
    EXPECT_EQ(r4.statusCode, 200);
}
