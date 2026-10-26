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
    // Wait for queued and running jobs, including jobs they enqueue. Call from outside the worker.
    void Drain();
    // Reject new external jobs and finish queued work before returning.
    void Stop();

  private:
    std::queue<std::function<void()>> m_jobs;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_drained;
    std::jthread m_worker;
    std::thread::id m_workerId;
    bool m_stop = false;
    bool m_active = false;
};
