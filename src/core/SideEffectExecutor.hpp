#pragma once
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>
#include "SideEffects.hpp"

class DatabaseManager;
class DiscordManager;
class MMRFetcher;

class SideEffectExecutor {
public:
    SideEffectExecutor();
    ~SideEffectExecutor();

    bool Enqueue(std::function<void()> job, bool critical = false);
    void Stop();
    void Execute(SideEffects&& effects,
                 std::shared_ptr<DatabaseManager> dbManager,
                 std::shared_ptr<DiscordManager> discordManager,
                 std::shared_ptr<MMRFetcher> mmrFetcher);

private:
    std::queue<std::function<void()>> m_jobs;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_stop = false;
    std::atomic<bool> m_replayKeyCancelled{false};
    std::atomic<int> m_replayKeyRequestId{0};
};
