#include <gtest/gtest.h>
#include "core/config/Config.hpp"
#include "utils/Storage.hpp"
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "database/DatabaseManager.hpp"
#include <thread>
#include <vector>
#include <atomic>

TEST(HistoryStoreTest, MutationPublishesSnapshotAndPreservesPreviousSnapshot) {
    database::store::HistoryStore history;
    const auto before = history.GetSnapshot();

    const int result = history.Mutate([](database::store::HistoryData& data) {
        data.initialMmr = 1200;
        data.playlistHistoryY["2v2"] = {1200.0f, 1210.0f};
        data.showLifetimeGraph = true;
        return data.initialMmr + 10;
    });

    const auto after = history.GetSnapshot();
    EXPECT_EQ(result, 1210);
    EXPECT_EQ(after->initialMmr, 1200);
    ASSERT_EQ(after->playlistHistoryY.at("2v2").size(), 2u);
    EXPECT_FLOAT_EQ(after->playlistHistoryY.at("2v2").back(), 1210.0f);
    EXPECT_TRUE(after->showLifetimeGraph);
    EXPECT_GT(after->version, before->version);
    EXPECT_EQ(before->initialMmr, -1);
    EXPECT_TRUE(before->playlistHistoryY.empty());
    EXPECT_FALSE(before->showLifetimeGraph);

    history.SetShowLifetimeGraph(false);
    const auto toggled = history.GetSnapshot();
    EXPECT_FALSE(toggled->showLifetimeGraph);
    EXPECT_GT(toggled->version, after->version);
    EXPECT_TRUE(after->showLifetimeGraph);
}

TEST(ConcurrencyTest, ConfigConcurrentReadWrite) {
    // Ensure data directory exists and set initial port in valid range
    Storage::InitializeEnvironment();
    Config::Update([](ConfigData& c) { c.port = 1000; });
    std::atomic<bool> running{true};

    // Writer thread
    std::thread writer([&running]() {
        int i = 0;
        while (running) {
            Config::Update([i](ConfigData& c) { c.port = 1000 + (i % 1000); });
            std::this_thread::yield();
            i++;
        }
    });

    // Reader threads
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&running]() {
            while (running) {
                ConfigData d = Config::Read();
                EXPECT_GE(d.port, 1000);
                EXPECT_LT(d.port, 2000);
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    writer.join();
    for (auto& r : readers) {
        r.join();
    }
}

TEST(ConcurrencyTest, SessionStateConcurrentRoster) {
    auto session = std::make_shared<SessionState>();
    std::atomic<bool> running{true};

    // Writer thread
    std::thread writer([&running, session]() {
        int i = 0;
        while (running) {
            std::string id = "steam|" + std::to_string(i % 10);
            session->liveMatch->UpdateMatchStateForTests([&](MatchState& ms) {
                ms.roster[id] = PlayerData{
                    .primaryId = id,
                    .name = "Player" + std::to_string(i % 10),
                    .team = i % 2};
            });
            std::this_thread::yield();
            i++;
        }
    });

    // Reader threads
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&running, session]() {
            while (running) {
                auto snap = session->liveMatch->GetSnapshot();
                for (const auto& [id, p] : snap->roster) {
                    EXPECT_FALSE(id.empty());
                    EXPECT_FALSE(p.name.empty());
                }
                std::this_thread::yield();
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    running = false;

    writer.join();
    for (auto& r : readers) {
        r.join();
    }
}
