#include <gtest/gtest.h>
#include "network/ReplayUploader.hpp"
#include "core/SessionState.hpp"
#include <memory>

TEST(ReplayUploaderTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    ReplayUploader uploader(state);

    EXPECT_NO_THROW(uploader.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NO_THROW(uploader.Stop());
}

TEST(ReplayUploaderTest, EvaluateUploadResponse201Done) {
    const std::string body = R"({"id":"abc-123","location":"https://ballchasing.com/replay/abc-123"})";
    auto decision = EvaluateUploadResponse(201, body, 0, 5, -1, 0);
    EXPECT_EQ(decision.newState, "done");
    EXPECT_FALSE(decision.tokenInvalid);
    EXPECT_EQ(decision.ballchasingId, "abc-123");
    EXPECT_EQ(decision.ballchasingUrl, "https://ballchasing.com/replay/abc-123");

    const std::string bodyNoLoc = R"({"id":"xyz-789"})";
    auto decision2 = EvaluateUploadResponse(201, bodyNoLoc, 0, 5, -1, 0);
    EXPECT_EQ(decision2.newState, "done");
    EXPECT_EQ(decision2.ballchasingId, "xyz-789");
    EXPECT_EQ(decision2.ballchasingUrl, "https://ballchasing.com/replay/xyz-789");
}

TEST(ReplayUploaderTest, EvaluateUploadResponse409Duplicate) {
    const std::string body = R"({"error":"duplicate","id":"dup-456","location":"https://ballchasing.com/replay/dup-456"})";
    auto decision = EvaluateUploadResponse(409, body, 0, 5, -1, 0);
    EXPECT_EQ(decision.newState, "duplicate");
    EXPECT_FALSE(decision.tokenInvalid);
    EXPECT_EQ(decision.ballchasingId, "dup-456");
    EXPECT_EQ(decision.ballchasingUrl, "https://ballchasing.com/replay/dup-456");
}

TEST(ReplayUploaderTest, EvaluateUploadResponse401And403InvalidToken) {
    auto d401 = EvaluateUploadResponse(401, "{}", 0, 5, -1, 0);
    EXPECT_EQ(d401.newState, "pending");
    EXPECT_TRUE(d401.tokenInvalid);

    auto d403 = EvaluateUploadResponse(403, "{}", 0, 5, -1, 0);
    EXPECT_EQ(d403.newState, "pending");
    EXPECT_TRUE(d403.tokenInvalid);
}

TEST(ReplayUploaderTest, EvaluateUploadResponse400Failed) {
    auto d400 = EvaluateUploadResponse(400, "Bad request", 0, 5, -1, 0);
    EXPECT_EQ(d400.newState, "failed");
    EXPECT_FALSE(d400.tokenInvalid);
}

TEST(ReplayUploaderTest, EvaluateUploadResponse429AndRetryAfter) {
    auto dWithRetryAfter = EvaluateUploadResponse(429, "Rate limited", 0, 5, 25, 0);
    EXPECT_EQ(dWithRetryAfter.newState, "pending");
    EXPECT_GE(dWithRetryAfter.nextAttemptDelaySeconds, 25);

    // Backoff = 5 * 2^attempts + jitter.
    auto dAttempt0 = EvaluateUploadResponse(429, "", 0, 5, -1, 0);
    EXPECT_EQ(dAttempt0.newState, "pending");
    EXPECT_EQ(dAttempt0.nextAttemptDelaySeconds, 5);

    auto dAttempt1 = EvaluateUploadResponse(429, "", 1, 5, -1, 0);
    EXPECT_EQ(dAttempt1.newState, "pending");
    EXPECT_EQ(dAttempt1.nextAttemptDelaySeconds, 10);

    auto dAttempt2 = EvaluateUploadResponse(429, "", 2, 5, -1, 2);
    EXPECT_EQ(dAttempt2.newState, "pending");
    EXPECT_EQ(dAttempt2.nextAttemptDelaySeconds, 22);
}

TEST(ReplayUploaderTest, EvaluateUploadResponse5xxAndNetworkError) {
    auto d500 = EvaluateUploadResponse(500, "Internal error", 0, 5, -1, 0);
    EXPECT_EQ(d500.newState, "pending");
    EXPECT_EQ(d500.nextAttemptDelaySeconds, 5);

    auto d503 = EvaluateUploadResponse(503, "Unavailable", 1, 5, -1, 0);
    EXPECT_EQ(d503.newState, "pending");
    EXPECT_EQ(d503.nextAttemptDelaySeconds, 10);

    auto dNet = EvaluateUploadResponse(0, "", 0, 5, -1, 0);
    EXPECT_EQ(dNet.newState, "pending");
}

TEST(ReplayUploaderTest, EvaluateUploadResponseCappedAttemptsFails) {
    auto dMax = EvaluateUploadResponse(500, "Internal error", 4, 5, -1, 0);
    EXPECT_EQ(dMax.newState, "failed");
    EXPECT_NE(dMax.errorMessage.find("Exceeded retry limit"), std::string::npos);
}

TEST(ReplayUploaderTest, EvaluateReplayFilterDecisions) {
    ReplayFilterContext ctxDefault;
    ctxDefault.filterRankedOnly = false;
    ctxDefault.filterWinsOnly = false;
    ctxDefault.isAttributed = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxDefault), ReplayFilterResult::Upload);

    ctxDefault.isAttributed = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxDefault), ReplayFilterResult::Upload);

    ReplayFilterContext ctxRanked;
    ctxRanked.filterRankedOnly = true;

    ctxRanked.isAttributed = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxRanked), ReplayFilterResult::Skip);

    ctxRanked.isAttributed = true;
    ctxRanked.hasMatchRecord = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxRanked), ReplayFilterResult::WaitForMatchResult);

    ctxRanked.hasMatchRecord = true;
    ctxRanked.isResultPending = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxRanked), ReplayFilterResult::WaitForMatchResult);

    ctxRanked.isResultPending = false;
    ctxRanked.isRanked = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxRanked), ReplayFilterResult::Skip);

    ctxRanked.isRanked = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxRanked), ReplayFilterResult::Upload);

    ReplayFilterContext ctxWins;
    ctxWins.filterWinsOnly = true;

    ctxWins.isAttributed = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxWins), ReplayFilterResult::Skip);

    ctxWins.isAttributed = true;
    ctxWins.hasMatchRecord = true;
    ctxWins.isResultPending = false;
    ctxWins.isWin = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxWins), ReplayFilterResult::Skip);

    ctxWins.isWin = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxWins), ReplayFilterResult::Upload);

    ReplayFilterContext ctxBoth;
    ctxBoth.filterRankedOnly = true;
    ctxBoth.filterWinsOnly = true;
    ctxBoth.isAttributed = true;
    ctxBoth.hasMatchRecord = true;
    ctxBoth.isResultPending = false;

    ctxBoth.isRanked = true;
    ctxBoth.isWin = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxBoth), ReplayFilterResult::Upload);

    ctxBoth.isRanked = true;
    ctxBoth.isWin = false;
    EXPECT_EQ(EvaluateReplayFilter(ctxBoth), ReplayFilterResult::Skip);

    ctxBoth.isRanked = false;
    ctxBoth.isWin = true;
    EXPECT_EQ(EvaluateReplayFilter(ctxBoth), ReplayFilterResult::Skip);
}
