#include "RmlUiControllerStateFixture.hpp"

TEST_F(RmlUiControllerStateTest, ThemePreviewSurvivesConfigEchoAndOnlyApplyPersistsColors) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.ResetThemeColors(); c.custom_themes.clear(); c.second_monitor_mode = true; }, false);
    const auto saved = Themes::Read(Config::Read());
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& match) {
        match.myPrimaryId = "Steam|theme";
        match.myTeam = 0;
        PlayerData player;
        player.primaryId = "Steam|theme";
        player.name = "Player";
        player.team = 0;
        player.mmr = 1234;
        player.rankTier = "Champion II";
        player.playlists["2v2"] = player.mmr;
        player.playlistTiers["2v2"] = player.rankTier;
        match.roster[player.primaryId] = player;
    });
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1.0f));
    SetSettingsPage(controller, 2);
    controller.Update(Config::Read(), true);
    controller.Render();
    const auto action = [&](const char* selector) {
        auto* element = SettingsRoot(controller)->QuerySelector(selector);
        ASSERT_NE(element, nullptr) << selector;
        Click(controller, element);
        controller.Render();
    };
    action("[data-theme-id='light']");
    const auto light = PreviewTheme(controller);
    EXPECT_FALSE(Themes::Equal(light, saved));
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), saved));
    auto* rank = Document(controller)->QuerySelector(".chip-mmr");
    ASSERT_NE(rank, nullptr);
    const auto ink = rank->GetProperty<Rml::Colourb>("color");
    ConfigData lightConfig;
    Themes::Apply(lightConfig, light);
    EXPECT_GE(Themes::Contrast({ink.red / 255.0f, ink.green / 255.0f, ink.blue / 255.0f, 1}, lightConfig.themeSettingsPanel), 4.5);
    Config::Update([](ConfigData& c) { c.use_roman_numerals = !c.use_roman_numerals; }, false);
    controller.Update(Config::Read(), true);
    EXPECT_TRUE(Themes::Equal(PreviewTheme(controller), light));
    action("[data-action='theme-apply']");
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), light));
    action("[data-theme-id='purple']");
    action("[data-action='theme-revert']");
    EXPECT_TRUE(Themes::Equal(PreviewTheme(controller), light));
    action("[data-theme-id='green']");
    state->ui.showMenu.store(false);
    controller.Update(Config::Read(), false);
    EXPECT_TRUE(Themes::Equal(PreviewTheme(controller), light));
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), light));
}

TEST_F(RmlUiControllerStateTest, ThemeImportResetAndSavedThemesPreserveAppliedColors) {
    WarpDevice warp;
    if (!warp.Create(800, 600)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& c) { c.ResetThemeColors(); c.custom_themes.clear(); }, false);
    const auto applied = Themes::Read(Config::Read());
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 800, 600, 1.0f));
    SetSettingsPage(controller, 2);
    controller.Update(Config::Read(), true);
    controller.Render();
    const auto action = [&](const char* selector) {
        auto* element = SettingsRoot(controller)->QuerySelector(selector);
        ASSERT_NE(element, nullptr) << selector;
        Click(controller, element);
        controller.Render();
    };
    action("[data-theme-id='purple']");
    const auto importedCode = Themes::Encode(PreviewTheme(controller));
    action("[data-action='reset-theme-colors']");
    action("[data-action='theme-undo-reset']");
    EXPECT_EQ(Themes::Encode(PreviewTheme(controller)), importedCode);
    ThemeInput(controller, "theme_name", "My purple");
    action("[data-action='theme-save']");
    ASSERT_EQ(Config::Read().custom_themes.size(), 1u);
    ThemeInput(controller, "theme_name", "My purple");
    action("[data-action='theme-save']");
    EXPECT_EQ(Config::Read().custom_themes.size(), 1u);
    action("[data-action='theme-revert']");
    ThemeInput(controller, "theme_import", importedCode);
    action("[data-action='theme-import']");
    EXPECT_EQ(Themes::Encode(PreviewTheme(controller)), importedCode);
    ThemeInput(controller, "theme_import", "omni-theme:v2:garbage");
    action("[data-action='theme-import']");
    EXPECT_EQ(Themes::Encode(PreviewTheme(controller)), importedCode);
    action("[data-action='toggle-advanced-appearance']");
    action("[data-action='theme-toggle-group'][data-theme-group='Text']");
    ThemeInput(controller, "theme_text", "#xyz");
    action("[data-action='theme-apply']");
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), applied));
    auto* invalidInput = dynamic_cast<Rml::ElementFormControl*>(SettingsRoot(controller)->QuerySelector("[data-setting='theme_text']"));
    ASSERT_NE(invalidInput, nullptr);
    EXPECT_EQ(invalidInput->GetValue(), "#xyz");
    ThemeInput(controller, "theme_text", "#000000");
    auto* warning = SettingsRoot(controller)->QuerySelector("#theme-error-theme_text");
    ASSERT_NE(warning, nullptr);
    EXPECT_FALSE(warning->GetInnerRML().empty());
    auto* revert = SettingsRoot(controller)->QuerySelector("[data-action='theme-revert']");
    ASSERT_NE(revert, nullptr);
    const auto revertInk = revert->GetProperty<Rml::Colourb>("color");
    EXPECT_GE(Themes::Contrast({revertInk.red / 255.0f, revertInk.green / 255.0f, revertInk.blue / 255.0f, 1},
                               Config::Read().themeSettingsPanel),
              4.5);
    ThemeInput(controller, "theme_accent", "#ffff00");
    action("[data-action='theme-apply']");
    EXPECT_FLOAT_EQ(Config::Read().themeAccent.r, 1);
    EXPECT_FLOAT_EQ(Config::Read().themeScoutTag.r, 0.55f);
    action("[data-action='theme-delete'][data-theme-index='0']");
    EXPECT_EQ(Config::Read().custom_themes.size(), 1u);
    action("[data-action='theme-delete'][data-theme-index='0']");
    EXPECT_TRUE(Config::Read().custom_themes.empty());
}

TEST_F(RmlUiControllerStateTest, StandardButtonColorsAndIndependentControlStatesArePreserved) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& config) { config.ResetThemeColors(); }, false);
    auto state = std::make_shared<SessionState>();
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1));
    SetSettingsPage(controller, 2);
    controller.Update(Config::Read(), true);
    auto probes = Document(controller)->CreateElement("div");
    probes->SetInnerRML("<button id='primary-probe' class='primary'>Primary</button>"
                        "<button id='secondary-probe'>Secondary</button>"
                        "<input id='input-probe' class='text' value='Text'/>"
                        "<div id='panel-probe' class='card'>Panel</div>");
    Document(controller)->AppendChild(std::move(probes));
    controller.Render();
    const auto color = [&](const char* id, const char* property) {
        return Document(controller)->GetElementById(id)->GetProperty<Rml::Colourb>(property);
    };
    EXPECT_EQ(color("primary-probe", "background-color"), Rml::Colourb(73, 0, 0, 255));
    EXPECT_EQ(color("primary-probe", "border-top-color"), Rml::Colourb(118, 0, 0, 255));
    EXPECT_EQ(color("secondary-probe", "background-color"), Rml::Colourb(37, 37, 37, 255));
    EXPECT_EQ(color("secondary-probe", "border-top-color"), Rml::Colourb(57, 57, 57, 255));
    EXPECT_EQ(color("input-probe", "background-color"), Rml::Colourb(18, 18, 18, 255));
    EXPECT_EQ(color("panel-probe", "border-top-color"), Rml::Colourb(38, 38, 38, 128));
    auto* primary = Document(controller)->GetElementById("primary-probe");
    primary->SetPseudoClass("hover", true);
    controller.Render();
    EXPECT_EQ(color("primary-probe", "background-color"), Rml::Colourb(73, 0, 0, 255));
    primary->SetPseudoClass("hover", false);
    ThemeInput(controller, "theme_primary_button", "#123456");
    ThemeInput(controller, "theme_primary_hover", "#245678");
    ThemeInput(controller, "theme_primary_pressed", "#102040");
    ThemeInput(controller, "theme_input_bg", "#203040");
    controller.Render();
    EXPECT_EQ(color("primary-probe", "background-color"), Rml::Colourb(18, 52, 86, 255));
    EXPECT_EQ(color("secondary-probe", "background-color"), Rml::Colourb(37, 37, 37, 255));
    EXPECT_EQ(color("input-probe", "background-color"), Rml::Colourb(32, 48, 64, 255));
    ThemeInput(controller, "theme_button_bg", "#274156");
    ThemeInput(controller, "theme_border", "#ff0000");
    controller.Render();
    EXPECT_EQ(color("secondary-probe", "background-color"), Rml::Colourb(39, 65, 86, 255));
    const auto border = color("panel-probe", "border-top-color");
    EXPECT_GT(border.red, 0);
    EXPECT_EQ(border.green, 0);
    EXPECT_EQ(border.blue, 0);
    EXPECT_EQ(border.alpha, 255);
    primary->SetPseudoClass("hover", true);
    controller.Render();
    EXPECT_EQ(color("primary-probe", "background-color"), Rml::Colourb(36, 86, 120, 255));
    primary->SetPseudoClass("active", true);
    controller.Render();
    EXPECT_EQ(color("primary-probe", "background-color"), Rml::Colourb(16, 32, 64, 255));
    EXPECT_FLOAT_EQ(Config::Read().themePrimaryButton.r, ConfigData{}.themePrimaryButton.r);
    Click(controller, SettingsRoot(controller)->QuerySelector("[data-action='theme-revert']"));
    Config::Update([](ConfigData& config) { config.themeInputBg = {0.2f, 0.3f, 0.4f, 1}; }, false);
    controller.Update(Config::Read(), true);
    controller.Render();
    EXPECT_EQ(color("input-probe", "background-color"), Rml::Colourb(51, 77, 102, 255));
}

TEST_F(RmlUiControllerStateTest, AdvancedColorTabsStayHorizontalAndHoverDoesNotDimTheWindow) {
    for (const auto [width, height] : {std::pair{1280, 800}, std::pair{800, 600}}) {
        WarpDevice warp;
        if (!warp.Create(width, height)) GTEST_SKIP() << "WARP not available.";
        Config::Update([](ConfigData& config) {
            config.ResetThemeColors();
            config.ui_scale = 1;
            config.second_monitor_mode = true;
        },
                       false);
        auto state = std::make_shared<SessionState>();
        state->ui.showMenu.store(true);
        RmlUiController controller(state, nullptr);
        ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), width, height, 1));
        SetSettingsPage(controller, 2);
        controller.Update(Config::Read(), true);
        controller.Render();
        Click(controller, SettingsRoot(controller)->QuerySelector("[data-action='toggle-advanced-appearance']"));
        controller.Render();
        auto* tabs = SettingsRoot(controller)->QuerySelector(".theme-group-tabs");
        ASSERT_NE(tabs, nullptr);
        tabs->ScrollIntoView(true);
        controller.Render();
        const auto origin = tabs->GetAbsoluteOffset(Rml::BoxArea::Border);
        float previousRight = origin.x;
        for (const char* group : {"Surfaces", "Text", "Results", "Graph", "Controls"}) {
            auto* tab = tabs->QuerySelector(std::string("[data-theme-group='") + group + "']");
            ASSERT_NE(tab, nullptr) << group;
            const auto offset = tab->GetAbsoluteOffset(Rml::BoxArea::Border);
            EXPECT_NEAR(offset.y, origin.y, 1) << width << ": " << group;
            EXPECT_GE(offset.x + 0.5f, previousRight) << width << ": " << group;
            EXPECT_GE(tab->GetOffsetHeight(), 40) << width << ": " << group;
            EXPECT_LE(offset.x + tab->GetOffsetWidth(), origin.x + tabs->GetOffsetWidth() + 0.5f) << width << ": " << group;
            previousRight = offset.x + tab->GetOffsetWidth();
        }
        auto* window = Document(controller)->QuerySelector(".dashboard-shell");
        ASSERT_NE(window, nullptr);
        const float opacity = window->GetProperty<float>("opacity");
        const auto border = window->GetProperty<Rml::Colourb>("border-top-color");
        auto* swatch = SettingsRoot(controller)->QuerySelector("#theme-swatch-theme_bg");
        ASSERT_NE(swatch, nullptr);
        const auto offset = swatch->GetAbsoluteOffset(Rml::BoxArea::Border);
        RmlContext(controller)->ProcessMouseMove(static_cast<int>(offset.x + swatch->GetOffsetWidth() / 2), static_cast<int>(offset.y + swatch->GetOffsetHeight() / 2), 0);
        controller.Render();
        EXPECT_FLOAT_EQ(window->GetProperty<float>("opacity"), opacity);
        EXPECT_EQ(window->GetProperty<Rml::Colourb>("border-top-color"), border);
    }
}

TEST_F(RmlUiControllerStateTest, SharedSurfacesFollowHexSlidersAndPickerWithoutChangingSavedColors) {
    WarpDevice warp;
    if (!warp.Create(1280, 800)) GTEST_SKIP() << "WARP not available.";
    Config::Update([](ConfigData& config) {
        config.ResetThemeColors();
        config.ui_scale = 1;
        config.second_monitor_mode = true;
    },
                   false);
    const auto saved = Themes::Read(Config::Read());
    auto state = std::make_shared<SessionState>();
    state->liveMatch->Update([](MatchState& match) {
        match.myPrimaryId = "Steam|theme";
        match.myTeam = 0;
        PlayerData player;
        player.primaryId = match.myPrimaryId;
        player.name = "Player";
        player.team = 0;
        player.mmr = 1234;
        player.rankTier = "Champion II";
        player.playlists["2v2"] = player.mmr;
        player.playlistTiers["2v2"] = player.rankTier;
        match.roster[player.primaryId] = player;
    });
    state->ui.showMenu.store(true);
    RmlUiController controller(state, nullptr);
    ASSERT_TRUE(controller.Initialize(nullptr, warp.device.Get(), warp.context.Get(), 1280, 800, 1));
    SetSettingsPage(controller, 2);
    controller.Update(Config::Read(), true);
    controller.Render();
    Click(controller, SettingsRoot(controller)->QuerySelector("[data-action='toggle-advanced-appearance']"));
    controller.Render();
    ThemeInput(controller, "theme_bg", "#15293d");
    ThemeInput(controller, "theme_roster_card", "#25374999");
    controller.Render();
    auto* panel = Document(controller)->QuerySelector(".settings-window");
    auto* player = Document(controller)->QuerySelector(".player-row.self");
    ASSERT_NE(panel, nullptr);
    ASSERT_NE(player, nullptr);
    EXPECT_EQ(panel->GetProperty<Rml::Colourb>("background-color"), Rml::Colourb(21, 41, 61, 255));
    EXPECT_EQ(player->GetProperty<Rml::Colourb>("background-color"), Rml::Colourb(37, 55, 73, 153));
    Click(controller, SettingsRoot(controller)->QuerySelector("#theme-swatch-theme_roster_card"));
    controller.Render();
    ThemeInput(controller, "theme_roster_card:r", "96");
    auto* slider = SettingsRoot(controller)->QuerySelector("#theme-color-r");
    ASSERT_NE(slider, nullptr);
    slider->DispatchEvent("blur", Rml::Dictionary{});
    controller.Render();
    EXPECT_EQ(player->GetProperty<Rml::Colourb>("background-color"), Rml::Colourb(96, 55, 73, 153));
    const auto beforePick = PreviewTheme(controller);
    auto* field = SettingsRoot(controller)->QuerySelector("#theme-color-field");
    ASSERT_NE(field, nullptr);
    field->ScrollIntoView(true);
    controller.Render();
    const auto offset = field->GetAbsoluteOffset();
    RmlContext(controller)->ProcessMouseMove(static_cast<int>(offset.x + field->GetClientWidth() * 0.45f), static_cast<int>(offset.y + field->GetClientHeight() * 0.55f), 0);
    RmlContext(controller)->ProcessMouseButtonDown(0, 0);
    RmlContext(controller)->ProcessMouseButtonUp(0, 0);
    controller.Render();
    const auto picked = PreviewTheme(controller);
    EXPECT_NE(picked[4].r, beforePick[4].r);
    for (size_t index : {5u, 7u, 8u, 9u, 32u}) {
        EXPECT_FLOAT_EQ(picked[index].r, picked[4].r);
        EXPECT_FLOAT_EQ(picked[index].g, picked[4].g);
        EXPECT_FLOAT_EQ(picked[index].b, picked[4].b);
        EXPECT_FLOAT_EQ(picked[index].a, 153.0f / 255);
    }
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), saved));
    Click(controller, SettingsRoot(controller)->QuerySelector("[data-action='theme-apply']"));
    EXPECT_TRUE(Themes::Equal(Themes::Read(Config::Read()), picked));
}
