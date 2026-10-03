#include <gtest/gtest.h>
#include "network/MMRFetcher.hpp"
#include "core/SessionState.hpp"
#include "core/Config.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr const char* kMe = "Steam|1001";
    constexpr const char* kAlt = "Epic|2002";

    struct Rating {
        int mmr = 0;
        int matches = -1;
    };

    class SessionMmrLifecycleTest : public ::testing::Test {
      protected:
        void SetUp() override {
            originalConfig = Config::Read();
            Config::Update([](ConfigData& config) { config.enable_mmr_tracking = true; }, false);
            state = std::make_shared<SessionState>();
            fetcher = std::make_shared<MMRFetcher>(state);
            state->game.myPrimaryId = kMe;
        }

        void TearDown() override {
            Config::Update([this](ConfigData& config) { config = originalConfig; }, false);
        }

        void PublishRoster(const std::map<std::string, Rating>& playlists,
                           const std::string& primaryId = kMe,
                           std::optional<uint64_t> fetchedInSession = std::nullopt) {
            MMRRequest req;
            req.primaryId = primaryId;
            req.name = "Player";
            req.reason = MMRRequestReason::Roster;
            req.sessionGeneration = fetchedInSession.value_or(state->game.sessionGeneration.load());
            NormalizedProfileResult profile;
            for (const auto& [playlist, rating] : playlists) {
                profile.playlistMMRs[playlist] = rating.mmr;
                profile.playlistMatches[playlist] = rating.matches;
                profile.bestMmr = (std::max)(profile.bestMmr, rating.mmr);
            }
            fetcher->PublishProfileResultForTests(req, profile);
        }

        void EnqueueMatch(const std::string& guid, const std::string& playlist, bool won,
                          Rating pre, const std::string& primaryId = kMe) {
            fetcher->EnqueuePostMatch(primaryId, "Player", guid, playlist, pre.mmr, pre.matches,
                                      pre.mmr > 0, won);
        }

        void PlayMatch(const std::string& guid, const std::string& playlist, bool won,
                       Rating pre, Rating post, const std::string& primaryId = kMe) {
            EnqueueMatch(guid, playlist, won, pre, primaryId);
            fetcher->ProcessPostMatchResponseForTests(guid, post.mmr, post.matches);
        }

        void StartNewSession() {
            std::unique_lock gameLock(state->game.mutex);
            std::unique_lock historyLock(state->history.mutex);
            state->startNewSessionLocked(1700000000);
        }

        void SwitchAccount(const std::string& primaryId) {
            std::unique_lock gameLock(state->game.mutex);
            state->game.myPrimaryId = primaryId;
            std::unique_lock historyLock(state->history.mutex);
            state->selectMmrOwnerLocked(primaryId);
        }

        std::optional<int> SessionChange(const std::string& playlist) {
            std::shared_lock lock(state->game.mutex);
            const auto& changes = state->game.sessionTotals.mmrChangeByPlaylist;
            const auto it = changes.find(playlist);
            if (it == changes.end()) return std::nullopt;
            return it->second;
        }

        int TotalChange() {
            std::shared_lock lock(state->game.mutex);
            return static_cast<int>(state->game.sessionTotals.totalMmrChange);
        }

        std::vector<float> Graph(const std::string& playlist) {
            std::shared_lock lock(state->history.mutex);
            const auto it = state->history.playlistHistoryY.find(playlist);
            return it == state->history.playlistHistoryY.end() ? std::vector<float>{} : it->second;
        }

        std::optional<int> Baseline(const std::string& playlist) {
            std::shared_lock lock(state->history.mutex);
            const auto it = state->history.playlistInitialMmr.find(playlist);
            if (it == state->history.playlistInitialMmr.end()) return std::nullopt;
            return it->second;
        }

        void RecordCountedGames(int wins) {
            std::unique_lock lock(state->game.mutex);
            state->game.sessionTotals.wins += wins;
        }

        std::optional<int> RecapChange(const std::string& playlist) {
            std::shared_lock lock(state->game.mutex);
            const auto& changes = state->game.lastSessionRecap.totals.mmrChangeByPlaylist;
            const auto it = changes.find(playlist);
            if (it == changes.end()) return std::nullopt;
            return it->second;
        }

        int RecapTotal() {
            std::shared_lock lock(state->game.mutex);
            return static_cast<int>(state->game.lastSessionRecap.totals.totalMmrChange);
        }

        std::shared_ptr<SessionState> state;
        std::shared_ptr<MMRFetcher> fetcher;
        ConfigData originalConfig;
    };
} // namespace

TEST_F(SessionMmrLifecycleTest, ConsecutiveSessionsMeasureFromTheirOwnStartingMmr) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("s1-g1", "2v2", true, {1200, 50}, {1210, 51});
    PlayMatch("s1-g2", "2v2", true, {1210, 51}, {1220, 52});
    ASSERT_EQ(SessionChange("2v2"), 20);

    StartNewSession();
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);

    PublishRoster({{"2v2", {1220, 52}}});
    EXPECT_EQ(SessionChange("2v2"), 0);

    PlayMatch("s2-g1", "2v2", false, {1220, 52}, {1211, 53});
    EXPECT_EQ(SessionChange("2v2"), -9);
    EXPECT_EQ(TotalChange(), -9);

    PlayMatch("s2-g2", "2v2", true, {1211, 53}, {1220, 54});
    EXPECT_EQ(SessionChange("2v2"), 0);

    PlayMatch("s2-g3", "2v2", true, {1220, 54}, {1231, 55});
    EXPECT_EQ(SessionChange("2v2"), 11);
    EXPECT_EQ(Baseline("2v2"), 1220);
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1220.0f, 1211.0f, 1220.0f, 1231.0f}));
}

TEST_F(SessionMmrLifecycleTest, NewSessionGraphStartsWithoutPreviousSessionPoints) {
    PublishRoster({{"2v2", {1200, 50}}, {"3v3", {900, 100}}});
    PlayMatch("s1-g1", "2v2", true, {1200, 50}, {1210, 51});
    PlayMatch("s1-g2", "3v3", true, {900, 100}, {909, 101});
    state->ui.graphOffset.store(5);
    const uint64_t historyVersion = state->history.version.load();

    StartNewSession();
    {
        std::shared_lock lock(state->history.mutex);
        EXPECT_TRUE(state->history.playlistHistoryY.empty());
        EXPECT_TRUE(state->history.playlistMatchPoints.empty());
        EXPECT_TRUE(state->history.playlistInitialMmr.empty());
        EXPECT_TRUE(state->history.mmrHistoryY.empty());
        EXPECT_EQ(state->history.initialMmr, -1);
    }
    EXPECT_GT(state->history.version.load(), historyVersion);
    EXPECT_EQ(state->ui.graphOffset.load(), 0);

    PublishRoster({{"2v2", {1210, 51}}, {"3v3", {909, 101}}});
    PlayMatch("s2-g1", "2v2", false, {1210, 51}, {1201, 52});
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1210.0f, 1201.0f}));
    EXPECT_EQ(Graph("3v3"), (std::vector<float>{909.0f}));
    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "s2-g1");
    EXPECT_EQ(points[0].historyIndex, 1u);
}

TEST_F(SessionMmrLifecycleTest, EachPlaylistKeepsAnIndependentSessionBaseline) {
    PublishRoster({{"2v2", {1200, 50}}, {"3v3", {900, 100}}});
    PlayMatch("twos-1", "2v2", true, {1200, 50}, {1209, 51});
    PlayMatch("threes-1", "3v3", false, {900, 100}, {891, 101});
    PlayMatch("twos-2", "2v2", true, {1209, 51}, {1218, 52});

    EXPECT_EQ(SessionChange("2v2"), 18);
    EXPECT_EQ(SessionChange("3v3"), -9);
    EXPECT_EQ(TotalChange(), 9);

    StartNewSession();
    PlayMatch("ones-1", "1v1", false, {700, 20}, {690, 21});
    PlayMatch("threes-2", "3v3", true, {891, 101}, {900, 102});

    EXPECT_EQ(SessionChange("1v1"), -10);
    EXPECT_EQ(SessionChange("3v3"), 9);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(Baseline("1v1"), 700);
    EXPECT_EQ(Baseline("3v3"), 891);
    EXPECT_EQ(TotalChange(), -1);
}

TEST_F(SessionMmrLifecycleTest, SwitchingAccountsNeverMixesSessionMmr) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("me-win", "2v2", true, {1200, 50}, {1220, 51});
    PlayMatch("me-pending", "2v2", false, {1220, 51}, {1220, 51});
    ASSERT_EQ(SessionChange("2v2"), 11);

    SwitchAccount(kAlt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);
    EXPECT_TRUE(Graph("2v2").empty());

    PublishRoster({{"2v2", {1500, 300}}}, kAlt);
    PlayMatch("alt-loss", "2v2", false, {1500, 300}, {1491, 301}, kAlt);
    EXPECT_EQ(SessionChange("2v2"), -9);
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1500.0f, 1491.0f}));

    SwitchAccount(kMe);
    EXPECT_EQ(SessionChange("2v2"), 11);
    EXPECT_EQ(TotalChange(), 11);
    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 2u);
    EXPECT_EQ(points[1].matchGuid, "me-pending");
    EXPECT_EQ(points[1].mmr, 1211);
    EXPECT_FALSE(points[1].trackerCovered);

    fetcher->ProcessPostMatchResponseForTests("me-pending", 1212, 52);
    EXPECT_EQ(SessionChange("2v2"), 12);
}

TEST_F(SessionMmrLifecycleTest, ProfileFetchedBeforeTheResetCannotSeedTheNewSession) {
    PublishRoster({{"2v2", {1200, 50}}});
    const uint64_t previousSession = state->game.sessionGeneration.load();
    StartNewSession();

    PublishRoster({{"2v2", {1210, 51}}}, kMe, previousSession);
    EXPECT_TRUE(Graph("2v2").empty());
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    {
        std::shared_lock lock(state->game.mutex);
        EXPECT_EQ(state->game.roster.at(kMe).playlists.at("2v2"), 1210);
    }

    PublishRoster({{"2v2", {1220, 52}}});
    PlayMatch("s2-g1", "2v2", false, {1220, 52}, {1211, 53});
    EXPECT_EQ(Baseline("2v2"), 1220);
    EXPECT_EQ(SessionChange("2v2"), -9);
}

TEST_F(SessionMmrLifecycleTest, PreviousSessionMatchConfirmedLateOnlyStartsTheNewSession) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("s1-g1", "2v2", true, {1200, 50}, {1210, 51});
    EnqueueMatch("s1-g2", "2v2", true, {1210, 51});
    StartNewSession();

    PublishRoster({{"2v2", {1210, 51}}});
    EXPECT_TRUE(Graph("2v2").empty());
    EXPECT_EQ(Baseline("2v2"), std::nullopt);

    fetcher->ProcessPostMatchResponseForTests("s1-g2", 1220, 52);
    EXPECT_EQ(Baseline("2v2"), 1220);
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1220.0f}));
    EXPECT_TRUE(fetcher->PlaylistMatchPointsForTests("2v2").empty());
    EXPECT_EQ(SessionChange("2v2"), 0);

    PlayMatch("s2-g1", "2v2", false, {1220, 52}, {1211, 53});
    EXPECT_EQ(SessionChange("2v2"), -9);
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1220.0f, 1211.0f}));
}

TEST_F(SessionMmrLifecycleTest, NewSessionMatchBehindUnpublishedPreviousSessionMatch) {
    PublishRoster({{"2v2", {1200, 50}}});
    EnqueueMatch("s1-g1", "2v2", true, {1200, 50});
    StartNewSession();

    EnqueueMatch("s2-g1", "2v2", false, {1210, 51});
    fetcher->ProcessPostMatchResponseForTests("s2-g1", 1201, 52);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].matchGuid, "s2-g1");
    EXPECT_EQ(Baseline("2v2"), 1210);
    EXPECT_EQ(SessionChange("2v2"), -9);
}

TEST_F(SessionMmrLifecycleTest, StaleSnapshotBehindPreviousSessionMatchIsNotABaseline) {
    PublishRoster({{"2v2", {1200, 50}}});
    EnqueueMatch("s1-g1", "2v2", true, {1200, 50});
    StartNewSession();

    EnqueueMatch("s2-g1", "2v2", false, {1200, 50});
    fetcher->ProcessPostMatchResponseForTests("s2-g1", 1201, 52);

    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_EQ(points[0].mmr, 1201);
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);
}

TEST_F(SessionMmrLifecycleTest, MissingPreMatchRatingNeverBecomesTheBaseline) {
    EnqueueMatch("no-pre", "2v2", false, {0, -1});
    fetcher->ProcessPostMatchResponseForTests("no-pre", 1211, 53);

    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1211.0f}));
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);

    fetcher->ProcessPostMatchResponseForTests("no-pre", 1211, 54);
    PublishRoster({{"2v2", {1211, 54}}});
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);
}

TEST_F(SessionMmrLifecycleTest, BestPlaylistMmrIsNeverUsedAsAPlaylistBaseline) {
    {
        std::unique_lock lock(state->game.mutex);
        state->game.roster[kMe] = PlayerData{.primaryId = kMe, .name = "Player", .mmr = 1500};
    }
    EnqueueMatch("unknown-twos", "2v2", true, {0, -1});
    fetcher->ProcessPostMatchResponseForTests("unknown-twos", 0, -1);
    EXPECT_TRUE(fetcher->PlaylistMatchPointsForTests("2v2").empty());

    fetcher->ProcessPostMatchResponseForTests("unknown-twos", 900, 31);
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);
}

TEST_F(SessionMmrLifecycleTest, LastConfirmedProfileReconstructsAMissingPreMatchRating) {
    PublishRoster({{"2v2", {1220, 52}}});
    StartNewSession();

    EnqueueMatch("s2-g1", "2v2", false, {0, -1});
    fetcher->ProcessPostMatchResponseForTests("s2-g1", 1211, 53);

    EXPECT_EQ(Baseline("2v2"), 1220);
    EXPECT_EQ(SessionChange("2v2"), -9);
    const auto points = fetcher->PlaylistMatchPointsForTests("2v2");
    ASSERT_EQ(points.size(), 1u);
    EXPECT_TRUE(points[0].trackerCovered);
    EXPECT_FALSE(points[0].valueEstimated);
}

TEST_F(SessionMmrLifecycleTest, UntrackedMatchesInvalidateTheReconstructedBaseline) {
    PublishRoster({{"2v2", {1220, 52}}});
    StartNewSession();

    EnqueueMatch("s2-g1", "2v2", false, {0, -1});
    fetcher->ProcessPostMatchResponseForTests("s2-g1", 1211, 55);

    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_EQ(TotalChange(), 0);
}

TEST_F(SessionMmrLifecycleTest, NewSessionFetchesAFreshProfileInsteadOfTheCachedOne) {
    {
        std::unique_lock lock(state->game.mutex);
        state->game.roster[kMe] = PlayerData{.primaryId = kMe, .name = "Player"};
    }
    PublishRoster({{"2v2", {1220, 52}}});
    fetcher->Enqueue(kMe, "Player");
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 0u);

    StartNewSession();
    fetcher->Enqueue(kMe, "Player");
    EXPECT_EQ(fetcher->PendingRequestCountForTests(), 1u);
}

TEST_F(SessionMmrLifecycleTest, RestartedAppStartsTrackingFromItsFirstProfile) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("run1-g1", "2v2", true, {1200, 50}, {1220, 51});
    ASSERT_EQ(SessionChange("2v2"), 20);

    fetcher.reset();
    state = std::make_shared<SessionState>();
    fetcher = std::make_shared<MMRFetcher>(state);
    state->game.myPrimaryId = kMe;
    EXPECT_EQ(SessionChange("2v2"), std::nullopt);
    EXPECT_TRUE(Graph("2v2").empty());

    PublishRoster({{"2v2", {1220, 51}}});
    PlayMatch("run2-g1", "2v2", false, {1220, 51}, {1211, 52});
    EXPECT_EQ(Baseline("2v2"), 1220);
    EXPECT_EQ(SessionChange("2v2"), -9);
    EXPECT_EQ(Graph("2v2"), (std::vector<float>{1220.0f, 1211.0f}));
}

TEST_F(SessionMmrLifecycleTest, LateConfirmationFinishesTheEndedSessionRecap) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("s1-g1", "2v2", true, {1200, 50}, {1210, 51});
    PlayMatch("s1-g2", "2v2", true, {1210, 51}, {1210, 51});
    RecordCountedGames(2);
    const std::optional<int> provisional = SessionChange("2v2");
    ASSERT_TRUE(provisional.has_value());
    ASSERT_NE(*provisional, 22);
    StartNewSession();
    ASSERT_EQ(RecapChange("2v2"), provisional);

    fetcher->ProcessPostMatchResponseForTests("s1-g2", 1222, 52);
    EXPECT_EQ(RecapChange("2v2"), 22);
    EXPECT_EQ(RecapTotal(), 22);
    EXPECT_EQ(Baseline("2v2"), 1222);
    EXPECT_EQ(SessionChange("2v2"), 0);
}

TEST_F(SessionMmrLifecycleTest, RecapAndNewBaselineWaitForTheEndedSessionsLastMatch) {
    PublishRoster({{"2v2", {1200, 50}}});
    PlayMatch("s1-g1", "2v2", true, {1200, 50}, {1200, 50});
    PlayMatch("s1-g2", "2v2", false, {1210, 51}, {1200, 50});
    RecordCountedGames(2);
    StartNewSession();
    const std::optional<int> provisional = RecapChange("2v2");
    ASSERT_TRUE(provisional.has_value());

    fetcher->ProcessPostMatchResponseForTests("s1-g1", 1210, 51);
    EXPECT_EQ(RecapChange("2v2"), provisional);
    EXPECT_EQ(Baseline("2v2"), std::nullopt);
    EXPECT_TRUE(Graph("2v2").empty());

    fetcher->ProcessPostMatchResponseForTests("s1-g2", 1201, 52);
    EXPECT_EQ(RecapChange("2v2"), 1);
    EXPECT_EQ(RecapTotal(), 1);
    EXPECT_EQ(Baseline("2v2"), 1201);
}

TEST_F(SessionMmrLifecycleTest, OlderSessionConfirmationNeverRewritesANewerRecap) {
    PublishRoster({{"2v2", {1200, 50}}});
    EnqueueMatch("s1-g1", "2v2", true, {1200, 50});
    RecordCountedGames(1);
    StartNewSession();

    PublishRoster({{"3v3", {900, 100}}});
    PlayMatch("s2-g1", "3v3", true, {900, 100}, {909, 101});
    RecordCountedGames(1);
    StartNewSession();
    ASSERT_EQ(RecapChange("3v3"), 9);

    fetcher->ProcessPostMatchResponseForTests("s1-g1", 1210, 51);
    EXPECT_EQ(RecapChange("2v2"), std::nullopt);
    EXPECT_EQ(RecapTotal(), 9);
}
