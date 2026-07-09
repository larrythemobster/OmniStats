#include "SideEffectExecutor.hpp"
#include "core/Storage.hpp"
#include "core/KeyPressSimulator.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/MMRFetcher.hpp"
#include <iostream>
#include <utility>
#include <windows.h>

SideEffectExecutor::SideEffectExecutor() {
    m_worker = std::jthread([this]() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
                if (m_stop && m_jobs.empty()) break;
                job = std::move(m_jobs.front());
                m_jobs.pop();
            }
            try { job(); } catch (...) {}
        }
    });
}

SideEffectExecutor::~SideEffectExecutor() { Stop(); }

bool SideEffectExecutor::Enqueue(std::function<void()> job, bool critical) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) return false;
        if (m_jobs.size() >= 256 && !critical) {
            std::cerr << "[SideEffectExecutor] Queue full; dropping non-critical side effect.\n";
            return false;
        }
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

void SideEffectExecutor::Stop() {
    m_replayKeyCancelled = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void SideEffectExecutor::Execute(SideEffects&& effects,
                                  std::shared_ptr<DatabaseManager> dbManager,
                                  std::shared_ptr<DiscordManager> discordManager,
                                  std::shared_ptr<MMRFetcher> mmrFetcher) {
    if (effects.pushDiscord && discordManager) {
        discordManager->PushPresenceUpdate(effects.discordSnapshot);
    }
    if (effects.fetchLifetimeHistory && dbManager) {
        dbManager->AsyncGetLifetimeMmrHistory(effects.lifetimePrimaryId, effects.lifetimeCategory);
    }
    if (effects.refreshDbStats && dbManager) {
        dbManager->AsyncRefreshDbStats(effects.refreshStatsPrimaryId);
    }
    for (const auto& [pid, name] : effects.fetchMmrQueue) {
        if (mmrFetcher) mmrFetcher->Enqueue(pid, name);
    }
    if (dbManager) {
        for (const auto& pid : effects.fetchEncounterQueue) {
            dbManager->AsyncGetPlayerEncounterRecord(pid);
        }
    }
    if (effects.saveMatch) {
        auto matchRecord = std::move(effects.matchRecord);
        auto snapshot = std::move(effects.saveSnapshot);
        auto db = dbManager;
        Enqueue([matchRecord = std::move(matchRecord),
                 snapshot = std::move(snapshot), db]() mutable {
            Storage::AppendMatchSync(matchRecord);
            if (db) db->AsyncSaveMatch(std::move(snapshot));
        }, true);
    }
    if (effects.replayKeyToPress != -1) {
        int reqId = ++m_replayKeyRequestId;
        int key = effects.replayKeyToPress;
        Enqueue([this, key, reqId]() {
            for (int i = 0; i < 30; ++i) {
                if (m_replayKeyCancelled || m_replayKeyRequestId != reqId) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (m_replayKeyCancelled || m_replayKeyRequestId != reqId) return;
            SimulateSaveReplayKeyPress(key, 1500);
        });
    }
}
