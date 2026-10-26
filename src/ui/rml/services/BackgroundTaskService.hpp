#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

class BackgroundTaskService {
  public:
    BackgroundTaskService() = default;
    ~BackgroundTaskService() {
        Shutdown();
    }

    BackgroundTaskService(const BackgroundTaskService&) = delete;
    BackgroundTaskService& operator=(const BackgroundTaskService&) = delete;

    bool StartTask(
        std::string startedToast,
        std::function<std::function<void()>(std::stop_token)> work,
        std::function<void(std::string, bool)> showToast);

    void PublishCompletion();

    bool IsRunning() const noexcept {
        return m_running.load(std::memory_order_relaxed);
    }

    void Shutdown();

  private:
    std::mutex m_mutex;
    std::function<void()> m_completion;
    std::atomic<bool> m_running{false};
    std::jthread m_task;
};
