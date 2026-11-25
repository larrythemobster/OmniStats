#include "network/mmr/OmniNetRankProvider.hpp"
#include "network/api/AccountClient.hpp"
#include "network/omninet/OmniNet.hpp"
#include "network/mmr/MmrServiceDetail.hpp"
#include "core/config/Config.hpp"
#include "utils/PrivacyLog.hpp"

#include <nlohmann/json.hpp>
#include <iostream>

using namespace MmrServiceDetail;

OmniNetRankProvider::OmniNetRankProvider(std::shared_ptr<AccountClient> accountClient,
                                         std::function<bool()> abortCheck,
                                         MmrTimings timings)
    : m_accountClient(std::move(accountClient)), m_abortCheck(std::move(abortCheck)), m_timings(timings) {}

bool OmniNetRankProvider::IsAvailable() const {
    if (!m_accountClient) return false;
    const auto config = Config::Read();
    if (!config.custom_api_enabled) return false;
    return m_accountClient->IsSignedIn();
}

CustomApiBatchResult OmniNetRankProvider::FetchBatch(const AssembledBatch& batch) {
    CustomApiBatchResult result;
    if (batch.requests.empty()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    if (!OmniNet::Available() || !Config::Read().custom_api_enabled) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    const auto reqBody = BuildCustomApiBatchJson(batch);
    if (reqBody.at("players").empty()) {
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }
    const std::string reqBodyStr = reqBody.dump();
    if (!m_accountClient) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    auto& accountClient = *m_accountClient;
    if (!accountClient.IsSignedIn()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    OmniNet::Request req;
    req.op = OmniNet::Op::Ranks;
    req.payload = reqBodyStr;
    req.shouldAbort = m_abortCheck;
    const auto auth = accountClient.SendAuthenticated(std::move(req), AccountClient::AuthenticatedRequestPolicy::Ranks);
    const auto& resp = auth.response;
    result.httpCode = resp.status;
    if (resp.updateRequired) {
        result.updateRequired = true;
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }
    if (auth.missingCredentials || auth.refreshFailed || auth.signedOut) {
        result.httpResult = auth.signedOut || !accountClient.IsSignedIn()
                                ? CustomApiFetchResult::AuthFailure
                                : CustomApiFetchResult::TransientError;
        return result;
    }

    if (resp.transportError != 0) {
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }
    if (resp.status == 403) {
        result.httpResult = CustomApiFetchResult::AuthFailure;
        return result;
    }
    if (resp.status == 429) {
        result.httpResult = CustomApiFetchResult::TransientError;
        std::chrono::steady_clock::duration lockout = m_timings.rateLimitLockoutMinimum;
        if (resp.retryAfterSeconds > 0) {
            lockout = (std::max)(lockout,
                                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::seconds(resp.retryAfterSeconds)));
        }
        result.rateLimitLockout = lockout;
        return result;
    }
    if (resp.status == 404) {
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }
    if (resp.status >= 500 && resp.status <= 599) {
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }
    if (resp.status != 200) {
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }

    nlohmann::json jsonResp;
    try {
        jsonResp = nlohmann::json::parse(resp.body);
    } catch (...) {
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }

    if (jsonResp.is_object() && jsonResp.contains("error") && jsonResp["error"].is_object()) {
        result.httpResult = CustomApiFetchResult::UnusableData;
        result.playerResults = ParseCustomApiBatchResponse(batch.requests, jsonResp);
        return result;
    }

    result.playerResults = ParseCustomApiBatchResponse(batch.requests, jsonResp);
    result.httpResult = CustomApiFetchResult::SuccessFinished;
    return result;
}
