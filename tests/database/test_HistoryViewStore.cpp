#include <gtest/gtest.h>

#include "core/state/store/HistoryViewStore.hpp"

TEST(HistoryViewStore, OlderQueryResultDoesNotOverwriteNewerRequest) {
    stores::HistoryViewStore store;
    store.RequestQuery(1);
    store.RequestQuery(2);

    MatchRow stale;
    stale.matchId = 7;
    store.PublishQueryResult(1, {stale}, 1, false);

    const auto snap = store.GetSnapshot();
    EXPECT_EQ(snap->requestId, 2u);
    EXPECT_TRUE(snap->loading);
    EXPECT_TRUE(snap->rows.empty());
}

TEST(HistoryViewStore, OlderDetailResultDoesNotOverwriteNewerRequest) {
    stores::HistoryViewStore store;
    store.RequestDetail(1);
    store.RequestDetail(2);

    MatchDetail stale;
    stale.found = true;
    store.PublishDetailResult(1, stale);

    const auto snap = store.GetSnapshot();
    EXPECT_EQ(snap->detailRequestId, 2u);
    EXPECT_TRUE(snap->detailLoading);
    EXPECT_FALSE(snap->detail.has_value());
}
