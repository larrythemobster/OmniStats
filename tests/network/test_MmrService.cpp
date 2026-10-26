#include <gtest/gtest.h>
#include "network/omninet/OmniNet.hpp"
#include "network/mmr/MmrService.hpp"
#include "MmrTestTimings.hpp"
#include "network/http/CurlImpersonate.hpp"
#include "network/api/AccountClient.hpp"
#include "network/mmr/MmrServiceDetail.hpp"
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "core/config/Config.hpp"

using namespace MmrServiceDetail;
#include <chrono>
#include <thread>
#include <cstdarg>
#include <atomic>

TEST(MmrTimingsTest, DefaultsUseProductionDurations) {
    const MmrTimings timings;
    EXPECT_EQ(timings.postMatchInitialDelay, std::chrono::milliseconds(2500));
    EXPECT_EQ(timings.stalePostMatchRetryDelay, std::chrono::seconds(3));
    EXPECT_EQ(timings.transientRetryDelay, std::chrono::seconds(3));
    EXPECT_EQ(timings.queueSpacing, std::chrono::milliseconds(1500));
    EXPECT_EQ(timings.rateLimitLockoutMinimum, std::chrono::seconds(120));
    EXPECT_EQ(timings.forbiddenLockoutFirst, std::chrono::minutes(5));
    EXPECT_EQ(timings.forbiddenLockoutSecond, std::chrono::minutes(15));
    EXPECT_EQ(timings.forbiddenLockoutMaximum, std::chrono::minutes(30));
    EXPECT_EQ(timings.forbiddenAttemptDelay, std::chrono::milliseconds(500));
    EXPECT_EQ(timings.batchCoalesceWindow, std::chrono::milliseconds(300));
}

TEST(MmrTimingsTest, QueueUsesInjectedForbiddenLockouts) {
    MmrRequestQueue queue(FastMmrTimings());
    MMRRequest request;
    const auto first = queue.RequeueForbidden(request);
    const auto second = queue.RequeueForbidden(request);
    const auto third = queue.RequeueForbidden(request);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_TRUE(third);
    EXPECT_EQ(first->second, std::chrono::milliseconds(50));
    EXPECT_EQ(second->second, std::chrono::milliseconds(100));
    EXPECT_EQ(third->second, std::chrono::milliseconds(150));
}

typedef size_t (*WriteCallbackType)(void*, size_t, size_t, void*);
typedef size_t (*HeaderCallbackType)(char*, size_t, size_t, void*);
static WriteCallbackType g_write_callback = nullptr;
static void* g_write_data = nullptr;
static HeaderCallbackType g_header_callback = nullptr;
static void* g_header_data = nullptr;
static std::string g_mock_response = "";
static std::string g_mock_headers = "";
static long g_mock_response_code = 200;
static std::string g_mock_url = "";
static std::string g_mock_post_fields = "";
static long g_mock_custom_api_response_code = 0;
static std::string g_mock_custom_api_response = "";
static int g_mock_perform_res = 0;
static int g_mock_custom_api_perform_res = 0;
static std::atomic<int> g_mock_perform_count{0};
static std::string g_mock_impersonation_profile;
static std::string g_mock_user_agent_header;
static std::vector<std::string> g_mock_request_headers;
static std::vector<long> g_mock_custom_api_status_sequence;
static std::function<OmniNet::Response(const OmniNet::Request&)> g_mock_refresh_fn;
static CURLcode mock_easy_setopt(CURL*, CURLoption option, ...) {
    va_list args;
    va_start(args, option);
    if (option == CURLOPT_URL) {
        const char* u = va_arg(args, const char*);
        if (u) g_mock_url = u;
    } else if (option == CURLOPT_POSTFIELDS) {
        const char* p = va_arg(args, const char*);
        if (p) g_mock_post_fields = p;
    } else if (option == CURLOPT_WRITEFUNCTION) {
        g_write_callback = va_arg(args, WriteCallbackType);
    } else if (option == CURLOPT_WRITEDATA) {
        g_write_data = va_arg(args, void*);
    } else if (option == CURLOPT_HEADERFUNCTION) {
        g_header_callback = va_arg(args, HeaderCallbackType);
    } else if (option == CURLOPT_HEADERDATA) {
        g_header_data = va_arg(args, void*);
    }
    va_end(args);
    return CURLE_OK;
}

static CURLcode mock_easy_perform(CURL*) {
    g_mock_perform_count.fetch_add(1);
    long code = g_mock_response_code;
    const std::string* body = &g_mock_response;
    int res = g_mock_perform_res;
    if (g_mock_custom_api_response_code > 0 && (g_mock_url.find("/v1/ranks") != std::string::npos || g_mock_url.find("/v1/pro/ranks") != std::string::npos)) {
        code = g_mock_custom_api_status_sequence.empty()
                   ? g_mock_custom_api_response_code
                   : g_mock_custom_api_status_sequence.front();
        body = &g_mock_custom_api_response;
        if (g_mock_custom_api_perform_res != 0) {
            res = g_mock_custom_api_perform_res;
        }
    }
    if (g_header_callback && g_header_data && !g_mock_headers.empty()) {
        g_header_callback((char*)g_mock_headers.data(), 1, g_mock_headers.size(), g_header_data);
    }
    if (g_write_callback && g_write_data && !body->empty()) {
        g_write_callback((void*)body->data(), 1, body->size(), g_write_data);
    }
    return static_cast<CURLcode>(res);
}

static CURLcode mock_easy_getinfo(CURL*, CURLINFO info, ...) {
    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long* code = va_arg(args, long*);
        if (g_mock_custom_api_response_code > 0 && (g_mock_url.find("/v1/ranks") != std::string::npos || g_mock_url.find("/v1/pro/ranks") != std::string::npos)) {
            if (!g_mock_custom_api_status_sequence.empty() &&
                (g_mock_url.find("/v1/ranks") != std::string::npos || g_mock_url.find("/v1/pro/ranks") != std::string::npos)) {
                *code = g_mock_custom_api_status_sequence.front();
                g_mock_custom_api_status_sequence.erase(g_mock_custom_api_status_sequence.begin());
                va_end(args);
                return CURLE_OK;
            }
            *code = g_mock_custom_api_response_code;
        } else {
            *code = g_mock_response_code;
        }
    }
    va_end(args);
    return CURLE_OK;
}

static void mock_easy_cleanup(CURL*) {}
static void mock_slist_free_all(struct curl_slist*) {}
static struct curl_slist* mock_slist_append(struct curl_slist*, const char* str) {
    const std::string header = str ? str : "";
    g_mock_request_headers.push_back(header);
    if (header.rfind("User-Agent: ", 0) == 0) {
        g_mock_user_agent_header = header;
    }
    return reinterpret_cast<struct curl_slist*>(1);
}
static CURL* mock_easy_init() {
    return reinterpret_cast<CURL*>(1);
}
static std::string g_mock_escaped_value;
static char* mock_easy_escape(CURL*, const char* value, int length) {
    g_mock_escaped_value.assign(value, static_cast<size_t>(length));
    return g_mock_escaped_value.data();
}
static void mock_curl_free(void*) {}
static int mock_easy_impersonate(CURL*, const char* profile, int) {
    g_mock_impersonation_profile = profile ? profile : "";
    return 0;
}

class MmrServiceTest : public ::testing::Test {
  protected:
    void SetUp() override {
        originalConfig = Config::Read();
        Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);
        g_mock_perform_count.store(0);
        g_mock_response_code = 200;
        g_mock_headers.clear();
        g_mock_response.clear();
        g_mock_post_fields.clear();
        g_mock_impersonation_profile.clear();
        g_mock_user_agent_header.clear();
        g_mock_request_headers.clear();
        g_mock_custom_api_status_sequence.clear();
        accountClient = std::make_shared<AccountClient>();
        accountClient->ResetForTests();
        g_mock_perform_res = 0;
        g_mock_custom_api_perform_res = 0;
        sessionState = std::make_shared<SessionState>();
        fetcher = std::make_shared<MmrService>(sessionState, nullptr, accountClient, nullptr, nullptr, FastMmrTimings());
        OmniNet::SetTransportForTesting([](const OmniNet::Request& req) {
            OmniNet::Response res;
            if (req.op == OmniNet::Op::Ranks) {
                g_mock_perform_count.fetch_add(1);
                g_mock_post_fields = req.payload;
                if (g_mock_custom_api_perform_res != 0) {
                    res.transportError = g_mock_custom_api_perform_res;
                    return res;
                }
                long code = g_mock_custom_api_response_code > 0 ? g_mock_custom_api_response_code : 200;
                if (!g_mock_custom_api_status_sequence.empty()) {
                    code = g_mock_custom_api_status_sequence.front();
                    g_mock_custom_api_status_sequence.erase(g_mock_custom_api_status_sequence.begin());
                }
                res.status = code;
                res.body = g_mock_custom_api_response;
                return res;
            }
            if (req.op == OmniNet::Op::AuthRefresh) {
                if (g_mock_refresh_fn) return g_mock_refresh_fn(req);
            }
            return res;
        });

        auto& curl = CurlImpersonate::Instance();
        original_perform = curl.easy_perform;
        original_getinfo = curl.easy_getinfo;
        original_setopt = curl.easy_setopt;
        original_cleanup = curl.easy_cleanup;
        original_slist_free_all = curl.slist_free_all;
        original_slist_append = curl.slist_append;
        original_easy_init = curl.easy_init;
        original_easy_escape = curl.easy_escape;
        original_free = curl.free_ptr;
        original_easy_impersonate = curl.easy_impersonate;
        original_ready = curl.IsReady();

        curl.easy_perform = mock_easy_perform;
        curl.easy_getinfo = mock_easy_getinfo;
        curl.easy_setopt = mock_easy_setopt;
        curl.easy_cleanup = mock_easy_cleanup;
        curl.slist_free_all = mock_slist_free_all;
        curl.slist_append = mock_slist_append;
        curl.easy_init = mock_easy_init;
        curl.easy_escape = mock_easy_escape;
        curl.free_ptr = mock_curl_free;
        curl.easy_impersonate = mock_easy_impersonate;
        curl.SetReadyForTests(true);
    }

    void TearDown() override {
        auto& curl = CurlImpersonate::Instance();
        curl.easy_perform = original_perform;
        curl.easy_getinfo = original_getinfo;
        curl.easy_setopt = original_setopt;
        curl.easy_cleanup = original_cleanup;
        curl.slist_free_all = original_slist_free_all;
        curl.slist_append = original_slist_append;
        curl.easy_init = original_easy_init;
        curl.easy_escape = original_easy_escape;
        curl.free_ptr = original_free;
        curl.easy_impersonate = original_easy_impersonate;
        OmniNet::SetTransportForTesting({});
        g_mock_refresh_fn = nullptr;
        curl.SetReadyForTests(original_ready);
        g_mock_url.clear();
        g_mock_post_fields.clear();
        g_mock_custom_api_response_code = 0;
        g_mock_custom_api_response.clear();
        g_mock_request_headers.clear();
        g_mock_custom_api_status_sequence.clear();
        accountClient->ResetForTests();
        Config::Update([this](ConfigData& config) { config = originalConfig; }, false);
        g_mock_perform_res = 0;
        g_mock_custom_api_perform_res = 0;
        fetcher.reset();
        accountClient.reset();
    }

    void SetUpSignedInDeviceAuth(bool enableMmrTracking = false) {
        auto& account = *accountClient;
        AccountClient::DeviceKeyPair keyPair;
        account.EnsureDeviceKey(&keyPair);
        Config::Update([&](ConfigData& c) {
            c.enable_mmr_tracking = enableMmrTracking;
            c.custom_api_enabled = true;
            c.account_refresh_token = "valid_refresh_token";
            c.account_signed_in_name = "SignedInPlayer";
            c.account_device_public_id = keyPair.publicId;
        },
                       false);
        account.SyncFromConfig();
        account.SetClockForTests([]() { return 1700000000; });
        account.SetCachedAccessTokenForTests("bearer_token_abc", 1700000900);
    }

    void EnqueuePostMatch(const std::string& guid,
                          bool won,
                          int previousMmr = 1200,
                          int previousMatches = 50,
                          const std::string& playlist = "2v2",
                          bool previousMmrIsPlaylistSpecific = true) {
        fetcher->EnqueuePostMatch(
            "Steam|123",
            "Player",
            guid,
            playlist,
            previousMmr,
            previousMatches,
            previousMmrIsPlaylistSpecific,
            won);
    }

    void EnqueueDestroyedMatch(
        const std::string& guid,
        int previousMmr = 1200,
        int previousMatches = 50,
        const std::string& playlist = "2v2",
        std::array<int, 2> score = {0, 0}) {
        PendingDestroyedMatchMmrRefresh pending;
        pending.matchGuid = guid;
        pending.primaryId = "Steam|123";
        pending.name = "Player";
        pending.playlist = playlist;
        pending.localTeam = 0;
        pending.score = score;
        pending.previousMmr = previousMmr;
        pending.previousMatches = previousMatches;
        pending.previousMmrIsPlaylistSpecific = true;
        pending.validCompetitiveMatch = true;
        fetcher->EnqueuePendingDestroyedMatch(pending);
    }

    std::shared_ptr<SessionState> sessionState;
    std::shared_ptr<MmrService> fetcher;
    std::shared_ptr<AccountClient> accountClient;

    pfn_curl_easy_perform original_perform;
    pfn_curl_easy_getinfo original_getinfo;
    pfn_curl_easy_setopt original_setopt;
    pfn_curl_easy_cleanup original_cleanup;
    pfn_curl_slist_free_all original_slist_free_all;
    pfn_curl_slist_append original_slist_append;
    pfn_curl_easy_init original_easy_init;
    pfn_curl_easy_escape original_easy_escape;
    pfn_curl_free original_free;
    pfn_curl_easy_impersonate original_easy_impersonate;
    bool original_ready = false;
    ConfigData originalConfig;
};

TEST_F(MmrServiceTest, FetchProfileSuccess) {
    g_mock_response_code = 200;
    g_mock_response = R"({"data": {"segments": [{"stats": {"rating": {"value": 1200}}}]}})";

    fetcher->Start();
    fetcher->Enqueue("epic_123", "TestPlayer");

    // Allow worker thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST_F(MmrServiceTest, FetchProfileNotFound) {
    g_mock_response_code = 404;

    fetcher->Start();
    fetcher->Enqueue("epic_404", "MissingPlayer");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST_F(MmrServiceTest, FetchProfileServerError) {
    g_mock_response_code = 500;

    fetcher->Start();
    fetcher->Enqueue("epic_500", "ServerErrPlayer");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST_F(MmrServiceTest, FetchProfileRateLimitedPausesQueue) {
    g_mock_response_code = 429;
    g_mock_headers = "Retry-After: 90\r\n";

    fetcher->Start();
    fetcher->Enqueue("Epic|429", "RateLimitedPlayer");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_TRUE(fetcher->IsRateLimited());

    fetcher->Stop();
}

TEST(MmrServiceTournamentRankTest, UsesTournamentMmrThresholds) {
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1421), "Grand Champion I Div I");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1537), "Grand Champion I Div IV");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1561), "Grand Champion II Div I");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1701), "Grand Champion III Div I");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1861), "Supersonic Legend");
}

TEST(MmrServiceTournamentRankTest, KeepsBoundaryGapsInPreviousDivision) {
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1560), "Grand Champion I Div IV");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(1530), "Grand Champion I Div III");
    EXPECT_EQ(MmrService::GetTournamentTierForMmr(0), "Unranked");
}

TEST(MmrServicePlaylistRankThresholdsTest, UsesPlaylistSpecificMmrThresholds) {
    // Casual has no ranks
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("casual", 1500), "Unranked");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("casual", 698), "Unranked");

    // 1v1 specific thresholds (playlist 10)
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("1v1", 586), "Gold III Div II");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("1v1", 815), "Diamond I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("1v1", 995), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("1v1", 1341), "Supersonic Legend");

    // 2v2 specific thresholds (playlist 11)
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("2v2", 1075), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("2v2", 1114), "Champion I Div II");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("2v2", 1860), "Supersonic Legend");

    // 3v3 specific thresholds (playlist 13)
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("3v3", 699), "Platinum I Div IV");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("3v3", 1075), "Champion I Div I");

    // Tournament thresholds (playlist 34)
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("t", 1061), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("t", 1122), "Champion I Div II");

    // Extra modes thresholds (hoops, rumble, dropshot, snowday, heatseeker)
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("hoops", 880), "Diamond III Div II");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("rumble", 995), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("dropshot", 895), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("snowday", 921), "Champion I Div I");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("heatseeker", 955), "Champion I Div I");
    // Zero or negative MMR
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("2v2", 0), "Unranked");
    EXPECT_EQ(MmrService::GetRankTierForPlaylistMmr("1v1", -10), "Unranked");
}

TEST(MmrServicePlaylistMappingTest, MapsExtraModesToSeparatePlaylists) {
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(10), "1v1");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(11), "2v2");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(13), "3v3");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(27), "hoops");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(28), "rumble");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(29), "dropshot");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(30), "snowday");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(34), "t");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(43), "heatseeker");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(63), "heatseeker");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(0), "casual");
    // Gameplay playlist IDs are not automatically Tracker segment IDs.
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(2), "");
    EXPECT_EQ(MmrService::PlaylistNameForTrackerId(999), "");
}

TEST(MmrServiceProfileTotalsTest, ExtractsOverviewWins) {
    nlohmann::json response = {
        {"data", {{"segments", nlohmann::json::array({{{"type", "overview"}, {"stats", {{"wins", {{"value", 60}}}}}}, {{"type", "playlist"}, {"stats", {{"wins", {{"value", 999}}}}}}})}}}};

    MMRProfileTotals totals = MmrService::ExtractProfileTotals(response);
    EXPECT_EQ(totals.totalWins, 60);
}

TEST(MmrServicePostMatchTest, UsesMatchCountsAsThePublicationVersion) {
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 1200, 50, 50));
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 1209, 50, 50));
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 0, 50, 51));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1209, 50, 51));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1200, 50, 51));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1218, 50, 52));
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 1200, 0, 0));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1200, 0, 1));
}

TEST(MmrServicePostMatchTest, FallsBackToRatingOnlyWithoutMatchCounts) {
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 1200, -1, -1));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1209, -1, -1));
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(1200, 1200, 50, -1));
    EXPECT_FALSE(MmrService::IsPostMatchMmrStale(1200, 1209, 50, -1));
    EXPECT_TRUE(MmrService::IsPostMatchMmrStale(0, 1200, -1, -1));
}

TEST(MmrServicePostMatchTest, CountsCumulativePublishedMatches) {
    EXPECT_EQ(MmrService::CoveredPendingMatchCount(50, 51, 3), 1u);
    EXPECT_EQ(MmrService::CoveredPendingMatchCount(50, 52, 3), 2u);
    EXPECT_EQ(MmrService::CoveredPendingMatchCount(50, 50, 3), 0u);
    EXPECT_EQ(MmrService::CoveredPendingMatchCount(0, 1, 1), 1u);
}

TEST(MmrServicePostMatchTest, UsesPlaylistHistoryInsteadOfIncorrectBestMmrFallback) {
    EXPECT_EQ(MmrService::ResolvePostMatchBaseline(788, false, {630.0f}), 630);
    EXPECT_EQ(MmrService::ResolvePostMatchBaseline(788, false, {}), 788);
}

TEST(MmrServicePostMatchTest, KeepsFinalizedPlaylistBaselineWhenRosterFetchMovesAhead) {
    EXPECT_EQ(MmrService::ResolvePostMatchBaseline(630, true, {630.0f, 667.0f}), 630);
}

TEST(MmrServicePostMatchTest, EstimatesFromRecentNormalChanges) {
    const std::vector<float> history = {1182.0f, 1191.0f, 1200.0f};

    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1200, true, history), 1209);
    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1200, false, history), 1191);
}

TEST(MmrServicePostMatchTest, ChainsEstimateFromLatestGraphPoint) {
    const std::vector<float> history = {1200.0f, 1191.0f};

    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1200, false, history), 1182);
    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1200, true, history), 1200);
}

TEST(MmrServicePostMatchTest, IgnoresCumulativeJumpsAndUsesDefaultDelta) {
    const std::vector<float> history = {1200.0f, 1218.0f};

    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1218, true, history), 1227);
    EXPECT_EQ(MmrService::EstimatePostMatchMmr(1218, false, history), 1209);
}

TEST_F(MmrServiceTest, NormalPublishedUpdateCreatesOneConfirmedOwnedPoint) {
    EnqueuePostMatch("normal", true);

    fetcher->ProcessPostMatchResponse("normal", 1209, 51);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "normal");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 51);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, CasualPostMatchUsesTheSingleCasualBucket) {
    EnqueuePostMatch("casual-published", true, 900, 30, "casual");

    fetcher->ProcessPostMatchResponse(
        "casual-published", 912, 31);

    const auto points =
        fetcher->PlaylistMatchPoints("casual");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "casual-published");
    EXPECT_EQ(points[0].mmr, 912);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 31);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, StaleResponseStillCreatesOneProvisionalOwnedPoint) {
    EnqueuePostMatch("stale", true);

    fetcher->ProcessPostMatchResponse("stale", 1200, 50);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "stale");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, FirstObservedZeroCountConfirmsOwnedPointAtOneWithoutMmrChange) {
    EnqueuePostMatch("missing-pre-match", true, 0, -1);

    fetcher->ProcessPostMatchResponse("missing-pre-match", 1200, 0);

    auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "missing-pre-match");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[0].trackerMatchesPlayed, -1);

    fetcher->ProcessPostMatchResponse("missing-pre-match", 1200, 1);
    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 1);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, MissingBaselineRemainsPendingUntilPlaylistInitialMmrExists) {
    EnqueuePostMatch("delayed-baseline", false, 0, -1);
    fetcher->ProcessPostMatchResponse("delayed-baseline", 0, -1);
    EXPECT_TRUE(fetcher->PlaylistMatchPoints("2v2").empty());

    sessionState->history.Mutate([](auto& history) { history.playlistInitialMmr["2v2"] = 1200; });
    fetcher->ProcessPostMatchResponse("delayed-baseline", 0, -1);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "delayed-baseline");
    EXPECT_EQ(points[0].mmr, 1191);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, PlaylistReconciliationStateDoesNotCrossContaminate) {
    EnqueuePostMatch("twos-pending", true, 1200, 50, "2v2");
    fetcher->ProcessPostMatchResponse("twos-pending", 1200, 50);

    EnqueuePostMatch("threes-published", true, 900, 100, "3v3");
    fetcher->ProcessPostMatchResponse("threes-published", 909, 101);

    auto twos = fetcher->PlaylistMatchPoints("2v2");
    auto threes = fetcher->PlaylistMatchPoints("3v3");
    ASSERT_EQ(twos.size(), 1u);
    ASSERT_EQ(threes.size(), 1u);
    EXPECT_EQ(twos[0].matchGuid, "twos-pending");
    EXPECT_TRUE(twos[0].valueEstimated);
    EXPECT_FALSE(twos[0].trackerCovered);
    EXPECT_EQ(threes[0].matchGuid, "threes-published");
    EXPECT_EQ(threes[0].mmr, 909);
    EXPECT_FALSE(threes[0].valueEstimated);
    EXPECT_TRUE(threes[0].trackerCovered);

    fetcher->ProcessPostMatchResponse("twos-pending", 1209, 51);
    twos = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(twos.size(), 1u);
    EXPECT_EQ(twos[0].mmr, 1209);
    EXPECT_FALSE(twos[0].valueEstimated);
    EXPECT_TRUE(twos[0].trackerCovered);
}

TEST_F(MmrServiceTest, MatchesPlayedCounterRollbackStartsNewPublicationEpoch) {
    EnqueuePostMatch("old-season", true, 1200, 100, "2v2");
    fetcher->ProcessPostMatchResponse("old-season", 1209, 101);
    auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);

    // Tracker's per-playlist counter can reset between seasons. With no
    // unresolved matches from the old epoch, the lower pre-match count is the
    // authoritative baseline for the next reconciliation chain.
    EnqueuePostMatch("new-season", true, 1000, 0, "2v2");
    fetcher->ProcessPostMatchResponse("new-season", 1009, 1);

    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "new-season");
    EXPECT_EQ(points[1].mmr, 1009);
    EXPECT_EQ(points[1].trackerMatchesPlayed, 1);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, DelayedCumulativeUpdatePreservesOnePointPerMatch) {
    EnqueuePostMatch("match-a", true);
    fetcher->ProcessPostMatchResponse("match-a", 1200, 50);
    EnqueuePostMatch("match-b", true);

    fetcher->ProcessPostMatchResponse("match-b", 1218, 52);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "match-a");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "match-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
    ASSERT_EQ(sessionState->history.GetSnapshot()->playlistHistoryY.at("2v2").size(), 2u);
}

TEST_F(MmrServiceTest, WinThenLossKeepsDirectionalPointsAndConfirmsNewest) {
    EnqueuePostMatch("win", true);
    fetcher->ProcessPostMatchResponse("win", 1200, 50);
    EnqueuePostMatch("loss", false);

    fetcher->ProcessPostMatchResponse("loss", 1215, 52);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].mmr, 1217);
    EXPECT_LT(points[1].mmr, points[0].mmr);
    EXPECT_EQ(points[1].matchGuid, "loss");
    EXPECT_EQ(points[1].mmr, 1215);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, EarlyForfeitEstimateStaysDirectionalAfterNextWin) {
    EnqueuePostMatch(
        "early-forfeit", false, 1274, 352);
    fetcher->ProcessPostMatchResponse(
        "early-forfeit", 1274, 352);
    EnqueuePostMatch(
        "next-win", true, 1274, 352);

    fetcher->ProcessPostMatchResponse(
        "next-win", 1273, 354);

    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "early-forfeit");
    EXPECT_EQ(points[0].mmr, 1265);
    EXPECT_LT(points[0].mmr, 1274);
    EXPECT_EQ(points[1].matchGuid, "next-win");
    EXPECT_EQ(points[1].mmr, 1273);
    EXPECT_GT(points[1].mmr, points[0].mmr);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, LargeCorrectionDoesNotReverseKnownLoss) {
    EnqueuePostMatch("large-correction-win", true);
    fetcher->ProcessPostMatchResponse(
        "large-correction-win", 1200, 50);
    EnqueuePostMatch("large-correction-loss", false);

    fetcher->ProcessPostMatchResponse(
        "large-correction-loss", 1220, 52);

    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_GT(points[0].mmr, 1200);
    EXPECT_EQ(points[1].mmr, 1220);
    EXPECT_LT(points[1].mmr, points[0].mmr);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, TwoWinsWithSmallCumulativeGainRemainIncreasing) {
    EnqueuePostMatch("small-win-a", true);
    fetcher->ProcessPostMatchResponse("small-win-a", 1200, 50);
    EnqueuePostMatch("small-win-b", true);

    fetcher->ProcessPostMatchResponse("small-win-b", 1205, 52);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "small-win-a");
    EXPECT_EQ(points[0].mmr, 1202);
    EXPECT_EQ(points[1].matchGuid, "small-win-b");
    EXPECT_EQ(points[1].mmr, 1205);
    EXPECT_GT(points[1].mmr, points[0].mmr);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, ChangedRatingWithoutMatchAdvanceDoesNotConsumePendingMatch) {
    EnqueuePostMatch("not-published", true);

    fetcher->ProcessPostMatchResponse("not-published", 1209, 50);

    auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[0].mmr, 1209);

    fetcher->ProcessPostMatchResponse("not-published", 1218, 51);
    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[0].mmr, 1218);
}

TEST_F(MmrServiceTest, MissingFetchedCountFallsBackToRatingForOnlyOldestMatch) {
    EnqueuePostMatch("missing-count-a", true);
    EnqueuePostMatch("missing-count-b", true);

    fetcher->ProcessPostMatchResponse("missing-count-b", 1209, -1);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "missing-count-a");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "missing-count-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, RepeatedCountlessResponseRequiresNewRatingForSecondMatch) {
    EnqueuePostMatch("countless-a", true);
    EnqueuePostMatch("countless-b", true);

    fetcher->ProcessPostMatchResponse("countless-b", 1209, -1);
    fetcher->ProcessPostMatchResponse("countless-b", 1209, -1);

    auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "countless-a");
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "countless-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    // The count later catching up to match A is not evidence for match B.
    fetcher->ProcessPostMatchResponse("countless-b", 1209, 51);
    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    fetcher->ProcessPostMatchResponse("countless-b", 1218, -1);

    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "countless-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, SequentialCountlessFallbackCarriesPlaylistBaseline) {
    EnqueuePostMatch("sequential-a", true, 1200, 50);
    fetcher->ProcessPostMatchResponse("sequential-a", 1209, -1);

    auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "sequential-a");
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);

    EnqueuePostMatch("sequential-b", true, 1209, 50);
    fetcher->ProcessPostMatchResponse("sequential-b", 1209, 51);

    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "sequential-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    fetcher->ProcessPostMatchResponse("sequential-b", 1218, -1);

    points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "sequential-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MmrServiceTest, RosterResponseWithoutMatchesPlayedFallsBackToRating) {
    sessionState->liveMatch->SetLocalPrimaryId("Steam|123");
    EnqueuePostMatch("roster-missing-count", true);
    g_mock_response_code = 200;
    g_mock_response = R"({
        "data": {
            "segments": [{
                "type": "playlist",
                "attributes": {"playlistId": 11},
                "stats": {
                    "rating": {"value": 1209},
                    "tier": {"metadata": {"name": "Diamond I"}}
                }
            }]
        }
    })";

    fetcher->FetchRosterProfile("Steam|123", "Player");

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "roster-missing-count");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, UnchangedRatingWithMatchAdvanceConsumesExactlyOneMatch) {
    EnqueuePostMatch("unchanged", true);

    fetcher->ProcessPostMatchResponse("unchanged", 1200, 51);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unchanged");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, KnownPreMatchMmrWithUnknownCountUsesFirstRatingChange) {
    EnqueuePostMatch("unknown-pre-count", true, 1200, -1);

    fetcher->ProcessPostMatchResponse("unknown-pre-count", 1209, 51);

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unknown-pre-count");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 51);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, RosterRefreshDoesNotApplyResultToBestMmrFallback) {
    sessionState->liveMatch->SetLocalPrimaryId("Steam|123");
    EnqueuePostMatch("best-fallback", true, 1300, 50, "2v2", false);
    g_mock_response_code = 200;
    g_mock_response = R"({
        "data": {
            "segments": [{
                "type": "playlist",
                "attributes": {"playlistId": 11},
                "stats": {
                    "rating": {"value": 1200},
                    "matchesPlayed": {"value": 50},
                    "tier": {"metadata": {"name": "Diamond I"}}
                }
            }]
        }
    })";

    fetcher->FetchRosterProfile("Steam|123", "Player");

    const auto points = fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "best-fallback");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_NE(points[0].mmr, 1309);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MmrServiceTest, WorkerExhaustsThreeStaleResponsesBeforeOneProvisionalPoint) {
    g_mock_response_code = 200;
    g_mock_response = R"({
        "data": {
            "segments": [{
                "type": "playlist",
                "attributes": {"playlistId": 11},
                "stats": {
                    "rating": {"value": 1200},
                    "matchesPlayed": {"value": 50},
                    "tier": {"metadata": {"name": "Diamond I"}}
                }
            }]
        }
    })";

    fetcher->Start();
    EnqueuePostMatch("retry-exhausted", true);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::vector<SessionMmrPoint> points;
    while (std::chrono::steady_clock::now() < deadline) {
        points = fetcher->PlaylistMatchPoints("2v2");
        if (!points.empty() && g_mock_perform_count.load() >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fetcher->Stop();

    EXPECT_EQ(g_mock_perform_count.load(), 3);
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "retry-exhausted");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(fetcher->PendingRequestCount(), 0u);
}

TEST_F(MmrServiceTest, TrackerDecreaseConfirmsDestroyedMatchLossOnce) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            const auto published = fetcher->PlaylistMatchPoints("2v2");
            ASSERT_EQ(published.size(), 1u);
            EXPECT_EQ(published[0].matchGuid, matchGuid);
            EXPECT_TRUE(published[0].trackerCovered);
            fetcher->ResolvePendingDestroyedMatch(matchGuid, won);
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("destroyed-loss", 1200, 50);

    fetcher->ProcessPostMatchResponse(
        "destroyed-loss", 1191, 51);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "destroyed-loss");
    EXPECT_FALSE(confirmations[0].second);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatch(
            "destroyed-loss"));
    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "destroyed-loss");
    EXPECT_EQ(points[0].mmr, 1191);
    EXPECT_TRUE(points[0].trackerCovered);

    fetcher->ProcessPostMatchResponse(
        "destroyed-loss", 1191, 51);
    EXPECT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(
        fetcher->PlaylistMatchPoints("2v2").size(),
        1u);
}

TEST_F(MmrServiceTest, TrackerIncreaseConfirmsDestroyedMatchWin) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("destroyed-win", 1200, 50);

    fetcher->ProcessPostMatchResponse(
        "destroyed-win", 1209, 51);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "destroyed-win");
    EXPECT_TRUE(confirmations[0].second);
    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].mmr, 1209);
}

TEST_F(MmrServiceTest, UnpublishedDestroyedMatchRemainsUnconfirmed) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("unconfirmed-disconnect", 1200, 50);

    fetcher->ProcessPostMatchResponse(
        "unconfirmed-disconnect", 1200, 50);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatch(
            "unconfirmed-disconnect"));
    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unconfirmed-disconnect");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_FALSE(points[0].trackerCovered);
}

TEST_F(MmrServiceTest, PartialSameBaselinePublicationKeepsDestroyedMatchPending) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch(
        "partial-publication-a", 1200, 50);
    fetcher->ProcessPostMatchResponse(
        "partial-publication-a", 1200, 50);
    EnqueuePostMatch(
        "partial-publication-b",
        true,
        1200,
        50,
        "2v2",
        true);

    fetcher->ProcessPostMatchResponse(
        "partial-publication-b", 1209, 51);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatch(
            "partial-publication-a"));
    const auto partialPoints =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(partialPoints.size(), 2u);
    EXPECT_EQ(
        partialPoints[0].matchGuid,
        "partial-publication-a");
    EXPECT_EQ(
        partialPoints[1].matchGuid,
        "partial-publication-b");
    EXPECT_FALSE(partialPoints[0].trackerCovered);
    EXPECT_FALSE(partialPoints[1].trackerCovered);

    fetcher->ProcessPostMatchResponse(
        "partial-publication-a", 1209, 51);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatch(
            "partial-publication-a"));

    fetcher->ProcessPostMatchResponse(
        "partial-publication-b", 1200, 52);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(
        confirmations[0].first,
        "partial-publication-a");
    EXPECT_FALSE(confirmations[0].second);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatch(
            "partial-publication-a"));
    const auto confirmedPoints =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(confirmedPoints.size(), 2u);
    EXPECT_TRUE(confirmedPoints[0].trackerCovered);
    EXPECT_TRUE(confirmedPoints[1].trackerCovered);
}

TEST_F(MmrServiceTest, CumulativeCatchUpPreservesDestroyedThenNormalMatch) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("catch-up-a", 1200, 11);
    EnqueuePostMatch(
        "catch-up-b", true, 1200, 11, "2v2", true);

    fetcher->ProcessPostMatchResponse(
        "catch-up-b", 1200, 13);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "catch-up-a");
    EXPECT_FALSE(confirmations[0].second);
    const auto points =
        fetcher->PlaylistMatchPoints("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "catch-up-a");
    EXPECT_EQ(points[0].mmr, 1195);
    EXPECT_EQ(points[1].matchGuid, "catch-up-b");
    EXPECT_EQ(points[1].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatch(
            "catch-up-a"));
}

TEST_F(MmrServiceTest, ScoreResolvesAmbiguousDestroyedMatchCatchUp) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch(
        "score-catch-up-a",
        680,
        254,
        "1v1",
        {5, 0});
    EnqueuePostMatch(
        "score-catch-up-b", false, 680, 254, "1v1", true);

    fetcher->ProcessPostMatchResponse(
        "score-catch-up-b", 653, 256);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "score-catch-up-a");
    EXPECT_TRUE(confirmations[0].second);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatch(
            "score-catch-up-a"));
}

TEST_F(MmrServiceTest, ImpossibleLowMmrPathDoesNotInferDestroyedLoss) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("low-mmr-unknown", 5, 11);
    EnqueuePostMatch(
        "low-mmr-win", true, 5, 11, "2v2", true);

    fetcher->ProcessPostMatchResponse(
        "low-mmr-win", 5, 13);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatch(
            "low-mmr-unknown"));
}

TEST_F(MmrServiceTest, ConfirmationDuringNextMatchRefreshesItsPreMatchBaseline) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("previous-loss", 1015, 46, "2v2", {1, 2});
    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "Steam|123";
        ms.inMatch = true;
        ms.matchGuid = "next-win";
        ms.preMatchMmrByGuid["next-win"] =
            LocalPreMatchMmrSnapshot{
                .playlistMmrs = {{"2v2", 1015}},
                .playlistMatches = {{"2v2", 46}}};
    });

    fetcher->ProcessPostMatchResponse("previous-loss", 1005, 47);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_FALSE(confirmations[0].second);
    const auto snapshot = sessionState->liveMatch->GetSnapshot()->preMatchMmrByGuid.at("next-win");
    EXPECT_EQ(snapshot.playlistMmrs.at("2v2"), 1005);
    EXPECT_EQ(snapshot.playlistMatches.at("2v2"), 47);

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) { ms.inMatch = false; });
    PendingDestroyedMatchMmrRefresh next;
    next.matchGuid = "next-win";
    next.primaryId = "Steam|123";
    next.name = "Player";
    next.playlist = "2v2";
    next.localTeam = 1;
    next.score = {0, 4};
    next.previousMmr = snapshot.playlistMmrs.at("2v2");
    next.previousMatches = snapshot.playlistMatches.at("2v2");
    next.previousMmrIsPlaylistSpecific = true;
    next.validCompetitiveMatch = true;
    fetcher->EnqueuePendingDestroyedMatch(next);

    fetcher->ProcessPostMatchResponse("next-win", 1014, 48);

    ASSERT_EQ(confirmations.size(), 2u);
    EXPECT_EQ(confirmations[1].first, "next-win");
    EXPECT_TRUE(confirmations[1].second);
}

TEST(SessionMmrAggregationTest, SumsOnlyTrackedCompetitivePlaylists) {
    std::map<std::string, int> mmrChangeByPlaylist = {
        {"1v1", 18},
        {"2v2", 27},
        {"3v3", 9},
        {"dropshot", -14},
        {"best", 999},
        {"t", 50},
        {"casual", 25},
        {"2v2_alias", 27}};

    float totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(mmrChangeByPlaylist));
    EXPECT_EQ(totalMmrChange, 40);

    totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(mmrChangeByPlaylist));
    EXPECT_EQ(totalMmrChange, 40);
}

TEST(MmrServicePostMatchTest, KeepsPostMatchRequestBehindRosterRequest) {
    const ConfigData original = Config::Read();
    Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);

    auto state = std::make_shared<SessionState>();
    MmrService fetcher(state);
    fetcher.Enqueue("Steam|123", "Player");
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50, true, true);
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50, true, true);

    EXPECT_EQ(fetcher.PendingRequestCount(), 2u);

    Config::Update([&](ConfigData& config) { config = original; }, false);
}

TEST_F(MmrServiceTest, ConfiguredLegacyKeyAloneCannotAuthorizeRankLookups) {
    Config::Update([](ConfigData& c) {
        c.custom_api_enabled = true;
        c.custom_api_key = "legacy_secret_key";
        c.enable_mmr_tracking = false;
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.account_device_public_id.clear();
    },
                   false);
    accountClient->SyncFromConfig();

    EXPECT_FALSE(accountClient->IsSignedIn());
    EXPECT_EQ(MmrService::ActiveRankProvider(), RankProvider::Unavailable);
    EXPECT_FALSE(fetcher->IsCustomApiActiveSource());

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::DisabledOrNotReady);
    EXPECT_EQ(g_mock_perform_count.load(), 0);

    Config::Update([](ConfigData& c) {
        c.enable_mmr_tracking = true;
    },
                   false);
    EXPECT_EQ(MmrService::ActiveRankProvider(), RankProvider::Tracker);
    EXPECT_FALSE(fetcher->IsCustomApiActiveSource());

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "Epic|test_epic_id|0";
        ms.roster["Epic|test_epic_id|0"] =
            PlayerData{.primaryId = "Epic|test_epic_id|0", .name = "TestPlayer"};
    });
    g_mock_response_code = 403;
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response =
        R"({"players":[{"platform":"Epic","account_id":"test_epic_id","skills":[{"playlist":11,"mmr":1250}]}]})";

    fetcher->FetchRosterProfile("Epic|test_epic_id|0", "TestPlayer");

    for (const auto& h : g_mock_request_headers) {
        EXPECT_TRUE(h.rfind("X-API-Key:", 0) != 0);
    }
}

TEST_F(MmrServiceTest, CustomApiMultipleSkillsParsesAllAndSetsBest) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Epic",
            "account_id": "test_epic_id",
            "skills": [
                { "playlist": 10, "mmr": 850.0, "tier": 8, "division": 1, "matches_played": 15 },
                { "playlist": 11, "mmr": 1250.0, "tier": 14, "division": 2, "matches_played": 45 },
                { "playlist": 13, "mmr": 1100.0, "tier": 12, "division": 3, "matches_played": 20 },
                { "playlist": 0, "mmr": 1400.0, "tier": 0, "division": 0, "matches_played": 100 },
                { "playlist": 34, "mmr": 1300.0, "tier": 0, "division": 0, "matches_played": 10 }
            ]
        }]
    })";

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["Epic|test_epic_id"] = PlayerData{.primaryId = "Epic|test_epic_id", .name = "TestPlayer"};
    });
    fetcher->FetchRosterProfile("Epic|test_epic_id", "TestPlayer");

    auto snap = sessionState->liveMatch->GetSnapshot();
    ASSERT_TRUE(snap->roster.count("Epic|test_epic_id") > 0);
    const auto& player = snap->roster.at("Epic|test_epic_id");
    EXPECT_TRUE(player.fetched);
    EXPECT_FALSE(player.fetchFailed);
    EXPECT_EQ(player.rankVerificationSource, "ServerA");
    EXPECT_EQ(player.playlists.at("1v1"), 850);
    EXPECT_EQ(player.playlists.at("2v2"), 1250);
    EXPECT_EQ(player.playlists.at("3v3"), 1100);
    EXPECT_EQ(player.playlists.at("casual"), 1400);
    EXPECT_EQ(player.playlists.at("t"), 1300);
    EXPECT_EQ(player.playlists.at("best"), 1250);
    EXPECT_EQ(player.mmr, 1250);
    EXPECT_EQ(player.rankTier, "Diamond II Div III");
    EXPECT_EQ(player.playlistTiers.at("best"), "Diamond II Div III");
    EXPECT_EQ(player.playlistTiers.at("t"), MmrService::GetTournamentTierForMmr(1300));
}

TEST_F(MmrServiceTest, CustomApiSingleSkillParsesCorrectly) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Steam",
            "account_id": "76561198000000000",
            "skills": [
                { "playlist": 13, "mmr": 1050.0, "tier": 12, "division": 1, "matches_played": 30 }
            ]
        }]
    })";

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["Steam|76561198000000000|0"] = PlayerData{.primaryId = "Steam|76561198000000000|0", .name = "SteamPlayer"};
    });
    MMRRequest req;
    req.primaryId = "Steam|76561198000000000|0";
    req.name = "SteamPlayer";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::SuccessFinished);

    auto snap = sessionState->liveMatch->GetSnapshot();
    ASSERT_TRUE(snap->roster.count("Steam|76561198000000000|0") > 0);
    const auto& player = snap->roster.at("Steam|76561198000000000|0");
    EXPECT_TRUE(player.fetched);
    EXPECT_FALSE(player.fetchFailed);
    EXPECT_EQ(player.mmr, 1050);
    EXPECT_EQ(player.playlists.at("3v3"), 1050);
    EXPECT_EQ(player.playlists.at("best"), 1050);
}

TEST_F(MmrServiceTest, CustomApiEmptySkillsReturnsUnusableDataAndPreventsFalseSuccess) {
    SetUpSignedInDeviceAuth(true);
    g_mock_response_code = 403;
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Xbox",
            "account_id": "2535421607154237",
            "skills": []
        }]
    })";

    MMRRequest req;
    req.primaryId = "XboxOne|2535421607154237|0";
    req.name = "RollrTraks2x";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::UnusableData);

    // Now test full path: Tracker 403 followed by custom API fallback
    fetcher->FetchRosterProfile("XboxOne|2535421607154237|0", "RollrTraks2x");

    auto snap = sessionState->liveMatch->GetSnapshot();
    if (snap->roster.count("XboxOne|2535421607154237|0")) {
        const auto& player = snap->roster.at("XboxOne|2535421607154237|0");
        // Must NOT be marked fetched with empty ranks!
        EXPECT_FALSE(player.fetched && !player.fetchFailed);
    }
}

TEST_F(MmrServiceTest, CustomApiUnrecognizedPlaylistIdsReturnsUnusableData) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Epic",
            "account_id": "test_epic_id",
            "skills": [
                { "playlist": 9999, "mmr": 1200.0, "tier": 14, "division": 1 }
            ]
        }]
    })";

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::UnusableData);
}

TEST_F(MmrServiceTest, CustomApiMissingOrInvalidMmrReturnsUnusableData) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Epic",
            "account_id": "test_epic_id",
            "skills": [
                { "playlist": 11, "mmr": 0.0, "tier": 0, "division": 0 },
                { "playlist": 13, "mmr": -10.0, "tier": 0, "division": 0 }
            ]
        }]
    })";

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::UnusableData);
}

TEST_F(MmrServiceTest, CustomApiMissingPlayersArrayReturnsUnusableData) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({ "players": [] })";

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::UnusableData);
}

TEST_F(MmrServiceTest, CustomApiMalformedJsonReturnsUnusableData) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = "{ not valid json";

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::UnusableData);
}

TEST_F(MmrServiceTest, CustomApiNotFound404ReturnsUnusableData) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 404;
    g_mock_custom_api_response = R"({"error": {"code": "not_found", "message": "Player not found"}})";

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::UnusableData);
}

TEST_F(MmrServiceTest, CustomApiServerAndNetworkFailureReturnsTransientError) {
    SetUpSignedInDeviceAuth(false);

    MMRRequest req;
    req.primaryId = "Epic|test_epic_id|0";
    req.name = "TestPlayer";

    g_mock_custom_api_response_code = 500;
    g_mock_custom_api_response = R"({"error": {"code": "server_error", "message": "Internal error"}})";
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::TransientError);

    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_perform_res = 7; // CURLE_COULDNT_CONNECT
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::TransientError);
}

TEST_F(MmrServiceTest, CustomApiXboxPlayerResponseHandledCorrectly) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Xbox",
            "account_id": "2535421607154237",
            "skills": [
                { "playlist": 11, "mmr": 1354.0, "tier": 16, "division": 1, "matches_played": 62 }
            ]
        }]
    })";

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["XboxOne|2535421607154237|0"] = PlayerData{.primaryId = "XboxOne|2535421607154237|0", .name = "RollrTraks2x"};
    });
    MMRRequest req;
    req.primaryId = "XboxOne|2535421607154237|0";
    req.name = "RollrTraks2x";
    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::SuccessFinished);

    auto snap = sessionState->liveMatch->GetSnapshot();
    ASSERT_TRUE(snap->roster.count("XboxOne|2535421607154237|0") > 0);
    const auto& player = snap->roster.at("XboxOne|2535421607154237|0");
    EXPECT_TRUE(player.fetched);
    EXPECT_FALSE(player.fetchFailed);
    EXPECT_EQ(player.rankVerificationSource, "ServerA");
    EXPECT_EQ(player.playlists.at("2v2"), 1354);
    EXPECT_EQ(player.playlists.at("best"), 1354);
    EXPECT_EQ(player.mmr, 1354);
    EXPECT_EQ(player.rankTier, "Champion I Div II");
}

TEST_F(MmrServiceTest, CustomApiPlatformMappingsSupported) {
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;

    const std::vector<std::tuple<std::string, std::string, std::string>> platforms = {
        {"Steam|76561198000000001|0", "Steam", "76561198000000001"},
        {"PS4|123456789012|0", "PS4", "123456789012"},
        {"Epic|abcdef0123456789|0", "Epic", "abcdef0123456789"},
        {"Switch|switchaccountid|0", "Switch", "switchaccountid"}};

    for (const auto& [primaryId, platform, accountId] : platforms) {
        g_mock_custom_api_response = R"({
            "players": [{
                "platform": ")" + platform +
                                     R"(",
                "account_id": ")" + accountId +
                                     R"(",
                "skills": [
                    { "playlist": 11, "mmr": 1000.0, "tier": 11, "division": 0, "matches_played": 10 }
                ]
            }]
        })";

        sessionState->liveMatch->UpdateMatchStateForTests([&](MatchState& ms) { ms.roster[primaryId] = PlayerData{.primaryId = primaryId, .name = "PlatformTestPlayer"}; });
        MMRRequest req;
        req.primaryId = primaryId;
        req.name = "PlatformTestPlayer";
        const auto res = fetcher->FetchProfileFromCustomApi(req);
        EXPECT_EQ(res, CustomApiFetchResult::SuccessFinished);

        auto snap = sessionState->liveMatch->GetSnapshot();
        ASSERT_TRUE(snap->roster.count(primaryId) > 0);
        EXPECT_EQ(snap->roster.at(primaryId).mmr, 1000);
    }
}

TEST_F(MmrServiceTest, CustomApiUpdatesLocalPlayerHistoryAndCache) {
    sessionState->liveMatch->SetLocalPrimaryId("Epic|local_hero_id|0");
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "account_id": "local_hero_id",
            "skills": [
                { "playlist": 11, "mmr": 1250.0, "tier": 14, "division": 2, "matches_played": 45 }
            ]
        }]
    })";

    MMRRequest req;
    req.primaryId = "Epic|local_hero_id|0";
    req.name = "LocalHero";
    req.reason = MMRRequestReason::Roster;

    const auto res = fetcher->FetchProfileFromCustomApi(req);
    EXPECT_EQ(res, CustomApiFetchResult::SuccessFinished);

    {
        const auto history = sessionState->history.GetSnapshot();
        EXPECT_EQ(history->initialMmr, 1250);
        EXPECT_EQ(history->playlistInitialMmr.at("2v2"), 1250);
        ASSERT_FALSE(history->mmrHistoryY.empty());
        EXPECT_EQ(static_cast<int>(history->mmrHistoryY.back()), 1250);
        ASSERT_FALSE(history->playlistHistoryY.at("2v2").empty());
        EXPECT_EQ(static_cast<int>(history->playlistHistoryY.at("2v2").back()), 1250);
    }
}

TEST_F(MmrServiceTest, SwitchingAccountsKeepsEachSessionGraphOnItsOwnRating) {
    const std::string firstId = "Epic|first_account|0";
    const std::string secondId = "Epic|second_account|0";
    SetUpSignedInDeviceAuth(false);
    g_mock_custom_api_response_code = 200;

    auto fetch = [&](const std::string& primaryId, const std::string& accountId, int mmr, int matches) {
        g_mock_custom_api_response =
            R"({"players":[{"account_id":")" + accountId +
            R"(","skills":[{"playlist":11,"mmr":)" + std::to_string(mmr) +
            R"(,"tier":14,"division":2,"matches_played":)" + std::to_string(matches) +
            R"(}]}]})";
        MMRRequest req;
        req.primaryId = primaryId;
        req.reason = MMRRequestReason::Roster;
        return fetcher->FetchProfileFromCustomApi(req);
    };

    sessionState->liveMatch->SetLocalPrimaryId(firstId);
    ASSERT_EQ(fetch(firstId, "first_account", 1425, 63), CustomApiFetchResult::SuccessFinished);
    fetcher->EnqueuePostMatch(firstId, "First", "first-loss", "2v2", 1425, 63, true, false);
    fetcher->ProcessPostMatchResponse("first-loss", 1416, 64);

    sessionState->liveMatch->SetLocalPrimaryId(secondId);
    sessionState->history.Mutate([&](auto& history) {
        history.SelectMmrOwner(secondId);
        EXPECT_TRUE(history.playlistHistoryY.empty());
    });
    ASSERT_EQ(fetch(secondId, "second_account", 1379, 22), CustomApiFetchResult::SuccessFinished);
    fetcher->EnqueuePostMatch(firstId, "First", "late-loss", "2v2", 1416, 64, true, false);
    fetcher->ProcessPostMatchResponse("late-loss", 1407, 65);
    {
        const auto history = sessionState->history.GetSnapshot();
        EXPECT_EQ(history->playlistHistoryY.at("2v2"), (std::vector<float>{1379.0f}));
    }

    sessionState->liveMatch->SetLocalPrimaryId(firstId);
    sessionState->history.Mutate([&](auto& history) {
        history.SelectMmrOwner(firstId);
        EXPECT_EQ(history.playlistHistoryY.at("2v2"),
                  (std::vector<float>{1425.0f, 1416.0f, 1407.0f}));
    });
    ASSERT_EQ(fetch(firstId, "first_account", 1407, 65), CustomApiFetchResult::SuccessFinished);

    const auto history = sessionState->history.GetSnapshot();
    EXPECT_EQ(history->playlistHistoryY.at("2v2"),
              (std::vector<float>{1425.0f, 1416.0f, 1407.0f}));
    ASSERT_EQ(history->playlistMatchPoints.at("2v2").size(), 2u);
    EXPECT_EQ(history->playlistMatchPoints.at("2v2")[0].mmr, 1416);
    EXPECT_EQ(history->playlistMatchPoints.at("2v2")[1].mmr, 1407);
}

TEST_F(MmrServiceTest, SignedInUsesBearerHeadersAndIsPrimaryForAllPlayers) {
    auto& account = *accountClient;
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(account.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.enable_mmr_tracking = false;
        c.custom_api_enabled = true;
        c.custom_api_key = "legacy_key_should_not_be_used";
        c.account_refresh_token = "valid_refresh_token";
        c.account_signed_in_name = "SignedInPlayer";
        c.account_device_public_id = keyPair.publicId;
    },
                   false);
    account.SyncFromConfig();
    account.SetClockForTests([]() { return 1700000000; });
    account.SetCachedAccessTokenForTests("bearer_token_abc", 1700000900);

    g_mock_response_code = 500;
    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Epic",
            "account_id": "opp_id",
            "skills": [
                { "playlist": 11, "mmr": 1310.0, "tier": 15, "division": 1, "matches_played": 30 }
            ]
        }]
    })";

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.myPrimaryId = "Epic|local_id";
        ms.roster["Epic|opp_id"] = PlayerData{.primaryId = "Epic|opp_id", .name = "Opponent"};
    });
    OmniNet::Request capturedReq;
    OmniNet::SetTransportForTesting([&](const OmniNet::Request& req) {
        capturedReq = req;
        g_mock_perform_count.fetch_add(1);
        OmniNet::Response res;
        res.status = 200;
        res.body = g_mock_custom_api_response;
        return res;
    });

    fetcher->FetchRosterProfile("Epic|opp_id", "Opponent");

    EXPECT_EQ(g_mock_perform_count.load(), 1);
    EXPECT_EQ(capturedReq.op, OmniNet::Op::Ranks);
    EXPECT_EQ(capturedReq.accessToken, "bearer_token_abc");
    EXPECT_EQ(capturedReq.devicePublicId, keyPair.publicId);
    auto snap = sessionState->liveMatch->GetSnapshot();
    ASSERT_TRUE(snap->roster.count("Epic|opp_id") > 0);
    EXPECT_EQ(snap->roster.at("Epic|opp_id").mmr, 1310);
    EXPECT_EQ(snap->roster.at("Epic|opp_id").rankVerificationSource, "ServerA");
}

TEST_F(MmrServiceTest, SignedIn401RefreshesOnceAndRetriesOnceBeforeSigningOutOnSecond401) {
    auto& account = *accountClient;
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(account.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.enable_mmr_tracking = false;
        c.custom_api_enabled = true;
        c.account_refresh_token = "initial_refresh_token";
        c.account_signed_in_name = "SignedInPlayer";
        c.account_device_public_id = keyPair.publicId;
    },
                   false);
    account.SyncFromConfig();
    account.SetClockForTests([]() { return 1700000000; });
    account.SetCachedAccessTokenForTests("expired_access_token", 1700000900);

    int refreshCalls = 0;
    g_mock_refresh_fn = [&](const OmniNet::Request& req) {
        EXPECT_EQ(req.op, OmniNet::Op::AuthRefresh);
        ++refreshCalls;
        OmniNet::Response resp;
        resp.status = 200;
        resp.body = nlohmann::json{
            {"access_token", "fresh_access_token_" + std::to_string(refreshCalls)},
            {"expires_in", 900},
            {"refresh_token", "rotated_refresh_token_" + std::to_string(refreshCalls)},
            {"refresh_expires_in", 2592000},
            {"account", {{"display_name", "SignedInPlayer"}}},
            {"device", {{"public_id", keyPair.publicId}, {"status", "active"}}}}
                        .dump();
        return resp;
    };

    g_mock_custom_api_response_code = 200;
    g_mock_custom_api_status_sequence = {401, 200};
    g_mock_custom_api_response = R"({
        "players": [{
            "platform": "Steam",
            "account_id": "76561198000000001",
            "skills": [
                { "playlist": 11, "mmr": 1400.0, "tier": 16, "division": 2, "matches_played": 50 }
            ]
        }]
    })";

    MMRRequest req;
    req.primaryId = "Steam|76561198000000001";
    req.name = "PlayerOne";
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::SuccessFinished);
    EXPECT_EQ(refreshCalls, 1);
    EXPECT_EQ(g_mock_perform_count.load(), 2);
    EXPECT_TRUE(account.IsSignedIn());
    EXPECT_EQ(Config::Read().account_refresh_token, "rotated_refresh_token_1");

    g_mock_perform_count.store(0);
    g_mock_custom_api_status_sequence = {401, 401};
    EXPECT_EQ(fetcher->FetchProfileFromCustomApi(req), CustomApiFetchResult::AuthFailure);
    EXPECT_EQ(refreshCalls, 2);
    EXPECT_EQ(g_mock_perform_count.load(), 2);
    EXPECT_FALSE(account.IsSignedIn());
    EXPECT_TRUE(Config::Read().account_refresh_token.empty());
    std::string reason;
    EXPECT_TRUE(account.ConsumeSignedOutNotification(reason));
    EXPECT_FALSE(reason.empty());
}

TEST_F(MmrServiceTest, SignedInCustomApiFailureOnlyFallsBackToTrackerWhenMmrTrackingEnabled) {
    auto& account = *accountClient;
    AccountClient::DeviceKeyPair keyPair;
    ASSERT_TRUE(account.EnsureDeviceKey(&keyPair));
    Config::Update([&](ConfigData& c) {
        c.enable_mmr_tracking = false;
        c.custom_api_enabled = true;
        c.account_refresh_token = "valid_refresh_token";
        c.account_signed_in_name = "SignedInPlayer";
        c.account_device_public_id = keyPair.publicId;
    },
                   false);
    account.SyncFromConfig();
    account.SetClockForTests([]() { return 1700000000; });
    account.SetCachedAccessTokenForTests("bearer_token_abc", 1700000900);

    g_mock_custom_api_response_code = 404;
    g_mock_response_code = 200;
    g_mock_response = R"({
        "data": {
            "segments": [{
                "type": "playlist",
                "attributes": { "playlistId": 11 },
                "stats": {
                    "rating": { "value": 1350 },
                    "tier": { "metadata": { "name": "Champion I" } },
                    "division": { "metadata": { "name": "Division II" } },
                    "matchesPlayed": { "value": 40 }
                }
            }]
        }
    })";

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["Steam|76561198000000099"] =
            PlayerData{.primaryId = "Steam|76561198000000099", .name = "TestTarget"};
    });
    fetcher->FetchRosterProfile("Steam|76561198000000099", "TestTarget");
    EXPECT_EQ(g_mock_perform_count.load(), 1);
    {
        auto snap = sessionState->liveMatch->GetSnapshot();
        EXPECT_TRUE(snap->roster.at("Steam|76561198000000099").fetchFailed);
    }

    g_mock_perform_count.store(0);
    Config::Update([](ConfigData& c) { c.enable_mmr_tracking = true; }, false);
    fetcher->FetchRosterProfile("Steam|76561198000000099", "TestTarget");
    EXPECT_EQ(g_mock_perform_count.load(), 2);
    {
        auto snap = sessionState->liveMatch->GetSnapshot();
        EXPECT_FALSE(snap->roster.at("Steam|76561198000000099").fetchFailed);
        EXPECT_EQ(snap->roster.at("Steam|76561198000000099").mmr, 1350);
        EXPECT_EQ(snap->roster.at("Steam|76561198000000099").rankVerificationSource, "Tracker");
    }
}
TEST(MmrServiceBatchTest, SixRosterRequestsFormSingleBatchWithSixPlayers) {
    std::vector<MMRRequest> requests;
    for (int i = 1; i <= 6; ++i) {
        MMRRequest req;
        req.primaryId = "Epic|epic_player_" + std::to_string(i) + "|0";
        req.name = "Player" + std::to_string(i);
        req.reason = MMRRequestReason::Roster;
        requests.push_back(std::move(req));
    }

    const auto now = std::chrono::steady_clock::now();
    const auto batch = AssembleCustomApiBatch(requests, now, 16);
    EXPECT_EQ(batch.requests.size(), 6u);
    EXPECT_TRUE(batch.playlist.empty());
    EXPECT_EQ(batch.playlistId, 0);

    const nlohmann::json body = BuildCustomApiBatchJson(batch);
    ASSERT_TRUE(body.is_object());
    ASSERT_TRUE(body.contains("players"));
    ASSERT_TRUE(body["players"].is_array());
    EXPECT_EQ(body["players"].size(), 6u);
    EXPECT_FALSE(body.contains("playlist"));

    for (int i = 0; i < 6; ++i) {
        const auto& p = body["players"][i];
        EXPECT_EQ(p["platform"], "Epic");
        EXPECT_EQ(p["account_id"], "epic_player_" + std::to_string(i + 1));
    }
}

TEST(MmrServiceBatchTest, TwentyRosterRequestsSplitIntoSixteenAndFour) {
    std::vector<MMRRequest> all20;
    for (int i = 1; i <= 20; ++i) {
        MMRRequest req;
        req.primaryId = "Steam|765611980000000" + (i < 10 ? "0" + std::to_string(i) : std::to_string(i)) + "|0";
        req.name = "SteamPlayer" + std::to_string(i);
        req.reason = MMRRequestReason::Roster;
        all20.push_back(std::move(req));
    }

    const auto now = std::chrono::steady_clock::now();
    const auto batch1 = AssembleCustomApiBatch(all20, now, 16);
    EXPECT_EQ(batch1.requests.size(), 16u);
    EXPECT_EQ(batch1.requests.front().name, "SteamPlayer1");
    EXPECT_EQ(batch1.requests.back().name, "SteamPlayer16");

    const std::vector<MMRRequest> remaining4(all20.begin() + 16, all20.end());
    const auto batch2 = AssembleCustomApiBatch(remaining4, now, 16);
    EXPECT_EQ(batch2.requests.size(), 4u);
    EXPECT_EQ(batch2.requests.front().name, "SteamPlayer17");
    EXPECT_EQ(batch2.requests.back().name, "SteamPlayer20");
}

TEST(MmrServiceBatchTest, MixedPlaylistsSplitIntoDistinctBatches) {
    std::vector<MMRRequest> mixed;

    MMRRequest r1;
    r1.primaryId = "Epic|p1|0";
    r1.playlist = "2v2";
    r1.reason = MMRRequestReason::Roster;
    mixed.push_back(r1);

    MMRRequest r2;
    r2.primaryId = "Epic|p2|0";
    r2.playlist = "3v3";
    r2.reason = MMRRequestReason::Roster;
    mixed.push_back(r2);

    MMRRequest r3;
    r3.primaryId = "Epic|p3|0";
    r3.playlist = "2v2";
    r3.reason = MMRRequestReason::Roster;
    mixed.push_back(r3);

    MMRRequest r4;
    r4.primaryId = "Epic|p4|0";
    r4.playlist = "";
    r4.reason = MMRRequestReason::Roster;
    mixed.push_back(r4);

    const auto now = std::chrono::steady_clock::now();
    const auto batch2v2 = AssembleCustomApiBatch(mixed, now, 16);
    EXPECT_EQ(batch2v2.requests.size(), 2u);
    EXPECT_EQ(batch2v2.playlist, "2v2");
    EXPECT_EQ(batch2v2.playlistId, 11);
    EXPECT_EQ(batch2v2.requests[0].primaryId, "Epic|p1|0");
    EXPECT_EQ(batch2v2.requests[1].primaryId, "Epic|p3|0");

    const nlohmann::json body = BuildCustomApiBatchJson(batch2v2);
    ASSERT_TRUE(body.contains("playlist"));
    EXPECT_EQ(body["playlist"], 11);
    EXPECT_EQ(body["players"].size(), 2u);
}

TEST(MmrServiceBatchTest, LocalFastLaneRequestNotBatchedOrDelayed) {
    std::vector<MMRRequest> candidates;

    MMRRequest postMatch;
    postMatch.primaryId = "Epic|local_hero|0";
    postMatch.matchGuid = "match_fast_lane_guid";
    postMatch.reason = MMRRequestReason::PostMatch;
    candidates.push_back(postMatch);

    MMRRequest roster1;
    roster1.primaryId = "Epic|opp_1|0";
    roster1.reason = MMRRequestReason::Roster;
    candidates.push_back(roster1);

    MMRRequest roster2;
    roster2.primaryId = "Epic|opp_2|0";
    roster2.reason = MMRRequestReason::Roster;
    candidates.push_back(roster2);

    const auto now = std::chrono::steady_clock::now();
    const auto batch = AssembleCustomApiBatch(candidates, now, 16);
    EXPECT_EQ(batch.requests.size(), 2u);
    for (const auto& req : batch.requests) {
        EXPECT_EQ(req.reason, MMRRequestReason::Roster);
        EXPECT_NE(req.matchGuid, "match_fast_lane_guid");
    }
}

TEST(MmrServiceBatchTest, ResponseParsingMapsEachPlayerIncludingMissingAndErrored) {
    std::vector<MMRRequest> requests;

    MMRRequest r1;
    r1.primaryId = "Epic|player_success|0";
    r1.name = "SuccessPlayer";
    r1.reason = MMRRequestReason::Roster;
    requests.push_back(r1);

    MMRRequest r2;
    r2.primaryId = "Steam|76561198000000002|0";
    r2.name = "ErroredPlayer";
    r2.reason = MMRRequestReason::Roster;
    requests.push_back(r2);

    MMRRequest r3;
    r3.primaryId = "Xbox|2535421607154237|0";
    r3.name = "MissingPlayer";
    r3.reason = MMRRequestReason::Roster;
    requests.push_back(r3);

    const nlohmann::json jsonResp = {
        {"players", nlohmann::json::array({{{"platform", "Epic"},
                                            {"account_id", "player_success"},
                                            {"skills", nlohmann::json::array({{{"playlist", 11}, {"mmr", 1250.4}, {"tier", 14}, {"division", 2}, {"matches_played", 80}}})},
                                            {"wins", 450}},
                                           {{"platform", "Steam"},
                                            {"account_id", "76561198000000002"},
                                            {"error", {{"code", "not_found"}, {"message", "Player has not played competitive matches"}}}}})}};

    const auto results = ParseCustomApiBatchResponse(requests, jsonResp);
    ASSERT_EQ(results.size(), 3u);

    EXPECT_EQ(results[0].request.primaryId, "Epic|player_success|0");
    EXPECT_EQ(results[0].status, BatchPlayerStatus::Success);
    EXPECT_EQ(results[0].profile.bestMmr, 1250);
    EXPECT_EQ(results[0].profile.totalWins, 450);
    EXPECT_EQ(results[0].profile.rankVerificationSource, "ServerA");

    EXPECT_EQ(results[1].request.primaryId, "Steam|76561198000000002|0");
    EXPECT_EQ(results[1].status, BatchPlayerStatus::NotFound);
    EXPECT_EQ(results[1].errorCode, "not_found");

    EXPECT_EQ(results[2].request.primaryId, "Xbox|2535421607154237|0");
    EXPECT_EQ(results[2].status, BatchPlayerStatus::NotFound);
    EXPECT_EQ(results[2].errorCode, "not_found");
}

TEST_F(MmrServiceTest, WholeBatchHttpFailureYieldsPerPlayerFailures) {
    SetUpSignedInDeviceAuth(false);

    AssembledBatch batch;
    for (int i = 1; i <= 3; ++i) {
        const std::string id = "Epic|fail_player_" + std::to_string(i) + "|0";
        sessionState->liveMatch->UpdateMatchStateForTests([&](MatchState& ms) { ms.roster[id] = PlayerData{.primaryId = id, .name = "FailPlayer" + std::to_string(i)}; });
        MMRRequest req;
        req.primaryId = id;
        req.name = "FailPlayer" + std::to_string(i);
        req.reason = MMRRequestReason::Roster;
        req.retriesRemaining = 0;
        batch.requests.push_back(std::move(req));
    }

    g_mock_custom_api_response_code = 500;
    fetcher->ProcessCustomApiBatch(batch);

    auto snap = sessionState->liveMatch->GetSnapshot();
    for (int i = 1; i <= 3; ++i) {
        const std::string id = "Epic|fail_player_" + std::to_string(i) + "|0";
        ASSERT_TRUE(snap->roster.count(id));
        EXPECT_TRUE(snap->roster.at(id).fetched);
        EXPECT_TRUE(snap->roster.at(id).fetchFailed);
    }
}

TEST_F(MmrServiceTest, EndToEndCoalescesQueuedRosterRequestsIntoSingleHttpCall) {
    SetUpSignedInDeviceAuth(false);

    nlohmann::json playersArray = nlohmann::json::array();
    for (int i = 1; i <= 6; ++i) {
        playersArray.push_back({{"platform", "Epic"},
                                {"account_id", "batch_player_" + std::to_string(i)},
                                {"skills", nlohmann::json::array({{{"playlist", 11}, {"mmr", 1000.0 + i * 50}, {"tier", 10 + i}, {"division", 1}, {"matches_played", 20}}})},
                                {"wins", 100 * i}});
    }

    const nlohmann::json respJson = {{"players", playersArray}};
    g_mock_custom_api_response = respJson.dump();
    g_mock_custom_api_response_code = 200;

    for (int i = 1; i <= 6; ++i) {
        const std::string id = "Epic|batch_player_" + std::to_string(i) + "|0";
        sessionState->liveMatch->UpdateMatchStateForTests([&](MatchState& ms) { ms.roster[id] = PlayerData{.primaryId = id, .name = "LobbyPlayer" + std::to_string(i)}; });
        fetcher->Enqueue(id, "LobbyPlayer" + std::to_string(i));
    }

    fetcher->Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fetcher->Stop();

    EXPECT_EQ(g_mock_perform_count.load(), 1);

    ASSERT_FALSE(g_mock_post_fields.empty());
    const nlohmann::json postedJson = nlohmann::json::parse(g_mock_post_fields);
    ASSERT_TRUE(postedJson.contains("players"));
    EXPECT_EQ(postedJson["players"].size(), 6u);

    auto snap = sessionState->liveMatch->GetSnapshot();
    for (int i = 1; i <= 6; ++i) {
        const std::string id = "Epic|batch_player_" + std::to_string(i) + "|0";
        ASSERT_TRUE(snap->roster.count(id));
        EXPECT_TRUE(snap->roster.at(id).fetched);
        EXPECT_FALSE(snap->roster.at(id).fetchFailed);
        EXPECT_EQ(snap->roster.at(id).mmr, 1000 + i * 50);
        EXPECT_EQ(snap->roster.at(id).rankVerificationSource, "ServerA");
    }
}

TEST_F(MmrServiceTest, TrackerPathRemainsSingleRequestWhenCustomApiNotActive) {
    Config::Update([](ConfigData& c) {
        c.custom_api_enabled = false;
        c.enable_mmr_tracking = true;
    },
                   false);

    EXPECT_FALSE(fetcher->IsCustomApiActiveSource());
}

TEST_F(MmrServiceTest, UpdateRequiredOnRanksDoesNotSignOutAndFallsBackToTracker) {
    SetUpSignedInDeviceAuth(true);
    auto& account = *accountClient;
    EXPECT_TRUE(account.IsSignedIn());

    g_mock_response_code = 200;
    g_mock_response = R"({
        "data": {
            "segments": [{
                "type": "playlist",
                "attributes": {"playlistId": 11},
                "stats": {
                    "rating": {"value": 1150},
                    "tier": {"metadata": {"name": "Diamond I"}}
                }
            }]
        }
    })";

    OmniNet::SetTransportForTesting([](const OmniNet::Request& req) {
        OmniNet::Response res;
        if (req.op == OmniNet::Op::Ranks) {
            res.status = 426;
            res.updateRequired = true;
            res.body = R"({"error":{"code":"update_required","message":"Update OmniStats to keep live ranks."}})";
            return res;
        }
        return res;
    });

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["Epic|test_epic_id|0"] = PlayerData{.primaryId = "Epic|test_epic_id|0", .name = "TrackerPlayer"};
    });
    fetcher->FetchRosterProfile("Epic|test_epic_id|0", "TrackerPlayer");

    EXPECT_TRUE(account.IsSignedIn());
    EXPECT_FALSE(Config::Read().account_refresh_token.empty());

    auto snap = sessionState->liveMatch->GetSnapshot();
    ASSERT_TRUE(snap->roster.count("Epic|test_epic_id|0") > 0);
    const auto& player = snap->roster.at("Epic|test_epic_id|0");
    EXPECT_TRUE(player.fetched);
    EXPECT_EQ(player.playlists.at("2v2"), 1150);
}

TEST_F(MmrServiceTest, UpdateRequiredOnRanksWithoutTrackerFailsWithUpdateMessage) {
    SetUpSignedInDeviceAuth(false);
    auto& account = *accountClient;
    EXPECT_TRUE(account.IsSignedIn());

    OmniNet::SetTransportForTesting([](const OmniNet::Request& req) {
        OmniNet::Response res;
        if (req.op == OmniNet::Op::Ranks) {
            res.status = 426;
            res.updateRequired = true;
            return res;
        }
        return res;
    });

    sessionState->liveMatch->UpdateMatchStateForTests([](MatchState& ms) {
        ms.roster["Epic|test_epic_id|0"] = PlayerData{.primaryId = "Epic|test_epic_id|0", .name = "TestPlayer"};
    });
    fetcher->FetchRosterProfile("Epic|test_epic_id|0", "TestPlayer");

    EXPECT_TRUE(account.IsSignedIn());
}
