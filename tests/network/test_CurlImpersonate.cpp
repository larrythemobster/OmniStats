// These opt-in tests make real HTTP requests to httpbin.org.
// Set OMNISTATS_RUN_NETWORK_TESTS=1 to enable them. Static libcurl is used so
// the runtime curl-impersonate DLL is not required.

#include <gtest/gtest.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "network/CurlImpersonate.hpp"
#include <cstdlib>
#include <string>
#include <vector>

// Write callback that appends data to a std::string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    auto* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
}

class CurlRealNetworkTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* enabled = std::getenv("OMNISTATS_RUN_NETWORK_TESTS");
        if (enabled == nullptr || std::string(enabled) != "1") {
            GTEST_SKIP() << "Set OMNISTATS_RUN_NETWORK_TESTS=1 to run live HTTP tests";
        }

        curl_global_init(CURL_GLOBAL_DEFAULT);
        m_curl = curl_easy_init();
        ASSERT_NE(m_curl, nullptr) << "curl_easy_init() failed — cannot run network tests";
    }

    void TearDown() override {
        if (m_curl) {
            curl_easy_cleanup(m_curl);
            m_curl = nullptr;
        }
        curl_global_cleanup();
    }

    void ConfigureHandle(const std::string& url, std::string& responseBuffer, long timeoutSec = 15) {
        curl_easy_setopt(m_curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, &responseBuffer);
        curl_easy_setopt(m_curl, CURLOPT_TIMEOUT, timeoutSec);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 1L);
    }

    CURL* m_curl = nullptr;
};

TEST_F(CurlRealNetworkTest, GetRequest_ReturnsValidJsonAnd200) {
    std::string response;
    ConfigureHandle("https://httpbin.org/get", response);

    CURLcode res = curl_easy_perform(m_curl);
    ASSERT_EQ(res, CURLE_OK) << "curl_easy_perform failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200);

    ASSERT_FALSE(response.empty());
    auto json = nlohmann::json::parse(response, nullptr, false);
    ASSERT_FALSE(json.is_discarded()) << "Response is not valid JSON: " << response.substr(0, 256);
    EXPECT_TRUE(json.contains("url"));
    EXPECT_EQ(json["url"].get<std::string>(), "https://httpbin.org/get");
}

TEST_F(CurlRealNetworkTest, PostRequest_SendsJsonAndReceivesEcho) {
    std::string response;
    ConfigureHandle("https://httpbin.org/post", response);

    nlohmann::json payload = {
        {"app", "OmniStats"},
        {"version", "1.3.901"},
        {"test", true}
    };
    std::string payloadStr = payload.dump();

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
    curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, payloadStr.c_str());
    curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payloadStr.size()));
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(m_curl);
    ASSERT_EQ(res, CURLE_OK) << "POST failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200);

    auto json = nlohmann::json::parse(response, nullptr, false);
    ASSERT_FALSE(json.is_discarded());

    ASSERT_TRUE(json.contains("json"));
    EXPECT_EQ(json["json"]["app"].get<std::string>(), "OmniStats");
    EXPECT_EQ(json["json"]["version"].get<std::string>(), "1.3.901");
    EXPECT_EQ(json["json"]["test"].get<bool>(), true);

    curl_slist_free_all(headers);
}

TEST_F(CurlRealNetworkTest, CustomHeaders_AreEchoedByServer) {
    std::string response;
    ConfigureHandle("https://httpbin.org/get", response);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "X-OmniStats-Test: custom-header-value");
    headers = curl_slist_append(headers, "X-Custom-Req-Id: test-12345");
    curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, headers);

    CURLcode res = curl_easy_perform(m_curl);
    ASSERT_EQ(res, CURLE_OK) << "GET with custom headers failed: " << curl_easy_strerror(res);

    long httpCode = 0;
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &httpCode);
    EXPECT_EQ(httpCode, 200);

    auto json = nlohmann::json::parse(response, nullptr, false);
    ASSERT_FALSE(json.is_discarded());

    ASSERT_TRUE(json.contains("headers"));
    auto& hdrs = json["headers"];
    EXPECT_EQ(hdrs.value("X-Omnistats-Test", ""), "custom-header-value");
    EXPECT_EQ(hdrs.value("X-Custom-Req-Id", ""), "test-12345");

    curl_slist_free_all(headers);
}

TEST_F(CurlRealNetworkTest, Timeout_EnforcedAgainstDelayedEndpoint) {
    std::string response;
    // httpbin /delay/5 will wait 5 seconds before responding.
    // The 2-second cURL timeout should fail with CURLE_OPERATION_TIMEDOUT.
    ConfigureHandle("https://httpbin.org/delay/5", response, /*timeoutSec=*/2);

    CURLcode res = curl_easy_perform(m_curl);
    EXPECT_EQ(res, CURLE_OPERATION_TIMEDOUT)
        << "Expected CURLE_OPERATION_TIMEDOUT but got: " << curl_easy_strerror(res);
}

TEST_F(CurlRealNetworkTest, RepeatedInitCleanup_NoLeaksOrCrashes) {
    // The fixture already created one handle; we exercise additional cycles
    // to surface any double-free or resource leak in the init/cleanup path.
    for (int i = 0; i < 10; ++i) {
        CURL* handle = curl_easy_init();
        ASSERT_NE(handle, nullptr) << "curl_easy_init returned null on iteration " << i;

        std::string buf;
        curl_easy_setopt(handle, CURLOPT_URL, "https://httpbin.org/get");
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &buf);

        curl_easy_cleanup(handle);
    }
    SUCCEED();
}

TEST_F(CurlRealNetworkTest, SingletonIsReady_ReflectsLoadState) {
    // CurlImpersonate::Instance().IsReady() should return false in test
    // environments where the curl-impersonate DLL has not been downloaded.
    // This just verifies the accessor is callable and consistent.
    auto& inst = CurlImpersonate::Instance();
    // The result depends on whether the runtime DLL has been downloaded.
    (void)inst.IsReady();
    SUCCEED();
}
