#pragma once

#include "network/mmr/IRankProvider.hpp"
#include "network/http/IHttpClient.hpp"
#include <functional>
#include <memory>

class TrnRankProvider : public IRankProvider {
  public:
    explicit TrnRankProvider(std::shared_ptr<Http::IHttpClient> httpClient = nullptr,
                             std::function<bool()> abortCheck = nullptr,
                             MmrTimings timings = {});
    ~TrnRankProvider() override = default;

    RankProvider ProviderType() const override {
        return RankProvider::Tracker;
    }

    bool IsAvailable() const override;

    bool FetchProfile(const MMRRequest& req,
                      NormalizedProfileResult& outProfile,
                      std::string& outDetail) override;

    MmrServiceDetail::CustomApiBatchResult FetchBatch(
        const MmrServiceDetail::AssembledBatch& batch) override;

    static std::string GetTRNPlatform(const std::string& primaryId);
    enum class Failure { None,
                         Unavailable,
                         Unsupported,
                         Http,
                         Incomplete,
                         Invalid };
    struct Result {
        Failure failure = Failure::None;
        int statusCode = 0;
        int transportError = 0;
        long retryAfterSeconds = 0;
    };
    const Result& LastResult() const {
        return m_lastResult;
    }

  private:
    std::shared_ptr<Http::IHttpClient> m_httpClient;
    std::function<bool()> m_abortCheck;
    MmrTimings m_timings;
    Result m_lastResult;
};
