#include <gtest/gtest.h>
#include "network/mmr/MmrReconciler.hpp"
#include "core/state/LiveMatchStore.hpp"
#include <mutex>
#include <shared_mutex>
#include <thread>

namespace {

    MMRRequest MakeRequest(const std::string& matchGuid,
                           const std::string& playlist = "2v2",
                           int previousMmr = 1200,
                           int previousMatches = 50,
                           bool won = true,
                           const std::string& primaryId = "Steam|123",
                           uint64_t sessionGen = 1) {
        MMRRequest req;
        req.matchGuid = matchGuid;
        req.playlist = playlist;
        req.previousMmr = previousMmr;
        req.previousMatches = previousMatches;
        req.previousMmrIsPlaylistSpecific = true;
        req.won = won;
        req.resultKnown = true;
        req.primaryId = primaryId;
        req.sessionGeneration = sessionGen;
        return req;
    }

} // namespace

TEST(MmrReconcilerTest, TrackerLagCreatesProvisionalThenReplacesWithConfirmed) {
    MmrReconciler reconciler;
    const std::string matchGuid = "lagged-match";
    reconciler.EnqueuePostMatch("Steam|123", matchGuid, "2v2", 1200, 50, true, true, 1);

    const auto req = MakeRequest(matchGuid, "2v2", 1200, 50, true);

    // 1. Initial poll returns stale data (Tracker hasn't updated yet)
    auto staleResult = reconciler.ReconcileTrackerResponse(req, 1200, 50, {}, 1, "Steam|123", false, false);
    EXPECT_FALSE(staleResult.applied);
    EXPECT_TRUE(staleResult.publication.updates.empty());

    // 2. Provisional point is generated while waiting for Tracker
    auto provResult = reconciler.EnsureProvisionalPoint(req, 1200, 1200, 50, {}, 1200);
    EXPECT_TRUE(provResult.applied);
    EXPECT_EQ(provResult.publication.matchGuid, matchGuid);
    EXPECT_EQ(provResult.publication.provisionalMmr, 1209);
    EXPECT_EQ(provResult.publication.matchesPlayed, 50);
    ASSERT_TRUE(provResult.dbUpdate.has_value());
    EXPECT_EQ(provResult.dbUpdate->matchGuid, matchGuid);
    EXPECT_EQ(provResult.dbUpdate->mmr, 1209);

    // 3. Second poll still lagged; provisional point is not duplicated
    auto stillLagged = reconciler.ReconcileTrackerResponse(req, 1200, 50, {}, 1, "Steam|123", false, false);
    EXPECT_FALSE(stillLagged.applied);

    // 4. Tracker lag resolves: matchesPlayed increments and confirmed MMR is published
    auto confirmedResult = reconciler.ReconcileTrackerResponse(req, 1211, 51, {1209.0f}, 1, "Steam|123", false, false);
    EXPECT_TRUE(confirmedResult.applied);
    EXPECT_TRUE(confirmedResult.allPendingCovered);
    ASSERT_EQ(confirmedResult.publication.updates.size(), 1u);
    EXPECT_EQ(confirmedResult.publication.updates[0].matchGuid, matchGuid);
    EXPECT_EQ(confirmedResult.publication.updates[0].mmr, 1211);
    EXPECT_EQ(confirmedResult.publication.updates[0].matchesPlayed, 51);
    EXPECT_FALSE(confirmedResult.publication.updates[0].valueEstimated);
    ASSERT_EQ(confirmedResult.dbUpdates.size(), 1u);
    EXPECT_EQ(confirmedResult.dbUpdates[0].matchGuid, matchGuid);
    EXPECT_EQ(confirmedResult.dbUpdates[0].mmr, 1211);
}

TEST(MmrReconcilerTest, CallerRetainsHistoryLockThroughReconciliationAndPublication) {
    SessionState state;
    state.liveMatch->UpdateMatchStateForTests([](MatchState& match) {
        match.myPrimaryId = "Steam|123";
        match.sessionGeneration = 1;
    });
    MmrReconciler reconciler;
    const std::string matchGuid = "locked-match";
    const auto req = MakeRequest(matchGuid);
    reconciler.EnqueuePostMatch(req.primaryId, matchGuid, req.playlist, 1200, 50, true, true, 1);

    std::unique_lock historyLock(state.history.mutex);
    auto expectLocksHeld = [&] {
        bool acquiredHistory = false;
        std::thread contender([&] {
            acquiredHistory = state.history.mutex.try_lock();
            if (acquiredHistory) state.history.mutex.unlock();
        });
        contender.join();
        EXPECT_FALSE(acquiredHistory);
    };

    auto provisional = reconciler.EnsureProvisionalPoint(req, 1200, 1200, 50, {}, 1200);
    ASSERT_TRUE(provisional.applied);
    expectLocksHeld();
    MmrPublisher::PublishProvisionalPointLocked(state, provisional.publication);
    expectLocksHeld();
    ASSERT_EQ(state.history.playlistMatchPoints["2v2"].size(), 1u);
    EXPECT_EQ(state.history.playlistMatchPoints["2v2"][0].mmr, 1209);
    EXPECT_FALSE(state.history.playlistMatchPoints["2v2"][0].trackerCovered);

    const auto confirmed = reconciler.ReconcileTrackerResponse(
        req, 1211, 51, state.history.playlistHistoryY["2v2"], 1, req.primaryId, false, false,
        &state.history.playlistMatchPoints["2v2"], 1200);
    ASSERT_TRUE(confirmed.applied);
    expectLocksHeld();
    MmrPublisher::PublishReconciledPointsLocked(state, confirmed.publication);
    expectLocksHeld();
    ASSERT_EQ(state.history.playlistMatchPoints["2v2"].size(), 1u);
    EXPECT_EQ(state.history.playlistMatchPoints["2v2"][0].mmr, 1211);
    EXPECT_TRUE(state.history.playlistMatchPoints["2v2"][0].trackerCovered);
    ASSERT_EQ(state.history.playlistHistoryY["2v2"].size(), 1u);
    EXPECT_FLOAT_EQ(state.history.playlistHistoryY["2v2"][0], 1211.0f);
}

TEST(MmrReconcilerTest, DestroyedMatchLossConfirmedByRatingDecrease) {
    MmrReconciler reconciler;
    std::vector<std::pair<std::string, bool>> callbacks;
    reconciler.SetDestroyedMatchConfirmationCallback([&](const std::string& guid, bool won) {
        callbacks.emplace_back(guid, won);
    });

    PendingDestroyedMatchMmrRefresh pending;
    pending.matchGuid = "dest-loss-guid";
    pending.primaryId = "Steam|123";
    pending.name = "Ace";
    pending.playlist = "2v2";
    pending.previousMmr = 1200;
    pending.previousMatches = 50;
    pending.previousMmrIsPlaylistSpecific = true;
    pending.score = {1, 3};
    pending.localTeam = 0;

    reconciler.EnqueuePendingDestroyedMatch(pending, 1);
    EXPECT_TRUE(reconciler.HasPendingDestroyedMatch("dest-loss-guid"));

    const auto req = MakeRequest("dest-loss-guid", "2v2", 1200, 50, false);
    auto recResult = reconciler.ReconcileTrackerResponse(req, 1191, 51, {}, 1, "Steam|123", false, false);
    EXPECT_TRUE(callbacks.empty());
    ASSERT_TRUE(recResult.confirmationCallback);
    for (const auto& [guid, won] : recResult.confirmations)
        recResult.confirmationCallback(guid, won);

    EXPECT_TRUE(recResult.applied);
    ASSERT_EQ(callbacks.size(), 1u);
    EXPECT_EQ(callbacks[0].first, "dest-loss-guid");
    EXPECT_FALSE(callbacks[0].second); // Loss confirmed
    ASSERT_EQ(recResult.confirmations.size(), 1u);
    EXPECT_EQ(recResult.confirmations[0].first, "dest-loss-guid");
    EXPECT_FALSE(recResult.confirmations[0].second);
    EXPECT_FALSE(reconciler.HasPendingDestroyedMatch("dest-loss-guid"));
}

TEST(MmrReconcilerTest, DestroyedMatchWinConfirmedByRatingIncrease) {
    MmrReconciler reconciler;
    std::vector<std::pair<std::string, bool>> callbacks;
    reconciler.SetDestroyedMatchConfirmationCallback([&](const std::string& guid, bool won) {
        callbacks.emplace_back(guid, won);
    });

    PendingDestroyedMatchMmrRefresh pending;
    pending.matchGuid = "dest-win-guid";
    pending.primaryId = "Steam|123";
    pending.name = "Ace";
    pending.playlist = "2v2";
    pending.previousMmr = 1200;
    pending.previousMatches = 50;
    pending.previousMmrIsPlaylistSpecific = true;

    reconciler.EnqueuePendingDestroyedMatch(pending, 1);
    EXPECT_TRUE(reconciler.HasPendingDestroyedMatch("dest-win-guid"));

    const auto req = MakeRequest("dest-win-guid", "2v2", 1200, 50, true);
    auto recResult = reconciler.ReconcileTrackerResponse(req, 1209, 51, {}, 1, "Steam|123", false, false);
    EXPECT_TRUE(callbacks.empty());
    ASSERT_TRUE(recResult.confirmationCallback);
    for (const auto& [guid, won] : recResult.confirmations)
        recResult.confirmationCallback(guid, won);

    EXPECT_TRUE(recResult.applied);
    ASSERT_EQ(callbacks.size(), 1u);
    EXPECT_EQ(callbacks[0].first, "dest-win-guid");
    EXPECT_TRUE(callbacks[0].second); // Win confirmed
    EXPECT_FALSE(reconciler.HasPendingDestroyedMatch("dest-win-guid"));
}

TEST(MmrReconcilerTest, CumulativeCatchUpReplacesMultipleProvisionalPoints) {
    MmrReconciler reconciler;
    reconciler.EnqueuePostMatch("Steam|123", "match-1", "2v2", 1200, 50, true, true, 1);
    reconciler.EnqueuePostMatch("Steam|123", "match-2", "2v2", 1209, 50, true, true, 1);

    const auto req1 = MakeRequest("match-1", "2v2", 1200, 50, true);
    const auto req2 = MakeRequest("match-2", "2v2", 1209, 50, true);

    auto prov1 = reconciler.EnsureProvisionalPoint(req1, 1200, 1200, 50, {}, 1200);
    EXPECT_TRUE(prov1.applied);
    EXPECT_EQ(prov1.publication.provisionalMmr, 1209);

    auto prov2 = reconciler.EnsureProvisionalPoint(req2, 1209, 1200, 50, {1209.0f}, 1200);
    EXPECT_TRUE(prov2.applied);
    EXPECT_EQ(prov2.publication.provisionalMmr, 1218);

    // Cumulative Tracker update: matches played jumped from 50 to 52, covering both matches
    auto recResult = reconciler.ReconcileTrackerResponse(req2, 1220, 52, {1209.0f, 1218.0f}, 1, "Steam|123", false, false);
    EXPECT_TRUE(recResult.applied);
    EXPECT_TRUE(recResult.allPendingCovered);
    ASSERT_EQ(recResult.publication.updates.size(), 2u);

    // Match 1 was intermediate: estimated
    EXPECT_EQ(recResult.publication.updates[0].matchGuid, "match-1");
    EXPECT_EQ(recResult.publication.updates[0].matchesPlayed, 51);
    EXPECT_TRUE(recResult.publication.updates[0].valueEstimated);

    // Match 2 was latest: confirmed with exact Tracker rating
    EXPECT_EQ(recResult.publication.updates[1].matchGuid, "match-2");
    EXPECT_EQ(recResult.publication.updates[1].mmr, 1220);
    EXPECT_EQ(recResult.publication.updates[1].matchesPlayed, 52);
    EXPECT_FALSE(recResult.publication.updates[1].valueEstimated);

    ASSERT_EQ(recResult.dbUpdates.size(), 2u);
    EXPECT_EQ(recResult.dbUpdates[0].matchGuid, "match-1");
    EXPECT_EQ(recResult.dbUpdates[1].matchGuid, "match-2");
    EXPECT_EQ(recResult.dbUpdates[1].mmr, 1220);
}

TEST(MmrReconcilerTest, AmbiguousDestroyedMatchResolvedByScore) {
    MmrReconciler reconciler;
    std::vector<std::pair<std::string, bool>> callbacks;
    reconciler.SetDestroyedMatchConfirmationCallback([&](const std::string& guid, bool won) {
        callbacks.emplace_back(guid, won);
    });

    PendingDestroyedMatchMmrRefresh pending;
    pending.matchGuid = "ambig-guid";
    pending.primaryId = "Steam|123";
    pending.name = "Ace";
    pending.playlist = "2v2";
    pending.previousMmr = 650;
    pending.previousMatches = 254;
    pending.previousMmrIsPlaylistSpecific = true;
    pending.localTeam = 1;
    pending.score = {2, 4}; // Team 1 scored 4 > 2 -> Win

    reconciler.EnqueuePendingDestroyedMatch(pending, 1);
    const auto req = MakeRequest("ambig-guid", "2v2", 650, 254, false);
    auto recResult = reconciler.ReconcileTrackerResponse(req, 659, 255, {}, 1, "Steam|123", false, false);
    EXPECT_TRUE(callbacks.empty());
    ASSERT_TRUE(recResult.confirmationCallback);
    for (const auto& [guid, won] : recResult.confirmations)
        recResult.confirmationCallback(guid, won);

    EXPECT_TRUE(recResult.applied);
    ASSERT_EQ(callbacks.size(), 1u);
    EXPECT_EQ(callbacks[0].first, "ambig-guid");
    EXPECT_TRUE(callbacks[0].second); // Win derived from score
}

TEST(MmrReconcilerTest, CounterRollbackResetsBaseline) {
    MmrReconciler reconciler;
    reconciler.EnqueuePostMatch("Steam|123", "match-old", "2v2", 1200, 100, true, true, 1);
    const auto reqOld = MakeRequest("match-old", "2v2", 1200, 100, true);
    auto resOld = reconciler.ReconcileTrackerResponse(reqOld, 1209, 101, {}, 1, "Steam|123", false, false);
    EXPECT_TRUE(resOld.applied);

    // New season starts with previousMatches = 0
    reconciler.EnqueuePostMatch("Steam|123", "match-new", "2v2", 1000, 0, true, true, 2);
    const auto reqNew = MakeRequest("match-new", "2v2", 1000, 0, true, "Steam|123", 2);
    auto resNew = reconciler.ReconcileTrackerResponse(reqNew, 1009, 1, {}, 2, "Steam|123", false, false);
    EXPECT_TRUE(resNew.applied);
    ASSERT_EQ(resNew.publication.updates.size(), 1u);
    EXPECT_EQ(resNew.publication.updates[0].matchGuid, "match-new");
    EXPECT_EQ(resNew.publication.updates[0].matchesPlayed, 1);
}
