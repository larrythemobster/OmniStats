#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

class WorkQueue {
  public:
    WorkQueue();
    ~WorkQueue();

    bool Enqueue(std::function<void()> job, bool critical = false);
    void Stop();

  private:
    std::queue<std::function<void()>> m_jobs;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::jthread m_worker;
    bool m_stop = false;
};
