#pragma once

// Tuning constants and stateless helpers shared by the MMRFetcher translation units.

#include <chrono>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace MMRFetcherDetail {
    inline constexpr int kTrackerForbiddenAttempts = 3;
    inline constexpr const char* kTrackerImpersonation = "chrome136";
    inline constexpr const char* kTrackerUserAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/136.0.0.0 Safari/537.36";

#ifdef OMNISTATS_TEST_ENVIRONMENT
    inline constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(20);
    inline constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(20);
    inline constexpr auto kTransientRetryDelay = std::chrono::milliseconds(20);
    inline constexpr auto kQueueSpacing = std::chrono::milliseconds(20);
    inline constexpr auto kRateLimitLockoutMinimum = std::chrono::milliseconds(50);
    inline constexpr auto kForbiddenLockoutFirst = std::chrono::milliseconds(50);
    inline constexpr auto kForbiddenLockoutSecond = std::chrono::milliseconds(100);
    inline constexpr auto kForbiddenLockoutMaximum = std::chrono::milliseconds(150);
    inline constexpr auto kForbiddenAttemptDelay = std::chrono::milliseconds(1);
#else
    inline constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(2500);
    inline constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(3000);
    inline constexpr auto kTransientRetryDelay = std::chrono::milliseconds(3000);
    inline constexpr auto kQueueSpacing = std::chrono::milliseconds(1500);
    inline constexpr auto kRateLimitLockoutMinimum = std::chrono::seconds(120);
    inline constexpr auto kForbiddenLockoutFirst = std::chrono::minutes(5);
    inline constexpr auto kForbiddenLockoutSecond = std::chrono::minutes(15);
    inline constexpr auto kForbiddenLockoutMaximum = std::chrono::minutes(30);
    inline constexpr auto kForbiddenAttemptDelay = std::chrono::milliseconds(500);
#endif

    bool CaseInsensitiveEquals(std::string_view a, std::string_view b);
    bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out);
    bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr);
    std::chrono::milliseconds ForbiddenLockoutForStrike(size_t strike);
    bool MmrPathPreservesResults(int initialMmr, const std::vector<int>& path, const std::vector<bool>& results);
    std::vector<int> BuildDirectionalMmrPath(int initialMmr, int finalMmr, const std::vector<bool>& results);
    std::vector<int> ReconcileEstimatedPath(int initialMmr, int finalMmr, const std::vector<int>& estimatedPath, const std::vector<bool>& results);
}
