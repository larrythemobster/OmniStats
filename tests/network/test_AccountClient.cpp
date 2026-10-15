#include <gtest/gtest.h>

#include "core/config/Config.hpp"
#include "utils/Storage.hpp"
#include "network/api/AccountClient.hpp"

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AccountClientTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        m_originalConfig = Config::Read();
        Config::Update([](ConfigData& c) {
            c.custom_api_enabled = true;
            c.custom_api_base_url = "https://api.omnistats.org";
            c.account_signed_in_name.clear();
            c.account_device_public_id.clear();
            c.account_refresh_token.clear();
            c.account_device_key.clear();
            c.custom_api_key.clear();
        });
        Config::Save();
        AccountClient::Instance().ResetForTests();
    }

    void TearDown() override {
        AccountClient::Instance().ResetForTests();
        Config::Update([this](ConfigData& c) { c = m_originalConfig; });
        Config::Save();
    }

    ConfigData m_originalConfig;
};

TEST_F(AccountClientTest, GoldenVectorTokenAndRefreshMatchGoServer) {
    std::array<uint8_t, 32> seed{};
    for (size_t i = 0; i < seed.size(); ++i) {
        seed[i] = static_cast<uint8_t>(i + 1);
    }

    std::array<uint8_t, 32> nonceBytes{};
    for (size_t i = 0; i < nonceBytes.size(); ++i) {
        nonceBytes[i] = static_cast<uint8_t>(i);
    }

    const auto keyPair = AccountClient::DeriveKeyPairFromSeed(seed);
    EXPECT_EQ(keyPair.publicKeyBase64Url, "ebVWLo_mVPlAeLES6KmLp5AfhTrmlb7X4OORC60ElmQ");
    EXPECT_EQ(keyPair.publicId, "ZbYGc9btiEvwHCwiLYKtoHQPKawzVdapJcgfF_R6J7g");

    const std::string nonce = AccountClient::Base64UrlEncode(nonceBytes.data(), nonceBytes.size());
    EXPECT_EQ(nonce, "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8");

    constexpr int64_t kTimestamp = 1700000000;

    const auto tokenProof = AccountClient::SignProofDeterministic(
        keyPair.secretKeyBase64Url,
        "token",
        keyPair.publicId,
        kTimestamp,
        nonce,
        "test-device-code-0001");

    const std::string expectedTokenCanonical =
        "OMNISTATS-DEVICE-PROOF-v1\n"
        "token\n"
        "ZbYGc9btiEvwHCwiLYKtoHQPKawzVdapJcgfF_R6J7g\n"
        "1700000000\n"
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8\n"
        "596ecd66e169abb0c00243f3c20638382d9a08d74cbc25ca82f93f57edd86592";
    const std::string expectedTokenSignature =
        "siGIhkv9MbgUiACJlZoG97NZAvJrqru6WSW7d12l-R5IP31VOgsSL5R2bRSyS255Y6sdzAvB6ud6bxN3CF7RAA";

    EXPECT_EQ(tokenProof.canonical, expectedTokenCanonical);
    EXPECT_EQ(tokenProof.signature, expectedTokenSignature);

    const auto refreshProof = AccountClient::SignProofDeterministic(
        keyPair.secretKeyBase64Url,
        "refresh",
        keyPair.publicId,
        kTimestamp,
        nonce,
        "test-refresh-token-0001");

    const std::string expectedRefreshCanonical =
        "OMNISTATS-DEVICE-PROOF-v1\n"
        "refresh\n"
        "ZbYGc9btiEvwHCwiLYKtoHQPKawzVdapJcgfF_R6J7g\n"
        "1700000000\n"
        "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8\n"
        "ecf0590a7b8bd6e7b7398b0902977d35e74f85deb856995a4535a12d42d86ea6";
    const std::string expectedRefreshSignature =
        "VZODbIdauvzyEsral-smWL7jSrAMk3vuJdKB061amFwqSWcXfNVreY089YnV3DsuSvQ9CoAFWYXUnoA7lgHhAg";

    EXPECT_EQ(refreshProof.canonical, expectedRefreshCanonical);
    EXPECT_EQ(refreshProof.signature, expectedRefreshSignature);
}

TEST_F(AccountClientTest, MatchesBackendAuthoritativeRfc8032Vector) {
    const std::array<uint8_t, 32> seed = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
        0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
        0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
        0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60};
    const auto keyPair = AccountClient::DeriveKeyPairFromSeed(seed);
    EXPECT_EQ(keyPair.publicKeyBase64Url, "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo");
    EXPECT_EQ(keyPair.publicId, "If4x36FUomFia_hUBG_SJxt77UtqvkWqWId-9H-XIbk");

    const auto proof = AccountClient::SignProofDeterministic(
        keyPair.secretKeyBase64Url,
        "refresh",
        keyPair.publicId,
        1700000000,
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
        "deadbeef");
    EXPECT_EQ(proof.signature,
              "SOwAlAmBtQbXriDWVGbGdvndNukGx9Rnm_HeNyGwFDvE01PKUjg-vfG13586IY02yzk8wzmn2uv1mpvtcWsFAw");
}

TEST_F(AccountClientTest, PublicIdDerivationRejectsMalformedPublicKeys) {
    EXPECT_TRUE(AccountClient::DerivePublicIdForPublicKey("").empty());
    EXPECT_TRUE(AccountClient::DerivePublicIdForPublicKey("short_key").empty());
    EXPECT_TRUE(AccountClient::DerivePublicIdForPublicKey("not valid base64!!!").empty());
}

TEST_F(AccountClientTest, EnsureDeviceKeyGeneratesAndReusesKeyPair) {
    AccountClient client;
    AccountClient::DeviceKeyPair first;
    ASSERT_TRUE(client.EnsureDeviceKey(&first));
    EXPECT_FALSE(first.publicId.empty());
    EXPECT_FALSE(first.publicKeyBase64Url.empty());
    EXPECT_FALSE(first.secretKeyBase64Url.empty());

    AccountClient::DeviceKeyPair second;
    ASSERT_TRUE(client.EnsureDeviceKey(&second));
    EXPECT_EQ(first.publicId, second.publicId);
    EXPECT_EQ(first.publicKeyBase64Url, second.publicKeyBase64Url);
    EXPECT_EQ(first.secretKeyBase64Url, second.secretKeyBase64Url);
}

TEST_F(AccountClientTest, PollStateMachineHandlesPendingSlowDownAndApproval) {
    AccountClient client;
    int64_t nowUnix = 1700000000;
    std::vector<int> sleepIntervals;
    int tokenCalls = 0;

    client.SetClockForTests([&]() { return nowUnix; });
    client.SetSleepForTests([&](int seconds) {
        sleepIntervals.push_back(seconds);
        nowUnix += seconds;
        return true;
    });
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        AccountClient::HttpResponse resp;
        const auto req = nlohmann::json::parse(body);
        if (url.find("/v1/auth/device/start") != std::string::npos) {
            EXPECT_EQ(req.at("platform"), "windows");
            EXPECT_FALSE(req.at("device_name").get<std::string>().empty());
            EXPECT_FALSE(req.at("public_key").get<std::string>().empty());
            EXPECT_EQ(req.at("device_public_id").get<std::string>(),
                      AccountClient::DerivePublicIdForPublicKey(req.at("public_key").get<std::string>()));
            resp.statusCode = 200;
            resp.body = R"({
                "device_code": "dev-code-123",
                "user_code": "K7Q-9MX",
                "verification_uri": "/device?user_code=K7Q-9MX",
                "expires_in": 900,
                "poll_interval": 5
            })";
            return resp;
        }
        if (url.find("/v1/auth/device/token") != std::string::npos) {
            ++tokenCalls;
            EXPECT_EQ(req.at("device_code"), "dev-code-123");
            EXPECT_TRUE(req.contains("proof"));
            if (tokenCalls == 1) {
                resp.statusCode = 428;
                resp.body = R"({"error":{"code":"authorization_pending","message":"Pending"}})";
                return resp;
            }
            if (tokenCalls == 2) {
                resp.statusCode = 429;
                resp.body = R"({"error":{"code":"slow_down","message":"Slow down"}})";
                return resp;
            }
            resp.statusCode = 200;
            resp.body = R"({
                "access_token": "acc-token-initial",
                "expires_in": 900,
                "refresh_token": "ref-token-initial",
                "refresh_expires_in": 2592000,
                "account": {
                    "public_id": "usr-1",
                    "email": "player@example.com",
                    "display_name": "AerialAce",
                    "status": "active"
                },
                "device": {
                    "public_id": ")" +
                        req.at("device_public_id").get<std::string>() + R"(",
                    "device_name": "Desktop",
                    "platform": "windows",
                    "app_version": "2.2.13",
                    "status": "active"
                }
            })";
            return resp;
        }
        resp.statusCode = 404;
        return resp;
    });

    const auto start = client.StartAuthorization();
    ASSERT_TRUE(start.ok);
    EXPECT_EQ(start.userCode, "K7Q-9MX");
    EXPECT_EQ(start.verificationUri, "https://omnistats.org/device?user_code=K7Q-9MX");
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::AwaitingApproval);

    const auto poll = client.PollToken();
    EXPECT_EQ(poll.outcome, AccountClient::PollOutcome::Approved);
    EXPECT_EQ(poll.displayName, "AerialAce");
    ASSERT_EQ(sleepIntervals.size(), 3u);
    EXPECT_EQ(sleepIntervals[0], 5);
    EXPECT_EQ(sleepIntervals[1], 5);
    EXPECT_EQ(sleepIntervals[2], 10);

    EXPECT_TRUE(client.IsSignedIn());
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::SignedIn);
    EXPECT_EQ(client.GetStatus().displayName, "AerialAce");
    EXPECT_EQ(Config::Read().account_refresh_token, "ref-token-initial");
    EXPECT_EQ(Config::Read().account_signed_in_name, "AerialAce");
}

TEST_F(AccountClientTest, PollStateMachineHandlesDeniedAndExpired) {
    AccountClient client;
    int64_t nowUnix = 1700000000;
    std::string mode = "denied";

    client.SetClockForTests([&]() { return nowUnix; });
    client.SetSleepForTests([&](int seconds) {
        nowUnix += seconds;
        return true;
    });
    client.SetTransportForTests([&](const std::string& url, const std::string&) {
        AccountClient::HttpResponse resp;
        if (url.find("/v1/auth/device/start") != std::string::npos) {
            resp.statusCode = 200;
            resp.body = R"({
                "device_code": "dev-code-err",
                "user_code": "DENY1234",
                "verification_uri": "https://omnistats.org/device?user_code=DENY1234",
                "expires_in": 60,
                "poll_interval": 5
            })";
            return resp;
        }
        if (mode == "denied") {
            resp.statusCode = 403;
            resp.body = R"({"error":{"code":"access_denied","message":"The device authorization was denied."}})";
        } else {
            resp.statusCode = 400;
            resp.body = R"({"error":{"code":"invalid_device_authorization","message":"Expired"}})";
        }
        return resp;
    });

    ASSERT_TRUE(client.StartAuthorization().ok);
    const auto deniedRes = client.PollToken();
    EXPECT_EQ(deniedRes.outcome, AccountClient::PollOutcome::Denied);
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);
    EXPECT_FALSE(client.IsSignedIn());

    mode = "expired";
    ASSERT_TRUE(client.StartAuthorization().ok);
    const auto expiredRes = client.PollToken();
    EXPECT_EQ(expiredRes.outcome, AccountClient::PollOutcome::Expired);
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);

    ASSERT_TRUE(client.StartAuthorization().ok);
    nowUnix += 120;
    const auto clockExpiredRes = client.PollToken();
    EXPECT_EQ(clockExpiredRes.outcome, AccountClient::PollOutcome::Expired);
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);
}

TEST_F(AccountClientTest, RefreshRotationPersistsNewTokenAndCachesAccessToken) {
    AccountClient client;
    int64_t nowUnix = 1700000000;
    int refreshCount = 0;

    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(client.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "refresh-token-v1";
        c.account_signed_in_name = "Tester";
    });
    Config::Save();
    client.SyncFromConfig();

    client.SetClockForTests([&]() { return nowUnix; });
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        AccountClient::HttpResponse resp;
        EXPECT_NE(url.find("/v1/auth/device/refresh"), std::string::npos);
        const auto req = nlohmann::json::parse(body);
        ++refreshCount;
        EXPECT_EQ(req.at("refresh_token"), refreshCount == 1 ? "refresh-token-v1" : "refresh-token-v2");
        resp.statusCode = 200;
        resp.body = nlohmann::json{
            {"access_token", "access-token-v" + std::to_string(refreshCount)},
            {"expires_in", 900},
            {"refresh_token", "refresh-token-v" + std::to_string(refreshCount + 1)},
            {"refresh_expires_in", 2592000},
            {"account", {{"display_name", "TesterUpdated"}}},
            {"device", {{"public_id", keyPair.publicId}, {"status", "active"}}}}
                        .dump();
        return resp;
    });

    EXPECT_EQ(client.AccessToken(), "access-token-v1");
    EXPECT_EQ(refreshCount, 1);
    EXPECT_EQ(Config::Read().account_refresh_token, "refresh-token-v2");
    EXPECT_EQ(Config::Read().account_signed_in_name, "TesterUpdated");

    Config::Load();
    EXPECT_EQ(Config::Read().account_refresh_token, "refresh-token-v2");

    nowUnix += 830;
    EXPECT_EQ(client.AccessToken(), "access-token-v1");
    EXPECT_EQ(refreshCount, 1);

    nowUnix += 15;
    EXPECT_EQ(client.AccessToken(), "access-token-v2");
    EXPECT_EQ(refreshCount, 2);
    EXPECT_EQ(Config::Read().account_refresh_token, "refresh-token-v3");
}

TEST_F(AccountClientTest, ConcurrentAccessTokenCallsShareSingleFlightRefresh) {
    AccountClient client;
    int64_t nowUnix = 1700000000;
    std::atomic<int> refreshCount{0};

    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(client.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "single-flight-refresh-token";
        c.account_signed_in_name = "ConcurrentUser";
    });
    Config::Save();
    client.SyncFromConfig();
    client.SetClockForTests([&]() { return nowUnix; });
    client.SetTransportForTests([&](const std::string&, const std::string&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const int n = ++refreshCount;
        AccountClient::HttpResponse resp;
        resp.statusCode = 200;
        resp.body = nlohmann::json{
            {"access_token", "shared-access-token-" + std::to_string(n)},
            {"expires_in", 900},
            {"refresh_token", "rotated-refresh-" + std::to_string(n)},
            {"refresh_expires_in", 2592000},
            {"account", {{"display_name", "ConcurrentUser"}}},
            {"device", {{"public_id", keyPair.publicId}, {"status", "active"}}}}
                        .dump();
        return resp;
    });

    std::string tokenA;
    std::string tokenB;
    std::thread t1([&]() { tokenA = client.AccessToken(); });
    std::thread t2([&]() { tokenB = client.AccessToken(); });
    t1.join();
    t2.join();

    EXPECT_EQ(refreshCount.load(), 1);
    EXPECT_EQ(tokenA, "shared-access-token-1");
    EXPECT_EQ(tokenB, "shared-access-token-1");
}

TEST_F(AccountClientTest, LogoutSendsSignedLogoutProofAndClearsRefreshToken) {
    AccountClient client;
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(client.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "logout-refresh-token-1";
        c.account_signed_in_name = "LogoutUser";
    });
    Config::Save();
    client.SyncFromConfig();

    bool logoutCalled = false;
    client.SetClockForTests([]() { return 1700000000; });
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        EXPECT_NE(url.find("/v1/auth/device/logout"), std::string::npos);
        const auto req = nlohmann::json::parse(body);
        EXPECT_EQ(req.at("refresh_token"), "logout-refresh-token-1");
        EXPECT_EQ(req.at("device_public_id"), keyPair.publicId);
        EXPECT_TRUE(req.contains("proof"));
        logoutCalled = true;
        AccountClient::HttpResponse resp;
        resp.statusCode = 200;
        resp.body = R"({"message":"Device signed out."})";
        return resp;
    });

    EXPECT_TRUE(client.Logout());
    EXPECT_TRUE(logoutCalled);
    EXPECT_FALSE(client.IsSignedIn());
    EXPECT_TRUE(Config::Read().account_refresh_token.empty());
    EXPECT_TRUE(Config::Read().account_signed_in_name.empty());
    EXPECT_FALSE(Config::Read().account_device_key.empty());
}

TEST_F(AccountClientTest, RefreshCompletingAfterLogoutStaysSignedOutAndRevokesRotatedToken) {
    AccountClient client;
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(client.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = "race-refresh-v1";
        c.account_signed_in_name = "RaceUser";
    });
    Config::Save();
    client.SyncFromConfig();
    client.SetClockForTests([]() { return 1700000000; });

    std::mutex m;
    std::condition_variable cv;
    bool refreshEntered = false;
    bool releaseRefresh = false;
    std::vector<std::string> logoutTokens;
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        AccountClient::HttpResponse resp;
        resp.statusCode = 200;
        const auto req = nlohmann::json::parse(body);
        std::unique_lock lock(m);
        if (url.find("/v1/auth/device/logout") != std::string::npos) {
            logoutTokens.push_back(req.at("refresh_token").get<std::string>());
            cv.notify_all();
            resp.body = R"({"message":"Device signed out."})";
            return resp;
        }
        refreshEntered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return releaseRefresh; });
        resp.body = nlohmann::json{{"access_token", "race-access"},
                                   {"expires_in", 900},
                                   {"refresh_token", "race-refresh-v2"},
                                   {"account", {{"display_name", "RaceUser"}}}}
                        .dump();
        return resp;
    });

    std::thread refresher([&] { (void)client.Refresh(); });
    {
        std::unique_lock lock(m);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return refreshEntered; }));
    }
    client.LogoutAsync();
    {
        std::lock_guard lock(m);
        releaseRefresh = true;
    }
    cv.notify_all();
    refresher.join();

    EXPECT_FALSE(client.IsSignedIn());
    EXPECT_TRUE(Config::Read().account_refresh_token.empty());
    {
        std::unique_lock lock(m);
        const auto sentRotated = [&] { return std::find(logoutTokens.begin(), logoutTokens.end(), "race-refresh-v2") != logoutTokens.end(); };
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), sentRotated));
    }
    client.Shutdown();
}

TEST_F(AccountClientTest, StartupExchangesLegacyKeyAndPersistsDeviceSession) {
    AccountClient client;
    Config::Update([](ConfigData& c) { c.custom_api_key = "oms_saved_key"; });
    std::atomic<int> exchanges{0};
    client.SetClockForTests([] { return 1700000000; });
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        EXPECT_NE(url.find("/v1/auth/device/exchange-key"), std::string::npos);
        ++exchanges;
        const auto request = nlohmann::json::parse(body);
        const auto proof = request.at("proof");
        std::vector<uint8_t> publicKey, signature;
        EXPECT_TRUE(AccountClient::Base64UrlDecode(request.at("public_key").get<std::string>(), publicKey));
        EXPECT_TRUE(AccountClient::Base64UrlDecode(proof.at("signature").get<std::string>(), signature));
        const auto canonical = AccountClient::BuildProofCanonical(
            "exchange", request.at("device_public_id").get<std::string>(), proof.at("timestamp").get<int64_t>(),
            proof.at("nonce").get<std::string>(), "oms_saved_key");
        if (publicKey.size() == crypto_sign_PUBLICKEYBYTES && signature.size() == crypto_sign_BYTES) {
            EXPECT_EQ(crypto_sign_verify_detached(signature.data(), reinterpret_cast<const unsigned char*>(canonical.data()),
                                                  canonical.size(), publicKey.data()),
                      0);
        } else {
            ADD_FAILURE() << "Invalid device proof";
        }
        return AccountClient::HttpResponse{0, 200, R"({"access_token":"access","refresh_token":"refresh","expires_in":900,"account":{"display_name":"Linked"}})"};
    });
    client.SyncFromConfig();
    for (int i = 0; i < 500 && !client.IsSignedIn(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(client.IsSignedIn());
    client.SyncFromConfig();
    client.Shutdown();
    EXPECT_EQ(exchanges.load(), 1);
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::SignedIn);
    EXPECT_EQ(client.AccessToken(), "access");
    EXPECT_EQ(Config::Read().account_refresh_token, "refresh");
    EXPECT_TRUE(Config::Read().custom_api_key.empty());
    Config::Load();
    EXPECT_EQ(Config::Read().account_refresh_token, "refresh");
    EXPECT_TRUE(Config::Read().custom_api_key.empty());
}

TEST_F(AccountClientTest, FailedStartupExchangeKeepsKeyAndExplainsRecovery) {
    struct Failure {
        long status;
        const char* code;
        const char* recovery;
    };
    for (const auto& failure : {
             Failure{409, "device_limit_reached", "Remove an unused device"},
             Failure{403, "email_verification_required", "Verify your OmniStats email"},
             Failure{401, "invalid_api_key", "Sign in in Settings"},
             Failure{503, "server_error", "Restart OmniStats to retry"},
             Failure{0, "network_error", "Restart OmniStats to retry"},
             Failure{200, "", "incomplete device session"}}) {
        AccountClient client;
        Config::Update([](ConfigData& c) { c.custom_api_key = "oms_saved_key"; });
        client.SetTransportForTests([&](const std::string&, const std::string&) {
            return AccountClient::HttpResponse{failure.status == 0 ? 7 : 0, failure.status,
                                               nlohmann::json{{"error", {{"code", failure.code}}}}.dump()};
        });
        client.SyncFromConfig();
        for (int i = 0; i < 500 && client.GetStatus().state == AccountAuthState::Migrating; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client.Shutdown();
        EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);
        EXPECT_NE(client.GetStatus().errorMessage.find(failure.recovery), std::string::npos);
        EXPECT_EQ(Config::Read().custom_api_key, "oms_saved_key");
        EXPECT_TRUE(Config::Read().account_refresh_token.empty());
    }
}

TEST_F(AccountClientTest, StartupExchangeCompletingAfterLogoutRevokesReturnedSession) {
    AccountClient client;
    Config::Update([](ConfigData& c) { c.custom_api_key = "oms_saved_key"; });
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false, revoked = false;
    client.SetTransportForTests([&](const std::string& url, const std::string& body) {
        std::unique_lock lock(mutex);
        if (url.ends_with("/logout")) {
            EXPECT_EQ(nlohmann::json::parse(body).at("refresh_token"), "cancelled-refresh");
            revoked = true;
            cv.notify_all();
            return AccountClient::HttpResponse{0, 200, "{}"};
        }
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
        return AccountClient::HttpResponse{0, 200, R"({"access_token":"cancelled-access","refresh_token":"cancelled-refresh","expires_in":900})"};
    });
    client.SyncFromConfig();
    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered; }));
    }
    client.LogoutAsync();
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    {
        std::unique_lock lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&] { return revoked; }));
    }
    client.Shutdown();
    EXPECT_FALSE(client.IsSignedIn());
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::SignedOut);
    EXPECT_TRUE(Config::Read().account_refresh_token.empty());
    EXPECT_TRUE(Config::Read().custom_api_key.empty());
}

TEST_F(AccountClientTest, ExplicitRetryMigratesPreservedKeyWithoutOpeningBrowser) {
    AccountClient client;
    Config::Update([](ConfigData& c) { c.custom_api_key = "oms_saved_key"; });
    std::atomic<int> requests{0};
    std::atomic<int> browserOpens{0};
    client.SetOpenUrlForTests([&](const std::string&) { ++browserOpens; });
    client.SetTransportForTests([&](const std::string&, const std::string&) {
        if (++requests == 1) return AccountClient::HttpResponse{7, 0, ""};
        return AccountClient::HttpResponse{0, 200, R"({"access_token":"retry-access","refresh_token":"retry-refresh","expires_in":900})"};
    });
    client.SyncFromConfig();
    for (int i = 0; i < 500 && client.GetStatus().state == AccountAuthState::Migrating; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);
    client.SyncFromConfig();
    EXPECT_EQ(client.GetStatus().state, AccountAuthState::Error);
    client.RetryKeyMigrationAsync();
    for (int i = 0; i < 500 && !client.IsSignedIn(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_TRUE(client.IsSignedIn());
    client.Shutdown();
    EXPECT_EQ(requests.load(), 2);
    EXPECT_EQ(browserOpens.load(), 0);
    EXPECT_EQ(Config::Read().account_refresh_token, "retry-refresh");
    EXPECT_TRUE(Config::Read().custom_api_key.empty());
}
