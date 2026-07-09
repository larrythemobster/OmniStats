#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <mutex>
#include <memory>
#include <chrono>

#include <queue>

class SessionState;

class ReplayUploader {
public:
    ReplayUploader(std::shared_ptr<SessionState> state = nullptr);
    ~ReplayUploader();

    void Start();
    void Stop();

private:
    void WorkerLoop();
    bool IsFileReady(const std::string& path);
    void UploadReplay(const std::string& path);
    void CheckAndSaveReplay();
    void MarkProcessed(const std::string& path);
    void SimulateSaveReplayKeyPress(int key);

    std::jthread m_worker;
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::unordered_set<std::string> m_processed;
    std::queue<std::string> m_processedQueue;
    
    std::shared_ptr<SessionState> m_state;
    bool m_wasInMatch = false;
    std::chrono::steady_clock::time_point m_lastSaveAttempt;
};
