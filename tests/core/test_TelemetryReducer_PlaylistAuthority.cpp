#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/SessionState.hpp"
#include "core/Storage.hpp"
#include "core/TelemetryReducer.hpp"
#include <memory>

namespace {
    struct ScopedConfigRestore {
        ConfigData original = Config::Read();
        ~ScopedConfigRestore() {
            Config::Update([this](ConfigData& c) { c = original; }, false);
        }
    };

    nlohmann::json PlayersForTeams(int blue, int orange) {
        nlohmann::json players = nlohmann::json::array();
        int id = 1;
        for (int team = 0; team < 2; ++team) {
            const int count = team == 0 ? blue : orange;
            for (int i = 0; i < count; ++i, ++id) {
                nlohmann::json player = {
                    {"PrimaryId", "Steam|" + std::to_string(id)},
                    {"TeamNum", team},
                    {"Name", "P" + std::to_string(id)}};
                if (id == 1) player["Boost"] = 100;
                players.push_back(std::move(player));
            }
        }
        return players;
    }

    nlohmann::json Update(int playlistId,
                          int blue,
                          int orange,
                          const std::string& arena = "Stadium_P",
                          bool casualFlag = false) {
        return nlohmann::json{
            {"Game",
             {{"PlaylistId", playlistId},
              {"Arena", arena},
              {"bReplay", false},
              {"bSpectator", false},
              {"bCasualMatch", casualFlag}}},
            {"Players", PlayersForTeams(blue, orange)}};
    }

    void CreateMatch(TelemetryReducer& reducer,
                     const std::shared_ptr<SessionState>& state,
                     const std::string& guid) {
        state->game.myPrimaryId = "Steam|1";
        reducer.Reduce(
            std::string(Constants::EVT_MATCH_CREATED),
            nlohmann::json{{"MatchGuid", guid}});
    }

    SideEffects EndMatch(TelemetryReducer& reducer,
                         const std::shared_ptr<SessionState>& state,
                         int winner = 0) {
        return reducer.Reduce(
            std::string(Constants::EVT_MATCH_ENDED),
            nlohmann::json{{"MatchGuid", state->game.matchGuid},
                           {"WinnerTeamNum", winner}});
    }
} // namespace

TEST(TelemetryReducerPlaylistAuthority, PlaylistIdOverridesUiArenaAndCasualFlag) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = false;
        c.graph_follow_current_playlist = false;
    },
                   false);

    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Rumble);
    state->ui.graphMmrCategory.store(MmrCategory::Rumble);
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "authoritative-ranked-ones");
    nlohmann::json rankedUpdate =
        Update(10, 1, 1, "UtopiaStadium_Snow_P", true);
    rankedUpdate["Game"]["bTraining"] = true;
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        rankedUpdate);
    state->game.roster["Steam|1"].playlists["1v1"] = 1200;
    state->game.roster["Steam|1"].playlistMatches["1v1"] = 50;
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_FALSE(state->game.excludedEarlyExitContext);
    SideEffects effects = EndMatch(reducer, state);

    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, 10);
    EXPECT_EQ(effects.saveSnapshot.gamemode, "1v1");
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "1v1");
}

TEST(TelemetryReducerPlaylistAuthority, UnknownPlaylistFailsClosedInsteadOfGuessingFromRoster) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.graph_follow_current_playlist = true;
        c.show_extra_playlists = true;
    },
                   false);

    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::TwoVTwo);
    state->ui.graphMmrCategory.store(MmrCategory::TwoVTwo);
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "unknown-playlist");
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(999, 2, 2));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_EQ(state->game.playlistId, 999);
    EXPECT_TRUE(state->game.excludedEarlyExitContext);
    EXPECT_EQ(state->game.earlyExitExclusionReason, "unknown_playlist_id");
    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::TwoVTwo);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::TwoVTwo);

    SideEffects effects = EndMatch(reducer, state);
    EXPECT_FALSE(effects.saveMatch);
    EXPECT_FALSE(effects.postMatchMmrRefresh.has_value());
    EXPECT_TRUE(state->game.lastMatchWasVoid);
    EXPECT_EQ(state->game.lastMatchVoidReason, "unknown_playlist_id");
}

TEST(TelemetryReducerPlaylistAuthority, CasualBackfillUsesPlaylistIdAndDoesNotPopulateLegacyPlayerHeuristics) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.graph_follow_current_playlist = true;
        c.show_extra_playlists = true;
    },
                   false);
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "casual-backfill");
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(2, 2, 1));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::Casual);
    EXPECT_EQ(state->ui.graphMmrCategory.load(), MmrCategory::Casual);
    EXPECT_EQ(state->game.legacyMaxPlayersSeen, 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[0], 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[1], 0);
    EXPECT_FALSE(state->game.legacyLobbyWasEverFull);

    state->game.roster["Steam|1"].playlists["casual"] = 900;
    state->game.roster["Steam|1"].playlistMatches["casual"] = 30;
    SideEffects effects = EndMatch(reducer, state);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, 2);
    EXPECT_EQ(effects.saveSnapshot.gamemode, "casual");
    EXPECT_EQ(effects.saveSnapshot.legacyPlayerCount, 0);
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "casual");
    EXPECT_EQ(effects.postMatchMmrRefresh->previousMmr, 900);
    EXPECT_EQ(effects.postMatchMmrRefresh->previousMatches, 30);
}

TEST(TelemetryReducerPlaylistAuthority, RankedPlaylistClearsTransientExclusionAndLatchesAcrossUpdates) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "playlist-latch");
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(-2, 2, 2));
    EXPECT_TRUE(state->game.excludedEarlyExitContext);

    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(11, 2, 2));
    EXPECT_EQ(state->game.playlistId, 11);
    EXPECT_FALSE(state->game.excludedEarlyExitContext);

    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.currentMatch.goals = 2;

    // A transient intermission update after the round starts must not replace
    // the match's established playlist.
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(-2, 2, 2));
    EXPECT_EQ(state->game.playlistId, 11);
    EXPECT_FALSE(state->game.excludedEarlyExitContext);

    // Arena metadata can update separately and must not reset the match or
    // discard the latched playlist/stats.
    nlohmann::json arenaUpdate = Update(11, 2, 2, "Mannfield_P");
    arenaUpdate["Game"].erase("PlaylistId");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), arenaUpdate);
    EXPECT_EQ(state->game.playlistId, 11);
    EXPECT_EQ(state->game.currentMatch.goals, 2);
    EXPECT_EQ(state->game.matchGuid, "playlist-latch");

    state->game.roster["Steam|1"].playlists["2v2"] = 1100;
    SideEffects effects = EndMatch(reducer, state);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, 11);
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "2v2");
}

TEST(TelemetryReducerPlaylistAuthority, CasualHeatseekerLtmUsesSingleCasualMmrBucket) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "heatseeker-ltm");
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(38, 3, 3));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.roster["Steam|1"].playlists["casual"] = 975;
    state->game.roster["Steam|1"].playlistMatches["casual"] = 44;

    SideEffects effects = EndMatch(reducer, state);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, 38);
    EXPECT_EQ(effects.saveSnapshot.gamemode, "casual");
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "casual");
    EXPECT_EQ(effects.postMatchMmrRefresh->previousMmr, 975);
    EXPECT_EQ(effects.postMatchMmrRefresh->previousMatches, 44);
}

TEST(TelemetryReducerPlaylistAuthority, BackToBackMatchesCanUseDifferentAuthoritativePlaylists) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "back-to-back-one");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Update(10, 1, 1));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.roster["Steam|1"].playlists["1v1"] = 1000;
    SideEffects first = EndMatch(reducer, state);
    ASSERT_TRUE(first.saveMatch);
    EXPECT_EQ(first.saveSnapshot.playlistId, 10);

    CreateMatch(reducer, state, "back-to-back-two");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Update(13, 3, 3));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.roster["Steam|1"].playlists["3v3"] = 1010;
    SideEffects second = EndMatch(reducer, state);
    ASSERT_TRUE(second.saveMatch);
    EXPECT_EQ(second.saveSnapshot.playlistId, 13);
    EXPECT_EQ(second.saveSnapshot.gamemode, "3v3");
    ASSERT_TRUE(second.postMatchMmrRefresh.has_value());
    EXPECT_EQ(second.postMatchMmrRefresh->playlist, "3v3");
}

TEST(TelemetryReducerPlaylistAuthority, LegacyCasualSignalBeatsArenaAndUiHeuristics) {
    Storage::InitializeEnvironment();
    ScopedConfigRestore restore;
    Config::Update([](ConfigData& c) {
        c.auto_switch_mmr_category = true;
        c.graph_follow_current_playlist = true;
        c.show_extra_playlists = true;
    },
                   false);

    auto state = std::make_shared<SessionState>();
    state->ui.rosterMmrCategory.store(MmrCategory::Hoops);
    state->ui.graphMmrCategory.store(MmrCategory::Hoops);
    TelemetryReducer reducer(state);
    CreateMatch(reducer, state, "legacy-casual-hoops-arena");

    nlohmann::json update = Update(17, 2, 2, "hoops_dunkhouse_p", true);
    update["Game"].erase("PlaylistId");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), update);
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_TRUE(state->game.fallbackCasualContext);
    EXPECT_FALSE(state->game.fallbackNonRecordableContext);
    EXPECT_EQ(state->ui.rosterMmrCategory.load(), MmrCategory::Casual);

    SideEffects effects = EndMatch(reducer, state);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, -1);
    EXPECT_EQ(effects.saveSnapshot.gamemode, "casual");
    EXPECT_FALSE(effects.postMatchMmrRefresh.has_value());
    EXPECT_TRUE(state->game.sessionGamemodes.empty());
}

TEST(TelemetryReducerPlaylistAuthority, LegacyNonRecordableSignalsFailClosedWithoutPlaylistId) {
    Storage::InitializeEnvironment();

    const auto runCase = [](const char* flag, const char* guid) {
        auto state = std::make_shared<SessionState>();
        TelemetryReducer reducer(state);
        CreateMatch(reducer, state, guid);

        nlohmann::json update = Update(10, 1, 1);
        update["Game"].erase("PlaylistId");
        update["Game"][flag] = true;
        reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), update);
        reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

        EXPECT_TRUE(state->game.fallbackNonRecordableContext);
        SideEffects effects = EndMatch(reducer, state);
        EXPECT_FALSE(effects.saveMatch);
        EXPECT_FALSE(effects.postMatchMmrRefresh.has_value());
        EXPECT_TRUE(state->game.lastMatchWasVoid);
        EXPECT_EQ(state->game.lastMatchVoidReason,
                  "explicit_non_competitive_context");
    };

    runCase("bTraining", "legacy-training");
    runCase("bPrivateMatch", "legacy-private");
    runCase("bExhibition", "legacy-exhibition");
}

TEST(TelemetryReducerPlaylistAuthority, AuthoritativeNonRecordablePlaylistsNeverPersistAsMatches) {
    Storage::InitializeEnvironment();

    struct Case {
        int playlistId;
        const char* guid;
        const char* reason;
    };
    const Case cases[] = {
        {6, "private-playlist", "private_match_playlist"},
        {9, "training-playlist", "training_playlist"},
        {73, "online-freeplay-playlist", "online_freeplay_playlist"},
    };

    for (const auto& testCase : cases) {
        SCOPED_TRACE(testCase.guid);
        auto state = std::make_shared<SessionState>();
        TelemetryReducer reducer(state);
        CreateMatch(reducer, state, testCase.guid);
        reducer.Reduce(
            std::string(Constants::EVT_UPDATE_STATE),
            Update(testCase.playlistId, 1, 1));
        reducer.Reduce(
            std::string(Constants::EVT_ROUND_STARTED),
            nlohmann::json{});

        EXPECT_TRUE(state->game.excludedEarlyExitContext);
        EXPECT_EQ(state->game.earlyExitExclusionReason, testCase.reason);

        SideEffects effects = EndMatch(reducer, state);
        EXPECT_FALSE(effects.saveMatch);
        EXPECT_FALSE(effects.postMatchMmrRefresh.has_value());
        EXPECT_TRUE(state->game.lastMatchWasVoid);
        EXPECT_EQ(state->game.lastMatchVoidReason, testCase.reason);
    }
}

TEST(TelemetryReducerPlaylistAuthority, RankedFreeplayRankedTransitionDoesNotCarryPlaylistTruth) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "ranked-before-freeplay");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Update(11, 2, 2));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.roster["Steam|1"].playlists["2v2"] = 1200;
    SideEffects rankedBefore = EndMatch(reducer, state);
    ASSERT_TRUE(rankedBefore.saveMatch);
    ASSERT_TRUE(rankedBefore.postMatchMmrRefresh.has_value());
    EXPECT_EQ(rankedBefore.postMatchMmrRefresh->playlist, "2v2");

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        nlohmann::json{{"MatchGuid", "ranked-before-freeplay"}});

    CreateMatch(reducer, state, "freeplay-between-ranked");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Update(73, 1, 0));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    SideEffects freeplay = EndMatch(reducer, state);
    EXPECT_FALSE(freeplay.saveMatch);
    EXPECT_FALSE(freeplay.postMatchMmrRefresh.has_value());
    EXPECT_EQ(state->game.playlistId, 73);

    reducer.Reduce(
        std::string(Constants::EVT_MATCH_DESTROYED),
        nlohmann::json{{"MatchGuid", "freeplay-between-ranked"}});

    CreateMatch(reducer, state, "ranked-after-freeplay");
    reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Update(13, 3, 3));
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});
    state->game.roster["Steam|1"].playlists["3v3"] = 1000;
    SideEffects rankedAfter = EndMatch(reducer, state);
    ASSERT_TRUE(rankedAfter.saveMatch);
    EXPECT_EQ(rankedAfter.saveSnapshot.playlistId, 13);
    ASSERT_TRUE(rankedAfter.postMatchMmrRefresh.has_value());
    EXPECT_EQ(rankedAfter.postMatchMmrRefresh->playlist, "3v3");
}

TEST(TelemetryReducerPlaylistAuthority, RankedPlaylistDoesNotUseObservedPlayerCountForValidity) {
    Storage::InitializeEnvironment();
    auto state = std::make_shared<SessionState>();
    TelemetryReducer reducer(state);

    CreateMatch(reducer, state, "ranked-incomplete-observation");
    // Playlist 11 is authoritative Ranked Doubles. Only one player per team is
    // present in this frame to simulate incomplete telemetry / join-in-progress
    // observation. That must not turn playlist detection into a player-count
    // heuristic or void the match.
    reducer.Reduce(
        std::string(Constants::EVT_UPDATE_STATE),
        Update(11, 1, 1));
    state->game.roster["Steam|1"].playlists["2v2"] = 1200;
    state->game.roster["Steam|1"].playlistMatches["2v2"] = 50;
    reducer.Reduce(std::string(Constants::EVT_ROUND_STARTED), nlohmann::json{});

    EXPECT_EQ(state->game.legacyMaxPlayersSeen, 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[0], 0);
    EXPECT_EQ(state->game.legacyMaxTeamPlayersSeen[1], 0);

    SideEffects effects = EndMatch(reducer, state);
    ASSERT_TRUE(effects.saveMatch);
    EXPECT_EQ(effects.saveSnapshot.playlistId, 11);
    EXPECT_EQ(effects.saveSnapshot.gamemode, "2v2");
    EXPECT_EQ(effects.saveSnapshot.legacyPlayerCount, 0);
    ASSERT_TRUE(effects.postMatchMmrRefresh.has_value());
    EXPECT_EQ(effects.postMatchMmrRefresh->playlist, "2v2");
}
