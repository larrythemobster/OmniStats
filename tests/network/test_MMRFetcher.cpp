#include <gtest/gtest.h>
#include "network/MMRFetcher.hpp"
#include "network/CurlImpersonate.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include <chrono>
#include <thread>
#include <cstdarg>

typedef size_t (*WriteCallbackType)(void*, size_t, size_t, void*);
static WriteCallbackType g_write_callback = nullptr;
static void* g_write_data = nullptr;
static std::string g_mock_response = "";
static long g_mock_response_code = 200;

static int mock_easy_setopt(void* curl, int option, ...) {
    va_list args;
    va_start(args, option);
    if (option == CI_CURLOPT_WRITEFUNCTION) {
        g_write_callback = va_arg(args, WriteCallbackType);
    } else if (option == CI_CURLOPT_WRITEDATA) {
        g_write_data = va_arg(args, void*);
    }
    va_end(args);
    return 0;
}

static int mock_easy_perform(void* curl) {
    if (g_write_callback && g_write_data && g_mock_response_code == 200) {
        g_write_callback((void*)g_mock_response.data(), 1, g_mock_response.size(), g_write_data);
    }
    return 0; // CURLE_OK
}

static int mock_easy_getinfo(void* curl, int info, ...) {
    va_list args;
    va_start(args, info);
    if (info == CI_CURLINFO_RESPONSE_CODE) {
        long* code = va_arg(args, long*);
        *code = g_mock_response_code;
    }
    va_end(args);
    return 0;
}

static void mock_easy_cleanup(void* curl) {}
static void mock_slist_free_all(void* list) {}
static void* mock_slist_append(void* list, const char* str) {
    return (void*)1;
}
static void* mock_easy_init() {
    return (void*)1;
}

class MMRFetcherTest : public ::testing::Test {
  protected:
    void SetUp() override {
        sessionState = std::make_shared<SessionState>();
        fetcher = std::make_shared<MMRFetcher>(sessionState);

        auto& curl = CurlImpersonate::Instance();
        original_perform = curl.easy_perform;
        original_getinfo = curl.easy_getinfo;
        original_setopt = curl.easy_setopt;
        original_cleanup = curl.easy_cleanup;
        original_slist_free_all = curl.slist_free_all;
        original_slist_append = curl.slist_append;
        original_easy_init = curl.easy_init;

        curl.easy_perform = mock_easy_perform;
        curl.easy_getinfo = mock_easy_getinfo;
        curl.easy_setopt = mock_easy_setopt;
        curl.easy_cleanup = mock_easy_cleanup;
        curl.slist_free_all = mock_slist_free_all;
        curl.slist_append = mock_slist_append;
        curl.easy_init = mock_easy_init;
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
    }

    std::shared_ptr<SessionState> sessionState;
    std::shared_ptr<MMRFetcher> fetcher;

    pfn_curl_easy_perform original_perform;
    pfn_curl_easy_getinfo original_getinfo;
    pfn_curl_easy_setopt original_setopt;
    pfn_curl_easy_cleanup original_cleanup;
    pfn_curl_slist_free_all original_slist_free_all;
    pfn_curl_slist_append original_slist_append;
    pfn_curl_easy_init original_easy_init;
};

TEST_F(MMRFetcherTest, FetchProfileSuccess) {
    g_mock_response_code = 200;
    g_mock_response = R"({"data": {"segments": [{"stats": {"rating": {"value": 1200}}}]}})";

    fetcher->Start();
    fetcher->Enqueue("epic_123", "TestPlayer");

    // Allow worker thread to process
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST_F(MMRFetcherTest, FetchProfileNotFound) {
    g_mock_response_code = 404;

    fetcher->Start();
    fetcher->Enqueue("epic_404", "MissingPlayer");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST_F(MMRFetcherTest, FetchProfileServerError) {
    g_mock_response_code = 500;

    fetcher->Start();
    fetcher->Enqueue("epic_500", "ServerErrPlayer");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fetcher->Stop();
    SUCCEED();
}

TEST(MMRFetcherTournamentRankTest, UsesTournamentMmrThresholds) {
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1421), "Grand Champion I Div I");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1537), "Grand Champion I Div IV");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1561), "Grand Champion II Div I");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1701), "Grand Champion III Div I");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1861), "Supersonic Legend");
}

TEST(MMRFetcherTournamentRankTest, KeepsBoundaryGapsInPreviousDivision) {
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1560), "Grand Champion I Div IV");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(1530), "Grand Champion I Div III");
    EXPECT_EQ(MMRFetcher::GetTournamentTierForMmr(0), "Unranked");
}

TEST(MMRFetcherPlaylistMappingTest, MapsExtraModesToSeparatePlaylists) {
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(10), "1v1");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(11), "2v2");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(13), "3v3");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(27), "hoops");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(28), "rumble");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(29), "dropshot");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(30), "snowday");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(34), "t");
    EXPECT_EQ(MMRFetcher::PlaylistNameForTrackerId(999), "");
}

TEST(MMRFetcherProfileTotalsTest, ExtractsOverviewWins) {
    nlohmann::json response = {
        {"data", {{"segments", nlohmann::json::array({{{"type", "overview"}, {"stats", {{"wins", {{"value", 60}}}}}}, {{"type", "playlist"}, {"stats", {{"wins", {{"value", 999}}}}}}})}}}};

    MMRProfileTotals totals = MMRFetcher::ExtractProfileTotals(response);
    EXPECT_EQ(totals.totalWins, 60);
}

TEST(MMRFetcherPostMatchTest, DetectsStalePostMatchRatings) {
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 50, 50));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 0, 50, 50));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1211, 50, 51));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 50, 51));
}

TEST(MMRFetcherPostMatchTest, KeepsPostMatchRequestBehindRosterRequest) {
    const ConfigData original = Config::Read();
    Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);

    auto state = std::make_shared<SessionState>();
    MMRFetcher fetcher(state);
    fetcher.Enqueue("Steam|123", "Player");
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50);
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50);

    EXPECT_EQ(fetcher.PendingRequestCountForTests(), 2u);

    Config::Update([&](ConfigData& config) { config = original; }, false);
}
