#pragma once

#include "network/mmr/MmrTimings.hpp"

inline MmrTimings FastMmrTimings() {
    return {
        .postMatchInitialDelay = std::chrono::milliseconds(20),
        .stalePostMatchRetryDelay = std::chrono::milliseconds(20),
        .transientRetryDelay = std::chrono::milliseconds(20),
        .queueSpacing = std::chrono::milliseconds(20),
        .rateLimitLockoutMinimum = std::chrono::milliseconds(50),
        .forbiddenLockoutFirst = std::chrono::milliseconds(50),
        .forbiddenLockoutSecond = std::chrono::milliseconds(100),
        .forbiddenLockoutMaximum = std::chrono::milliseconds(150),
        .forbiddenAttemptDelay = std::chrono::milliseconds(1),
        .batchCoalesceWindow = std::chrono::milliseconds(10),
    };
}
