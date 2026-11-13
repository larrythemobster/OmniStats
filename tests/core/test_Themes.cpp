#include "core/config/Config.hpp"
#include "core/config/Themes.hpp"
#include "utils/Storage.hpp"
#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>

TEST(Themes, ShareCodeRoundTripsAndRejectsInvalidInputAtomically) {
    auto palette = Themes::Presets().back().colors;
    const std::string code = Themes::Encode(palette);
    Themes::Palette decoded{};
    std::string error;
    ASSERT_TRUE(Themes::Decode(code, decoded, error));
    EXPECT_EQ(Themes::Encode(decoded), code);
    const auto before = decoded;
    for (const auto& invalid : {std::string("omni-theme:v3:") + code.substr(14), code.substr(0, code.size() - 1), code + "0", code.substr(0, 20) + "!" + code.substr(21), std::string(10000, 'a')}) {
        EXPECT_FALSE(Themes::Decode(invalid, decoded, error));
        EXPECT_FALSE(error.empty());
        EXPECT_TRUE(Themes::Equal(decoded, before));
    }
    palette[0].r = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(Themes::Encode(palette).empty());
    palette[0].r = 1.01f;
    EXPECT_TRUE(Themes::Encode(palette).empty());
}

TEST(Themes, LinkedColorsChangeOnlyAfterLeaderEdit) {
    Themes::Palette distinct{};
    for (size_t i = 0; i < distinct.size(); ++i) {
        const float value = static_cast<float>(i + 1) / 40.0f;
        distinct[i] = {value, value / 2, value / 3, value / 4};
    }
    ConfigData config;
    Themes::Apply(config, distinct);
    EXPECT_TRUE(Themes::Equal(Themes::Read(config), distinct));
    const auto code = Themes::Encode(distinct);
    Themes::Palette decoded{};
    std::string error;
    ASSERT_TRUE(Themes::Decode(code, decoded, error));
    EXPECT_EQ(Themes::Encode(decoded), code);
    for (size_t i = 1; i < decoded.size(); ++i)
        EXPECT_NE(decoded[i].r, decoded[0].r);

    const auto checkFamily = [&](std::string_view leader, std::initializer_list<size_t> family) {
        auto edited = distinct;
        const auto leaderIndex = *family.begin();
        edited[leaderIndex] = {0.91f, 0.72f, 0.53f, 0.34f};
        auto expected = distinct;
        for (const auto index : family)
            expected[index] = edited[leaderIndex];
        Themes::LinkColor(edited, leader);
        EXPECT_TRUE(Themes::Equal(edited, expected)) << leader;
    };
    checkFamily("theme_bg", {0, 1, 2, 3});
    checkFamily("theme_roster_card", {4, 5, 7, 8, 9, 32});

    auto unchanged = distinct;
    Themes::LinkColor(unchanged, "theme_panel");
    Themes::LinkColor(unchanged, "unknown");
    EXPECT_TRUE(Themes::Equal(unchanged, distinct));
}

TEST(Themes, PresetsAndContrast) {
    EXPECT_TRUE(Themes::Equal(Themes::Read(ConfigData{}), Themes::Presets()[0].colors));
    for (size_t i = 1; i < Themes::Presets().size(); ++i) {
        ConfigData c;
        Themes::Apply(c, Themes::Presets()[i].colors);
        EXPECT_TRUE(Themes::Warnings(c).empty()) << Themes::Presets()[i].name;
    }
    for (const auto& preset : Themes::Presets().subspan(1)) {
        const auto& p = preset.colors;
        for (size_t surface : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u})
            EXPECT_GE(Themes::Contrast(p[10], p[surface]), 4.5) << preset.name << " " << surface;
        for (size_t background : {18u, 20u, 21u, 23u, 25u, 34u})
            EXPECT_GE(Themes::Contrast(p[22], p[background]), 4.5) << preset.name << " " << background;
        EXPECT_GE(Themes::Contrast(p[28], p[26]), 4.5) << preset.name;
    }
    EXPECT_NEAR(Themes::Contrast({0, 0, 0, 1}, {1, 1, 1, 1}), 21.0, 0.001);
    const auto composite = Themes::Composite({1, 1, 1, 0.5f}, {0, 0, 0, 1});
    EXPECT_FLOAT_EQ(composite.r, 0.5f);
    EXPECT_FLOAT_EQ(composite.a, 1.0f);
}

TEST(Themes, LegacyShareCodeRetainsOriginalColorsAndDerivesRoles) {
    const auto original = Themes::Presets()[2].colors;
    const auto v2 = Themes::Encode(original);
    const std::string v1 = "omni-theme:v1:" + v2.substr(14, 18 * 8);
    Themes::Palette decoded{};
    std::string error;
    ASSERT_TRUE(Themes::Decode(v1, decoded, error));
    EXPECT_EQ(Themes::Encode(decoded).substr(14, 18 * 8), v1.substr(14));
    EXPECT_FLOAT_EQ(decoded[18].r, decoded[11].r * 0.62f);
    EXPECT_FLOAT_EQ(decoded[19].g, decoded[11].g);
    EXPECT_FLOAT_EQ(decoded[30].b, decoded[11].b * 0.28f);
    EXPECT_FLOAT_EQ(decoded[6].r, original[6].r);
}

TEST(Themes, ShareCodePreservesIndependentControlStates) {
    ConfigData config;
    config.themePrimaryHover = {0.1f, 0.2f, 0.3f, 0.4f};
    config.themePrimaryPressed = {0.5f, 0.6f, 0.7f, 0.8f};
    config.themeButtonPressed = {0.9f, 0.8f, 0.7f, 0.6f};
    const auto code = Themes::Encode(Themes::Read(config));
    Themes::Palette decoded{};
    std::string error;
    ASSERT_TRUE(Themes::Decode(code, decoded, error));
    Themes::Apply(config, decoded);
    EXPECT_NEAR(config.themePrimaryHover.r, 0.1f, 1.0f / 255);
    EXPECT_NEAR(config.themePrimaryPressed.r, 0.5f, 1.0f / 255);
    EXPECT_NEAR(config.themeButtonPressed.r, 0.9f, 1.0f / 255);
    EXPECT_EQ(Themes::Encode(Themes::Read(config)), code);
}

class SavedThemesTest : public ::testing::Test {
  protected:
    ConfigData original;
    std::string path;
    void SetUp() override {
        Storage::InitializeEnvironment();
        original = Config::Read();
        path = Storage::GetDataDirectory() + "config.json";
    }
    void TearDown() override {
        Config::Update([&](ConfigData& c) { c = original; }, false);
        Config::Save();
    }
    nlohmann::json ReadFile() {
        std::ifstream file(path);
        return nlohmann::json::parse(file);
    }
    void WriteFile(const nlohmann::json& value) {
        std::ofstream file(path, std::ios::trunc);
        file << value.dump(4);
    }
};

TEST_F(SavedThemesTest, PersistsUnicodeAndExactChannels) {
    const Themes::SavedTheme expected{"\xe6\x98\x9f\xe7\xa9\xba", Themes::Presets()[0].colors};
    Config::Update([&](ConfigData& c) { c.custom_themes = {expected}; }, false);
    Config::Save();
    Config::Update([](ConfigData& c) { c.custom_themes.clear(); }, false);
    Config::Load();
    const auto saved = Config::Read().custom_themes;
    ASSERT_EQ(saved.size(), 1u);
    EXPECT_EQ(saved[0].name, expected.name);
    EXPECT_TRUE(Themes::Equal(saved[0].colors, expected.colors));
}

TEST_F(SavedThemesTest, InvalidSavedPaletteIsPreservedOnSave) {
    Config::Update([](ConfigData& c) { c.custom_themes.clear(); }, false);
    Config::Save();
    auto file = ReadFile();
    auto colors = nlohmann::json::array();
    for (int i = 0; i < 35; ++i)
        colors.push_back(nlohmann::json::array({0, 0, 0, 1}));
    auto incomplete = colors;
    incomplete.erase(incomplete.end() - 1);
    auto outOfRange = colors;
    outOfRange[4][0] = 1.01;
    auto notNumeric = colors;
    notNumeric[4][0] = nullptr;
    for (const auto& invalidColors : {incomplete, outOfRange, notNumeric}) {
        file["custom_themes"] = {{{"name", "broken"}, {"colors", invalidColors}}};
        const auto invalid = file["custom_themes"];
        WriteFile(file);
        Config::Load();
        EXPECT_TRUE(Config::Read().custom_themes.empty());
        Config::Save();
        EXPECT_EQ(ReadFile()["custom_themes"], invalid);
    }
    file["custom_themes"] = nlohmann::json::array();
    WriteFile(file);
    Config::Load();
}

TEST_F(SavedThemesTest, PersistsIndependentControlStates) {
    Config::Update([](ConfigData& c) {
        c.themePrimaryHover = {0.12f, 0.23f, 0.34f, 0.45f};
        c.themePrimaryPressed = {0.56f, 0.67f, 0.78f, 0.89f};
        c.themeButtonPressed = {0.91f, 0.82f, 0.73f, 0.64f};
    },
                   false);
    Config::Save();
    Config::Update([](ConfigData& c) { c.ResetThemeColors(); }, false);
    Config::Load();
    const auto c = Config::Read();
    EXPECT_FLOAT_EQ(c.themePrimaryHover.r, 0.12f);
    EXPECT_FLOAT_EQ(c.themePrimaryHover.a, 0.45f);
    EXPECT_FLOAT_EQ(c.themePrimaryPressed.r, 0.56f);
    EXPECT_FLOAT_EQ(c.themePrimaryPressed.a, 0.89f);
    EXPECT_FLOAT_EQ(c.themeButtonPressed.r, 0.91f);
    EXPECT_FLOAT_EQ(c.themeButtonPressed.a, 0.64f);
}
TEST_F(SavedThemesTest, MigratesLegacySavedAndActiveColorsWithoutLosingValues) {
    Config::Save();
    auto file = ReadFile();
    const auto source = Themes::Presets()[1].colors;
    auto legacy = nlohmann::json::array();
    for (size_t i = 0; i < 18; ++i) {
        const auto c = source[i];
        legacy.push_back({c.r, c.g, c.b, c.a});
    }
    file["custom_themes"] = {{{"name", "old"}, {"colors", legacy}}};
    auto& active = file["theme"];
    for (size_t i = 18; i < Themes::Fields().size(); ++i)
        active.erase(std::string(Themes::Fields()[i].key).substr(6));
    active["accent"] = {0.2f, 0.4f, 0.6f, 0.8f};
    active["scout_tag"] = {0.7f, 0.6f, 0.5f, 0.4f};
    active["primary_hover"] = {0.9f, 0.8f, 0.7f, 0.6f};
    WriteFile(file);
    Config::Load();
    const auto config = Config::Read();
    ASSERT_EQ(config.custom_themes.size(), 1u);
    EXPECT_FALSE(config.custom_themes_unreadable);
    for (size_t i = 0; i < 18; ++i) {
        EXPECT_FLOAT_EQ(config.custom_themes[0].colors[i].r, source[i].r);
        EXPECT_FLOAT_EQ(config.custom_themes[0].colors[i].a, source[i].a);
    }
    EXPECT_FLOAT_EQ(config.custom_themes[0].colors[18].r, source[11].r * 0.62f);
    EXPECT_FLOAT_EQ(config.themeAccent.g, 0.4f);
    EXPECT_FLOAT_EQ(config.themeScoutTag.r, 0.7f);
    EXPECT_FLOAT_EQ(config.themePrimaryButton.r, 0.2f * 0.62f);
    EXPECT_FLOAT_EQ(config.themePrimaryHover.r, 0.9f);
    Config::Save();
    const auto saved = ReadFile();
    EXPECT_EQ(saved["custom_themes"][0]["colors"].size(), 35u);
    EXPECT_FLOAT_EQ(saved["theme"]["primary_hover"][0].get<float>(), 0.9f);
    EXPECT_FLOAT_EQ(saved["theme"]["primary_button"][0].get<float>(), 0.2f * 0.62f);
}

TEST_F(SavedThemesTest, InvalidMemoryDoesNotOverwriteSavedFile) {
    Config::Update([](ConfigData& c) { c.custom_themes.clear(); }, false);
    Config::Save();
    const auto before = ReadFile();
    Config::Update([](ConfigData& c) { c.custom_themes = {{std::string(81, 'x'), Themes::Presets()[0].colors}}; }, false);
    Config::Save();
    EXPECT_EQ(ReadFile(), before);
}
