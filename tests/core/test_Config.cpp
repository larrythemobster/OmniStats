#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <mutex>
#include <shared_mutex>

class ConfigTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Ensure the data directory exists (required on CI runners)
        Storage::InitializeEnvironment();
        // Backup current config
        original_config = Config::Read();
    }

    void TearDown() override {
        // Restore config
        Config::Update([this](ConfigData& c) { c = original_config; });
    }

    ConfigData original_config;
};

TEST_F(ConfigTest, ReadAndWriteState) {
    Config::Update([](ConfigData& c) {
        c.port = 9999;
        c.host = "127.0.0.99";
    });

    ConfigData readData = Config::Read();
    EXPECT_EQ(readData.port, 9999);
    EXPECT_EQ(readData.host, "127.0.0.99");
}

TEST_F(ConfigTest, SaveAndLoad) {
    Config::Update([](ConfigData& c) {
        c.port = 12345;
        c.host = "test_host";
    });
    Config::Save();

    // Modify memory to ensure load overwrites
    Config::Update([](ConfigData& c) {
        c.port = 0;
        c.host = "";
    },
                   false);

    Config::Load();
    ConfigData loadedData = Config::Read();
    EXPECT_EQ(loadedData.port, 12345);
    EXPECT_EQ(loadedData.host, "test_host");
}

TEST_F(ConfigTest, ConcurrencyReadUpdate) {
    std::atomic<bool> start{false};
    std::atomic<int> completed{0};

    auto reader = [&]() {
        while (!start) {
        }
        for (int i = 0; i < 1000; ++i) {
            ConfigData c = Config::Read();
            EXPECT_GE(c.port, 0);
        }
        completed++;
    };

    auto updater = [&]() {
        while (!start) {
        }
        for (int i = 0; i < 1000; ++i) {
            Config::Update([i](ConfigData& c) {
                c.port = i;
            },
                           false);
        }
        completed++;
    };

    std::thread t1(reader);
    std::thread t2(updater);
    std::thread t3(reader);

    start = true;
    t1.join();
    t2.join();
    t3.join();
    EXPECT_EQ(completed.load(), 3);
}

TEST_F(ConfigTest, LobbyRanksConfigVisibility) {
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = false;
        c.show_lobby_rank_2v2 = true;
        c.show_lobby_rank_3v3 = false;
        c.show_lobby_rank_casual = true;
        c.show_lobby_rank_tourny = false;
        c.show_lobby_rank_hoops = true;
        c.show_lobby_rank_rumble = false;
        c.show_lobby_rank_dropshot = true;
        c.show_lobby_rank_snowday = false;
        c.show_lobby_rank_heatseeker = true;
    });
    Config::Save();

    // Reset values in memory
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = true;
        c.show_lobby_rank_2v2 = false;
        c.show_lobby_rank_3v3 = true;
        c.show_lobby_rank_casual = false;
        c.show_lobby_rank_tourny = true;
        c.show_lobby_rank_hoops = false;
        c.show_lobby_rank_rumble = true;
        c.show_lobby_rank_dropshot = false;
        c.show_lobby_rank_snowday = true;
        c.show_lobby_rank_heatseeker = false;
    },
                   false);

    Config::Load();
    ConfigData loaded = Config::Read();
    EXPECT_FALSE(loaded.show_lobby_rank_1v1);
    EXPECT_TRUE(loaded.show_lobby_rank_2v2);
    EXPECT_FALSE(loaded.show_lobby_rank_3v3);
    EXPECT_TRUE(loaded.show_lobby_rank_casual);
    EXPECT_FALSE(loaded.show_lobby_rank_tourny);
    EXPECT_TRUE(loaded.show_lobby_rank_hoops);
    EXPECT_FALSE(loaded.show_lobby_rank_rumble);
    EXPECT_TRUE(loaded.show_lobby_rank_dropshot);
    EXPECT_FALSE(loaded.show_lobby_rank_snowday);
    EXPECT_TRUE(loaded.show_lobby_rank_heatseeker);
}

TEST_F(ConfigTest, LobbyRanksConfigFallback) {
    // Attempting to set all to false should trigger fallback to enable "2v2"
    Config::Update([](ConfigData& c) {
        c.show_lobby_rank_1v1 = false;
        c.show_lobby_rank_2v2 = false;
        c.show_lobby_rank_3v3 = false;
        c.show_lobby_rank_casual = false;
        c.show_lobby_rank_tourny = false;
        c.show_lobby_rank_hoops = false;
        c.show_lobby_rank_rumble = false;
        c.show_lobby_rank_dropshot = false;
        c.show_lobby_rank_snowday = false;
        c.show_lobby_rank_heatseeker = false;
    },
                   false);

    ConfigData loaded = Config::Read();
    EXPECT_TRUE(loaded.show_lobby_rank_2v2);
}

TEST_F(ConfigTest, MigratesLegacySharedMmrCategory) {
    const std::string configPath =
        Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream file(configPath);
        file << nlohmann::json{
            {"mmr_category", "3v3"},
            {"auto_switch_mmr_category", false},
            {"port", 49200}}
                    .dump(2);
    }

    Config::Load();
    const ConfigData migrated = Config::Read();
    EXPECT_EQ(migrated.mmr_category, "3v3");
    EXPECT_FALSE(migrated.auto_switch_mmr_category);
    EXPECT_EQ(migrated.graph_mmr_category, "3v3");
    EXPECT_FALSE(migrated.graph_follow_current_playlist);
    EXPECT_EQ(migrated.port, 49200);

    Config::Save();
    nlohmann::json saved;
    {
        std::ifstream file(configPath);
        file >> saved;
    }
    EXPECT_EQ(saved["mmr_category"], "3v3");
    EXPECT_EQ(saved["auto_switch_mmr_category"], false);
    EXPECT_EQ(saved["graph_mmr_category"], "3v3");
    EXPECT_EQ(saved["graph_follow_current_playlist"], false);
}

TEST_F(ConfigTest, PersistsIndependentMmrSettings) {
    Config::Update([](ConfigData& c) {
        c.mmr_category = "best";
        c.auto_switch_mmr_category = false;
        c.graph_mmr_category = "2v2";
        c.graph_follow_current_playlist = true;
    });
    Config::Save();

    Config::Update([](ConfigData& c) {
        c.mmr_category = "1v1";
        c.auto_switch_mmr_category = true;
        c.graph_mmr_category = "3v3";
        c.graph_follow_current_playlist = false;
    },
                   false);

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.mmr_category, "best");
    EXPECT_FALSE(loaded.auto_switch_mmr_category);
    EXPECT_EQ(loaded.graph_mmr_category, "2v2");
    EXPECT_TRUE(loaded.graph_follow_current_playlist);
}

TEST_F(ConfigTest, InvalidMmrCategoriesUseSafeDefaults) {
    const std::string configPath =
        Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream file(configPath);
        file << nlohmann::json{
            {"mmr_category", "not-a-playlist"},
            {"graph_mmr_category", "also-invalid"},
            {"graph_follow_current_playlist", true}}
                    .dump(2);
    }

    Config::Load();
    const ConfigData loaded = Config::Read();
    EXPECT_EQ(loaded.mmr_category, "best");
    EXPECT_EQ(loaded.graph_mmr_category, "2v2");
    EXPECT_TRUE(loaded.graph_follow_current_playlist);
}
