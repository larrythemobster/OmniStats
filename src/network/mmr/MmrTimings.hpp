#pragma once

#include <chrono>

struct MmrTimings {
    std::chrono::milliseconds postMatchInitialDelay{2500};
    std::chrono::milliseconds stalePostMatchRetryDelay{3000};
    std::chrono::milliseconds transientRetryDelay{3000};
    std::chrono::milliseconds queueSpacing{1500};
    std::chrono::milliseconds rateLimitLockoutMinimum{std::chrono::seconds(120)};
    std::chrono::milliseconds forbiddenLockoutFirst{std::chrono::minutes(5)};
    std::chrono::milliseconds forbiddenLockoutSecond{std::chrono::minutes(15)};
    std::chrono::milliseconds forbiddenLockoutMaximum{std::chrono::minutes(30)};
    std::chrono::milliseconds forbiddenAttemptDelay{500};
    std::chrono::milliseconds batchCoalesceWindow{300};
};
