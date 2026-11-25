#pragma once

#include "network/mmr/IRankProvider.hpp"
#include "network/http/IHttpClient.hpp"
#include <functional>
#include <memory>

class TrnRankProvider : public ITrackerRankProvider {
  public:
    explicit TrnRankProvider(std::shared_ptr<Http::IHttpClient> httpClient = nullptr,
                             std::function<bool()> abortCheck = nullptr,
                             MmrTimings timings = {});
    ~TrnRankProvider() override = default;

    bool IsAvailable() const;

    TrackerFetchResult FetchProfile(const MMRRequest& req) override;

    static std::string GetTRNPlatform(const std::string& primaryId);

  private:
    std::shared_ptr<Http::IHttpClient> m_httpClient;
    std::function<bool()> m_abortCheck;
    MmrTimings m_timings;
};
