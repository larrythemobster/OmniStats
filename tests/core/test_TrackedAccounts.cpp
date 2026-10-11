#include <gtest/gtest.h>

#include "core/AccountManager.hpp"
#include "core/Config.hpp"
#include "core/Constants.hpp"
#include "core/SessionState.hpp"
#include "core/Storage.hpp"
#include "core/TelemetryReducer.hpp"
#include "core/TrackedAccounts.hpp"
#include "database/DatabaseManager.hpp"
#include "network/MMRFetcher.hpp"

#include <memory>

using namespace TrackedAccounts;

namespace {
    const std::string kEpicHex = "0123456789abcdef0123456789abcdef";
    const std::string kSteamId = "76561198000000001";
    const std::string kSteamOtherId = "76561198000000002";
}

TEST(TrackedAccountIdentity, CanonicalizesTelemetryAndManualForms) {
    const auto telemetry = FromPrimaryId("Epic|" + std::string("0123456789ABCDEF0123456789ABCDEF") + "|0");
    ASSERT_TRUE(telemetry.has_value());
    EXPECT_EQ(telemetry->primaryId, "Epic|" + kEpicHex);
    EXPECT_EQ(telemetry->platform, Platform::Epic);

    const auto manual = ParseManualInput(Platform::Epic, "  " + kEpicHex + "  ");
    ASSERT_TRUE(manual.identity.has_value());
    EXPECT_EQ(*manual.identity, *telemetry);

    EXPECT_EQ(CanonicalPrimaryId("Steam|" + kSteamId + "|0"), "Steam|" + kSteamId);
    EXPECT_EQ(CanonicalPrimaryId("xboxone|2535421607154237|0"), "XboxOne|2535421607154237");
    EXPECT_EQ(CanonicalPrimaryId("PS5|1234567890"), "PS4|1234567890");
}

TEST(TrackedAccountIdentity, RejectsUnknownMalformedAndUnsupportedIdentities) {
    EXPECT_FALSE(FromPrimaryId("").has_value());
    EXPECT_FALSE(FromPrimaryId("Unknown|SomePlayer").has_value());
    EXPECT_FALSE(FromPrimaryId("Steam|123").has_value());
    EXPECT_FALSE(FromPrimaryId("Epic|not-a-hex-id").has_value());
    EXPECT_FALSE(FromPrimaryId("Stadia|12345").has_value());
    EXPECT_FALSE(FromPrimaryId("Steam").has_value());
    EXPECT_FALSE(FromPrimaryId(std::string(400, '7')).has_value());
}

TEST(TrackedAccountIdentity, ManualInputExplainsDisplayNamesAndUnresolvableLinks) {
    auto result = ParseManualInput(Platform::Epic, "CoolPlayer123");
    EXPECT_FALSE(result.identity.has_value());
    EXPECT_EQ(result.error, InputError::DisplayName);
    EXPECT_FALSE(result.message.empty());

    result = ParseManualInput(Platform::Steam, "https://steamcommunity.com/id/vanityname");
    EXPECT_EQ(result.error, InputError::UnresolvableUrl);

    result = ParseManualInput(Platform::Steam, "https://steamcommunity.com/profiles/" + kSteamId + "/?l=en");
    ASSERT_TRUE(result.identity.has_value());
    EXPECT_EQ(result.identity->primaryId, "Steam|" + kSteamId);

    result = ParseManualInput(Platform::Steam, "https://rocketleague.tracker.network/rocket-league/profile/steam/" + kSteamId + "/overview");
    ASSERT_TRUE(result.identity.has_value());
    EXPECT_EQ(result.identity->primaryId, "Steam|" + kSteamId);

    result = ParseManualInput(Platform::Epic, "https://rocketleague.tracker.network/rocket-league/profile/epic/CoolPlayer/overview");
    EXPECT_EQ(result.error, InputError::UnresolvableUrl);

    result = ParseManualInput(Platform::Epic, "https://steamcommunity.com/profiles/" + kSteamId);
    EXPECT_EQ(result.error, InputError::PlatformMismatch);

    result = ParseManualInput(Platform::Epic, "Steam|" + kSteamId + "|0");
    EXPECT_EQ(result.error, InputError::PlatformMismatch);

    result = ParseManualInput(Platform::Steam, "   ");
    EXPECT_EQ(result.error, InputError::Empty);

    result = ParseManualInput(Platform::Steam, std::string(kMaxInputLength + 1, '7'));
    EXPECT_EQ(result.error, InputError::TooLong);

    result = ParseManualInput(Platform::Xbox, "Major Nelson");
    EXPECT_EQ(result.error, InputError::DisplayName);
}

TEST(TrackedAccountIdentity, SameAccountIdOnDifferentPlatformsIsDistinct) {
    const auto xbox = ParseManualInput(Platform::Xbox, "1234567890");
    const auto playStation = ParseManualInput(Platform::PlayStation, "1234567890");
    ASSERT_TRUE(xbox.identity && playStation.identity);
    EXPECT_NE(xbox.identity->primaryId, playStation.identity->primaryId);
}

TEST(TrackedAccountIdentity, NormalizesLabelsWithoutSplittingUtf8) {
    EXPECT_EQ(NormalizeLabel("  Main\t\x01 Account  ", kMaxNicknameLength), "Main Account");
    const std::string accented = "\xC3\xA9";
    std::string longName;
    for (int i = 0; i < 50; ++i)
        longName += accented;
    const std::string clamped = NormalizeLabel(longName, kMaxNicknameLength);
    EXPECT_EQ(clamped.size(), kMaxNicknameLength * accented.size());
    EXPECT_EQ(NormalizeLabel("bad\xFF\xFEname", 40), "badname");
}

TEST(TrackedAccountIdentity, SupportedPlaylistsComeFromPlaylistMetadata) {
    const auto& playlists = SupportedPlaylists();
    ASSERT_GE(playlists.size(), 3u);
    EXPECT_EQ(playlists[0].key, "1v1");
    EXPECT_EQ(playlists[1].key, "2v2");
    EXPECT_EQ(playlists[2].key, "3v3");
    for (size_t i = 0; i < playlists.size(); ++i)
        EXPECT_EQ(playlists[i].standard, i < 3) << playlists[i].key;
    for (const char* key : {"hoops", "rumble", "dropshot", "snowday", "heatseeker", "t"}) {
        EXPECT_TRUE(IsSupportedPlaylistKey(key)) << key;
    }
    EXPECT_FALSE(IsSupportedPlaylistKey("casual"));
    EXPECT_FALSE(IsSupportedPlaylistKey("best"));
}

class TrackedAccountDiscoveryTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        original = Config::Read();
        Config::Update([](ConfigData& c) {
            c.last_primary_id.clear();
            c.known_primary_ids.clear();
            c.enable_mmr_tracking = false;
            c.custom_api_enabled = false;
            c.custom_api_key.clear();
        },
                       false);
        state = std::make_shared<SessionState>();
        db = std::make_shared<DatabaseManager>(state);
        ASSERT_TRUE(db->Initialize(":memory:"));
        fetcher = std::make_shared<MMRFetcher>(state, db);
        service = std::make_shared<AccountManagerService>(db, fetcher);
        service->SetClockForTests([this]() { return clock; });
    }

    void TearDown() override {
        service->Stop();
        Config::Update([this](ConfigData& c) { c = original; }, false);
    }

    void Start() {
        service->Start();
        service->FlushForTests();
    }

    nlohmann::json Lobby(const std::string& localId, const std::string& localName) {
        nlohmann::json data;
        data["Players"] = nlohmann::json::array();
        data["Players"].push_back({{"PrimaryId", localId}, {"Name", localName}, {"TeamNum", 0}, {"Boost", 33}});
        data["Players"].push_back({{"PrimaryId", "Epic|" + kEpicHex + "|0"}, {"Name", "Opponent"}, {"TeamNum", 1}});
        data["Players"].push_back({{"PrimaryId", "PS4|555|0"}, {"Name", "Teammate"}, {"TeamNum", 0}});
        return data;
    }

    ConfigData original;
    int64_t clock = 1'800'000'000;
    std::shared_ptr<SessionState> state;
    std::shared_ptr<DatabaseManager> db;
    std::shared_ptr<MMRFetcher> fetcher;
    std::shared_ptr<AccountManagerService> service;
};

TEST_F(TrackedAccountDiscoveryTest, ReducerConfirmsOnlyTheIdentifiedLocalPlayerOnce) {
    TelemetryReducer reducer(state);
    const std::string me = "Steam|" + kSteamId + "|0";

    auto effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Lobby(me, "Me"));
    ASSERT_TRUE(effects.confirmedLocalAccount.has_value());
    EXPECT_EQ(effects.confirmedLocalAccount->primaryId, me);
    EXPECT_EQ(effects.confirmedLocalAccount->name, "Me");

    effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Lobby(me, "Me"));
    EXPECT_FALSE(effects.confirmedLocalAccount.has_value());

    effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Lobby(me, "Me Renamed"));
    ASSERT_TRUE(effects.confirmedLocalAccount.has_value());
    EXPECT_EQ(effects.confirmedLocalAccount->name, "Me Renamed");

    const std::string alt = "Steam|" + kSteamOtherId + "|0";
    effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Lobby(alt, "Alt"));
    ASSERT_TRUE(effects.confirmedLocalAccount.has_value());
    EXPECT_EQ(effects.confirmedLocalAccount->primaryId, alt);

    effects = reducer.Reduce(std::string(Constants::EVT_UPDATE_STATE), Lobby(me, "Me Renamed"));
    ASSERT_TRUE(effects.confirmedLocalAccount.has_value());
    EXPECT_EQ(effects.confirmedLocalAccount->primaryId, me);
}

TEST_F(TrackedAccountDiscoveryTest, ConfirmedAccountsRegisterOnceAndKeepNicknames) {
    Start();
    service->ObserveLocalAccount("Steam|" + kSteamId + "|0", "Me");
    service->FlushForTests();

    auto stored = db->GetTrackedAccount("Steam|" + kSteamId);
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->discoverySource, DiscoverySource::Auto);
    EXPECT_EQ(stored->lastKnownName, "Me");
    EXPECT_EQ(stored->lastSeenAtUnix, clock);

    ASSERT_TRUE(service->Rename("Steam|" + kSteamId, "Main"));
    clock += 600;
    service->ObserveLocalAccount("Steam|" + kSteamId + "|0", "Me");
    service->FlushForTests();
    stored = db->GetTrackedAccount("Steam|" + kSteamId);
    EXPECT_EQ(stored->lastSeenAtUnix, clock - 600);

    service->ObserveLocalAccount("Steam|" + kSteamOtherId + "|0", "Alt");
    service->ObserveLocalAccount("Steam|" + kSteamId + "|0", "Me New");
    service->FlushForTests();

    const auto snapshot = service->GetSnapshot();
    ASSERT_EQ(snapshot.accounts.size(), 2u);
    stored = db->GetTrackedAccount("Steam|" + kSteamId);
    EXPECT_EQ(stored->nickname, "Main");
    EXPECT_EQ(stored->lastKnownName, "Me New");
    EXPECT_EQ(stored->lastSeenAtUnix, clock);
    EXPECT_EQ(db->ListTrackedAccounts().size(), 2u);
}

TEST_F(TrackedAccountDiscoveryTest, ObservationsBeforeLoadAreAppliedAfterLoad) {
    service->ObserveLocalAccount("Steam|" + kSteamId + "|0", "Early");
    Start();
    service->FlushForTests();
    const auto snapshot = service->GetSnapshot();
    ASSERT_EQ(snapshot.accounts.size(), 1u);
    EXPECT_EQ(snapshot.accounts[0].record.lastKnownName, "Early");
}

TEST_F(TrackedAccountDiscoveryTest, UnknownIdentitiesAndSharedDisplayNames) {
    Start();
    service->ObserveLocalAccount("Unknown|Someone", "Someone");
    service->ObserveLocalAccount("Steam|123", "Short");
    service->ObserveLocalAccount("", "");
    EXPECT_TRUE(service->GetSnapshot().accounts.empty());

    service->ObserveLocalAccount("Steam|" + kSteamId, "Same");
    service->ObserveLocalAccount("Epic|" + kEpicHex, "Same");
    service->FlushForTests();
    EXPECT_EQ(service->GetSnapshot().accounts.size(), 2u);
}

TEST_F(TrackedAccountDiscoveryTest, ManualRegistrationValidatesAndDetectsDuplicates) {
    Start();
    auto result = service->AddManual(Platform::Epic, "SomeDisplayName", "Alt");
    EXPECT_EQ(result.status, AccountManagerService::AddResult::Status::Invalid);
    EXPECT_FALSE(result.message.empty());

    result = service->AddManual(Platform::Xbox, "1234567890", "Xbox alt");
    EXPECT_EQ(result.status, AccountManagerService::AddResult::Status::Added);
    result = service->AddManual(Platform::PlayStation, "1234567890", "PS alt");
    EXPECT_EQ(result.status, AccountManagerService::AddResult::Status::Added);

    result = service->AddManual(Platform::Xbox, "XboxOne|1234567890|0", "Again");
    EXPECT_EQ(result.status, AccountManagerService::AddResult::Status::AlreadyTracked);
    EXPECT_EQ(result.primaryId, "XboxOne|1234567890");
    EXPECT_NE(result.message.find("Xbox alt"), std::string::npos);

    service->FlushForTests();
    const auto snapshot = service->GetSnapshot();
    ASSERT_EQ(snapshot.accounts.size(), 2u);
    EXPECT_EQ(snapshot.accounts[0].refreshState, AccountManagerService::RefreshState::ProviderUnavailable);
}

TEST_F(TrackedAccountDiscoveryTest, ImportDeduplicatesAndLeavesLiveIdentityAlone) {
    Config::Update([](ConfigData& c) {
        c.known_primary_ids = {"Steam|" + kSteamId + "|0", "Steam|" + kSteamId, "Unknown|Bob", "garbage", "Steam|42",
                               "Epic|" + kEpicHex + "|0", "Steam|" + kSteamOtherId + "|0"};
        c.last_primary_id = "Epic|" + kEpicHex + "|0";
    },
                   false);
    Start();
    service->ObserveLocalAccount("Steam|" + kSteamOtherId + "|0", "Already");
    service->FlushForTests();

    const auto candidates = service->ImportCandidates();
    ASSERT_EQ(candidates.size(), 2u);
    EXPECT_EQ(candidates[0].identity.primaryId, "Steam|" + kSteamId);
    EXPECT_EQ(candidates[1].identity.primaryId, "Epic|" + kEpicHex);

    EXPECT_EQ(service->ImportKnown(), 2u);
    EXPECT_EQ(service->ImportKnown(), 0u);
    EXPECT_TRUE(service->ImportCandidates().empty());
    service->FlushForTests();

    EXPECT_EQ(db->ListTrackedAccounts().size(), 3u);
    EXPECT_EQ(db->GetTrackedAccount("Epic|" + kEpicHex)->discoverySource, DiscoverySource::Import);
    EXPECT_EQ(Config::Read().last_primary_id, "Epic|" + kEpicHex + "|0");
    EXPECT_EQ(Config::Read().known_primary_ids.size(), 7u);
    std::shared_lock lock(state->game.mutex);
    EXPECT_TRUE(state->game.myPrimaryId.empty());
}

TEST_F(TrackedAccountDiscoveryTest, ImportedAndManualAccountsTakeTheirNameFromMatchHistory) {
    const auto saveMatch = [&](const std::string& guid, int64_t endedAtMs, const std::string& localName) {
        MatchSaveSnapshot snap;
        snap.arenaName = "DFH Stadium";
        snap.matchGuid = guid;
        snap.playlistId = 11;
        snap.myTeam = 0;
        snap.winnerTeam = 0;
        snap.validResult = true;
        snap.score[0] = 2;
        snap.score[1] = 1;
        snap.endedAtUnixMs = endedAtMs;
        snap.myPrimaryId = "Steam|" + kSteamId + "|0";
        snap.roster[snap.myPrimaryId] = PlayerData{.primaryId = snap.myPrimaryId, .name = localName, .team = 0, .mmr = 1200};
        const std::string epic = "Epic|" + std::string("0123456789ABCDEF0123456789ABCDEF") + "|0";
        snap.roster[epic] = PlayerData{.primaryId = epic, .name = "EpicAlt", .team = 1, .mmr = 1180};
        db->SaveMatch(snap);
    };
    saveMatch("name-guid-1", 1'700'000'000'000LL, "OldName");
    saveMatch("name-guid-2", 1'700'000'600'000LL, "NewName");

    Config::Update([](ConfigData& c) { c.known_primary_ids = {"Steam|" + kSteamId + "|0"}; }, false);
    Start();

    const auto candidates = service->ImportCandidates();
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].lastKnownName, "NewName");

    ASSERT_EQ(service->ImportKnown(), 1u);
    ASSERT_EQ(service->AddManual(Platform::Epic, kEpicHex, "").status, AccountManagerService::AddResult::Status::Added);
    ASSERT_EQ(service->AddManual(Platform::Steam, kSteamOtherId, "").status, AccountManagerService::AddResult::Status::Added);
    service->FlushForTests();

    EXPECT_EQ(db->GetTrackedAccount("Steam|" + kSteamId)->lastKnownName, "NewName");
    EXPECT_EQ(db->GetTrackedAccount("Epic|" + kEpicHex)->lastKnownName, "EpicAlt");
    EXPECT_TRUE(db->GetTrackedAccount("Steam|" + kSteamOtherId)->lastKnownName.empty());
    const auto snapshot = service->GetSnapshot();
    ASSERT_EQ(snapshot.accounts.size(), 3u);
    EXPECT_EQ(TrackedAccounts::DisplayLabel(snapshot.accounts[1].record), "EpicAlt");
}

TEST_F(TrackedAccountDiscoveryTest, ExistingUnnamedAccountsAreBackfilledOnStart) {
    MatchSaveSnapshot snap;
    snap.arenaName = "Mannfield";
    snap.matchGuid = "backfill-guid";
    snap.playlistId = 11;
    snap.myTeam = 0;
    snap.winnerTeam = 1;
    snap.validResult = true;
    snap.endedAtUnixMs = 1'700'000'000'000LL;
    snap.myPrimaryId = "Steam|" + kSteamId + "|0";
    snap.roster[snap.myPrimaryId] = PlayerData{.primaryId = snap.myPrimaryId, .name = "Backfilled", .team = 0, .mmr = 1200};
    db->SaveMatch(snap);
    ASSERT_EQ(db->RegisterTrackedAccount(MakeRecord(*FromPrimaryId("Steam|" + kSteamId), "Nick", "", DiscoverySource::Import)),
              TrackedAccountInsertResult::Inserted);

    Start();
    service->FlushForTests();

    EXPECT_EQ(service->GetSnapshot().accounts[0].record.lastKnownName, "Backfilled");
    EXPECT_EQ(db->GetTrackedAccount("Steam|" + kSteamId)->lastKnownName, "Backfilled");
    EXPECT_EQ(db->GetTrackedAccount("Steam|" + kSteamId)->nickname, "Nick");
}
