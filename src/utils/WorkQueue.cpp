#include "utils/WorkQueue.hpp"

#include <exception>
#include <iostream>
#include <utility>

WorkQueue::WorkQueue() {
    m_worker = std::jthread([this]() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
                if (m_stop && m_jobs.empty()) {
                    break;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop();
            }
            try {
                job();
            } catch (const std::exception& error) {
                std::cerr << "[WorkQueue] Job failed: " << error.what() << '\n';
            } catch (...) {
                std::cerr << "[WorkQueue] Job failed with an unknown exception.\n";
            }
        }
    });
}

WorkQueue::~WorkQueue() {
    Stop();
}

bool WorkQueue::Enqueue(std::function<void()> job, bool critical) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop) {
            return false;
        }
        if (m_jobs.size() >= 256 && !critical) {
            std::cerr << "[WorkQueue] Queue full; dropping non-critical job.\n";
            return false;
        }
        m_jobs.push(std::move(job));
    }
    m_cv.notify_one();
    return true;
}

void WorkQueue::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}
