#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "network/AccountClient.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <cmath>
#include <cctype>
#include <optional>
#include <string_view>
#include "network/MMRFetcherDetail.hpp"
#include "network/HttpSecurity.hpp"

using namespace MMRFetcherDetail;

namespace {
    struct FetchHeaderState {
        long retryAfterSeconds = 0;
        std::string location;
    };

    size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
        const size_t total = size * nitems;
        if (!userdata || !buffer) return total;
        auto* state = static_cast<FetchHeaderState*>(userdata);

        std::string_view line(buffer, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }

        constexpr std::string_view kRetryAfter = "retry-after:";
        if (line.size() >= kRetryAfter.size()) {
            bool match = true;
            for (size_t i = 0; i < kRetryAfter.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != kRetryAfter[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string_view val = line.substr(kRetryAfter.size());
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                try {
                    long parsed = std::stol(std::string(val));
                    if (parsed > 0) {
                        state->retryAfterSeconds = parsed;
                    }
                } catch (...) {
                }
            }
        }
        constexpr std::string_view kLocation = "location:";
        if (line.size() >= kLocation.size()) {
            bool match = true;
            for (size_t i = 0; i < kLocation.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != kLocation[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string_view val = line.substr(kLocation.size());
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                state->location = std::string(val);
            }
        }
        return total;
    }

    // Helper function for libcurl to write the HTTP response into a std::string
    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    int CIProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
        std::atomic<bool>* running = (std::atomic<bool>*)clientp;
        if (running && !running->load()) {
            return 1; // Abort
        }
        return 0;
    }
}

bool MMRFetcher::IsCustomApiActiveSource() const {
    const auto config = Config::Read();
    if (!config.custom_api_enabled) return false;
    const bool signedIn = AccountClient::Instance().IsSignedIn();
    const bool hasKey = !config.custom_api_key.empty();
    if (!signedIn && !hasKey) return false;

    if (signedIn) return true;
    if (!config.enable_mmr_tracking) return true;
    if (m_useCustomApiFallback.load()) return true;

    return false;
}

MMRFetcherDetail::CustomApiBatchResult MMRFetcher::FetchBatchFromCustomApi(const AssembledBatch& batch) {
    CustomApiBatchResult result;
    if (batch.requests.empty()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    const auto config = Config::Read();
    if (!config.custom_api_enabled) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }
    auto& accountClient = AccountClient::Instance();
    const bool signedIn = accountClient.IsSignedIn();
    if (!signedIn && config.custom_api_key.empty()) {
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }

    if (!signedIn) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_rejectedCustomApiKey.empty()) {
            if (m_rejectedCustomApiKey == config.custom_api_key) {
                if (!m_state->ui.customApiKeyRejected.exchange(true)) {
                    m_state->ui.customApiKeyRejectedVersion.fetch_add(1);
                }
                result.httpResult = CustomApiFetchResult::AuthFailure;
                return result;
            }
            m_rejectedCustomApiKey.clear();
            m_state->ui.customApiKeyRejected.store(false);
        }
    } else {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_rejectedCustomApiKey.empty()) {
            m_rejectedCustomApiKey.clear();
            m_state->ui.customApiKeyRejected.store(false);
        }
    }

    std::string baseUrl = config.custom_api_base_url;
    if (baseUrl.empty()) {
        baseUrl = "https://api.omnistats.org";
    }
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }

    nlohmann::json reqBody = BuildCustomApiBatchJson(batch);
    const std::string reqBodyStr = reqBody.dump();
    const std::string url = baseUrl + "/v1/ranks";
    const auto parsedUrl = HttpSecurity::ParseUrl(url);
    std::string urlError;
    if (!HttpSecurity::IsAllowedSensitiveUrl(parsedUrl, &urlError)) {
        std::cout << "[MMRFetcher] Custom API URL rejected: " << urlError << "\n";
        result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
        return result;
    }
    std::string readBuffer;
    FetchHeaderState headerState;
    int res = 0;
    long httpCode = 0;

    const auto performRequest = [&](const std::string& bearerToken, const std::string& deviceId) -> bool {
        std::string currentUrl = url;
        constexpr int kMaxRedirects = 5;

        for (int redirectCount = 0; redirectCount <= kMaxRedirects; ++redirectCount) {
            readBuffer.clear();
            headerState = {};
            res = 0;
            httpCode = 0;

            void* ci_curl = ci.easy_init();
            if (!ci_curl) return false;

            void* headers = nullptr;
            headers = ci.slist_append(headers, "Content-Type: application/json");
            headers = ci.slist_append(headers, "Accept: application/json");
            if (signedIn) {
                headers = ci.slist_append(headers, ("Authorization: Bearer " + bearerToken).c_str());
                headers = ci.slist_append(headers, ("X-Omni-Device-Id: " + deviceId).c_str());
            } else {
                headers = ci.slist_append(headers, ("X-API-Key: " + config.custom_api_key).c_str());
            }

            ci.easy_setopt(ci_curl, CI_CURLOPT_URL, currentUrl.c_str());
            ci.easy_setopt(ci_curl, CI_CURLOPT_POST, 1L);
            ci.easy_setopt(ci_curl, CI_CURLOPT_POSTFIELDS, reqBodyStr.c_str());
            ci.easy_setopt(ci_curl, CI_CURLOPT_POSTFIELDSIZE, static_cast<long>(reqBodyStr.size()));
            ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
            ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
            ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
            ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERFUNCTION, HeaderCallback);
            ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERDATA, &headerState);
            ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 10L);
            ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
            ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 0L);
            ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 1L);

            res = ci.easy_perform(ci_curl);
            ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);

            ci.slist_free_all(headers);
            ci.easy_cleanup(ci_curl);

            if (res != 0) {
                return true;
            }

            if (httpCode == 301 || httpCode == 302 || httpCode == 303 || httpCode == 307 || httpCode == 308) {
                if (redirectCount == kMaxRedirects) {
                    std::cout << "[MMRFetcher] Custom API exceeded max redirect limit.\n";
                    res = -1;
                    return true;
                }
                const auto val = HttpSecurity::ValidateRedirect(currentUrl, headerState.location, /*isSensitiveRequest=*/true);
                if (!val.allowed) {
                    std::cout << "[MMRFetcher] Custom API redirect rejected: " << val.reason << "\n";
                    res = -1;
                    return true;
                }
                currentUrl = val.resolvedUrl;
                continue;
            }
            break;
        }
        return true;
    };

    if (signedIn) {
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
        if (!performRequest(accessToken, deviceId)) {
            result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
            return result;
        }
        if (res == 0 && httpCode == 401) {
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
                accountClient.MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Integrations.");
                result.httpResult = CustomApiFetchResult::AuthFailure;
                return result;
            }
            if (!performRequest(accessToken, deviceId)) {
                result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
                return result;
            }
            if (res == 0 && httpCode == 401) {
                accountClient.MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Integrations.");
                std::cout << "[MMRFetcher] Custom API rejected refreshed device token (HTTP 401). Signed out.\n";
                result.httpResult = CustomApiFetchResult::AuthFailure;
                return result;
            }
        }
    } else {
        if (!performRequest("", "")) {
            result.httpResult = CustomApiFetchResult::DisabledOrNotReady;
            return result;
        }
    }

    result.httpCode = httpCode;

    if (res != 0) {
        std::cout << "[MMRFetcher] Custom API network error: curl error " << res << ".\n";
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }
    if (httpCode == 401) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_rejectedCustomApiKey = config.custom_api_key;
        }
        if (!m_state->ui.customApiKeyRejected.exchange(true)) {
            m_state->ui.customApiKeyRejectedVersion.fetch_add(1);
        }
        std::cout << "[MMRFetcher] Custom API rejected the configured API key (HTTP 401).\n";
        result.httpResult = CustomApiFetchResult::AuthFailure;
        return result;
    }
    if (httpCode == 403) {
        std::cout << "[MMRFetcher] Custom API authentication failed (HTTP 403).\n";
        result.httpResult = CustomApiFetchResult::AuthFailure;
        return result;
    }
    if (httpCode == 429) {
        std::cout << "[MMRFetcher] Custom API rate limited (HTTP 429).\n";
        result.httpResult = CustomApiFetchResult::TransientError;
        std::chrono::steady_clock::duration lockout = kRateLimitLockoutMinimum;
        if (headerState.retryAfterSeconds > 0) {
            lockout = (std::max)(lockout,
                                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::seconds(headerState.retryAfterSeconds)));
        }
        result.rateLimitLockout = lockout;
        return result;
    }
    if (httpCode == 404) {
        std::cout << "[MMRFetcher] Custom API returned HTTP 404 (not found).\n";
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }
    if (httpCode >= 500 && httpCode <= 599) {
        std::cout << "[MMRFetcher] Custom API server error (HTTP " << httpCode << ").\n";
        result.httpResult = CustomApiFetchResult::TransientError;
        return result;
    }
    if (httpCode != 200) {
        std::cout << "[MMRFetcher] Custom API returned unexpected HTTP " << httpCode << ".\n";
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }

    nlohmann::json jsonResp;
    try {
        jsonResp = nlohmann::json::parse(readBuffer);
    } catch (...) {
        std::cout << "[MMRFetcher] Custom API returned malformed JSON.\n";
        result.httpResult = CustomApiFetchResult::UnusableData;
        return result;
    }

    if (jsonResp.is_object() && jsonResp.contains("error") && jsonResp["error"].is_object()) {
        std::string errCode = jsonResp["error"].value("code", "unknown");
        std::string errMsg = jsonResp["error"].value("message", "");
        std::cout << "[MMRFetcher] Custom API error: " << errCode;
        if (!errMsg.empty()) std::cout << " (" << errMsg << ")";
        std::cout << ".\n";
        result.httpResult = CustomApiFetchResult::UnusableData;
        result.playerResults = ParseCustomApiBatchResponse(batch.requests, jsonResp);
        return result;
    }

    result.playerResults = ParseCustomApiBatchResponse(batch.requests, jsonResp);
    result.httpResult = CustomApiFetchResult::SuccessFinished;
    return result;
}

void MMRFetcher::ProcessCustomApiBatch(const AssembledBatch& batch) {
    if (batch.requests.empty()) return;

    CustomApiBatchResult res = FetchBatchFromCustomApi(batch);
    const auto now = std::chrono::steady_clock::now();
    const auto config = Config::Read();

    if (res.httpResult == CustomApiFetchResult::AuthFailure) {
        m_useCustomApiFallback.store(false);
        for (const auto& req : batch.requests) {
            if (config.enable_mmr_tracking && m_rateLimitedUntil <= now) {
                const bool requeued = FetchProfileTracker(req);
                if (!requeued) FinishRequest(req);
            } else {
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& p = m_state->game.roster[req.primaryId];
                    p.fetched = true;
                    p.fetchFailed = true;
                    m_state->game.version++;
                }
                FinishRequest(req);
            }
        }
        return;
    }

    if (res.httpResult == CustomApiFetchResult::TransientError) {
        const auto delay = (res.httpCode == 429 && res.rateLimitLockout > std::chrono::steady_clock::duration::zero())
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(res.rateLimitLockout)
                               : kTransientRetryDelay;
        const char* reason = (res.httpCode == 429) ? "custom API rate limited" : "transient custom API failure";
        if (res.httpCode == 429) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_rateLimitedUntil = now + res.rateLimitLockout;
        }
        for (const auto& req : batch.requests) {
            if (!ScheduleRetry(req, delay, reason)) {
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& p = m_state->game.roster[req.primaryId];
                    p.fetched = true;
                    p.fetchFailed = true;
                    m_state->game.version++;
                }
                FinishRequest(req);
            }
        }
        return;
    }

    if (res.httpResult == CustomApiFetchResult::UnusableData || res.httpResult == CustomApiFetchResult::DisabledOrNotReady) {
        for (const auto& req : batch.requests) {
            if (config.enable_mmr_tracking && m_rateLimitedUntil <= now) {
                const bool requeued = FetchProfileTracker(req);
                if (!requeued) FinishRequest(req);
            } else {
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& p = m_state->game.roster[req.primaryId];
                    p.fetched = true;
                    p.fetchFailed = true;
                    m_state->game.version++;
                }
                FinishRequest(req);
            }
        }
        return;
    }

    for (const auto& pRes : res.playerResults) {
        const auto& req = pRes.request;
        if (pRes.status == BatchPlayerStatus::Success) {
            const bool requeued = PublishProfileResult(req, pRes.profile);
            if (!requeued) FinishRequest(req);
        } else {
            if (config.enable_mmr_tracking && m_rateLimitedUntil <= now) {
                const bool requeued = FetchProfileTracker(req);
                if (!requeued) FinishRequest(req);
            } else {
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& p = m_state->game.roster[req.primaryId];
                    p.fetched = true;
                    p.fetchFailed = true;
                    m_state->game.version++;
                }
                FinishRequest(req);
            }
        }
    }
}

CustomApiFetchResult MMRFetcher::FetchProfileFromCustomApi(const MMRRequest& req) {
    CustomApiPlayerTarget target;
    if (!TryParseCustomApiTarget(req.primaryId, target)) {
        std::cout << "[MMRFetcher] Custom API skipped for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": invalid primaryId format.\n";
        return CustomApiFetchResult::UnusableData;
    }

    AssembledBatch batch;
    batch.requests = {req};
    batch.playlist = req.playlist;
    batch.playlistId = CustomApiPlaylistIdForName(req.playlist);

    CustomApiBatchResult bRes = FetchBatchFromCustomApi(batch);
    if (bRes.httpResult != CustomApiFetchResult::SuccessFinished) {
        return bRes.httpResult;
    }

    if (bRes.playerResults.empty()) {
        return CustomApiFetchResult::UnusableData;
    }

    const auto& pRes = bRes.playerResults.front();
    if (pRes.status == BatchPlayerStatus::Success) {
        const bool requeued = PublishProfileResult(req, pRes.profile);
        return requeued ? CustomApiFetchResult::SuccessRequeued : CustomApiFetchResult::SuccessFinished;
    }
    return CustomApiFetchResult::UnusableData;
}

bool MMRFetcher::FetchProfile(MMRRequest req) {
    if (req.reason == MMRRequestReason::PostMatch) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
        if (recordIt != m_postMatchRecordsByGuid.end() &&
            recordIt->second.reconciliationState == PostMatchReconciliationState::Confirmed) {
            return false;
        }
    }
    const ConfigData config = Config::Read();
    const bool signedInPrimary = config.custom_api_enabled && AccountClient::Instance().IsSignedIn();
    const bool tryCustomApiFirst = signedInPrimary || m_useCustomApiFallback.load();
    if (tryCustomApiFirst) {
        const auto customResult = FetchProfileFromCustomApi(req);
        if (customResult == CustomApiFetchResult::SuccessFinished) {
            return false;
        }
        if (customResult == CustomApiFetchResult::SuccessRequeued) {
            return true;
        }
        if (customResult == CustomApiFetchResult::AuthFailure) {
            m_useCustomApiFallback.store(false);
        } else if (!config.enable_mmr_tracking || !signedInPrimary) {
            if (customResult == CustomApiFetchResult::TransientError) {
                if (ScheduleRetry(req, kTransientRetryDelay, "transient custom API failure")) {
                    return true;
                }
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& player = m_state->game.roster[req.primaryId];
                    player.fetched = true;
                    player.fetchFailed = true;
                    m_state->game.version++;
                }
                return false;
            } else if (customResult == CustomApiFetchResult::UnusableData) {
                const auto now = std::chrono::steady_clock::now();
                if (!config.enable_mmr_tracking || m_rateLimitedUntil > now) {
                    std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                    if (m_state->game.roster.count(req.primaryId)) {
                        auto& player = m_state->game.roster[req.primaryId];
                        player.fetched = true;
                        player.fetchFailed = true;
                        m_state->game.version++;
                    }
                    return false;
                }
            }
        } else {
            const auto now = std::chrono::steady_clock::now();
            if (m_rateLimitedUntil > now) {
                if (customResult == CustomApiFetchResult::TransientError &&
                    ScheduleRetry(req, kTransientRetryDelay, "transient custom API failure")) {
                    return true;
                }
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& player = m_state->game.roster[req.primaryId];
                    player.fetched = true;
                    player.fetchFailed = true;
                    m_state->game.version++;
                }
                return false;
            }
        }
    }
    return FetchProfileTracker(req);
}

bool MMRFetcher::FetchProfileTracker(MMRRequest req) {
    const ConfigData config = Config::Read();
    const bool signedInPrimary = config.custom_api_enabled && AccountClient::Instance().IsSignedIn();
    const bool tryCustomApiFirst = signedInPrimary || m_useCustomApiFallback.load();
    if (!config.enable_mmr_tracking) {
        if (req.reason == MMRRequestReason::PostMatch) {
            EnsureProvisionalPoint(req, req.previousMmr);
        }
        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }
    const auto localCustomApiFallback = [&]() -> std::optional<bool> {
        if (tryCustomApiFirst) return std::nullopt;
        {
            std::shared_lock<std::shared_mutex> gameLock(m_state->game.mutex);
            if (req.primaryId != m_state->game.myPrimaryId) return std::nullopt;
        }
        const auto result = FetchProfileFromCustomApi(req);
        if (result == CustomApiFetchResult::SuccessFinished) return false;
        if (result == CustomApiFetchResult::SuccessRequeued) return true;
        return std::nullopt;
    };
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        std::cout << "[MMRFetcher] Skipping " << PrivacyLog::Sensitive(req.name, "player name")
                  << ": curl-impersonate not loaded\n";
        return false;
    }

    const std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) return false;

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return false;

    const size_t delim = req.primaryId.find('|');
    std::string ident;
    if (plat == "steam") {
        if (delim != std::string::npos) {
            std::string sub = req.primaryId.substr(delim + 1);
            const size_t secondDelim = sub.find('|');
            ident = secondDelim != std::string::npos ? sub.substr(0, secondDelim) : sub;
        } else {
            ident = req.primaryId;
        }
    } else {
        ident = req.name;
    }

    char* escapedIdent = ci.easy_escape(ci_curl, ident.c_str(), static_cast<int>(ident.length()));
    if (!escapedIdent) {
        ci.easy_cleanup(ci_curl);
        return false;
    }
    const std::string finalIdent = escapedIdent;
    ci.free_ptr(escapedIdent);

    const std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + finalIdent;
    std::cout << "[MMRFetcher] Fetching " << PrivacyLog::Sensitive(req.name, "player name") << " via " << plat << "...\n";

    ci.easy_impersonate(ci_curl, kTrackerImpersonation, 0);

    std::string readBuffer;
    FetchHeaderState headerState;
    void* headers = nullptr;
    headers = ci.slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = ci.slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = ci.slist_append(headers, "Origin: https://rocketleague.tracker.network");
    headers = ci.slist_append(headers, "Referer: https://rocketleague.tracker.network/");
    headers = ci.slist_append(
        headers, (std::string("User-Agent: ") + kTrackerUserAgent).c_str());

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERFUNCTION, HeaderCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERDATA, &headerState);
    ci.easy_setopt(ci_curl, CI_CURLOPT_ACCEPT_ENCODING, "");
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 15L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFODATA, &m_isRunning);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 0L);

    int res = 0;
    long httpCode = 0;
    for (int attempt = 1; attempt <= kTrackerForbiddenAttempts; ++attempt) {
        readBuffer.clear();
        headerState = {};
        res = ci.easy_perform(ci_curl);
        ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);
        if (res != 0 || httpCode != 403 ||
            attempt == kTrackerForbiddenAttempts) {
            break;
        }
        std::cout
            << "[MMRFetcher] Tracker.gg returned HTTP 403; retrying with "
            << "Chrome 136 (attempt " << attempt + 1 << "/"
            << kTrackerForbiddenAttempts << ").\n";
        std::this_thread::sleep_for(kForbiddenAttemptDelay);
    }

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0 || httpCode != 200) {
        std::cout << "[MMRFetcher] Failed to fetch " << PrivacyLog::Sensitive(req.name, "player name")
                  << " (HTTP " << httpCode << ") - Curl error: " << res << "\n";

        if (httpCode == 403) {
            if (!tryCustomApiFirst) {
                std::cout
                    << "[MMRFetcher] Tracker.gg returned HTTP 403 after "
                    << kTrackerForbiddenAttempts
                    << " attempts. Attempting fallback to custom API...\n";
                const auto customResult = FetchProfileFromCustomApi(req);
                if (customResult == CustomApiFetchResult::SuccessFinished) {
                    std::cout << "[MMRFetcher] Successfully fetched ranks from custom API for "
                              << PrivacyLog::Sensitive(req.name, "player name") << "!\n";
                    m_useCustomApiFallback.store(true);
                    return false;
                }
                if (customResult == CustomApiFetchResult::SuccessRequeued) {
                    std::cout << "[MMRFetcher] Successfully fetched ranks from custom API for "
                              << PrivacyLog::Sensitive(req.name, "player name") << "!\n";
                    m_useCustomApiFallback.store(true);
                    return true;
                }
                std::cout << "[MMRFetcher] Custom API fallback was unavailable or failed.\n";
            }
            if (!m_isRunning) return false;

            size_t strike = 0;
            std::chrono::steady_clock::duration lockout{};
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                strike = ++m_forbiddenStrikeCount;
                lockout = ForbiddenLockoutForStrike(strike);
                const auto retryAt = std::chrono::steady_clock::now() + lockout;

                // Treat a 403 as a Tracker-wide/WAF block. Keep the failed
                // request queued without consuming its ordinary retry budget.
                // The global gate means only one queued request probes Tracker
                // after each cooldown expires.
                m_rateLimitedUntil = retryAt;
                req.notBefore = retryAt;
                m_queue.push_front(std::move(req));
            }
            std::cout
                << "[MMRFetcher] Tracker.gg blocked requests (HTTP 403). Circuit breaker strike "
                << strike << "; pausing all Tracker requests for "
                << std::chrono::duration_cast<std::chrono::seconds>(lockout).count()
                << " seconds.\n";
            m_cv.notify_one();
            return true;
        } else if (httpCode == 429) {
            const auto retryDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(headerState.retryAfterSeconds));
            const auto lockout = (retryDuration > kRateLimitLockoutMinimum) ? retryDuration : kRateLimitLockoutMinimum;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_rateLimitedUntil = std::chrono::steady_clock::now() + lockout;
            }
            std::cout << "[MMRFetcher] Rate limited by Tracker.gg (HTTP 429). Pausing requests for "
                      << std::chrono::duration_cast<std::chrono::seconds>(lockout).count() << " seconds.\n";
            if (const auto requeued = localCustomApiFallback()) return *requeued;
            if (ScheduleRetry(req, lockout, "rate limited")) return true;
        } else if (res != 0 || httpCode == 408 || (httpCode >= 500 && httpCode <= 599)) {
            if (ScheduleRetry(req, kTransientRetryDelay, "transient Tracker failure")) return true;
        }

        if (const auto requeued = localCustomApiFallback()) return *requeued;
        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }

    bool recoveredFromForbidden = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        recoveredFromForbidden = m_forbiddenStrikeCount > 0;
        m_forbiddenStrikeCount = 0;
        m_rateLimitedUntil = {};
    }
    if (recoveredFromForbidden) {
        m_useCustomApiFallback.store(false);
        std::cout
            << "[MMRFetcher] Tracker.gg requests recovered; circuit breaker reset.\n";
    }

    try {
        const nlohmann::json jsonResp = nlohmann::json::parse(readBuffer);
        const MMRProfileTotals profileTotals = ExtractProfileTotals(jsonResp);
        if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
            !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
            if (ScheduleRetry(req, kTransientRetryDelay, "incomplete Tracker response")) return true;
            if (const auto requeued = localCustomApiFallback()) return *requeued;
            return false;
        }

        int bestMMR = 0;
        std::string bestTier = "Unranked";
        std::string bestPlaylistName = "best";
        std::map<std::string, int> playlistMMRs;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;

        for (const auto& seg : jsonResp["data"]["segments"]) {
            if (!seg.is_object() || !seg.contains("type") || !seg["type"].is_string() || seg["type"] != "playlist" ||
                !seg.contains("attributes") || !seg["attributes"].is_object()) {
                continue;
            }

            const auto& attrs = seg["attributes"];
            if (!attrs.contains("playlistId") || !attrs["playlistId"].is_number_integer()) continue;

            const std::string playlistName = PlaylistNameForTrackerId(attrs["playlistId"].get<int>());
            if (playlistName.empty() || !seg.contains("stats") || !seg["stats"].is_object()) continue;

            const auto& stats = seg["stats"];
            if (!stats.contains("rating") || !stats["rating"].is_object()) continue;
            const auto& rating = stats["rating"];
            if (!rating.contains("value") || !rating["value"].is_number()) continue;

            const int mmr = rating["value"].get<int>();
            std::string tier = "Unranked";
            if (stats.contains("tier") && stats["tier"].is_object()) {
                const auto& tierObj = stats["tier"];
                if (tierObj.contains("metadata") && tierObj["metadata"].is_object() &&
                    tierObj["metadata"].contains("name") && tierObj["metadata"]["name"].is_string()) {
                    tier = tierObj["metadata"]["name"].get<std::string>();
                }
            }

            std::string division;
            if (stats.contains("division") && stats["division"].is_object()) {
                const auto& divObj = stats["division"];
                if (divObj.contains("metadata") && divObj["metadata"].is_object() &&
                    divObj["metadata"].contains("name") && divObj["metadata"]["name"].is_string()) {
                    division = divObj["metadata"]["name"].get<std::string>();
                }
                if (division.empty() && divObj.contains("displayValue") && divObj["displayValue"].is_string()) {
                    division = divObj["displayValue"].get<std::string>();
                }
                if (division.empty() && divObj.contains("value") && divObj["value"].is_number()) {
                    switch (divObj["value"].get<int>()) {
                    case 1:
                        division = "Div I";
                        break;
                    case 2:
                        division = "Div II";
                        break;
                    case 3:
                        division = "Div III";
                        break;
                    case 4:
                        division = "Div IV";
                        break;
                    default:
                        break;
                    }
                }
            }

            if (!division.empty()) {
                const size_t pos = division.find("Division ");
                if (pos != std::string::npos) division.replace(pos, 9, "Div ");
                if (tier != "Unranked") tier += " " + division;
            }
            if (playlistName == "t") tier = GetTournamentTierForMmr(mmr);

            int matches = -1;
            if (stats.contains("matchesPlayed") && stats["matchesPlayed"].is_object() &&
                stats["matchesPlayed"].contains("value") && stats["matchesPlayed"]["value"].is_number()) {
                matches = stats["matchesPlayed"]["value"].get<int>();
            }

            if (ShouldReplacePlaylistBucket(playlistMMRs, playlistName, mmr)) {
                playlistMMRs[playlistName] = mmr;
                playlistTiers[playlistName] = tier;
            }
            if (matches >= 0) playlistMatches[playlistName] += matches;

            if (playlistName != "casual" && playlistName != "t" && mmr > bestMMR) {
                bestMMR = mmr;
                bestTier = tier;
                bestPlaylistName = playlistName;
            }
        }

        playlistMMRs["best"] = bestMMR;
        playlistTiers["best"] = bestTier;
        if (playlistMatches.count(bestPlaylistName)) {
            playlistMatches["best"] = playlistMatches[bestPlaylistName];
        }
        NormalizedProfileResult profile;
        profile.bestMmr = bestMMR;
        profile.bestTier = bestTier;
        profile.bestPlaylistName = bestPlaylistName;
        profile.playlistMMRs = std::move(playlistMMRs);
        profile.playlistTiers = std::move(playlistTiers);
        profile.playlistMatches = std::move(playlistMatches);
        profile.totalWins = profileTotals.totalWins;
        profile.rankVerificationSource = "Tracker";

        return PublishProfileResult(req, profile);
    } catch (const std::exception& e) {
        std::cout << "[MMRFetcher] JSON Parse Error: " << e.what() << "\n";
        if (ScheduleRetry(req, kTransientRetryDelay, "invalid Tracker response")) return true;
        if (const auto requeued = localCustomApiFallback()) return *requeued;

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }
}

#ifdef OMNISTATS_TEST_ENVIRONMENT
std::vector<MMRFetcherDetail::BatchPlayerResult> MMRFetcher::FetchBatchFromCustomApiForTests(const MMRFetcherDetail::AssembledBatch& batch) {
    auto res = FetchBatchFromCustomApi(batch);
    return res.playerResults;
}

void MMRFetcher::ProcessCustomApiBatchForTests(const MMRFetcherDetail::AssembledBatch& batch) {
    ProcessCustomApiBatch(batch);
}

bool MMRFetcher::IsCustomApiActiveSourceForTests() const {
    return IsCustomApiActiveSource();
}
#endif
