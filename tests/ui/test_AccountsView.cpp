#include <gtest/gtest.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "core/accounts/AccountManager.hpp"
#include "core/config/Config.hpp"
#include "core/state/SessionState.hpp"
#include "utils/Storage.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include "database/DatabaseManager.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "ui/rml/controller/RmlUiController.hpp"
#include "ui/rml/views/AccountsView.hpp"
#include "ui/Formatting.hpp"

namespace {
    struct WarpDevice {
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;

        bool Create(UINT width, UINT height) {
            D3D_FEATURE_LEVEL level;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context)))
                return false;
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc = {1, 0};
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) return false;
            if (FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) return false;
            const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
            context->ClearRenderTargetView(target.Get(), clear);
            return true;
        }
    };
}

class AccountsViewTest : public ::testing::Test {
  protected:
    ConfigData original;
    int64_t simulatedTime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

    void SetUp() override {
        Storage::InitializeEnvironment();
        original = Config::Read();
        Config::Update([](ConfigData& c) {
            c.enable_mmr_tracking = true;
            c.custom_api_enabled = false;
            c.custom_api_key.clear();
            c.onboarding_completed = true;
            c.account_link_prompt_shown = true;
            c.last_primary_id.clear();
        },
                       true);
    }

    void TearDown() override {
        Config::Update([this](ConfigData& c) { c = original; }, true);
    }

    Rml::ElementDocument* AccountsDocument(RmlUiController& controller) {
        return controller.m_accountsDoc;
    }
    AccountsView& AccountsModel(RmlUiController& controller) {
        return controller.m_accounts;
    }
    bool IsAccountsVisible(const RmlUiController& controller) const {
        return controller.m_accountsVisible;
    }
    void Click(RmlUiController& controller, Rml::Element* element) {
        controller.HandleClick(element);
    }
    void ReloadResources(RmlUiController& controller) {
        controller.ReloadUiResources();
    }
};

TEST(AccountsViewTiers, PlacementRanksWithMmrShowTheTierForThatMmr) {
    AccountsView view;
    view.SetColumnSet("extra");
    view.SetColumnSet("competitive");

    AccountManagerService::AccountView account;
    account.record = TrackedAccounts::MakeRecord(*TrackedAccounts::FromPrimaryId("Epic|0123456789abcdef0123456789abcdef"), "Main", "", TrackedAccounts::DiscoverySource::Manual);
    account.record.ranks["1v1"] = TrackedAccounts::PlaylistRank{"1v1", 642, "Unranked", 3, "OmniStats API", 1};
    account.record.ranks["2v2"] = TrackedAccounts::PlaylistRank{"2v2", 1246, "Champion II Div II", 296, "OmniStats API", 1};
    account.record.ranks["3v3"] = TrackedAccounts::PlaylistRank{"3v3", 879, "", std::nullopt, "OmniStats API", 1};
    AccountManagerService::Snapshot snapshot;
    snapshot.loaded = true;
    snapshot.accounts = {account};
    view.UpdateSnapshot(snapshot, "", 0, 2);

    ASSERT_EQ(view.Rows().size(), 1u);
    const auto& cells = view.Rows()[0].ranks;
    ASSERT_EQ(cells.size(), 3u);
    const std::string solo = MMRFetcher::GetRankTierForPlaylistMmr("1v1", 642);
    const std::string standard = MMRFetcher::GetRankTierForPlaylistMmr("3v3", 879);
    ASSERT_NE(solo, "Unranked");
    ASSERT_NE(standard, "Unranked");
    EXPECT_EQ(cells[0].tier, Format::AbbreviateRank(solo));
    EXPECT_EQ(cells[1].tier, Format::AbbreviateRank("Champion II Div II"));
    EXPECT_EQ(cells[2].tier, Format::AbbreviateRank(standard));
}

TEST_F(AccountsViewTest, EmptyStateRenders) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);

    EXPECT_FALSE(controller.WantsAttention());
    EXPECT_FALSE(IsAccountsVisible(controller));

    controller.OpenAccounts();
    EXPECT_TRUE(IsAccountsVisible(controller));
    EXPECT_TRUE(controller.WantsAttention());

    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_EQ(AccountsModel(controller).Rows().size(), 0u);
    ASSERT_NE(AccountsDocument(controller), nullptr);
    EXPECT_NE(AccountsDocument(controller)->QuerySelector(".accounts-empty"), nullptr);
    EXPECT_NE(AccountsDocument(controller)->QuerySelector("[data-action='accounts-open-add']"), nullptr);
    EXPECT_FALSE(AccountsDocument(controller)->QuerySelector(".accounts-table")->IsVisible());

    service->Stop();
}

TEST_F(AccountsViewTest, SingleAccountRenders) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    auto addRes = service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "SoloPlayer");
    EXPECT_EQ(addRes.status, AccountManagerService::AddResult::Status::Added);
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, "SoloPlayer");
    EXPECT_EQ(AccountsModel(controller).Rows()[0].primary_id, "Steam|76561198000000001");

    auto* headerLabel = AccountsDocument(controller)->QuerySelector(".view-header .label");
    ASSERT_NE(headerLabel, nullptr);
    EXPECT_EQ(headerLabel->GetInnerRML(), "1 account tracked");

    service->Stop();
}

TEST_F(AccountsViewTest, ManyAccountsRenderAndScroll) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    for (int i = 0; i < 20; ++i) {
        std::string idStr = "765611980000000" + (i < 10 ? std::string("0") : std::string()) + std::to_string(i);
        std::string nameStr = "Account_" + std::to_string(i);
        service->AddManual(TrackedAccounts::Platform::Steam, idStr, nameStr);
    }
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 20u);
    auto* body = AccountsDocument(controller)->QuerySelector(".accounts-body");
    ASSERT_NE(body, nullptr);

    Rml::ElementList rowElems;
    AccountsDocument(controller)->QuerySelectorAll(rowElems, ".accounts-row.item");
    const auto visibleRows = std::count_if(rowElems.begin(), rowElems.end(), [](Rml::Element* e) { return e->IsVisible(); });
    EXPECT_EQ(visibleRows, 20);

    service->Stop();
}

TEST_F(AccountsViewTest, LongNicknameBounded) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    const std::string longNick = "ThisIsAFortyCharacterLongNickname1234567";
    service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", longNick);
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, longNick);

    auto* nameElem = AccountsDocument(controller)->QuerySelector(".accounts-name");
    ASSERT_NE(nameElem, nullptr);
    EXPECT_EQ(nameElem->GetInnerRML(), longNick);

    service->Stop();
}

TEST_F(AccountsViewTest, SortingByPlaylistDescWithMissingValuesLastAndNickname) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    const std::string pidA = "Steam|76561198000000001";
    const std::string pidB = "Steam|76561198000000002";
    const std::string pidC = "Steam|76561198000000003";

    auto idA = TrackedAccounts::FromPrimaryId(pidA);
    auto idB = TrackedAccounts::FromPrimaryId(pidB);
    auto idC = TrackedAccounts::FromPrimaryId(pidC);

    db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*idA, "Alice", "", TrackedAccounts::DiscoverySource::Manual));
    db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*idB, "Bob", "", TrackedAccounts::DiscoverySource::Manual));
    db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*idC, "Charlie", "", TrackedAccounts::DiscoverySource::Manual));

    TrackedAccounts::PlaylistRank rankA;
    rankA.playlistKey = "2v2";
    rankA.mmr = 1200;
    rankA.rankTier = "Diamond I";
    db->SaveTrackedAccountRanks(pidA, {rankA}, simulatedTime);

    TrackedAccounts::PlaylistRank rankB;
    rankB.playlistKey = "2v2";
    rankB.mmr = 1400;
    rankB.rankTier = "Champion I";
    db->SaveTrackedAccountRanks(pidB, {rankB}, simulatedTime);

    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 3u);

    auto* sort2v2 = AccountsDocument(controller)->QuerySelector("[data-action='accounts-sort'][data-sort='2v2']");
    ASSERT_NE(sort2v2, nullptr);
    Click(controller, sort2v2);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 3u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, "Bob");
    EXPECT_EQ(AccountsModel(controller).Rows()[1].display_label, "Alice");
    EXPECT_EQ(AccountsModel(controller).Rows()[2].display_label, "Charlie");

    Click(controller, sort2v2);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 3u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, "Alice");
    EXPECT_EQ(AccountsModel(controller).Rows()[1].display_label, "Bob");
    EXPECT_EQ(AccountsModel(controller).Rows()[2].display_label, "Charlie");

    auto* sortNick = AccountsDocument(controller)->QuerySelector("[data-action='accounts-sort'][data-sort='nickname']");
    ASSERT_NE(sortNick, nullptr);
    Click(controller, sortNick);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 3u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, "Alice");
    EXPECT_EQ(AccountsModel(controller).Rows()[1].display_label, "Bob");
    EXPECT_EQ(AccountsModel(controller).Rows()[2].display_label, "Charlie");

    service->Stop();
}

TEST_F(AccountsViewTest, RenameAndRemoveThroughClicks) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "InitialName");
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* rowElem = AccountsDocument(controller)->QuerySelector(".accounts-row.item");
    ASSERT_NE(rowElem, nullptr);
    Click(controller, rowElem);
    controller.Render();

    EXPECT_TRUE(AccountsModel(controller).IsDetailOpen());
    auto* renameStartBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-detail-start-rename']");
    ASSERT_NE(renameStartBtn, nullptr);
    Click(controller, renameStartBtn);
    controller.Render();

    AccountsModel(controller).SetRenameValue("RenamedViaClick");

    auto* renameSaveBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-detail-save-rename']");
    ASSERT_NE(renameSaveBtn, nullptr);
    Click(controller, renameSaveBtn);
    service->FlushForTests();

    auto snap = service->GetSnapshot();
    ASSERT_EQ(snap.accounts.size(), 1u);
    EXPECT_EQ(snap.accounts[0].record.nickname, "RenamedViaClick");

    auto* removeStartBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-detail-start-remove']");
    ASSERT_NE(removeStartBtn, nullptr);
    Click(controller, removeStartBtn);
    controller.Render();

    auto* removeConfirmBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-detail-confirm-remove']");
    ASSERT_NE(removeConfirmBtn, nullptr);
    Click(controller, removeConfirmBtn);
    service->FlushForTests();

    EXPECT_FALSE(AccountsModel(controller).IsDetailOpen());
    auto snapAfter = service->GetSnapshot();
    EXPECT_TRUE(snapAfter.accounts.empty());

    service->Stop();
}

TEST_F(AccountsViewTest, ActiveAccountHighlighting) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "LocalUser");
    service->FlushForTests();

    {
        std::unique_lock lock(state->game.mutex);
        state->game.myPrimaryId = "Steam|76561198000000001|0";
    }

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_TRUE(AccountsModel(controller).Rows()[0].is_active);

    auto* activeLabel = AccountsDocument(controller)->QuerySelector(".accounts-active-label");
    ASSERT_NE(activeLabel, nullptr);
    EXPECT_EQ(activeLabel->GetInnerRML(), "Active");

    service->Stop();
}

TEST_F(AccountsViewTest, StaleDataLabelAfterClockAdvance) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    const std::string pid = "Steam|76561198000000001";
    auto id = TrackedAccounts::FromPrimaryId(pid);
    db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*id, "StaleUser", "", TrackedAccounts::DiscoverySource::Manual));
    TrackedAccounts::PlaylistRank rank;
    rank.playlistKey = "2v2";
    rank.mmr = 1000;
    db->SaveTrackedAccountRanks(pid, {rank}, simulatedTime);

    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].status_text, "Updated");

    simulatedTime += (AccountManagerService::kStaleAfterSeconds + 100);
    service->SetClockForTests([this]() { return simulatedTime; });

    auto snap = service->GetSnapshot();
    AccountsModel(controller).UpdateSnapshot(snap, "", 0, simulatedTime);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].status_text, "Stale");
    EXPECT_TRUE(AccountsModel(controller).Rows()[0].is_stale_or_failed);

    service->Stop();
}

TEST_F(AccountsViewTest, NoApiBannerWhenProviderUnavailable) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.enable_mmr_tracking = false;
        c.custom_api_enabled = false;
    },
                   true);

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* banner = AccountsDocument(controller)->QuerySelector(".accounts-provider-banner");
    ASSERT_NE(banner, nullptr);

    auto* openSettingsBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-open-settings']");
    ASSERT_NE(openSettingsBtn, nullptr);
    Click(controller, openSettingsBtn);

    EXPECT_TRUE(state->ui.showMenu.load());

    service->Stop();
}

TEST_F(AccountsViewTest, RefreshFailureStateShowsSavedMmr) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));

    const std::string pid = "Steam|76561198000000001";
    auto id = TrackedAccounts::FromPrimaryId(pid);
    db->RegisterTrackedAccount(TrackedAccounts::MakeRecord(*id, "SavedMmrUser", "", TrackedAccounts::DiscoverySource::Manual));
    TrackedAccounts::PlaylistRank rank;
    rank.playlistKey = "2v2";
    rank.mmr = 1350;
    rank.rankTier = "Diamond II";
    db->SaveTrackedAccountRanks(pid, {rank}, simulatedTime);

    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);

    auto snap = service->GetSnapshot();
    ASSERT_EQ(snap.accounts.size(), 1u);
    snap.accounts[0].refreshState = AccountManagerService::RefreshState::Failed;

    AccountsModel(controller).UpdateSnapshot(snap, "", 0, simulatedTime);
    controller.Render();

    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    const auto& row = AccountsModel(controller).Rows()[0];
    EXPECT_EQ(row.status_text, "Refresh failed");
    EXPECT_TRUE(row.is_stale_or_failed);
    const auto twos = std::find_if(row.ranks.begin(), row.ranks.end(), [](const auto& cell) { return cell.playlist_key == "2v2"; });
    ASSERT_NE(twos, row.ranks.end());
    EXPECT_EQ(twos->mmr, "1350");
    EXPECT_TRUE(twos->is_stale);

    service->Stop();
}

TEST_F(AccountsViewTest, OpenCloseRepeatedly) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);

    EXPECT_FALSE(controller.WantsAttention());
    EXPECT_FALSE(IsAccountsVisible(controller));

    controller.OpenAccounts();
    EXPECT_TRUE(IsAccountsVisible(controller));
    EXPECT_TRUE(controller.WantsAttention());

    controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(IsAccountsVisible(controller));
    EXPECT_FALSE(controller.WantsAttention());

    controller.OpenAccounts();
    EXPECT_TRUE(IsAccountsVisible(controller));
    auto* closeBtn = AccountsDocument(controller)->QuerySelector("[data-action='accounts-close']");
    ASSERT_NE(closeBtn, nullptr);
    Click(controller, closeBtn);
    EXPECT_FALSE(IsAccountsVisible(controller));

    service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "DetailUser");
    service->FlushForTests();
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    AccountsModel(controller).OpenDetail("Steam|76561198000000001");
    EXPECT_TRUE(AccountsModel(controller).IsDetailOpen());
    controller.ProcessWindowMessage(nullptr, WM_KEYDOWN, VK_ESCAPE, 0);
    EXPECT_FALSE(AccountsModel(controller).IsDetailOpen());
    EXPECT_TRUE(IsAccountsVisible(controller));

    service->Stop();
}

TEST_F(AccountsViewTest, ReloadUiResourcesPreservesState) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto fetcher = std::make_shared<MMRFetcher>(state, db);
    auto service = std::make_shared<AccountManagerService>(db, fetcher);
    service->SetClockForTests([this]() { return simulatedTime; });
    service->Start();
    service->FlushForTests();

    service->AddManual(TrackedAccounts::Platform::Steam, "76561198000000001", "ReloadUser");
    service->FlushForTests();

    RmlUiController controller(state, db, service);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.OpenAccounts();
    controller.Update(Config::Read(), false);
    controller.Render();

    AccountsModel(controller).OpenDetail("Steam|76561198000000001");
    EXPECT_TRUE(AccountsModel(controller).IsDetailOpen());
    EXPECT_TRUE(IsAccountsVisible(controller));

    ReloadResources(controller);
    controller.Render();

    EXPECT_TRUE(IsAccountsVisible(controller));
    ASSERT_EQ(AccountsModel(controller).Rows().size(), 1u);
    EXPECT_EQ(AccountsModel(controller).Rows()[0].display_label, "ReloadUser");
    EXPECT_TRUE(AccountsModel(controller).IsDetailOpen());
    EXPECT_EQ(AccountsModel(controller).DetailPrimaryId(), "Steam|76561198000000001");

    service->Stop();
}

TEST(AccountsViewHistory, BuildsDistinctAccountSeriesAndSeparatesUnobservedGaps) {
    const std::string a = "Steam|76561198000000001";
    const std::string b = "Steam|76561198000000002";
    AccountManagerService::Snapshot snapshot;
    snapshot.loaded = true;
    for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{{a, "Main"}, {b, "Alt"}}) {
        AccountManagerService::AccountView account;
        account.record = TrackedAccounts::MakeRecord(*TrackedAccounts::FromPrimaryId(id), name, name,
                                                     TrackedAccounts::DiscoverySource::Manual);
        account.record.lastSuccessfulRefreshAtUnix = 1713000000;
        snapshot.accounts.push_back(std::move(account));
    }
    AccountsView view;
    view.SetHistoryWindow("all");
    view.UpdateSnapshot(snapshot, "", 0, 1713000000);
    ASSERT_EQ(view.HistoryAccountIds().size(), 2u);

    snapshot.history.loaded = true;
    snapshot.history.playlistKey = "2v2";
    snapshot.history.window = "all";
    snapshot.history.accountIds = view.HistoryAccountIds();
    TrackedAccounts::RankHistorySeries main;
    main.primaryId = a;
    main.highestObservedMmr = 1270;
    main.totalObservations = 3;
    for (const auto& [mmr, when] : std::vector<std::pair<int, int64_t>>{{1200, 1710000000}, {1230, 1710000100}, {1270, 1712000000}}) {
        main.observations.push_back({a, "2v2", mmr, "Diamond", 10, "Tracker", when});
    }
    TrackedAccounts::RankHistorySeries alt;
    alt.primaryId = b;
    alt.highestObservedMmr = 1400;
    alt.totalObservations = 1;
    alt.observations.push_back({b, "2v2", 1400, "Champion", 11, "OmniStats API", 1711000000});
    snapshot.history.series = {main, alt};
    view.UpdateSnapshot(snapshot, "", 0, 1713000000);
    ASSERT_EQ(view.HistoryPlots().size(), 2u);
    EXPECT_EQ(view.HistoryPlots()[0].label, "Main");
    EXPECT_EQ(view.HistoryPlots()[0].change, "+70");
    EXPECT_EQ(view.HistoryPlots()[0].high, "1270");
    ASSERT_EQ(view.HistoryPlots()[0].segments.size(), 1u); // 23-day gap is not connected
    EXPECT_EQ(view.HistoryPlots()[1].label, "Alt");
    EXPECT_TRUE(view.HistoryPlots()[1].has_points);
    EXPECT_TRUE(view.HistoryPlots()[1].segments.empty()); // one actual sample is a dot, not a fake line
    EXPECT_TRUE(view.ToggleHistoryAccount(b));
    ASSERT_EQ(view.HistoryAccountIds().size(), 1u);
    EXPECT_EQ(view.HistoryAccountIds().front(), a);
    view.SetHistoryPlaylist("3v3");
    EXPECT_EQ(view.HistoryPlaylist(), "3v3");
}
