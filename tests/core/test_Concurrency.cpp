#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Storage.hpp"
#include "core/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include <thread>
#include <vector>
#include <atomic>

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
            {
                std::unique_lock<std::shared_mutex> lock(session->game.mutex);
                std::string id = "steam|" + std::to_string(i % 10);
                session->game.roster[id] = PlayerData{
                    .primaryId = id,
                    .name = "Player" + std::to_string(i % 10),
                    .team = i % 2
                };
            }
            std::this_thread::yield();
            i++;
        }
    });

    // Reader threads
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&running, session]() {
            while (running) {
                std::shared_lock<std::shared_mutex> lock(session->game.mutex);
                for (const auto& [id, p] : session->game.roster) {
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
