#pragma once

#include "network/mmr/MmrServiceDetail.hpp"
#include "core/config/Config.hpp"
#include <memory>
#include <string>

enum class RankProvider {
    Unavailable,
    Tracker,
    OmniStatsApi,
    OmniStatsApiWithTrackerFallback
};

enum class TrackerFetchFailure { None,
                                 Unavailable,
                                 Unsupported,
                                 Http,
                                 Incomplete,
                                 Invalid };

struct TrackerFetchResult {
    TrackerFetchFailure failure = TrackerFetchFailure::None;
    int statusCode = 0;
    int transportError = 0;
    long retryAfterSeconds = 0;
    bool success = false;
    NormalizedProfileResult profile;
    std::string detail;
};

class ITrackerRankProvider {
  public:
    virtual ~ITrackerRankProvider() = default;
    virtual TrackerFetchResult FetchProfile(const MMRRequest& req) = 0;
};

class IBatchRankProvider {
  public:
    virtual ~IBatchRankProvider() = default;
    virtual MmrServiceDetail::CustomApiBatchResult FetchBatch(const MmrServiceDetail::AssembledBatch& batch) = 0;
};

inline RankProvider SelectActiveRankProvider(const ConfigData& conf, bool isSignedIn) {
    const bool customApi = conf.custom_api_enabled && isSignedIn;
    if (customApi) {
        return conf.enable_mmr_tracking ? RankProvider::OmniStatsApiWithTrackerFallback : RankProvider::OmniStatsApi;
    }
    return conf.enable_mmr_tracking ? RankProvider::Tracker : RankProvider::Unavailable;
}
