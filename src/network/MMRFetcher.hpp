#pragma once
#include <string>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <memory>
#include <atomic>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include "core/SessionState.hpp"

struct MMRRequest {
    std::string primaryId;
    std::string name;
};

struct MMRProfileTotals {
    int totalWins = -1;
};

class MMRFetcher {
  public:
    MMRFetcher(std::shared_ptr<SessionState> state);
    ~MMRFetcher();

    void Start();
    void Stop();

    // Pushes a player into the queue to be fetched
    void Enqueue(const std::string& primaryId, const std::string& name);

    static std::string GetTournamentTierForMmr(int mmr);
    static std::string PlaylistNameForTrackerId(int playlistId);
    static MMRProfileTotals ExtractProfileTotals(const nlohmann::json& jsonResp);

  private:
    void WorkerLoop();
    void FetchProfile(const MMRRequest& req, bool& rateLimited);
    std::string GetTRNPlatform(const std::string& primaryId);

    std::shared_ptr<SessionState> m_state;

    std::queue<MMRRequest> m_queue;
    std::unordered_set<std::string> m_queuedOrInFlight;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;

    std::jthread m_workerThread;
    std::atomic<bool> m_isRunning{false};
};
