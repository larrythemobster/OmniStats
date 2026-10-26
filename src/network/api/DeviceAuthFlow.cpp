#include "network/api/DeviceAuthFlow.hpp"
#include "network/api/DeviceIdentity.hpp"
#include "network/api/TokenStore.hpp"
#include "core/config/AppVersion.hpp"
#include "core/config/Config.hpp"
#include "network/http/HttpSecurity.hpp"
#include "network/omninet/OmniNet.hpp"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <thread>

namespace {

    std::string TrimWhitespace(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

    struct ParsedApiError {
        std::string code;
        std::string message;
        int retryAfterSeconds = 0;
    };

    ParsedApiError ParseApiError(const std::string& body) {
        ParsedApiError out;
        if (body.empty()) return out;
        try {
            const auto j = nlohmann::json::parse(body);
            if (!j.is_object()) return out;
            if (j.contains("error")) {
                const auto& err = j["error"];
                if (err.is_object()) {
                    if (err.contains("code") && err["code"].is_string()) {
                        out.code = err["code"].get<std::string>();
                    }
                    if (err.contains("message") && err["message"].is_string()) {
                        out.message = err["message"].get<std::string>();
                    }
                    if (err.contains("retry_after_seconds") && err["retry_after_seconds"].is_number_integer()) {
                        out.retryAfterSeconds = err["retry_after_seconds"].get<int>();
                    }
                } else if (err.is_string()) {
                    out.code = err.get<std::string>();
                }
            }
            if (out.message.empty() && j.contains("error_description") && j["error_description"].is_string()) {
                out.message = j["error_description"].get<std::string>();
            }
        } catch (...) {
        }
        return out;
    }

    std::string ExtractDisplayName(const nlohmann::json& j, const std::string& fallback) {
        if (j.is_object() && j.contains("account") && j["account"].is_object()) {
            const auto& acct = j["account"];
            if (acct.contains("display_name") && acct["display_name"].is_string()) {
                std::string name = TrimWhitespace(acct["display_name"].get<std::string>());
                if (!name.empty()) return name;
            }
            if (acct.contains("email") && acct["email"].is_string()) {
                std::string email = TrimWhitespace(acct["email"].get<std::string>());
                if (!email.empty()) return email;
            }
        }
        return fallback.empty() ? "OmniStats User" : fallback;
    }

} // namespace

DeviceAuthFlow::DeviceAuthFlow() = default;

std::string DeviceAuthFlow::SiteBaseUrl() {
    return "https://omnistats.org";
}

std::string DeviceAuthFlow::ManageDevicesUrl() {
    return SiteBaseUrl() + "/account/devices";
}

std::string DeviceAuthFlow::ResolveVerificationUri(std::string_view rawUri) {
    std::string uri = TrimWhitespace(rawUri);
    if (uri.empty()) {
        return SiteBaseUrl() + "/device";
    }
    if (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0) {
        const auto parsed = HttpSecurity::ParseUrl(uri);
        std::string hostLower = parsed.host;
        for (char& c : hostLower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (parsed.valid && parsed.scheme == "https" && hostLower == "omnistats.org") {
            return uri;
        }
        return SiteBaseUrl() + "/device";
    }
    if (uri.rfind("/device/device?", 0) == 0) {
        uri.erase(0, 7);
    }
    if (uri.front() != '/') {
        uri.insert(uri.begin(), '/');
    }
    return SiteBaseUrl() + uri;
}

int64_t DeviceAuthFlow::NowUnix() const {
    if (m_clock) return m_clock();
    return static_cast<int64_t>(std::time(nullptr));
}

bool DeviceAuthFlow::SleepInterruptible(int seconds) {
    if (m_sleep) return m_sleep(seconds);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_cancelRequested) return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

void DeviceAuthFlow::OpenUrl(const std::string& url) {
    if (m_openUrl) {
        m_openUrl(url);
        return;
    }
    const auto parsed = HttpSecurity::ParseUrl(url);
    if (!parsed.valid || (parsed.scheme != "https" && !(parsed.scheme == "http" && HttpSecurity::IsLoopbackHost(parsed.host)))) {
        return;
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (len <= 0) return;
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wide.data(), len);
    ShellExecuteW(nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

DeviceAuthFlow::StartAuthResult DeviceAuthFlow::StartAuthorization(DeviceIdentity& identity,
                                                                   std::function<bool()> shouldAbort) {
    StartAuthResult result;
    DeviceIdentity::DeviceKeyPair keyPair;
    if (!identity.EnsureDeviceKey(&keyPair)) {
        result.errorCode = "key_error";
        result.errorMessage = "Unable to initialize the local device key.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    const nlohmann::json reqJson = {
        {"device_public_id", keyPair.publicId},
        {"device_name", DeviceIdentity::LocalDeviceName()},
        {"platform", "windows"},
        {"app_version", AppVersion::Current},
        {"public_key", keyPair.publicKeyBase64Url}};

    OmniNet::Request omniReq;
    omniReq.op = OmniNet::Op::AuthStart;
    omniReq.payload = reqJson.dump();
    omniReq.shouldAbort = std::move(shouldAbort);

    const OmniNet::Response resp = OmniNet::Send(omniReq);
    if (resp.updateRequired) {
        result.errorCode = "update_required";
        result.errorMessage = "This version of OmniStats is out of date. Update to sign in.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }
    if (resp.transportError != 0) {
        result.errorCode = "network_error";
        result.errorMessage = "Unable to reach OmniStats to start sign-in.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    if (resp.status != 200) {
        const ParsedApiError apiErr = ParseApiError(resp.body);
        result.errorCode = apiErr.code.empty() ? "http_" + std::to_string(resp.status) : apiErr.code;
        result.errorMessage = apiErr.message.empty()
                                  ? "Unable to start sign-in (HTTP " + std::to_string(resp.status) + ")."
                                  : apiErr.message;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        result.deviceCode = j.value("device_code", "");
        result.userCode = j.value("user_code", "");
        result.verificationUri = ResolveVerificationUri(j.value("verification_uri", ""));
        result.expiresIn = (std::max)(1, j.value("expires_in", 900));
        result.pollInterval = (std::max)(1, j.value("poll_interval", 5));
    } catch (...) {
        result.errorCode = "invalid_response";
        result.errorMessage = "Received an invalid sign-in response from OmniStats.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    if (result.deviceCode.empty() || result.userCode.empty()) {
        result.errorCode = "invalid_response";
        result.errorMessage = "Received an incomplete sign-in response from OmniStats.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    const int64_t now = NowUnix();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cancelRequested = false;
        m_deviceCode = result.deviceCode;
        m_userCode = result.userCode;
        m_verificationUri = result.verificationUri;
        m_expiresAtUnix = now + result.expiresIn;
        m_pollIntervalSeconds = result.pollInterval;
        m_errorMessage.clear();
        m_state = AccountAuthState::AwaitingApproval;
        ++m_statusVersion;
    }

    result.ok = true;
    return result;
}

DeviceAuthFlow::PollResult DeviceAuthFlow::PollTokenOnce(DeviceIdentity& identity,
                                                         TokenStore& tokenStore,
                                                         std::function<bool()> shouldAbort) {
    PollResult result;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelRequested) {
            result.outcome = PollOutcome::Cancelled;
            return result;
        }
    }

    std::string deviceCode;
    int64_t expiresAtUnix = 0;
    int pollInterval = 5;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        deviceCode = m_deviceCode;
        expiresAtUnix = m_expiresAtUnix;
        pollInterval = m_pollIntervalSeconds;
    }
    result.pollIntervalSeconds = pollInterval;

    if (deviceCode.empty()) {
        result.outcome = PollOutcome::Error;
        result.errorCode = "no_pending_authorization";
        result.errorMessage = "No device sign-in request is active.";
        return result;
    }

    const int64_t now = NowUnix();
    if (expiresAtUnix > 0 && now >= expiresAtUnix) {
        result.outcome = PollOutcome::Expired;
        result.errorCode = "expired";
        result.errorMessage = "The sign-in code expired. Start a new sign-in request.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
        return result;
    }

    DeviceIdentity::DeviceKeyPair keyPair;
    if (!identity.EnsureDeviceKey(&keyPair)) {
        result.outcome = PollOutcome::Error;
        result.errorCode = "key_error";
        result.errorMessage = "Unable to read the local device key.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
        return result;
    }

    const DeviceIdentity::DeviceProof proof = identity.CreateProof(keyPair.secretKeyBase64Url, "token", keyPair.publicId, deviceCode, now);
    const nlohmann::json reqJson = {
        {"device_code", deviceCode},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    OmniNet::Request omniReq;
    omniReq.op = OmniNet::Op::AuthToken;
    omniReq.payload = reqJson.dump();
    omniReq.shouldAbort = shouldAbort;

    const OmniNet::Response resp = OmniNet::Send(omniReq);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cancelRequested) {
            result.outcome = PollOutcome::Cancelled;
            return result;
        }
    }

    if (resp.updateRequired) {
        result.outcome = PollOutcome::Error;
        result.errorCode = "update_required";
        result.errorMessage = "This version of OmniStats is out of date. Update to sign in.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
        return result;
    }

    if (resp.transportError != 0 || (resp.status >= 500 && resp.status <= 599)) {
        result.outcome = PollOutcome::Pending;
        return result;
    }

    if (resp.status == 200) {
        try {
            const auto j = nlohmann::json::parse(resp.body);
            const std::string accessToken = j.value("access_token", "");
            const std::string refreshToken = j.value("refresh_token", "");
            const int expiresIn = (std::max)(1, j.value("expires_in", 900));
            const std::string displayName = ExtractDisplayName(j, "");
            if (accessToken.empty() || refreshToken.empty()) {
                throw std::runtime_error("missing token");
            }

            tokenStore.SetTokens(accessToken, now + expiresIn, refreshToken, displayName, keyPair.publicId);

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_deviceCode.clear();
                m_userCode.clear();
                m_verificationUri.clear();
                m_errorMessage.clear();
                m_state = AccountAuthState::SignedIn;
                ++m_statusVersion;
            }

            result.outcome = PollOutcome::Approved;
            result.displayName = displayName;
            return result;
        } catch (...) {
            result.outcome = PollOutcome::Error;
            result.errorCode = "invalid_response";
            result.errorMessage = "Received an invalid token response from OmniStats.";
            std::lock_guard<std::mutex> lock(m_mutex);
            m_deviceCode.clear();
            m_errorMessage = result.errorMessage;
            m_state = AccountAuthState::Error;
            ++m_statusVersion;
            return result;
        }
    }

    const ParsedApiError apiErr = ParseApiError(resp.body);
    result.errorCode = apiErr.code;
    result.errorMessage = apiErr.message;

    if (apiErr.code == "authorization_pending" || resp.status == 428) {
        result.outcome = PollOutcome::Pending;
        return result;
    }

    if (apiErr.code == "slow_down" || resp.status == 429 || apiErr.code == "rate_limited") {
        int newInterval = pollInterval + 5;
        if (apiErr.retryAfterSeconds > newInterval) {
            newInterval = apiErr.retryAfterSeconds;
        }
        if (resp.retryAfterSeconds > newInterval) {
            newInterval = static_cast<int>(resp.retryAfterSeconds);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pollIntervalSeconds = newInterval;
        }
        result.outcome = PollOutcome::SlowDown;
        result.pollIntervalSeconds = newInterval;
        return result;
    }

    if (apiErr.code == "access_denied" || apiErr.code == "denied") {
        result.outcome = PollOutcome::Denied;
        if (result.errorMessage.empty()) {
            result.errorMessage = "The device sign-in request was denied.";
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
        return result;
    }

    if (apiErr.code == "invalid_device_authorization" || apiErr.code == "expired" || apiErr.code == "expired_token") {
        result.outcome = PollOutcome::Expired;
        if (result.errorMessage.empty()) {
            result.errorMessage = "The sign-in code expired. Start a new sign-in request.";
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
        return result;
    }

    result.outcome = PollOutcome::Error;
    if (result.errorMessage.empty()) {
        result.errorMessage = "Device sign-in failed (HTTP " + std::to_string(resp.status) + ").";
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage = result.errorMessage;
        m_state = AccountAuthState::Error;
        ++m_statusVersion;
    }
    return result;
}

DeviceAuthFlow::PollResult DeviceAuthFlow::PollToken(DeviceIdentity& identity,
                                                     TokenStore& tokenStore,
                                                     std::function<bool()> shouldAbort) {
    while (true) {
        int sleepSeconds = 5;
        int64_t expiresAtUnix = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sleepSeconds = (std::max)(1, m_pollIntervalSeconds);
            expiresAtUnix = m_expiresAtUnix;
        }
        const int64_t now = NowUnix();
        if (expiresAtUnix > 0 && now >= expiresAtUnix) {
            return PollTokenOnce(identity, tokenStore, shouldAbort);
        }
        if (!SleepInterruptible(sleepSeconds)) {
            PollResult cancelled;
            cancelled.outcome = PollOutcome::Cancelled;
            return cancelled;
        }
        PollResult step = PollTokenOnce(identity, tokenStore, shouldAbort);
        if (step.outcome != PollOutcome::Pending && step.outcome != PollOutcome::SlowDown) {
            return step;
        }
    }
}

void DeviceAuthFlow::CancelAuthorization() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelRequested = true;
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    m_errorMessage.clear();
    m_state = AccountAuthState::SignedOut;
    ++m_statusVersion;
}

void DeviceAuthFlow::SetState(AccountAuthState state, std::string errorMessage) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
    m_errorMessage = std::move(errorMessage);
    ++m_statusVersion;
}

void DeviceAuthFlow::MarkSignedOut(std::string reason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelRequested = true;
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    m_errorMessage = std::move(reason);
    if (!m_errorMessage.empty()) {
        m_pendingSignedOutToast = m_errorMessage;
        m_state = AccountAuthState::Error;
    } else {
        m_state = AccountAuthState::SignedOut;
    }
    ++m_statusVersion;
}

bool DeviceAuthFlow::ConsumeSignedOutNotification(std::string& outReason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pendingSignedOutToast.empty()) return false;
    outReason = std::move(m_pendingSignedOutToast);
    m_pendingSignedOutToast.clear();
    return true;
}

void DeviceAuthFlow::OpenVerificationBrowser() {
    std::string uri;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uri = m_verificationUri;
    }
    if (uri.empty()) uri = SiteBaseUrl() + "/device";
    OpenUrl(uri);
}

void DeviceAuthFlow::OpenManageDevicesBrowser() {
    OpenUrl(ManageDevicesUrl());
}

AccountStatusSnapshot DeviceAuthFlow::GetStatus(const TokenStore& tokenStore) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    AccountStatusSnapshot snap;
    snap.state = m_state;
    if (snap.state == AccountAuthState::SignedOut && tokenStore.IsSignedIn()) {
        snap.state = AccountAuthState::SignedIn;
    }
    snap.displayName = tokenStore.DisplayName();
    snap.devicePublicId = tokenStore.DevicePublicId();
    snap.userCode = m_userCode;
    snap.verificationUri = m_verificationUri;
    snap.expiresAtUnix = m_expiresAtUnix;
    snap.pollIntervalSeconds = m_pollIntervalSeconds;
    snap.errorMessage = m_errorMessage;
    snap.version = m_statusVersion;
    return snap;
}

void DeviceAuthFlow::SetClockForTests(ClockFn clockFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = std::move(clockFn);
}

void DeviceAuthFlow::SetSleepForTests(SleepFn sleepFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sleep = std::move(sleepFn);
}

void DeviceAuthFlow::SetOpenUrlForTests(OpenUrlFn openUrlFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openUrl = std::move(openUrlFn);
}

void DeviceAuthFlow::ResetForTests() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = AccountAuthState::SignedOut;
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    m_expiresAtUnix = 0;
    m_pollIntervalSeconds = 5;
    m_errorMessage.clear();
    m_pendingSignedOutToast.clear();
    m_cancelRequested = false;
    m_clock = nullptr;
    m_sleep = nullptr;
    m_openUrl = nullptr;
    ++m_statusVersion;
}
