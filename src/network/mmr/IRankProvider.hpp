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

class IRankProvider {
  public:
    virtual ~IRankProvider() = default;
    virtual RankProvider ProviderType() const = 0;
    virtual bool IsAvailable() const = 0;
    virtual bool FetchProfile(const MMRRequest& req,
                              NormalizedProfileResult& outProfile,
                              std::string& outDetail) = 0;
    virtual MmrServiceDetail::CustomApiBatchResult FetchBatch(const MmrServiceDetail::AssembledBatch& batch) = 0;
};

inline RankProvider SelectActiveRankProvider(const ConfigData& conf, bool isSignedIn) {
    const bool customApi = conf.custom_api_enabled && isSignedIn;
    if (customApi) {
        return conf.enable_mmr_tracking ? RankProvider::OmniStatsApiWithTrackerFallback : RankProvider::OmniStatsApi;
    }
    return conf.enable_mmr_tracking ? RankProvider::Tracker : RankProvider::Unavailable;
}
