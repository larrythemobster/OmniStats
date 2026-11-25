#include "RmlUiControllerStateFixture.hpp"

namespace {
    const std::string kMain = "Steam|76561198000000001|0";
    const std::string kAlt = "Epic|aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|0";
    const std::string kRival = "Steam|76561198000000002|0";
    const std::string kNeverPlayed = "Epic|bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|0";

    void SaveSoloMatch(DatabaseManager& db, const std::string& guid, const std::string& account, const std::string& name) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.playlistId = 11;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.myPrimaryId = account;
        snap.myTeam = 0;
        snap.endedAtUnixMs = 1'700'000'000'000LL;
        snap.roster[account] = PlayerData{.primaryId = account, .name = name, .team = 0, .mmr = 1000};
        db.SaveMatch(snap);
    }

    std::filesystem::path WriteDejaVuFile(const char* dirName) {
        const auto dir = std::filesystem::temp_directory_path() / dirName;
        std::filesystem::create_directories(dir);
        const auto file = dir / "player_counter.json";
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << R"({"players": {
            "76561198000000002": {"name": "Rival", "playlistData": {"11": {"records": {"against": {"wins": 2, "losses": 1}}}}},
            "2535421607154237": {"name": "Mate", "playlistData": {"10": {"records": {"with": {"wins": 1, "losses": 0}}}}}
        }})";
        return file;
    }

    template <typename Pred>
    bool WaitFor(Pred pred) {
        for (int i = 0; i < 200 && !pred(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return pred();
    }
}

TEST_F(RmlUiControllerStateTest, DejaVuPromptImportsIntoTheChosenAccountOnce) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    const auto file = WriteDejaVuFile("OmniStatsDejaVuPromptImport");
    Config::Update([](ConfigData& c) {
        c.last_primary_id = kMain;
        c.known_primary_ids = {kMain, kAlt, kNeverPlayed};
    },
                   true);
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    SaveSoloMatch(*db, "main", kMain, "MainName");
    SaveSoloMatch(*db, "alt", kAlt, "AltName");
    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(PromptRoot(controller)->QuerySelector(".prompt-card"), nullptr);
    EXPECT_FALSE(controller.WantsAttention());

    db->AsyncOfferDejaVuImport(file, Config::Read().known_primary_ids);
    ASSERT_TRUE(WaitFor([&] { return state->ui.dejaVuPlayers.load() == 2; }));
    EXPECT_TRUE(controller.WantsAttention());
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* import = PromptRoot(controller)->QuerySelector("[data-action='dejavu-prompt-import']");
    ASSERT_NE(import, nullptr);
    EXPECT_NE(PromptRoot(controller)->GetInnerRML().find("records for 2 players"), std::string::npos);
    auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(PromptRoot(controller)->QuerySelector("[data-setting='dejavu_account']"));
    ASSERT_NE(select, nullptr);
    ASSERT_EQ(select->GetNumOptions(), 2);
    EXPECT_EQ(select->GetValue(), kMain);
    std::string mainLabel;
    for (int i = 0; i < select->GetNumOptions(); ++i)
        if (select->GetOption(i)->GetAttribute<Rml::String>("value", "") == kMain) mainLabel = select->GetOption(i)->GetInnerRML();
    EXPECT_EQ(mainLabel, "MainName (Steam)");
    CaptureKickoffView(warp, controller, "dejavu-prompt.png");

    for (int i = 0; i < select->GetNumOptions(); ++i)
        if (select->GetOption(i)->GetAttribute<Rml::String>("value", "") == kAlt) select->SetSelection(i);
    ASSERT_EQ(select->GetValue(), kAlt);
    Click(controller, import);
    EXPECT_EQ(PromptRoot(controller)->QuerySelector(".prompt-card"), nullptr);
    ASSERT_TRUE(WaitFor([&] { return !StatsApiRepairRunning(controller); }));
    controller.Update(Config::Read(), false);

    EXPECT_EQ(db->GetPlayerEncounterRecord(kAlt, kRival)->winsAgainst, 2);
    EXPECT_EQ(db->GetPlayerEncounterRecord(kMain, kRival)->GamesAgainst(), 0);
    EXPECT_EQ(db->CountDejaVuOffer(file), 0);
    EXPECT_FALSE(controller.WantsAttention());

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
}

TEST_F(RmlUiControllerStateTest, DeclinedDejaVuPromptIsNotAskedAgain) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    const auto file = WriteDejaVuFile("OmniStatsDejaVuPromptDecline");
    Config::Update([](ConfigData& c) {
        c.last_primary_id = kMain;
        c.known_primary_ids = {kMain};
    },
                   true);
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    RmlUiController controller(state, db);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));

    db->AsyncOfferDejaVuImport(file, {kMain, kNeverPlayed});
    ASSERT_TRUE(WaitFor([&] { return state->ui.dejaVuPlayers.load() == 2; }));
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(PromptRoot(controller)->QuerySelector("[data-setting='dejavu_account']"), nullptr);
    auto* decline = PromptRoot(controller)->QuerySelector("[data-action='dejavu-prompt-decline']");
    ASSERT_NE(decline, nullptr);
    Click(controller, decline);
    EXPECT_EQ(PromptRoot(controller)->QuerySelector(".prompt-card"), nullptr);
    EXPECT_FALSE(controller.WantsAttention());

    ASSERT_TRUE(WaitFor([&] { return db->CountDejaVuOffer(file) == 0; }));
    EXPECT_FALSE(db->ImportDejaVuHistory(file, kMain).imported);
    EXPECT_EQ(db->GetPlayerEncounterRecord(kMain, kRival)->GamesAgainst(), 0);

    std::error_code ec;
    std::filesystem::remove_all(file.parent_path(), ec);
}
