#pragma once

#include "network/mmr/IRankProvider.hpp"
#include <functional>
#include <memory>

class AccountClient;

class OmniNetRankProvider : public IRankProvider {
  public:
    explicit OmniNetRankProvider(std::shared_ptr<AccountClient> accountClient,
                                 std::function<bool()> abortCheck = nullptr,
                                 MmrTimings timings = {});
    ~OmniNetRankProvider() override = default;

    RankProvider ProviderType() const override {
        return RankProvider::OmniStatsApi;
    }

    bool IsAvailable() const override;

    bool FetchProfile(const MMRRequest& req,
                      NormalizedProfileResult& outProfile,
                      std::string& outDetail) override;

    MmrServiceDetail::CustomApiBatchResult FetchBatch(
        const MmrServiceDetail::AssembledBatch& batch) override;

  private:
    std::shared_ptr<AccountClient> m_accountClient;
    std::function<bool()> m_abortCheck;
    MmrTimings m_timings;
};
