#include <gtest/gtest.h>
#include "network/MMRFetcher.hpp"
#include "network/CurlImpersonate.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include <chrono>
#include <thread>
#include <cstdarg>
#include <atomic>

typedef size_t (*WriteCallbackType)(void*, size_t, size_t, void*);
static WriteCallbackType g_write_callback = nullptr;
static void* g_write_data = nullptr;
static std::string g_mock_response = "";
static long g_mock_response_code = 200;
static std::atomic<int> g_mock_perform_count{0};

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
    g_mock_perform_count.fetch_add(1);
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
static std::string g_mock_escaped_value;
static char* mock_easy_escape(void*, const char* value, int length) {
    g_mock_escaped_value.assign(value, static_cast<size_t>(length));
    return g_mock_escaped_value.data();
}
static void mock_curl_free(void*) {}
static int mock_easy_impersonate(void*, const char*, int) {
    return 0;
}

class MMRFetcherTest : public ::testing::Test {
  protected:
    void SetUp() override {
        originalConfig = Config::Read();
        Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);
        g_mock_perform_count.store(0);
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
        curl.SetReadyForTests(original_ready);
        Config::Update([this](ConfigData& config) { config = originalConfig; }, false);
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
        const std::string& playlist = "2v2") {
        PendingDestroyedMatchMmrRefresh pending;
        pending.matchGuid = guid;
        pending.primaryId = "Steam|123";
        pending.name = "Player";
        pending.playlist = playlist;
        pending.localTeam = 0;
        pending.score = {0, 0};
        pending.previousMmr = previousMmr;
        pending.previousMatches = previousMatches;
        pending.previousMmrIsPlaylistSpecific = true;
        pending.validCompetitiveMatch = true;
        fetcher->EnqueuePendingDestroyedMatch(pending);
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
    pfn_curl_easy_escape original_easy_escape;
    pfn_curl_free original_free;
    pfn_curl_easy_impersonate original_easy_impersonate;
    bool original_ready = false;
    ConfigData originalConfig;
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

TEST(MMRFetcherPostMatchTest, UsesMatchCountsAsThePublicationVersion) {
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 50, 50));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1209, 50, 50));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 0, 50, 51));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1209, 50, 51));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 50, 51));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1218, 50, 52));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 0, 0));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 0, 1));
}

TEST(MMRFetcherPostMatchTest, FallsBackToRatingOnlyWithoutMatchCounts) {
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, -1, -1));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1209, -1, -1));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(1200, 1200, 50, -1));
    EXPECT_FALSE(MMRFetcher::IsPostMatchMmrStale(1200, 1209, 50, -1));
    EXPECT_TRUE(MMRFetcher::IsPostMatchMmrStale(0, 1200, -1, -1));
}

TEST(MMRFetcherPostMatchTest, CountsCumulativePublishedMatches) {
    EXPECT_EQ(MMRFetcher::CoveredPendingMatchCount(50, 51, 3), 1u);
    EXPECT_EQ(MMRFetcher::CoveredPendingMatchCount(50, 52, 3), 2u);
    EXPECT_EQ(MMRFetcher::CoveredPendingMatchCount(50, 50, 3), 0u);
    EXPECT_EQ(MMRFetcher::CoveredPendingMatchCount(0, 1, 1), 1u);
}

TEST(MMRFetcherPostMatchTest, UsesPlaylistHistoryInsteadOfIncorrectBestMmrFallback) {
    EXPECT_EQ(MMRFetcher::ResolvePostMatchBaseline(788, false, {630.0f}), 630);
    EXPECT_EQ(MMRFetcher::ResolvePostMatchBaseline(788, false, {}), 788);
}

TEST(MMRFetcherPostMatchTest, KeepsFinalizedPlaylistBaselineWhenRosterFetchMovesAhead) {
    EXPECT_EQ(MMRFetcher::ResolvePostMatchBaseline(630, true, {630.0f, 667.0f}), 630);
}

TEST(MMRFetcherPostMatchTest, EstimatesFromRecentNormalChanges) {
    const std::vector<float> history = {1182.0f, 1191.0f, 1200.0f};

    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1200, true, history), 1209);
    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1200, false, history), 1191);
}

TEST(MMRFetcherPostMatchTest, ChainsEstimateFromLatestGraphPoint) {
    const std::vector<float> history = {1200.0f, 1191.0f};

    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1200, false, history), 1182);
    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1200, true, history), 1200);
}

TEST(MMRFetcherPostMatchTest, IgnoresCumulativeJumpsAndUsesDefaultDelta) {
    const std::vector<float> history = {1200.0f, 1218.0f};

    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1218, true, history), 1227);
    EXPECT_EQ(MMRFetcher::EstimatePostMatchMmr(1218, false, history), 1209);
}

TEST_F(MMRFetcherTest, NormalPublishedUpdateCreatesOneConfirmedOwnedPoint) {
    EnqueuePostMatch("normal", true);

    fetcher->ProcessPostMatchResponseForTests("normal", 1209, 51);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "normal");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 51);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, StaleResponseStillCreatesOneProvisionalOwnedPoint) {
    EnqueuePostMatch("stale", true);

    fetcher->ProcessPostMatchResponseForTests("stale", 1200, 50);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "stale");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, FirstObservedZeroCountConfirmsOwnedPointAtOneWithoutMmrChange) {
    EnqueuePostMatch("missing-pre-match", true, 0, -1);

    fetcher->ProcessPostMatchResponseForTests("missing-pre-match", 1200, 0);

    auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "missing-pre-match");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[0].trackerMatchesPlayed, -1);

    fetcher->ProcessPostMatchResponseForTests("missing-pre-match", 1200, 1);
    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 1);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, MissingBaselineRemainsPendingUntilPlaylistInitialMmrExists) {
    EnqueuePostMatch("delayed-baseline", false, 0, -1);
    fetcher->ProcessPostMatchResponseForTests("delayed-baseline", 0, -1);
    EXPECT_TRUE(fetcher->PlaylistMatchPointsForTests("2v2").empty());

    sessionState->history.playlistInitialMmr["2v2"] = 1200;
    fetcher->ProcessPostMatchResponseForTests("delayed-baseline", 0, -1);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "delayed-baseline");
    EXPECT_EQ(points[0].mmr, 1191);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, DelayedCumulativeUpdatePreservesOnePointPerMatch) {
    EnqueuePostMatch("match-a", true);
    fetcher->ProcessPostMatchResponseForTests("match-a", 1200, 50);
    EnqueuePostMatch("match-b", true);

    fetcher->ProcessPostMatchResponseForTests("match-b", 1218, 52);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "match-a");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "match-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
    ASSERT_EQ(sessionState->history.playlistHistoryY["2v2"].size(), 2u);
}

TEST_F(MMRFetcherTest, WinThenLossKeepsDirectionalPointsAndConfirmsNewest) {
    EnqueuePostMatch("win", true);
    fetcher->ProcessPostMatchResponseForTests("win", 1200, 50);
    EnqueuePostMatch("loss", false);

    fetcher->ProcessPostMatchResponseForTests("loss", 1215, 52);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
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

TEST_F(MMRFetcherTest, TwoWinsWithSmallCumulativeGainRemainIncreasing) {
    EnqueuePostMatch("small-win-a", true);
    fetcher->ProcessPostMatchResponseForTests("small-win-a", 1200, 50);
    EnqueuePostMatch("small-win-b", true);

    fetcher->ProcessPostMatchResponseForTests("small-win-b", 1205, 52);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
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

TEST_F(MMRFetcherTest, ChangedRatingWithoutMatchAdvanceDoesNotConsumePendingMatch) {
    EnqueuePostMatch("not-published", true);

    fetcher->ProcessPostMatchResponseForTests("not-published", 1209, 50);

    auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
    EXPECT_EQ(points[0].mmr, 1209);

    fetcher->ProcessPostMatchResponseForTests("not-published", 1218, 51);
    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[0].mmr, 1218);
}

TEST_F(MMRFetcherTest, MissingFetchedCountFallsBackToRatingForOnlyOldestMatch) {
    EnqueuePostMatch("missing-count-a", true);
    EnqueuePostMatch("missing-count-b", true);

    fetcher->ProcessPostMatchResponseForTests("missing-count-b", 1209, -1);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "missing-count-a");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "missing-count-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);
}

TEST_F(MMRFetcherTest, RepeatedCountlessResponseRequiresNewRatingForSecondMatch) {
    EnqueuePostMatch("countless-a", true);
    EnqueuePostMatch("countless-b", true);

    fetcher->ProcessPostMatchResponseForTests("countless-b", 1209, -1);
    fetcher->ProcessPostMatchResponseForTests("countless-b", 1209, -1);

    auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "countless-a");
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
    EXPECT_EQ(points[1].matchGuid, "countless-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    // The count later catching up to match A is not evidence for match B.
    fetcher->ProcessPostMatchResponseForTests("countless-b", 1209, 51);
    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    fetcher->ProcessPostMatchResponseForTests("countless-b", 1218, -1);

    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "countless-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MMRFetcherTest, SequentialCountlessFallbackCarriesPlaylistBaseline) {
    EnqueuePostMatch("sequential-a", true, 1200, 50);
    fetcher->ProcessPostMatchResponseForTests("sequential-a", 1209, -1);

    auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "sequential-a");
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);

    EnqueuePostMatch("sequential-b", true, 1209, 50);
    fetcher->ProcessPostMatchResponseForTests("sequential-b", 1209, 51);

    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "sequential-b");
    EXPECT_FALSE(points[1].trackerCovered);
    EXPECT_TRUE(points[1].valueEstimated);

    fetcher->ProcessPostMatchResponseForTests("sequential-b", 1218, -1);

    points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "sequential-b");
    EXPECT_EQ(points[1].mmr, 1218);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(points[1].valueEstimated);
}

TEST_F(MMRFetcherTest, RosterResponseWithoutMatchesPlayedFallsBackToRating) {
    sessionState->game.myPrimaryId = "Steam|123";
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

    fetcher->FetchRosterProfileForTests("Steam|123", "Player");

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "roster-missing-count");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, UnchangedRatingWithMatchAdvanceConsumesExactlyOneMatch) {
    EnqueuePostMatch("unchanged", true);

    fetcher->ProcessPostMatchResponseForTests("unchanged", 1200, 51);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unchanged");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, KnownPreMatchMmrWithUnknownCountUsesFirstRatingChange) {
    EnqueuePostMatch("unknown-pre-count", true, 1200, -1);

    fetcher->ProcessPostMatchResponseForTests("unknown-pre-count", 1209, 51);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unknown-pre-count");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_EQ(points[0].trackerMatchesPlayed, 51);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, RosterRefreshDoesNotApplyResultToBestMmrFallback) {
    sessionState->game.myPrimaryId = "Steam|123";
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

    fetcher->FetchRosterProfileForTests("Steam|123", "Player");

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "best-fallback");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_NE(points[0].mmr, 1309);
    EXPECT_FALSE(points[0].trackerCovered);
    EXPECT_TRUE(points[0].valueEstimated);
}

TEST_F(MMRFetcherTest, WorkerExhaustsThreeStaleResponsesBeforeOneProvisionalPoint) {
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
        points = fetcher->PlaylistMatchPointsForTests("2v2");
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
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 0u);
}

TEST_F(MMRFetcherTest, TrackerDecreaseConfirmsDestroyedMatchLossOnce) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("destroyed-loss", 1200, 50);

    fetcher->ProcessPostMatchResponseForTests(
        "destroyed-loss", 1191, 51);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "destroyed-loss");
    EXPECT_FALSE(confirmations[0].second);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "destroyed-loss"));
    const auto points =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "destroyed-loss");
    EXPECT_EQ(points[0].mmr, 1191);
    EXPECT_TRUE(points[0].trackerCovered);

    fetcher->ProcessPostMatchResponseForTests(
        "destroyed-loss", 1191, 51);
    EXPECT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(
        fetcher->PlaylistMatchPointsForTests("2v2").size(),
        1u);
}

TEST_F(MMRFetcherTest, TrackerIncreaseConfirmsDestroyedMatchWin) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("destroyed-win", 1200, 50);

    fetcher->ProcessPostMatchResponseForTests(
        "destroyed-win", 1209, 51);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "destroyed-win");
    EXPECT_TRUE(confirmations[0].second);
    const auto points =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].mmr, 1209);
}

TEST_F(MMRFetcherTest, UnpublishedDestroyedMatchRemainsUnconfirmed) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("unconfirmed-disconnect", 1200, 50);

    fetcher->ProcessPostMatchResponseForTests(
        "unconfirmed-disconnect", 1200, 50);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatchForTests(
            "unconfirmed-disconnect"));
    const auto points =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "unconfirmed-disconnect");
    EXPECT_EQ(points[0].mmr, 1200);
    EXPECT_FALSE(points[0].trackerCovered);
}

TEST_F(MMRFetcherTest, PartialSameBaselinePublicationKeepsDestroyedMatchPending) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch(
        "partial-publication-a", 1200, 50);
    fetcher->ProcessPostMatchResponseForTests(
        "partial-publication-a", 1200, 50);
    EnqueuePostMatch(
        "partial-publication-b",
        true,
        1200,
        50,
        "2v2",
        true);

    fetcher->ProcessPostMatchResponseForTests(
        "partial-publication-b", 1209, 51);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatchForTests(
            "partial-publication-a"));
    const auto partialPoints =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(partialPoints.size(), 2u);
    EXPECT_EQ(
        partialPoints[0].matchGuid,
        "partial-publication-a");
    EXPECT_EQ(
        partialPoints[1].matchGuid,
        "partial-publication-b");
    EXPECT_FALSE(partialPoints[0].trackerCovered);
    EXPECT_FALSE(partialPoints[1].trackerCovered);

    fetcher->ProcessPostMatchResponseForTests(
        "partial-publication-a", 1209, 51);

    EXPECT_TRUE(confirmations.empty());
    EXPECT_TRUE(
        fetcher->HasPendingDestroyedMatchForTests(
            "partial-publication-a"));

    fetcher->ProcessPostMatchResponseForTests(
        "partial-publication-b", 1200, 52);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(
        confirmations[0].first,
        "partial-publication-a");
    EXPECT_FALSE(confirmations[0].second);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "partial-publication-a"));
    const auto confirmedPoints =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(confirmedPoints.size(), 2u);
    EXPECT_TRUE(confirmedPoints[0].trackerCovered);
    EXPECT_TRUE(confirmedPoints[1].trackerCovered);
}

TEST_F(MMRFetcherTest, CumulativeCatchUpPreservesDestroyedThenNormalMatch) {
    std::vector<std::pair<std::string, bool>> confirmations;
    fetcher->SetDestroyedMatchConfirmationCallback(
        [&](const std::string& matchGuid, bool won) {
            confirmations.emplace_back(matchGuid, won);
        });
    EnqueueDestroyedMatch("catch-up-a", 1200, 11);
    EnqueuePostMatch(
        "catch-up-b", true, 1200, 11, "2v2", true);

    fetcher->ProcessPostMatchResponseForTests(
        "catch-up-b", 1200, 13);

    ASSERT_EQ(confirmations.size(), 1u);
    EXPECT_EQ(confirmations[0].first, "catch-up-a");
    EXPECT_FALSE(confirmations[0].second);
    const auto points =
        fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[0].matchGuid, "catch-up-a");
    EXPECT_EQ(points[0].mmr, 1195);
    EXPECT_EQ(points[1].matchGuid, "catch-up-b");
    EXPECT_EQ(points[1].mmr, 1200);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_TRUE(points[1].trackerCovered);
    EXPECT_FALSE(
        fetcher->HasPendingDestroyedMatchForTests(
            "catch-up-a"));
}

TEST(SessionMmrAggregationTest, SumsOnlyTrackedCompetitivePlaylists) {
    SessionState state;
    state.game.sessionTotals.mmrChangeByPlaylist = {
        {"1v1", 18},
        {"2v2", 27},
        {"3v3", 9},
        {"dropshot", -14},
        {"best", 999},
        {"t", 50},
        {"casual", 25},
        {"2v2_alias", 27}};

    state.ui.graphMmrCategory.store(MmrCategory::OneVOne);
    state.game.sessionTotals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(state.game.sessionTotals.mmrChangeByPlaylist));
    EXPECT_EQ(state.game.sessionTotals.totalMmrChange, 40);

    state.ui.graphMmrCategory.store(MmrCategory::Dropshot);
    state.game.sessionTotals.totalMmrChange = static_cast<float>(
        CalculateTrackedSessionMmrChange(state.game.sessionTotals.mmrChangeByPlaylist));
    EXPECT_EQ(state.game.sessionTotals.totalMmrChange, 40);
}

TEST(MMRFetcherPostMatchTest, KeepsPostMatchRequestBehindRosterRequest) {
    const ConfigData original = Config::Read();
    Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);

    auto state = std::make_shared<SessionState>();
    MMRFetcher fetcher(state);
    fetcher.Enqueue("Steam|123", "Player");
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50, true, true);
    fetcher.EnqueuePostMatch("Steam|123", "Player", "match-guid", "2v2", 1200, 50, true, true);

    EXPECT_EQ(fetcher.PendingRequestCountForTests(), 2u);

    Config::Update([&](ConfigData& config) { config = original; }, false);
}
