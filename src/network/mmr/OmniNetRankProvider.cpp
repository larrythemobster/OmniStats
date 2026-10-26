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
                                         std::function<bool()> abortCheck)
    : m_accountClient(std::move(accountClient)), m_abortCheck(std::move(abortCheck)) {}

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

    const std::string reqBodyStr = BuildCustomApiBatchJson(batch).dump();
    if (!m_accountClient) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    auto& accountClient = *m_accountClient;
    if (!accountClient.IsSignedIn()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    const auto performRequest = [&](const std::string& bearerToken, const std::string& deviceId) -> OmniNet::Response {
        OmniNet::Request req;
        req.op = OmniNet::Op::Ranks;
        req.payload = reqBodyStr;
        req.accessToken = bearerToken;
        req.devicePublicId = deviceId;
        req.shouldAbort = m_abortCheck;
        return OmniNet::Send(req);
    };

    std::string accessToken = accountClient.AccessToken();
    std::string deviceId = accountClient.DevicePublicId();
    if (accessToken.empty() || deviceId.empty()) {
        if (!accountClient.IsSignedIn()) {
            result.httpResult = CustomApiFetchResult::AuthFailure;
            return result;
        }
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }

    OmniNet::Response resp = performRequest(accessToken, deviceId);
    if (resp.updateRequired) {
        result.updateRequired = true;
        result.httpCode = resp.status;
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    if (resp.transportError == 0 && resp.status == 401) {
        if (!accountClient.Refresh()) {
            if (!accountClient.IsSignedIn()) {
                result.httpResult = CustomApiFetchResult::AuthFailure;
            } else {
                result.httpResult = CustomApiFetchResult::TransientError;
            }
            return result;
        }
        accessToken = accountClient.AccessToken();
        deviceId = accountClient.DevicePublicId();
        if (accessToken.empty() || deviceId.empty()) {
            accountClient.MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Accounts & services.");
            result.httpResult = CustomApiFetchResult::AuthFailure;
            return result;
        }
        resp = performRequest(accessToken, deviceId);
        if (resp.updateRequired) {
            result.updateRequired = true;
            result.httpCode = resp.status;
            result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
            return result;
        }
        if (resp.transportError == 0 && resp.status == 401) {
            accountClient.MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Accounts & services.");
            result.httpResult = CustomApiFetchResult::AuthFailure;
            return result;
        }
    }

    result.httpCode = resp.status;

    if (resp.transportError != 0) {
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }
    if (resp.status == 401) {
        accountClient.MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Accounts & services.");
        result.httpResult = CustomApiFetchResult::AuthFailure;
        return result;
    }
    if (resp.status == 403) {
        result.httpResult = CustomApiFetchResult::AuthFailure;
        return result;
    }
    if (resp.status == 429) {
        result.httpResult = CustomApiFetchResult::TransientError;
        std::chrono::steady_clock::duration lockout = kRateLimitLockoutMinimum;
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

bool OmniNetRankProvider::FetchProfile(const MMRRequest& req,
                                       NormalizedProfileResult& outProfile,
                                       std::string& outDetail) {
    AssembledBatch batch;
    batch.playlist = req.playlist;
    batch.playlistId = CustomApiPlaylistIdForName(req.playlist);
    batch.requests.push_back(req);

    CustomApiBatchResult res = FetchBatch(batch);
    if (res.httpResult != CustomApiFetchResult::SuccessFinished) {
        outDetail = "OmniNet rank fetch failed with HTTP " + std::to_string(res.httpCode);
        return false;
    }

    for (const auto& playerRes : res.playerResults) {
        if (playerRes.request.primaryId == req.primaryId) {
            if (playerRes.status == BatchPlayerStatus::Success) {
                outProfile = playerRes.profile;
                return true;
            }
            outDetail = playerRes.errorMessage.empty() ? playerRes.errorCode : playerRes.errorMessage;
            return false;
        }
    }

    outDetail = "Player not found in OmniNet response";
    return false;
}
