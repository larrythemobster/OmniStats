#pragma once

// Tuning constants and stateless helpers shared by the MmrService translation units.

#include <chrono>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include "network/mmr/MmrTypes.hpp"
#include "network/mmr/MmrTimings.hpp"

namespace MmrServiceDetail {
    inline constexpr int kTrackerForbiddenAttempts = 3;
    inline constexpr const char* kTrackerImpersonation = "chrome136";
    inline constexpr const char* kTrackerUserAgent =
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/136.0.0.0 Safari/537.36";

    inline constexpr size_t kMaxCustomApiBatchSize = 8;

    struct CustomApiPlayerTarget {
        std::string platform;
        std::string accountId;
    };

    struct AssembledBatch {
        std::vector<MMRRequest> requests;
        std::string playlist;
        int playlistId = 0;
    };

    enum class BatchPlayerStatus {
        Success,
        NotFound,
        Error,
        UnusableData
    };

    struct BatchPlayerResult {
        MMRRequest request;
        BatchPlayerStatus status = BatchPlayerStatus::UnusableData;
        NormalizedProfileResult profile;
        std::string errorCode;
        std::string errorMessage;
    };
    struct CustomApiBatchResult {
        CustomApiFetchResult httpResult = CustomApiFetchResult::DisabledOrNotReady;
        long httpCode = 0;
        std::chrono::steady_clock::duration rateLimitLockout{};
        std::vector<BatchPlayerResult> playerResults;
        bool updateRequired = false;
    };

    bool CaseInsensitiveEquals(std::string_view a, std::string_view b);
    bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out);
    bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr);
    std::chrono::milliseconds ForbiddenLockoutForStrike(size_t strike, const MmrTimings& timings);
    bool MmrPathPreservesResults(int initialMmr, const std::vector<int>& path, const std::vector<bool>& results);
    std::vector<int> BuildDirectionalMmrPath(int initialMmr, int finalMmr, const std::vector<bool>& results);
    std::vector<int> ReconcileEstimatedPath(int initialMmr, int finalMmr, const std::vector<int>& estimatedPath, const std::vector<bool>& results);
    bool TryParseCustomApiTarget(const std::string& primaryId, CustomApiPlayerTarget& outTarget);
    int CustomApiPlaylistIdForName(const std::string& playlist);
    AssembledBatch AssembleCustomApiBatch(
        const std::vector<MMRRequest>& candidates,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point::max(),
        size_t maxBatchSize = kMaxCustomApiBatchSize);
    nlohmann::json BuildCustomApiBatchJson(const AssembledBatch& batch);
    bool TryParseCustomApiPlayerProfile(
        const nlohmann::json& playerJson,
        NormalizedProfileResult& outProfile);
    std::vector<BatchPlayerResult> ParseCustomApiBatchResponse(
        const std::vector<MMRRequest>& batchRequests,
        const nlohmann::json& jsonResp);

    inline std::string PendingPostMatchKey(const std::string& primaryId, const std::string& playlist) {
        return primaryId + '\x1f' + playlist;
    }
}
