#include <gtest/gtest.h>

#include "core/accounts/AccountManager.hpp"
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include "network/api/AccountClient.hpp"
#include "network/http/CurlImpersonate.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/mmr/MMRFetcherDetail.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

using namespace MMRFetcherDetail;

namespace {
    using WriteFn = size_t (*)(void*, size_t, size_t, void*);
    using HeaderFn = size_t (*)(char*, size_t, size_t, void*);

    WriteFn g_write = nullptr;
    void* g_writeData = nullptr;
    HeaderFn g_header = nullptr;
    void* g_headerData = nullptr;
    std::string g_url;
    std::string g_postFields;
    std::vector<std::string> g_postHistory;
    std::string g_apiBody;
    std::vector<long> g_apiStatuses;
    long g_apiStatus = 200;
    long g_trackerStatus = 200;
    std::string g_trackerBody;
    std::string g_headers;
    long g_lastCode = 0;
    std::atomic<int> g_performs{0};

    bool IsApiUrl() {
        return g_url.find("/v1/ranks") != std::string::npos;
    }

    int MockSetopt(void*, int option, ...) {
        va_list args;
        va_start(args, option);
        if (option == CI_CURLOPT_URL) {
            if (const char* u = va_arg(args, const char*)) g_url = u;
        } else if (option == CI_CURLOPT_POSTFIELDS) {
            if (const char* p = va_arg(args, const char*)) {
                g_postFields = p;
                g_postHistory.push_back(p);
            }
        } else if (option == CI_CURLOPT_WRITEFUNCTION) {
            g_write = va_arg(args, WriteFn);
        } else if (option == CI_CURLOPT_WRITEDATA) {
            g_writeData = va_arg(args, void*);
        } else if (option == CI_CURLOPT_HEADERFUNCTION) {
            g_header = va_arg(args, HeaderFn);
        } else if (option == CI_CURLOPT_HEADERDATA) {
            g_headerData = va_arg(args, void*);
        }
        va_end(args);
        return 0;
    }

    int MockPerform(void*) {
        g_performs.fetch_add(1);
        const bool api = IsApiUrl();
        if (api) {
            g_lastCode = g_apiStatuses.empty() ? g_apiStatus : g_apiStatuses.front();
            if (!g_apiStatuses.empty()) g_apiStatuses.erase(g_apiStatuses.begin());
        } else {
            g_lastCode = g_trackerStatus;
        }
        const std::string& body = api ? g_apiBody : g_trackerBody;
        if (g_header && g_headerData && !g_headers.empty()) g_header(g_headers.data(), 1, g_headers.size(), g_headerData);
        if (g_write && g_writeData && !body.empty()) g_write(const_cast<char*>(body.data()), 1, body.size(), g_writeData);
        return 0;
    }

    int MockGetinfo(void*, int info, ...) {
        va_list args;
        va_start(args, info);
        if (info == CI_CURLINFO_RESPONSE_CODE) *va_arg(args, long*) = g_lastCode;
        va_end(args);
        return 0;
    }

    void MockCleanup(void*) {}
    void MockSlistFree(void*) {}
    void* MockSlistAppend(void*, const char*) {
        return reinterpret_cast<void*>(1);
    }
    void* MockInit() {
        return reinterpret_cast<void*>(1);
    }
    std::string g_escaped;
    char* MockEscape(void*, const char* value, int length) {
        g_escaped.assign(value, static_cast<size_t>(length));
        return g_escaped.data();
    }
    void MockFree(void*) {}
    int MockImpersonate(void*, const char*, int) {
        return 0;
    }

    const std::string kEpic = "Epic|0123456789abcdef0123456789abcdef";
    const std::string kEpicOther = "Epic|fedcba9876543210fedcba9876543210";
    const std::string kSteam = "Steam|76561198000000001";
    const std::string kPlayStation = "PS4|1234567890";
    const std::string kXbox = "XboxOne|1234567890";

    nlohmann::json Skill(int playlist, double mmr, int tier = 15, int division = 1, int matches = 40) {
        return {{"playlist", playlist}, {"mmr", mmr}, {"tier", tier}, {"division", division}, {"matches_played", matches}};
    }

    nlohmann::json Player(const std::string& platform, const std::string& accountId, nlohmann::json skills) {
        nlohmann::json player = nlohmann::json::object();
        player["platform"] = platform;
        player["account_id"] = accountId;
        player["skills"] = std::move(skills);
        return player;
    }

    nlohmann::json Player(const std::string& platform, const std::string& accountId, std::initializer_list<nlohmann::json> skills) {
        return Player(platform, accountId, nlohmann::json::array_t(skills.begin(), skills.end()));
    }

    std::string Players(std::initializer_list<nlohmann::json> players) {
        nlohmann::json body = nlohmann::json::object();
        body["players"] = nlohmann::json::array_t(players.begin(), players.end());
        return body.dump();
    }
}

class TrackedAccountRefreshTest : public ::testing::Test {
  protected:
    void SetUp() override {
        originalConfig = Config::Read();
        AccountClient::Instance().ResetForTests();
        UseCustomApi();

        auto& curl = CurlImpersonate::Instance();
        saved = {curl.easy_perform, curl.easy_getinfo, curl.easy_setopt, curl.easy_cleanup, curl.slist_free_all,
                 curl.slist_append, curl.easy_init, curl.easy_escape, curl.free_ptr, curl.easy_impersonate, curl.IsReady()};
        curl.easy_perform = MockPerform;
        curl.easy_getinfo = MockGetinfo;
        curl.easy_setopt = MockSetopt;
        curl.easy_cleanup = MockCleanup;
        curl.slist_free_all = MockSlistFree;
        curl.slist_append = MockSlistAppend;
        curl.easy_init = MockInit;
        curl.easy_escape = MockEscape;
        curl.free_ptr = MockFree;
        curl.easy_impersonate = MockImpersonate;
        curl.SetReadyForTests(true);

        g_url.clear();
        g_postFields.clear();
        g_postHistory.clear();
        g_apiBody.clear();
        g_apiStatuses.clear();
        g_apiStatus = 200;
        g_trackerStatus = 200;
        g_trackerBody.clear();
        g_headers.clear();
        g_performs.store(0);

        state = std::make_shared<SessionState>();
        db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
        fetcher = std::make_shared<MMRFetcher>(state, db);
        fetcher->SetTrackedAccountSink([this](const TrackedAccountFetchEvent& event) {
            std::lock_guard<std::mutex> lock(eventsMutex);
            events.push_back(event);
        });
    }

    void TearDown() override {
        if (service) service->Stop();
        service.reset();
        fetcher->Stop();
        auto& curl = CurlImpersonate::Instance();
        curl.easy_perform = saved.perform;
        curl.easy_getinfo = saved.getinfo;
        curl.easy_setopt = saved.setopt;
        curl.easy_cleanup = saved.cleanup;
        curl.slist_free_all = saved.slistFree;
        curl.slist_append = saved.slistAppend;
        curl.easy_init = saved.init;
        curl.easy_escape = saved.escape;
        curl.free_ptr = saved.free;
        curl.easy_impersonate = saved.impersonate;
        curl.SetReadyForTests(saved.ready);
        AccountClient::Instance().ResetForTests();
        Config::Update([this](ConfigData& c) { c = originalConfig; }, false);
    }

    void UseCustomApi() {
        auto& account = AccountClient::Instance();
        AccountClient::DeviceKeyPair keyPair;
        account.EnsureDeviceKey(&keyPair);
        Config::Update([&](ConfigData& c) {
            c.custom_api_enabled = true;
            c.custom_api_key.clear();
            c.account_refresh_token = "valid_refresh_token";
            c.account_signed_in_name = "SignedInPlayer";
            c.account_device_public_id = keyPair.publicId;
            c.enable_mmr_tracking = false;
        },
                       false);
        account.SyncFromConfig();
        account.SetClockForTests([]() { return 1700000000; });
        account.SetCachedAccessTokenForTests("bearer_token_abc", 1700000900);
    }

    void UseTrackerOnly() {
        Config::Update([](ConfigData& c) {
            c.custom_api_enabled = false;
            c.custom_api_key.clear();
            c.account_refresh_token.clear();
            c.account_signed_in_name.clear();
            c.account_device_public_id.clear();
            c.enable_mmr_tracking = true;
        },
                       false);
        AccountClient::Instance().SyncFromConfig();
    }

    void UseNoProvider() {
        Config::Update([](ConfigData& c) {
            c.custom_api_enabled = false;
            c.custom_api_key.clear();
            c.account_refresh_token.clear();
            c.account_signed_in_name.clear();
            c.account_device_public_id.clear();
            c.enable_mmr_tracking = false;
        },
                       false);
        AccountClient::Instance().SyncFromConfig();
    }

    void StartService(int64_t now = 1'800'000'000) {
        clock = now;
        service = std::make_shared<AccountManagerService>(db, fetcher);
        service->SetClockForTests([this]() { return clock.load(); });
        service->Start();
        service->FlushForTests();
    }

    void RegisterInDb(const std::string& primaryId, const std::string& nickname = {}) {
        const auto identity = TrackedAccounts::FromPrimaryId(primaryId);
        ASSERT_TRUE(identity.has_value());
        ASSERT_EQ(db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*identity, nickname, {}, TrackedAccounts::DiscoverySource::Manual)),
                  TrackedAccountInsertResult::Inserted);
    }

    std::vector<TrackedAccountFetchEvent> Events() {
        std::lock_guard<std::mutex> lock(eventsMutex);
        return events;
    }

    std::optional<TrackedAccountFetchEvent> LastTerminal(const std::string& primaryId) {
        std::optional<TrackedAccountFetchEvent> last;
        for (const auto& event : Events()) {
            if (event.primaryId != primaryId) continue;
            if (event.status == TrackedAccountFetchStatus::Fetching) continue;
            if (event.retryAt != std::chrono::system_clock::time_point{}) continue;
            last = event;
        }
        return last;
    }

    void ProcessNextBatch() {
        const auto batch = fetcher->TakeNextCustomApiBatchForTests();
        ASSERT_FALSE(batch.requests.empty());
        fetcher->ProcessCustomApiBatchForTests(batch);
    }

    const AccountManagerService::AccountView* FindView(const AccountManagerService::Snapshot& snapshot, const std::string& primaryId) {
        for (const auto& view : snapshot.accounts) {
            if (view.record.primaryId == primaryId) return &view;
        }
        return nullptr;
    }

    struct SavedCurl {
        pfn_curl_easy_perform perform;
        pfn_curl_easy_getinfo getinfo;
        pfn_curl_easy_setopt setopt;
        pfn_curl_easy_cleanup cleanup;
        pfn_curl_slist_free_all slistFree;
        pfn_curl_slist_append slistAppend;
        pfn_curl_easy_init init;
        pfn_curl_easy_escape escape;
        pfn_curl_free free;
        pfn_curl_easy_impersonate impersonate;
        bool ready;
    } saved{};

    ConfigData originalConfig;
    std::shared_ptr<SessionState> state;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<MMRFetcher> fetcher;
    std::shared_ptr<AccountManagerService> service;
    std::atomic<int64_t> clock{1'800'000'000};
    std::mutex eventsMutex;
    std::vector<TrackedAccountFetchEvent> events;
};

TEST_F(TrackedAccountRefreshTest, SingleAccountBatchSucceedsAndReportsProfile) {
    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1502.4, 16, 2, 120), Skill(10, 1100)})});
    ASSERT_EQ(fetcher->EnqueueTrackedAccount(kEpic, ""), TrackedEnqueueResult::Queued);
    ProcessNextBatch();

    const auto outcome = LastTerminal(kEpic);
    ASSERT_TRUE(outcome.has_value());
    ASSERT_EQ(outcome->status, TrackedAccountFetchStatus::Success) << outcome->detail;
    EXPECT_EQ(outcome->profile.playlistMMRs.at("2v2"), 1502);
    EXPECT_EQ(outcome->profile.playlistMatches.at("2v2"), 120);
    EXPECT_EQ(outcome->profile.playlistTiers.at("2v2"), "Champion I Div III");
    EXPECT_EQ(outcome->profile.rankVerificationSource, "ServerA");
}

TEST_F(TrackedAccountRefreshTest, MixedPlatformBatchMatchesFullIdentityNotJustAccountId) {
    g_apiBody = Players({Player("Xbox", "1234567890", {Skill(11, 1600)}),
                         Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(13, 1300)})});
    ASSERT_EQ(fetcher->EnqueueTrackedAccount(kPlayStation, ""), TrackedEnqueueResult::Queued);
    ASSERT_EQ(fetcher->EnqueueTrackedAccount(kXbox, ""), TrackedEnqueueResult::Queued);
    ASSERT_EQ(fetcher->EnqueueTrackedAccount(kEpic, ""), TrackedEnqueueResult::Queued);

    const auto batch = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(batch.requests.size(), 3u);
    fetcher->ProcessCustomApiBatchForTests(batch);
    EXPECT_EQ(g_performs.load(), 1);

    const auto posted = nlohmann::json::parse(g_postFields);
    ASSERT_EQ(posted["players"].size(), 3u);
    EXPECT_EQ(posted["players"][0]["platform"], "PS4");
    EXPECT_EQ(posted["players"][1]["platform"], "Xbox");
    EXPECT_FALSE(posted.contains("playlist"));

    const auto playStation = LastTerminal(kPlayStation);
    const auto xbox = LastTerminal(kXbox);
    const auto epic = LastTerminal(kEpic);
    ASSERT_TRUE(playStation && xbox && epic);
    EXPECT_EQ(playStation->status, TrackedAccountFetchStatus::NotFound);
    EXPECT_EQ(xbox->status, TrackedAccountFetchStatus::Success);
    EXPECT_EQ(xbox->profile.playlistMMRs.at("2v2"), 1600);
    EXPECT_EQ(epic->status, TrackedAccountFetchStatus::Success);
    EXPECT_EQ(epic->profile.playlistMMRs.at("3v3"), 1300);
}

TEST_F(TrackedAccountRefreshTest, ResponseWithoutPlatformIsNotAssignedWhenAccountIdIsAmbiguous) {
    nlohmann::json unlabeled = {{"account_id", "1234567890"}, {"skills", nlohmann::json::array({Skill(11, 1700)})}};
    const std::vector<MMRRequest> requests = {
        MMRRequest{.primaryId = kPlayStation, .reason = MMRRequestReason::TrackedAccount},
        MMRRequest{.primaryId = kXbox, .reason = MMRRequestReason::TrackedAccount}};
    const auto results = ParseCustomApiBatchResponse(requests, {{"players", nlohmann::json::array({unlabeled})}});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].status, BatchPlayerStatus::NotFound);
    EXPECT_EQ(results[1].status, BatchPlayerStatus::NotFound);

    const auto single = ParseCustomApiBatchResponse({requests[0]}, {{"players", nlohmann::json::array({Player("Xbox", "1234567890", {Skill(11, 1700)})})}});
    ASSERT_EQ(single.size(), 1u);
    EXPECT_EQ(single[0].status, BatchPlayerStatus::NotFound);
}

TEST_F(TrackedAccountRefreshTest, MissingInvalidAndEmptyPlayersMapToDistinctStatuses) {
    g_apiBody = Players({Player("Epic", "fedcba9876543210fedcba9876543210", nlohmann::json::array({{{"playlist", "bad"}}})),
                         Player("Steam", "76561198000000001", nlohmann::json::array())});
    fetcher->EnqueueTrackedAccount(kEpic, "");
    fetcher->EnqueueTrackedAccount(kEpicOther, "");
    fetcher->EnqueueTrackedAccount(kSteam, "");
    ProcessNextBatch();

    EXPECT_EQ(LastTerminal(kEpic)->status, TrackedAccountFetchStatus::NotFound);
    EXPECT_EQ(LastTerminal(kEpicOther)->status, TrackedAccountFetchStatus::Failed);
    EXPECT_EQ(LastTerminal(kSteam)->status, TrackedAccountFetchStatus::NoRankedData);
}

TEST_F(TrackedAccountRefreshTest, AuthenticationFailuresReportSignInRequired) {
    for (const long code : {401L, 403L}) {
        events.clear();
        AccountClient::Instance().ResetForTests();
        UseCustomApi();
        AccountClient::Instance().SetTransportForTests([code](const std::string&, const std::string&) {
            AccountClient::HttpResponse resp;
            resp.statusCode = code;
            resp.body = R"({"error":{"code":"invalid_grant","message":"Expired"}})";
            return resp;
        });
        g_apiStatus = code;
        g_apiBody = R"({"error":{"code":"invalid_token","message":"Unauthorized"}})";
        fetcher->EnqueueTrackedAccount(kEpic, "");
        ProcessNextBatch();
        const auto outcome = LastTerminal(kEpic);
        ASSERT_TRUE(outcome.has_value()) << code;
        EXPECT_EQ(outcome->status, TrackedAccountFetchStatus::AuthRequired) << code;
    }
}

TEST_F(TrackedAccountRefreshTest, NotFoundAndExhaustedTransientFailuresAreTerminal) {
    g_apiStatus = 404;
    fetcher->EnqueueTrackedAccount(kEpic, "");
    ProcessNextBatch();
    EXPECT_EQ(LastTerminal(kEpic)->status, TrackedAccountFetchStatus::NotFound);

    g_apiStatus = 503;
    fetcher->EnqueueTrackedAccount(kSteam, "");
    ProcessNextBatch();
    EXPECT_EQ(LastTerminal(kSteam)->status, TrackedAccountFetchStatus::Failed);
}

TEST_F(TrackedAccountRefreshTest, RateLimitHonoursRetryAfterThenSucceeds) {
    g_headers = "Retry-After: 0\r\n";
    g_apiStatuses = {429, 200};
    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1450)})});
    fetcher->Start();
    ASSERT_EQ(fetcher->EnqueueTrackedAccount(kEpic, ""), TrackedEnqueueResult::Queued);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto outcome = LastTerminal(kEpic);
        if (outcome && outcome->status == TrackedAccountFetchStatus::Success) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    fetcher->Stop();

    const auto all = Events();
    const bool sawRateLimit = std::any_of(all.begin(), all.end(), [](const TrackedAccountFetchEvent& e) {
        return e.status == TrackedAccountFetchStatus::RateLimited && e.retryAt != std::chrono::system_clock::time_point{};
    });
    EXPECT_TRUE(sawRateLimit);
    ASSERT_TRUE(LastTerminal(kEpic).has_value());
    EXPECT_EQ(LastTerminal(kEpic)->status, TrackedAccountFetchStatus::Success);
    EXPECT_EQ(g_performs.load(), 2);
}

TEST_F(TrackedAccountRefreshTest, RateLimitWithoutRetryBudgetIsReportedAsRateLimited) {
    g_apiStatus = 429;
    fetcher->EnqueueTrackedAccount(kEpic, "");
    ProcessNextBatch();
    EXPECT_TRUE(fetcher->IsRateLimitedForTests());
    EXPECT_EQ(LastTerminal(kEpic)->status, TrackedAccountFetchStatus::RateLimited);
}

TEST_F(TrackedAccountRefreshTest, DuplicateRequestsCoalesceAndDoNotShareRosterDeduplication) {
    EXPECT_EQ(fetcher->EnqueueTrackedAccount(kSteam, ""), TrackedEnqueueResult::Queued);
    EXPECT_EQ(fetcher->EnqueueTrackedAccount(kSteam, ""), TrackedEnqueueResult::AlreadyQueued);
    fetcher->Enqueue(kSteam, "Live");
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 2u);

    const auto roster = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(roster.requests.size(), 1u);
    EXPECT_EQ(roster.requests[0].reason, MMRRequestReason::Roster);
    const auto tracked = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(tracked.requests.size(), 1u);
    EXPECT_EQ(tracked.requests[0].reason, MMRRequestReason::TrackedAccount);

    fetcher->FinishRequestForTests(tracked.requests[0]);
    EXPECT_EQ(fetcher->EnqueueTrackedAccount(kSteam, ""), TrackedEnqueueResult::Queued);
}

TEST_F(TrackedAccountRefreshTest, LiveRequestsTakePriorityAndRefreshAllRespectsBatchCapacity) {
    for (int i = 0; i < 20; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "Steam|765611980000001%02d", i);
        ASSERT_EQ(fetcher->EnqueueTrackedAccount(id, ""), TrackedEnqueueResult::Queued);
    }
    fetcher->Enqueue("Epic|live_opponent|0", "Opponent");

    auto batch = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(batch.requests.size(), 1u);
    EXPECT_EQ(batch.requests[0].reason, MMRRequestReason::Roster);

    batch = fetcher->TakeNextCustomApiBatchForTests();
    EXPECT_EQ(batch.requests.size(), 8u);
    batch = fetcher->TakeNextCustomApiBatchForTests();
    EXPECT_EQ(batch.requests.size(), 8u);
    batch = fetcher->TakeNextCustomApiBatchForTests();
    EXPECT_EQ(batch.requests.size(), 4u);
    EXPECT_TRUE(fetcher->TakeNextCustomApiBatchForTests().requests.empty());

    fetcher->EnqueueTrackedAccount(kEpic, "");
    fetcher->Enqueue("Epic|live_teammate|0", "Teammate");
    const auto single = fetcher->TakeNextTrackerRequestForTests();
    ASSERT_TRUE(single.has_value());
    EXPECT_EQ(single->reason, MMRRequestReason::Roster);
    const auto next = fetcher->TakeNextTrackerRequestForTests();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->reason, MMRRequestReason::TrackedAccount);

    std::vector<MMRRequest> candidates = {MMRRequest{.primaryId = kEpic, .reason = MMRRequestReason::TrackedAccount},
                                          MMRRequest{.primaryId = "Epic|p1|0", .reason = MMRRequestReason::Roster}};
    const auto assembled = AssembleCustomApiBatch(candidates, std::chrono::steady_clock::now(), 16);
    ASSERT_EQ(assembled.requests.size(), 1u);
    EXPECT_EQ(assembled.requests[0].reason, MMRRequestReason::Roster);
}

TEST_F(TrackedAccountRefreshTest, ProviderUnavailableIsReportedWithoutQueueing) {
    UseNoProvider();
    EXPECT_EQ(fetcher->EnqueueTrackedAccount(kEpic, ""), TrackedEnqueueResult::ProviderUnavailable);
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 0u);
    EXPECT_EQ(MMRFetcher::ActiveRankProvider(), RankProvider::Unavailable);

    RegisterInDb(kEpic, "Main");
    StartService();
    EXPECT_EQ(service->Refresh(kEpic), AccountManagerService::RefreshRequest::ProviderUnavailable);
    const auto snapshot = service->GetSnapshot();
    ASSERT_NE(FindView(snapshot, kEpic), nullptr);
    EXPECT_EQ(FindView(snapshot, kEpic)->refreshState, AccountManagerService::RefreshState::ProviderUnavailable);
}

TEST_F(TrackedAccountRefreshTest, TrackerPathNeedsNameForEpicAndUsesSteamId) {
    UseTrackerOnly();
    fetcher->FetchTrackedAccountForTests(kEpic, "");
    EXPECT_EQ(g_performs.load(), 0);
    EXPECT_EQ(LastTerminal(kEpic)->status, TrackedAccountFetchStatus::Unsupported);

    g_trackerBody = R"({"data":{"segments":[{"type":"playlist","attributes":{"playlistId":11},"stats":{"rating":{"value":1333},"tier":{"metadata":{"name":"Champion I"}},"matchesPlayed":{"value":12}}}]}})";
    fetcher->FetchTrackedAccountForTests(kSteam, "");
    EXPECT_NE(g_url.find("/profile/steam/76561198000000001"), std::string::npos);
    const auto outcome = LastTerminal(kSteam);
    ASSERT_TRUE(outcome.has_value());
    EXPECT_EQ(outcome->status, TrackedAccountFetchStatus::Success);
    EXPECT_EQ(outcome->profile.playlistMMRs.at("2v2"), 1333);
    EXPECT_EQ(outcome->profile.rankVerificationSource, "Tracker");

    g_trackerStatus = 404;
    fetcher->FetchTrackedAccountForTests(kEpicOther, "SomeName");
    EXPECT_EQ(LastTerminal(kEpicOther)->status, TrackedAccountFetchStatus::NotFound);
}

TEST_F(TrackedAccountRefreshTest, TrackerHandleFillsMissingInGameNameOnly) {
    UseTrackerOnly();
    RegisterInDb(kSteam, "Main");
    StartService();

    g_trackerBody = R"({"data":{"platformInfo":{"platformUserHandle":"SteamHandle"},"segments":[{"type":"playlist","attributes":{"playlistId":11},"stats":{"rating":{"value":1333}}}]}})";
    fetcher->FetchTrackedAccountForTests(kSteam, "");
    service->FlushForTests();
    EXPECT_EQ(db->GetTrackedAccount(kSteam)->lastKnownName, "SteamHandle");
    EXPECT_EQ(db->GetTrackedAccount(kSteam)->nickname, "Main");

    g_trackerBody = R"({"data":{"platformInfo":{"platformUserHandle":"Renamed"},"segments":[{"type":"playlist","attributes":{"playlistId":11},"stats":{"rating":{"value":1340}}}]}})";
    fetcher->FetchTrackedAccountForTests(kSteam, "SteamHandle");
    service->FlushForTests();
    EXPECT_EQ(db->GetTrackedAccount(kSteam)->lastKnownName, "SteamHandle");
}

TEST_F(TrackedAccountRefreshTest, TrackedRefreshNeverTouchesLiveGameState) {
    const std::string liveId = kSteam;
    {
        std::unique_lock gameLock(state->game.mutex);
        state->game.myPrimaryId = liveId;
        state->game.inMatch = true;
        state->game.matchGuid = "live-match";
        state->game.roster[liveId] = PlayerData{.primaryId = liveId, .name = "Me", .team = 0, .mmr = 1200, .fetched = true};
        state->game.roster[liveId].playlists = {{"2v2", 1200}};
    }
    {
        std::unique_lock historyLock(state->history.mutex);
        state->history.SelectMmrOwner(liveId);
        state->history.mmrHistoryY = {1190.0f, 1200.0f};
        state->history.playlistHistoryY["2v2"] = {1190.0f, 1200.0f};
        state->history.playlistInitialMmr["2v2"] = 1190;
    }
    const uint64_t gameVersion = state->game.version.load();
    const uint64_t historyVersion = state->history.version.load();

    g_apiBody = Players({Player("Steam", "76561198000000001", {Skill(11, 1777)}),
                         Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1500)})});
    fetcher->EnqueueTrackedAccount(liveId, "Me");
    fetcher->EnqueueTrackedAccount(kEpic, "");
    ProcessNextBatch();
    ASSERT_EQ(LastTerminal(liveId)->status, TrackedAccountFetchStatus::Success);

    g_apiStatus = 500;
    fetcher->EnqueueTrackedAccount(liveId, "Me");
    ProcessNextBatch();
    ASSERT_EQ(LastTerminal(liveId)->status, TrackedAccountFetchStatus::Failed);

    std::shared_lock gameLock(state->game.mutex);
    std::shared_lock historyLock(state->history.mutex);
    EXPECT_EQ(state->game.myPrimaryId, liveId);
    ASSERT_EQ(state->game.roster.size(), 1u);
    EXPECT_EQ(state->game.roster.at(liveId).mmr, 1200);
    EXPECT_EQ(state->game.roster.at(liveId).playlists.at("2v2"), 1200);
    EXPECT_TRUE(state->game.roster.at(liveId).fetched);
    EXPECT_FALSE(state->game.roster.at(liveId).fetchFailed);
    EXPECT_TRUE(state->game.preMatchMmrByGuid.empty());
    EXPECT_EQ(state->history.mmrOwnerPrimaryId, liveId);
    EXPECT_EQ(state->history.mmrHistoryY, (std::vector<float>{1190.0f, 1200.0f}));
    EXPECT_EQ(state->history.playlistHistoryY.at("2v2"), (std::vector<float>{1190.0f, 1200.0f}));
    EXPECT_EQ(state->history.playlistInitialMmr.at("2v2"), 1190);
    EXPECT_TRUE(state->history.playlistMatchPoints.empty());
    EXPECT_EQ(state->game.version.load(), gameVersion);
    EXPECT_EQ(state->history.version.load(), historyVersion);
}

TEST_F(TrackedAccountRefreshTest, PostMatchReconciliationIsUnaffectedByQueuedTrackedRefreshes) {
    UseTrackerOnly();
    fetcher->EnqueueTrackedAccount("Steam|76561198000000123", "");
    fetcher->EnqueuePostMatch("Steam|76561198000000123", "Me", "pm-guid", "2v2", 1200, 50, true, true);
    fetcher->ProcessPostMatchResponseForTests("pm-guid", 1209, 51);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "pm-guid");
    EXPECT_EQ(points[0].mmr, 1209);
    EXPECT_TRUE(points[0].trackerCovered);

    const auto next = fetcher->TakeNextTrackerRequestForTests();
    ASSERT_TRUE(next.has_value());
    EXPECT_EQ(next->reason, MMRRequestReason::TrackedAccount);
}

TEST_F(TrackedAccountRefreshTest, ServicePersistsPartialResultsAndKeepsRanksOnFailure) {
    RegisterInDb(kEpic, "Main");
    ASSERT_TRUE(db->SaveTrackedAccountRanks(kEpic,
                                            {TrackedAccounts::PlaylistRank{"1v1", 1100, "Diamond III Div I", 30, "OmniStats API", 0},
                                             TrackedAccounts::PlaylistRank{"2v2", 1500, "Champion II Div I", 200, "OmniStats API", 0},
                                             TrackedAccounts::PlaylistRank{"3v3", 1400, "Champion I Div IV", 90, "OmniStats API", 0}},
                                            1'700'000'000));
    StartService();

    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1525, 17, 0, 201)})});
    ASSERT_EQ(service->Refresh(kEpic), AccountManagerService::RefreshRequest::Queued);
    EXPECT_EQ(service->Refresh(kEpic), AccountManagerService::RefreshRequest::AlreadyQueued);
    ProcessNextBatch();
    service->FlushForTests();

    auto stored = db->GetTrackedAccount(kEpic);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->ranks.at("2v2").mmr, 1525);
    EXPECT_EQ(stored->ranks.at("1v1").mmr, 1100);
    EXPECT_EQ(stored->ranks.at("3v3").mmr, 1400);
    EXPECT_EQ(stored->lastSuccessfulRefreshAtUnix, clock.load());

    clock += 3600;
    g_apiStatus = 500;
    EXPECT_EQ(service->Refresh(kEpic), AccountManagerService::RefreshRequest::Queued);
    ProcessNextBatch();
    service->FlushForTests();

    const auto snapshot = service->GetSnapshot();
    const auto* view = FindView(snapshot, kEpic);
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(view->refreshState, AccountManagerService::RefreshState::Failed);
    EXPECT_EQ(view->record.ranks.at("2v2").mmr, 1525);
    EXPECT_EQ(view->record.lastSuccessfulRefreshAtUnix, clock.load() - 3600);
    EXPECT_EQ(view->record.lastRefreshAttemptAtUnix, clock.load());
    stored = db->GetTrackedAccount(kEpic);
    EXPECT_EQ(stored->ranks.at("2v2").mmr, 1525);
    EXPECT_EQ(stored->lastSuccessfulRefreshAtUnix, clock.load() - 3600);
}

TEST_F(TrackedAccountRefreshTest, ManualRefreshCooldownAndStaleRefresh) {
    RegisterInDb(kEpic);
    RegisterInDb(kSteam);
    ASSERT_TRUE(db->SaveTrackedAccountRanks(kSteam, {TrackedAccounts::PlaylistRank{"2v2", 1500, "", std::nullopt, "OmniStats API", 0}}, 1'800'000'000));
    StartService(1'800'000'100);

    EXPECT_EQ(service->RefreshStale(), 1u);
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 1u);
    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1300)})});
    ProcessNextBatch();
    EXPECT_EQ(service->RefreshStale(), 0u);

    EXPECT_EQ(service->Refresh(kSteam), AccountManagerService::RefreshRequest::Queued);
    ProcessNextBatch();
    EXPECT_EQ(service->Refresh(kSteam), AccountManagerService::RefreshRequest::CoolingDown);
    clock += AccountManagerService::kManualRefreshCooldownSeconds;
    EXPECT_EQ(service->Refresh(kSteam), AccountManagerService::RefreshRequest::Queued);
}

TEST_F(TrackedAccountRefreshTest, OnlyLaunchQueuesStaleAccountsAndDiscoveryDoesNotFetch) {
    RegisterInDb(kEpic);
    RegisterInDb(kSteam);
    ASSERT_TRUE(db->SaveTrackedAccountRanks(kSteam, {TrackedAccounts::PlaylistRank{"2v2", 1500, "", std::nullopt, "OmniStats API", 0}}, 1'800'000'000));
    StartService(1'800'000'100);
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 0u);

    service->QueueStartupRefresh();
    service->FlushForTests();
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 1u);
    const auto launched = service->GetSnapshot();
    const auto* queued = FindView(launched, kEpic);
    ASSERT_NE(queued, nullptr);
    EXPECT_EQ(queued->refreshState, AccountManagerService::RefreshState::Queued);

    service->ObserveLocalAccount(kEpicOther + "|0", "New Alt");
    service->FlushForTests();
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 1u);
    const auto snapshot = service->GetSnapshot();
    const auto* discovered = FindView(snapshot, kEpicOther);
    ASSERT_NE(discovered, nullptr);
    EXPECT_EQ(discovered->refreshState, AccountManagerService::RefreshState::Idle);
}

TEST_F(TrackedAccountRefreshTest, RemovingDuringInFlightRefreshDoesNotRecreateAccount) {
    StartService();
    const auto added = service->AddManual(TrackedAccounts::Platform::Epic, "0123456789ABCDEF0123456789ABCDEF", "Alt");
    ASSERT_EQ(added.status, AccountManagerService::AddResult::Status::Added);
    EXPECT_EQ(added.primaryId, kEpic);
    service->FlushForTests();

    const auto batch = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(batch.requests.size(), 1u);
    ASSERT_TRUE(service->Remove(kEpic));
    service->FlushForTests();

    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 1500)})});
    fetcher->ProcessCustomApiBatchForTests(batch);
    service->FlushForTests();

    EXPECT_FALSE(db->GetTrackedAccount(kEpic).has_value());
    EXPECT_TRUE(db->GetTrackedAccountRanks(kEpic).empty());
    EXPECT_TRUE(service->GetSnapshot().accounts.empty());

    const auto readded = service->AddManual(TrackedAccounts::Platform::Epic, kEpic, "Alt again");
    ASSERT_EQ(readded.status, AccountManagerService::AddResult::Status::Added);
    service->FlushForTests();
    ProcessNextBatch();
    service->FlushForTests();
    const auto stored = db->GetTrackedAccount(kEpic);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->nickname, "Alt again");
    EXPECT_EQ(stored->ranks.at("2v2").mmr, 1500);
}

TEST_F(TrackedAccountRefreshTest, FiveManualAccountsRefreshInOneBatchAndSurviveRestart) {
    StartService();
    const std::vector<std::pair<TrackedAccounts::Platform, std::string>> inputs = {
        {TrackedAccounts::Platform::Epic, "0123456789abcdef0123456789abcdef"},
        {TrackedAccounts::Platform::Epic, "fedcba9876543210fedcba9876543210"},
        {TrackedAccounts::Platform::Steam, "76561198000000001"},
        {TrackedAccounts::Platform::Steam, "https://steamcommunity.com/profiles/76561198000000002/"},
        {TrackedAccounts::Platform::Steam, "Steam|76561198000000003|0"}};
    int index = 0;
    for (const auto& [platform, input] : inputs) {
        const auto result = service->AddManual(platform, input, "Account " + std::to_string(++index));
        ASSERT_EQ(result.status, AccountManagerService::AddResult::Status::Added) << input;
    }
    EXPECT_EQ(service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "Dupe").status,
              AccountManagerService::AddResult::Status::AlreadyTracked);

    g_apiBody = Players({Player("Epic", "0123456789abcdef0123456789abcdef", {Skill(11, 2015), Skill(10, 1740)}),
                         Player("Epic", "fedcba9876543210fedcba9876543210", {Skill(11, 1462)}),
                         Player("Steam", "76561198000000001", {Skill(11, 1530)}),
                         Player("Steam", "76561198000000002", {Skill(13, 940)}),
                         Player("Steam", "76561198000000003", {Skill(11, 1020)})});
    service->FlushForTests();
    const auto batch = fetcher->TakeNextCustomApiBatchForTests();
    ASSERT_EQ(batch.requests.size(), 5u);
    fetcher->ProcessCustomApiBatchForTests(batch);
    service->Stop();
    service.reset();

    StartService();
    const auto snapshot = service->GetSnapshot();
    ASSERT_TRUE(snapshot.loaded);
    ASSERT_EQ(snapshot.accounts.size(), 5u);
    EXPECT_EQ(snapshot.accounts[0].record.nickname, "Account 1");
    EXPECT_EQ(snapshot.accounts[0].record.ranks.at("2v2").mmr, 2015);
    EXPECT_EQ(snapshot.accounts[0].record.ranks.at("1v1").mmr, 1740);
    EXPECT_EQ(snapshot.accounts[3].record.primaryId, "Steam|76561198000000002");
    EXPECT_FALSE(snapshot.accounts[3].record.ranks.count("2v2"));
    EXPECT_EQ(snapshot.accounts[4].record.ranks.at("2v2").mmr, 1020);
}
