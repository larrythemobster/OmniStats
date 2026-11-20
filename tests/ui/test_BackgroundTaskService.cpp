#include <gtest/gtest.h>
#include "ui/rml/services/BackgroundTaskService.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

TEST(BackgroundTaskService, ThrowingTaskReportsFailureInsteadOfTerminating) {
    BackgroundTaskService service;
    std::string toast;
    bool toastIsError = false;
    ASSERT_TRUE(service.StartTask(
        {},
        [](std::stop_token) -> std::function<void()> { throw std::runtime_error("disk full"); },
        [&](std::string message, bool error) {
            toast = std::move(message);
            toastIsError = error;
        }));

    for (int i = 0; i < 500 && service.IsRunning(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_FALSE(service.IsRunning());

    service.PublishCompletion();
    EXPECT_TRUE(toastIsError);
    EXPECT_FALSE(toast.empty());
}
