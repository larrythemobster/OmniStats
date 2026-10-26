#include "ui/rml/services/BackgroundTaskService.hpp"

bool BackgroundTaskService::StartTask(
    std::string startedToast,
    std::function<std::function<void()>(std::stop_token)> work,
    std::function<void(std::string, bool)> showToast) {
    if (m_running.load()) {
        if (showToast) showToast("Another data task is still running. Try again when it finishes.", false);
        return false;
    }
    if (m_task.joinable()) m_task.join();
    m_running.store(true);
    m_task = std::jthread([this, work = std::move(work)](std::stop_token stop) {
        std::function<void()> completion = work(stop);
        {
            std::lock_guard lock(m_mutex);
            m_completion = std::move(completion);
        }
        m_running.store(false);
    });
    if (!startedToast.empty() && showToast) {
        showToast(std::move(startedToast), false);
    }
    return true;
}

void BackgroundTaskService::PublishCompletion() {
    std::function<void()> completion;
    {
        std::lock_guard lock(m_mutex);
        completion.swap(m_completion);
    }
    if (!completion) return;
    completion();
}

void BackgroundTaskService::Shutdown() {
    if (m_task.joinable()) {
        m_task.request_stop();
        m_task.join();
    }
    std::lock_guard lock(m_mutex);
    m_completion = nullptr;
    m_running.store(false);
}
