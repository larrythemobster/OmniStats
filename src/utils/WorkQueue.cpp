#include "utils/WorkQueue.hpp"

#include <exception>
#include <iostream>
#include <utility>

WorkQueue::WorkQueue() {
    m_worker = std::jthread([this]() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_workerId = std::this_thread::get_id();
        }
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
                if (m_stop && m_jobs.empty()) {
                    m_workerId = {};
                    break;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop();
                m_active = true;
            }
            try {
                job();
            } catch (const std::exception& error) {
                std::cerr << "[WorkQueue] Job failed: " << error.what() << '\n';
            } catch (...) {
                std::cerr << "[WorkQueue] Job failed with an unknown exception.\n";
            }
            job = {};
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_active = false;
                if (m_jobs.empty()) m_drained.notify_all();
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
        if (m_stop && std::this_thread::get_id() != m_workerId) {
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

void WorkQueue::Drain() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_drained.wait(lock, [this] { return m_jobs.empty() && !m_active; });
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
