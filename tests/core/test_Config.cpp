#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include <fstream>
#include <filesystem>
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
