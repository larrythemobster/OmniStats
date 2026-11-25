#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, FirstRunWizardShowsUntilFinished) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.onboarding_completed = false; }, true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_TRUE(controller.WantsAttention());
    EXPECT_TRUE(controller.WantsInteraction());

    Rml::ElementList buttons;
    OnboardingDocument(controller)->QuerySelectorAll(buttons, "[data-action='onboarding-finish']");
    ASSERT_FALSE(buttons.empty());
    Click(controller, buttons.front());

    EXPECT_TRUE(Config::Read().onboarding_completed);
    EXPECT_FALSE(controller.WantsAttention());
}

TEST_F(RmlUiControllerStateTest, FirstRunRequiresConsentBeforeSetupCanFinish) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) {
        c.onboarding_completed = false;
        c.privacy_policy_accepted_version.clear();
        c.terms_accepted_version.clear();
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    ASSERT_TRUE(controller.IsOnboardingVisible());

    Rml::ElementList finishButtons;
    OnboardingDocument(controller)->QuerySelectorAll(finishButtons, "[data-action='onboarding-finish']");
    ASSERT_FALSE(finishButtons.empty());
    Click(controller, finishButtons.front());
    EXPECT_FALSE(Config::Read().onboarding_completed);
    EXPECT_TRUE(controller.IsOnboardingVisible());

    Rml::ElementList acceptButtons;
    OnboardingDocument(controller)->QuerySelectorAll(acceptButtons, "[data-action='onboarding-accept-terms']");
    ASSERT_FALSE(acceptButtons.empty());
    Click(controller, acceptButtons.front());
    EXPECT_TRUE(Config::HasCurrentConsent(Config::Read()));
    EXPECT_FALSE(Config::Read().onboarding_completed);
    EXPECT_TRUE(controller.IsOnboardingVisible());
    EXPECT_EQ(OnboardingModel(controller).Step(), 0);
}

TEST_F(RmlUiControllerStateTest, ReturningUserReacceptsUpdatedTermsWithoutRerunningSetup) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.terms_accepted_version = "2000-01-01"; }, true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    ASSERT_TRUE(controller.IsOnboardingVisible());

    Rml::ElementList acceptButtons;
    OnboardingDocument(controller)->QuerySelectorAll(acceptButtons, "[data-action='onboarding-accept-terms']");
    ASSERT_FALSE(acceptButtons.empty());
    Click(controller, acceptButtons.front());
    EXPECT_TRUE(Config::HasCurrentConsent(Config::Read()));
    EXPECT_FALSE(controller.IsOnboardingVisible());
}

TEST_F(RmlUiControllerStateTest, AccountLinkPrompt_EligibleExistingUserShowsMenuAndPersistsPromptSeen) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = false;
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
    },
                   true);

    OmniNet::SetTransportForTesting([](const OmniNet::Request&) { return OmniNet::Response{}; });
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    EXPECT_TRUE(state->ui.showMenu.load());
    EXPECT_TRUE(controller.WantsAttention());
    EXPECT_TRUE(controller.WantsInteraction());
    EXPECT_TRUE(Config::Read().account_link_prompt_shown);

    controller.Update(Config::Read(), false);
    controller.Render();

    auto* root = SettingsRoot(controller);
    ASSERT_NE(root, nullptr);
    EXPECT_NE(root->QuerySelector("[data-action='account-sign-in']"), nullptr);
}

TEST_F(RmlUiControllerStateTest, AccountLinkPrompt_AlreadyLinkedUserIsNotPrompted) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = false;
        c.account_refresh_token = "mock_refresh_token";
        c.account_signed_in_name = "PlayerOne";
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    EXPECT_FALSE(state->ui.showMenu.load());
    EXPECT_FALSE(controller.WantsAttention());
    EXPECT_FALSE(Config::Read().account_link_prompt_shown);
}

TEST_F(RmlUiControllerStateTest, AccountLinkPrompt_AlreadySeenPromptIsNotPromptedAgain) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = true;
        c.account_refresh_token.clear();
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    EXPECT_FALSE(state->ui.showMenu.load());
    EXPECT_FALSE(controller.WantsAttention());
}

TEST_F(RmlUiControllerStateTest, AccountLinkPromptWaitsForMigrationAndOffersRecoveryOnFailure) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    auto account = std::make_shared<AccountClient>();
    std::promise<void> release;
    bool released = false;
    const auto gate = release.get_future().share();
    struct Cleanup {
        std::shared_ptr<AccountClient> account;
        std::promise<void>& release;
        bool& released;
        ~Cleanup() {
            if (!released) release.set_value();
            OmniNet::SetTransportForTesting({});
        }
    } cleanup{account, release, released};
    OmniNet::SetTransportForTesting([gate](const OmniNet::Request&) {
        gate.wait();
        OmniNet::Response res;
        res.status = 503;
        res.body = "{}";
        return res;
    });
    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = false;
        c.account_refresh_token.clear();
        c.custom_api_key = "oms_legacy_key";
    });
    account->SyncFromConfig();
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr, nullptr, nullptr, nullptr, nullptr, account);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    EXPECT_FALSE(state->ui.showMenu.load());
    EXPECT_FALSE(Config::Read().account_link_prompt_shown);
    state->ui.showMenu.store(true);
    SetSettingsPage(controller, 4);
    controller.Update(Config::Read(), false);
    controller.Render();
    auto* root = SettingsRoot(controller);
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->QuerySelector("[data-action='account-sign-in']"), nullptr);
    state->ui.showMenu.store(false);
    release.set_value();
    released = true;
    for (int i = 0; i < 700 && !state->ui.showMenu.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        controller.Update(Config::Read(), false);
    }
    EXPECT_TRUE(state->ui.showMenu.load());
    EXPECT_TRUE(Config::Read().account_link_prompt_shown);
    controller.Render();
    root = SettingsRoot(controller);
    ASSERT_NE(root, nullptr);
    EXPECT_NE(root->QuerySelector("[data-action='account-retry-migration']"), nullptr);
    EXPECT_NE(root->QuerySelector("[data-action='account-sign-in']"), nullptr);
}

TEST_F(RmlUiControllerStateTest, RejectedSessionPromptsUntilUserStartsSignIn) {
    WarpDevice warp;
    if (!warp.Create(1280, 900)) GTEST_SKIP() << "WARP not available.";
    struct Cleanup {
        ~Cleanup() {
            OmniNet::SetTransportForTesting({});
        }
    } cleanup;
    OmniNet::SetTransportForTesting([](const OmniNet::Request& request) {
        OmniNet::Response response;
        if (request.op == OmniNet::Op::AuthRefresh) {
            response.status = 401;
        } else if (request.op == OmniNet::Op::AuthStart) {
            response.status = 200;
            response.body = R"({"device_code":"dev","user_code":"CODE-123","verification_uri":"/device?user_code=CODE-123","expires_in":900,"poll_interval":5})";
        } else {
            response.status = 428;
            response.body = R"({"error":{"code":"authorization_pending","message":"Pending"}})";
        }
        return response;
    });
    Config::Update([](ConfigData& c) {
        c.second_monitor_mode = true;
        c.custom_api_enabled = true;
        c.enable_mmr_tracking = false;
        c.account_refresh_token = "expired-refresh";
        c.account_signed_in_name = "Expired";
    },
                   true);
    auto account = std::make_shared<AccountClient>();
    account->SetOpenUrlForTests([](const std::string&) {});
    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr, nullptr, nullptr, nullptr, nullptr, account);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 900, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(PromptRoot(controller)->QuerySelector("[data-action='session-sign-in']"), nullptr);

    ASSERT_FALSE(account->Refresh());
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_TRUE(controller.WantsAttention());
    ASSERT_NE(PromptRoot(controller)->QuerySelector("[data-action='session-sign-in']"), nullptr);
    CaptureKickoffView(warp, controller, "session-expired-prompt.png");

    auto* notNow = PromptRoot(controller)->QuerySelector("[data-action='session-prompt-close']");
    ASSERT_NE(notNow, nullptr);
    Click(controller, notNow);
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(PromptRoot(controller)->QuerySelector(".prompt-card"), nullptr);
    auto* hintSignIn = DashboardRoot(controller)->QuerySelector(".rank-source-hint [data-action='session-sign-in']");
    ASSERT_NE(hintSignIn, nullptr);
    CaptureKickoffView(warp, controller, "session-expired-hint.png");

    Click(controller, hintSignIn);
    EXPECT_TRUE(state->ui.showMenu.load());
    for (int i = 0; i < 100 && account->GetStatus().state != AccountAuthState::AwaitingApproval; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(account->GetStatus().state, AccountAuthState::AwaitingApproval);
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(DashboardRoot(controller)->GetInnerRML().find("session expired"), std::string::npos);
    account->CancelAuthorization();
    account->Shutdown();
}

TEST_F(RmlUiControllerStateTest, OnboardingWizard_StepsTransitionAndSkip) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    OmniNet::SetTransportForTesting([](const OmniNet::Request&) { return OmniNet::Response{}; });
    bool browserOpened = false;
    auto account = std::make_shared<AccountClient>();
    account->SetOpenUrlForTests([&](const std::string&) { browserOpened = true; });

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = false;
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.custom_api_key.clear();
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr, nullptr, nullptr, nullptr, nullptr, account);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    auto& onboarding = OnboardingModel(controller);
    EXPECT_EQ(onboarding.Step(), 0);

    Rml::ElementList nextButtons;
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 1);

    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 2);

    EXPECT_EQ(account->GetStatus().state, AccountAuthState::SignedOut);
    EXPECT_FALSE(browserOpened);
    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());

    EXPECT_EQ(onboarding.Step(), 3);
    EXPECT_EQ(account->GetStatus().state, AccountAuthState::SignedOut);
    EXPECT_FALSE(browserOpened);

    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 4);

    Rml::ElementList backButtons;
    OnboardingDocument(controller)->QuerySelectorAll(backButtons, "[data-action='onboarding-back']");
    ASSERT_FALSE(backButtons.empty());
    Click(controller, backButtons.front());
    EXPECT_EQ(onboarding.Step(), 3);

    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 4);

    Rml::ElementList finishButtons;
    OnboardingDocument(controller)->QuerySelectorAll(finishButtons, "[data-action='onboarding-finish']");
    ASSERT_FALSE(finishButtons.empty());
    Click(controller, finishButtons.front());
    EXPECT_TRUE(Config::Read().onboarding_completed);
    EXPECT_FALSE(controller.WantsAttention());
    OmniNet::SetTransportForTesting({});
}

TEST_F(RmlUiControllerStateTest, OnboardingWizard_StubBuildHasFourStepsAndSkipsLiveRanks) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    if (OmniNetBackend::Available()) GTEST_SKIP() << "Covers source builds that link the public stub.";
    OmniNet::SetTransportForTesting({});

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = false;
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.custom_api_key.clear();
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    controller.Update(Config::Read(), false);
    controller.Render();

    auto& onboarding = OnboardingModel(controller);
    EXPECT_EQ(onboarding.Step(), 0);
    EXPECT_EQ(OnboardingView::StepCount(), 4);

    Rml::ElementList nextButtons;
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 1);

    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 2);

    nextButtons.clear();
    OnboardingDocument(controller)->QuerySelectorAll(nextButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(nextButtons.empty());
    Click(controller, nextButtons.front());
    EXPECT_EQ(onboarding.Step(), 3);

    Rml::ElementList finishButtons;
    OnboardingDocument(controller)->QuerySelectorAll(finishButtons, "[data-action='onboarding-finish']");
    ASSERT_FALSE(finishButtons.empty());
    Click(controller, finishButtons.front());
    EXPECT_TRUE(Config::Read().onboarding_completed);
}

TEST_F(RmlUiControllerStateTest, OnboardingWizard_ConnectLiveRanks_FlowAndFallback) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto account = std::make_shared<AccountClient>();
    account->SetOpenUrlForTests([](const std::string&) {});
    OmniNet::SetTransportForTesting([](const OmniNet::Request& omniReq) {
        OmniNet::Response resp;
        if (omniReq.op == OmniNet::Op::AuthStart) {
            resp.status = 200;
            resp.body = R"({
                "device_code": "dev-code-test",
                "user_code": "FALLBACK-123",
                "verification_uri": "/device?user_code=FALLBACK-123",
                "expires_in": 900,
                "poll_interval": 5
            })";
            return resp;
        }
        resp.status = 428;
        resp.body = R"({"error":{"code":"authorization_pending","message":"Pending"}})";
        return resp;
    });

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = false;
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.custom_api_key.clear();
    },
                   true);

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr, nullptr, nullptr, nullptr, nullptr, account);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    auto& onboarding = OnboardingModel(controller);
    onboarding.SetStep(2);
    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_EQ(onboarding.Step(), 2);
    EXPECT_FALSE(onboarding.ShowFallback());

    Rml::ElementList signInButtons;
    OnboardingDocument(controller)->QuerySelectorAll(signInButtons, "[data-action='account-sign-in']");
    ASSERT_FALSE(signInButtons.empty());
    Click(controller, signInButtons.front());

    for (int i = 0; i < 200 && account->GetStatus().state != AccountAuthState::AwaitingApproval; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(account->GetStatus().state, AccountAuthState::AwaitingApproval);

    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_FALSE(onboarding.ShowFallback());

    Rml::ElementList fallbackButtons;
    OnboardingDocument(controller)->QuerySelectorAll(fallbackButtons, "[data-action='account-toggle-fallback']");
    ASSERT_FALSE(fallbackButtons.empty());
    Click(controller, fallbackButtons.front());

    EXPECT_TRUE(onboarding.ShowFallback());

    Rml::ElementList cancelButtons;
    OnboardingDocument(controller)->QuerySelectorAll(cancelButtons, "[data-action='account-cancel']");
    ASSERT_FALSE(cancelButtons.empty());
    Click(controller, cancelButtons.front());

    EXPECT_EQ(account->GetStatus().state, AccountAuthState::SignedOut);
    EXPECT_FALSE(onboarding.ShowFallback());
    OmniNet::SetTransportForTesting({});
}

TEST_F(RmlUiControllerStateTest, OnboardingWizard_ConnectLiveRanks_SuccessAndExplicitContinue) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";

    auto account = std::make_shared<AccountClient>();

    Config::Update([](ConfigData& c) {
        c.onboarding_completed = false;
        c.account_refresh_token = "mock_token";
        c.account_signed_in_name = "SpeedyPilot";
        c.custom_api_enabled = true;
    },
                   true);
    account->SyncFromConfig();
    EXPECT_TRUE(account->IsSignedIn());

    auto state = std::make_shared<SessionState>();
    RmlUiController controller(state, nullptr, nullptr, nullptr, nullptr, nullptr, account);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));

    auto& onboarding = OnboardingModel(controller);
    onboarding.SetStep(2);
    controller.Update(Config::Read(), false);
    controller.Render();

    EXPECT_EQ(onboarding.Step(), 2);

    Rml::ElementList continueButtons;
    OnboardingDocument(controller)->QuerySelectorAll(continueButtons, "[data-action='onboarding-next']");
    ASSERT_FALSE(continueButtons.empty());
    Click(controller, continueButtons.front());

    EXPECT_EQ(onboarding.Step(), 3);
}

TEST_F(RmlUiControllerStateTest, AccountMenuShowsCapturedNamesAndKeepsIdentityValues) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    const std::string savedId = "Epic|0123456789abcdef0123456789abcdef|0";
    const std::string liveId = "Steam|76561198000000001|0";
    Config::Update([&](ConfigData& c) {
        c.last_primary_id = savedId;
        c.known_primary_ids = {savedId, liveId};
    },
                   false);
    auto state = std::make_shared<SessionState>();
    auto db = std::make_shared<DatabaseManager>(state);
    ASSERT_TRUE(db->Initialize(":memory:"));
    auto worker = std::make_shared<WorkQueue>();
    auto accounts = std::make_shared<AccountManagerService>(db, nullptr, nullptr, worker);
    accounts->Start();
    worker->Drain();
    accounts->ObserveLocalAccount(savedId, "Saved <Player>");
    accounts->ObserveLocalAccount(liveId, "Old name");
    worker->Drain();
    accounts->Stop();

    worker = std::make_shared<WorkQueue>();
    accounts = std::make_shared<AccountManagerService>(db, nullptr, nullptr, worker);
    accounts->Start();
    worker->Drain();
    state->liveMatch->Update([&](MatchState& ms) {
        PlayerData player;
        player.primaryId = liveId;
        player.name = "Current player";
        ms.roster[liveId] = player;
    });
    RmlUiController controller(state, nullptr, accounts);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    SetSettingsPage(controller, 4);
    state->ui.showMenu.store(true);
    controller.Update(Config::Read());
    controller.Render();

    const auto choices = IdentityChoices(controller);
    ASSERT_EQ(choices.size(), 3u);
    EXPECT_EQ(choices[0].label, "Auto-detect");
    EXPECT_EQ(choices[1].value, savedId);
    EXPECT_EQ(choices[1].label, "Saved <Player>");
    EXPECT_EQ(choices[2].value, liveId);
    EXPECT_EQ(choices[2].label, "Current player");
    EXPECT_NE(SettingsRoot(controller)->GetInnerRML().find("Saved &lt;Player&gt;"), std::string::npos);
    auto* choose = SettingsRoot(controller)->QuerySelector("[data-action='accounts-open']");
    ASSERT_NE(choose, nullptr);
    Click(controller, choose);
    controller.Update(Config::Read(), false);
    controller.Render();

    auto* use = AccountsDocument(controller)->QuerySelector("[data-action='accounts-use'][data-id='Steam|76561198000000001']");
    ASSERT_NE(use, nullptr);
    ASSERT_TRUE(use->IsVisible());
    Click(controller, use);
    controller.Update(Config::Read(), false);
    controller.Render();
    EXPECT_EQ(Config::Read().last_primary_id, liveId);
    EXPECT_EQ(state->liveMatch->GetSnapshot()->myPrimaryId, liveId);
    auto* active = AccountsDocument(controller)->QuerySelector("[data-action='accounts-open-detail'][data-id='Steam|76561198000000001']");
    ASSERT_NE(active, nullptr);
    EXPECT_TRUE(active->QuerySelector(".accounts-active-label")->IsVisible());
    EXPECT_FALSE(active->QuerySelector("[data-action='accounts-use']")->IsVisible());
    EXPECT_NE(SettingsRoot(controller)->GetInnerRML().find("Current player"), std::string::npos);

    accounts->ObserveLocalAccount(savedId, "Renamed player");
    controller.Update(Config::Read(), false);
    EXPECT_EQ(IdentityChoices(controller)[1].label, "Renamed player");
    auto* autoDetect = AccountsDocument(controller)->QuerySelector("[data-action='accounts-auto-detect']");
    ASSERT_NE(autoDetect, nullptr);
    Click(controller, autoDetect);
    controller.Update(Config::Read(), false);
    EXPECT_TRUE(Config::Read().last_primary_id.empty());
    EXPECT_TRUE(state->liveMatch->GetSnapshot()->myPrimaryId.empty());
    worker->Drain();
}
