#include <gtest/gtest.h>

#include "ui/rml/views/PlayerNoteView.hpp"

namespace {
    const std::string kPlayer = "Epic|0123456789abcdef0123456789abcdef|0";
}

TEST(PlayerScoutView, SharedEditorUsesTheStablePlayerIdentity) {
    PlayerNoteView view;
    view.Open({kPlayer, "Same Display Name", "With 2-1, Vs 3-0"}, std::nullopt);
    EXPECT_TRUE(view.IsOpen());
    ASSERT_TRUE(view.Identity());
    EXPECT_EQ(view.Identity()->Key(), "Epic|0123456789abcdef0123456789abcdef");
    EXPECT_EQ(view.PrimaryId(), kPlayer);
    view.SetTags({"Defensive", "Strong mechanics"});
    view.SetTagDraft("Fast aerials");
    EXPECT_EQ(view.TagDraft(), "Fast aerials");
    EXPECT_TRUE(view.BeginTagChange(7));
    EXPECT_FALSE(view.BeginTagChange(8));        // do not queue concurrent edits
    view.ApplyTagResult(6, true, {}, {"Wrong"}); // ignore late/stale response
    EXPECT_EQ(view.PendingTagRequestId(), 7u);
    view.ApplyTagResult(7, true, {}, {"Defensive", "Fast aerials"});
    EXPECT_EQ(view.PendingTagRequestId(), 0u);
    EXPECT_TRUE(view.TagDraft().empty());
    view.Close();
    EXPECT_FALSE(view.IsOpen());
}

TEST(PlayerScoutView, FailurePreservesUnsubmittedTagAndNoteDraft) {
    PlayerNoteView view;
    view.Open({kPlayer, "Rival", ""}, std::nullopt);
    view.SetDraft("Good kickoffs");
    view.SetTagDraft("Hard challenger");
    ASSERT_TRUE(view.BeginTagChange(21));
    view.ApplyTagResult(21, false, "The local database is unavailable", {});
    EXPECT_EQ(view.TagDraft(), "Hard challenger");
    EXPECT_TRUE(view.IsDirty());
    EXPECT_FALSE(view.RequestClose()); // retain unsaved personal note
    view.KeepEditing();
    EXPECT_TRUE(view.IsOpen());
}

TEST(PlayerScoutView, EncounterDataCanBeReplacedWithoutChangingTheDraft) {
    PlayerNoteView view;
    view.Open({kPlayer, "Rival", ""}, std::nullopt);
    view.SetDraft("Plays aggressively");
    PlayerScoutProfile profile;
    profile.record.winsWith = 2;
    profile.record.winsAgainst = 3;
    profile.record.lastSeenUnix = 1'700'000'000;
    profile.mostFrequentPlaylist = "2v2";
    profile.recent.push_back({.matchId = 44, .playedAtUnix = 1'700'000'000, .playlist = "2v2", .opponent = true, .win = false});
    view.SetScoutLoading("GC1 (current roster)");
    view.SetScoutResult(profile);
    EXPECT_EQ(view.Draft(), "Plays aggressively");
    EXPECT_TRUE(view.IsDirty());
}
