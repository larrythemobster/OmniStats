#include "network/AccountClient.hpp"

#include "core/AppVersion.hpp"
#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "network/HttpSecurity.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string_view>

namespace {
    struct CurlHeaderState {
        long retryAfterSeconds = 0;
        std::string location;
    };

    size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        const size_t total = size * nmemb;
        if (userp && contents && total > 0) {
            static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), total);
        }
        return total;
    }

    size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
        const size_t total = size * nitems;
        if (!userdata || !buffer) return total;
        auto* state = static_cast<CurlHeaderState*>(userdata);

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
                    const long parsed = std::stol(std::string(val));
                    if (parsed > 0) state->retryAfterSeconds = parsed;
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

    int CurlProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto* stopFlag = static_cast<std::atomic<bool>*>(clientp);
        if (stopFlag && stopFlag->load(std::memory_order_relaxed)) {
            return 1;
        }
        return 0;
    }

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

AccountClient& AccountClient::Instance() {
    static AccountClient instance;
    return instance;
}

bool AccountClient::EnsureSodiumInitialized() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, []() {
        initialized = (sodium_init() >= 0);
    });
    return initialized;
}

AccountClient::AccountClient() {
    EnsureSodiumInitialized();
    SyncFromConfig();
}

AccountClient::~AccountClient() {
    Shutdown();
}

std::string AccountClient::Base64UrlEncode(const uint8_t* data, size_t len) {
    EnsureSodiumInitialized();
    if (!data || len == 0) return {};
    const size_t maxLen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string encoded(maxLen, '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    encoded.resize(std::strlen(encoded.c_str()));
    return encoded;
}

bool AccountClient::Base64UrlDecode(std::string_view encoded, std::vector<uint8_t>& out) {
    EnsureSodiumInitialized();
    if (encoded.empty()) {
        out.clear();
        return false;
    }
    out.assign(encoded.size(), 0);
    size_t binLen = 0;
    if (sodium_base642bin(out.data(),
                          out.size(),
                          encoded.data(),
                          encoded.size(),
                          nullptr,
                          &binLen,
                          nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        out.clear();
        return false;
    }
    out.resize(binLen);
    return true;
}

std::string AccountClient::Sha256Hex(std::string_view input) {
    EnsureSodiumInitialized();
    std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    return std::string(hex.data(), crypto_hash_sha256_BYTES * 2);
}

std::string AccountClient::DerivePublicIdForPublicKey(std::string_view publicKeyBase64Url) {
    std::vector<uint8_t> raw;
    if (!Base64UrlDecode(publicKeyBase64Url, raw) || raw.size() != crypto_sign_ed25519_PUBLICKEYBYTES) {
        return {};
    }
    std::array<uint8_t, crypto_hash_sha256_BYTES> sum{};
    crypto_hash_sha256(sum.data(), raw.data(), raw.size());
    return Base64UrlEncode(sum.data(), sum.size());
}

AccountClient::DeviceKeyPair AccountClient::DeriveKeyPairFromSeed(const std::array<uint8_t, 32>& seed) {
    EnsureSodiumInitialized();
    std::array<uint8_t, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
    std::array<uint8_t, crypto_sign_ed25519_SECRETKEYBYTES> sk{};
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), seed.data());

    DeviceKeyPair pair;
    pair.publicKeyBase64Url = Base64UrlEncode(pk.data(), pk.size());
    pair.secretKeyBase64Url = Base64UrlEncode(seed.data(), seed.size());
    pair.publicId = DerivePublicIdForPublicKey(pair.publicKeyBase64Url);
    sodium_memzero(sk.data(), sk.size());
    return pair;
}

bool AccountClient::DeriveKeyPairFromSecret(std::string_view secretKeyBase64Url, DeviceKeyPair& out) {
    std::vector<uint8_t> raw;
    if (!Base64UrlDecode(secretKeyBase64Url, raw)) {
        return false;
    }
    if (raw.size() != crypto_sign_ed25519_SEEDBYTES && raw.size() != crypto_sign_ed25519_SECRETKEYBYTES) {
        sodium_memzero(raw.data(), raw.size());
        return false;
    }
    std::array<uint8_t, 32> seed{};
    std::memcpy(seed.data(), raw.data(), seed.size());
    sodium_memzero(raw.data(), raw.size());
    out = DeriveKeyPairFromSeed(seed);
    sodium_memzero(seed.data(), seed.size());
    return !out.publicId.empty();
}

std::string AccountClient::BuildProofCanonical(std::string_view operation,
                                               std::string_view publicId,
                                               int64_t timestamp,
                                               std::string_view nonceBase64Url,
                                               std::string_view credential) {
    std::string out;
    out.reserve(160 + publicId.size() + nonceBase64Url.size());
    out.append("OMNISTATS-DEVICE-PROOF-v1\n");
    out.append(operation);
    out.push_back('\n');
    out.append(publicId);
    out.push_back('\n');
    out.append(std::to_string(timestamp));
    out.push_back('\n');
    out.append(nonceBase64Url);
    out.push_back('\n');
    out.append(Sha256Hex(credential));
    return out;
}

AccountClient::DeviceProof AccountClient::SignProofDeterministic(std::string_view secretKeyBase64Url,
                                                                 std::string_view operation,
                                                                 std::string_view publicId,
                                                                 int64_t timestamp,
                                                                 std::string_view nonceBase64Url,
                                                                 std::string_view credential) {
    EnsureSodiumInitialized();
    DeviceProof proof;
    std::vector<uint8_t> rawSecret;
    if (!Base64UrlDecode(secretKeyBase64Url, rawSecret)) {
        return proof;
    }
    if (rawSecret.size() != crypto_sign_ed25519_SEEDBYTES && rawSecret.size() != crypto_sign_ed25519_SECRETKEYBYTES) {
        sodium_memzero(rawSecret.data(), rawSecret.size());
        return proof;
    }

    std::array<uint8_t, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
    std::array<uint8_t, crypto_sign_ed25519_SECRETKEYBYTES> sk{};
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), rawSecret.data());
    sodium_memzero(rawSecret.data(), rawSecret.size());

    proof.timestamp = timestamp;
    proof.nonce = std::string(nonceBase64Url);
    proof.canonical = BuildProofCanonical(operation, publicId, timestamp, nonceBase64Url, credential);

    std::array<uint8_t, crypto_sign_ed25519_BYTES> sig{};
    crypto_sign_ed25519_detached(sig.data(),
                                 nullptr,
                                 reinterpret_cast<const unsigned char*>(proof.canonical.data()),
                                 proof.canonical.size(),
                                 sk.data());
    sodium_memzero(sk.data(), sk.size());
    proof.signature = Base64UrlEncode(sig.data(), sig.size());
    return proof;
}

AccountClient::DeviceProof AccountClient::CreateProof(std::string_view secretKeyBase64Url,
                                                      std::string_view operation,
                                                      std::string_view publicId,
                                                      std::string_view credential,
                                                      int64_t timestamp) {
    EnsureSodiumInitialized();
    std::array<uint8_t, 32> nonceBytes{};
    randombytes_buf(nonceBytes.data(), nonceBytes.size());
    const std::string nonceEncoded = Base64UrlEncode(nonceBytes.data(), nonceBytes.size());
    return SignProofDeterministic(secretKeyBase64Url, operation, publicId, timestamp, nonceEncoded, credential);
}

std::string AccountClient::ApiBaseUrl() {
    std::string url = TrimWhitespace(Config::Read().custom_api_base_url);
    if (url.empty()) {
        url = "https://api.omnistats.org";
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

std::string AccountClient::SiteBaseUrl(std::string_view apiBaseUrl) {
    std::string url = apiBaseUrl.empty() ? ApiBaseUrl() : TrimWhitespace(apiBaseUrl);
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (url.empty()) {
        return "https://omnistats.org";
    }
    constexpr std::string_view kHttpsApi = "https://api.";
    constexpr std::string_view kHttpApi = "http://api.";
    if (url.rfind(kHttpsApi, 0) == 0 && url.size() > kHttpsApi.size()) {
        return "https://" + url.substr(kHttpsApi.size());
    }
    if (url.rfind(kHttpApi, 0) == 0 && url.size() > kHttpApi.size()) {
        return "http://" + url.substr(kHttpApi.size());
    }
    return url;
}

std::string AccountClient::ManageDevicesUrl() {
    return SiteBaseUrl() + "/account/devices";
}

std::string AccountClient::ResolveVerificationUri(std::string_view rawUri, std::string_view apiBaseUrl) {
    std::string uri = TrimWhitespace(rawUri);
    if (uri.empty()) {
        return SiteBaseUrl(apiBaseUrl) + "/device";
    }
    if (uri.rfind("https://", 0) == 0 || uri.rfind("http://", 0) == 0) {
        return uri;
    }
    if (uri.rfind("/device/device?", 0) == 0) {
        uri.erase(0, 7);
    }
    if (uri.front() != '/') {
        uri.insert(uri.begin(), '/');
    }
    return SiteBaseUrl(apiBaseUrl) + uri;
}

std::string AccountClient::LocalDeviceName() {
    wchar_t buffer[256] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, buffer, &size) || size == 0) {
        size = static_cast<DWORD>(std::size(buffer));
        if (!GetComputerNameW(buffer, &size) || size == 0) {
            return "Windows PC";
        }
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "Windows PC";
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), utf8.data(), needed, nullptr, nullptr);
    utf8 = TrimWhitespace(utf8);
    if (utf8.empty()) return "Windows PC";
    if (utf8.size() > 100) utf8.resize(100);
    return utf8;
}

bool AccountClient::EnsureDeviceKey(DeviceKeyPair* outKey) {
    EnsureSodiumInitialized();
    const ConfigData conf = Config::Read();
    DeviceKeyPair pair;
    if (!conf.account_device_key.empty() && DeriveKeyPairFromSecret(conf.account_device_key, pair)) {
        if (conf.account_device_public_id != pair.publicId) {
            Config::Update([&](ConfigData& c) {
                c.account_device_public_id = pair.publicId;
            });
            Config::Save();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_devicePublicId = pair.publicId;
        }
        if (outKey) *outKey = pair;
        return true;
    }

    std::array<uint8_t, 32> seed{};
    randombytes_buf(seed.data(), seed.size());
    pair = DeriveKeyPairFromSeed(seed);
    sodium_memzero(seed.data(), seed.size());
    if (pair.publicId.empty()) return false;

    Config::Update([&](ConfigData& c) {
        c.account_device_key = pair.secretKeyBase64Url;
        c.account_device_public_id = pair.publicId;
    });
    Config::Save();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_devicePublicId = pair.publicId;
    }
    if (outKey) *outKey = pair;
    return true;
}

AccountClient::StartAuthResult AccountClient::StartAuthorization() {
    StartAuthResult result;
    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) {
        result.errorCode = "key_error";
        result.errorMessage = "Unable to initialize the local device key.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    const std::string baseUrl = ApiBaseUrl();
    const nlohmann::json reqJson = {
        {"device_public_id", keyPair.publicId},
        {"device_name", LocalDeviceName()},
        {"platform", "windows"},
        {"app_version", AppVersion::Current},
        {"public_key", keyPair.publicKeyBase64Url}};

    const HttpResponse resp = PerformHttpPost(baseUrl + "/v1/auth/device/start", reqJson.dump());
    if (resp.curlCode != 0) {
        result.errorCode = "network_error";
        result.errorMessage = "Unable to reach OmniStats to start sign-in.";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = AccountAuthState::Error;
        m_errorMessage = result.errorMessage;
        ++m_statusVersion;
        return result;
    }

    if (resp.statusCode != 200) {
        const ParsedApiError apiErr = ParseApiError(resp.body);
        result.errorCode = apiErr.code.empty() ? "http_" + std::to_string(resp.statusCode) : apiErr.code;
        result.errorMessage = apiErr.message.empty()
                                  ? "Unable to start sign-in (HTTP " + std::to_string(resp.statusCode) + ")."
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
        result.verificationUri = ResolveVerificationUri(j.value("verification_uri", ""), baseUrl);
        result.expiresIn = std::max(1, j.value("expires_in", 900));
        result.pollInterval = std::max(1, j.value("poll_interval", 5));
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
        m_cancelPolling.store(false, std::memory_order_relaxed);
        m_devicePublicId = keyPair.publicId;
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

AccountClient::PollResult AccountClient::PollTokenOnce() {
    PollResult result;
    if (m_cancelPolling.load(std::memory_order_relaxed) || m_stopWorker.load(std::memory_order_relaxed)) {
        result.outcome = PollOutcome::Cancelled;
        return result;
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

    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) {
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

    const DeviceProof proof = CreateProof(keyPair.secretKeyBase64Url, "token", keyPair.publicId, deviceCode, now);
    const nlohmann::json reqJson = {
        {"device_code", deviceCode},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    const HttpResponse resp = PerformHttpPost(ApiBaseUrl() + "/v1/auth/device/token", reqJson.dump());
    if (m_cancelPolling.load(std::memory_order_relaxed) || m_stopWorker.load(std::memory_order_relaxed)) {
        result.outcome = PollOutcome::Cancelled;
        return result;
    }

    if (resp.curlCode != 0 || (resp.statusCode >= 500 && resp.statusCode <= 599)) {
        result.outcome = PollOutcome::Pending;
        return result;
    }

    if (resp.statusCode == 200) {
        try {
            const auto j = nlohmann::json::parse(resp.body);
            const std::string accessToken = j.value("access_token", "");
            const std::string refreshToken = j.value("refresh_token", "");
            const int expiresIn = std::max(1, j.value("expires_in", 900));
            const std::string displayName = ExtractDisplayName(j, "");
            if (accessToken.empty() || refreshToken.empty()) {
                throw std::runtime_error("missing token");
            }

            Config::Update([&](ConfigData& c) {
                c.account_refresh_token = refreshToken;
                c.account_signed_in_name = displayName;
                c.account_device_public_id = keyPair.publicId;
            });
            Config::Save();

            const int64_t issuedAt = NowUnix();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_accessToken = accessToken;
                m_accessTokenExpiresAt = issuedAt + expiresIn;
                m_displayName = displayName;
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

    if (apiErr.code == "authorization_pending" || resp.statusCode == 428) {
        result.outcome = PollOutcome::Pending;
        return result;
    }

    if (apiErr.code == "slow_down" || resp.statusCode == 429 || apiErr.code == "rate_limited") {
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
        result.errorMessage = "Device sign-in failed (HTTP " + std::to_string(resp.statusCode) + ").";
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

AccountClient::PollResult AccountClient::PollToken() {
    while (true) {
        int sleepSeconds = 5;
        int64_t expiresAtUnix = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sleepSeconds = std::max(1, m_pollIntervalSeconds);
            expiresAtUnix = m_expiresAtUnix;
        }
        const int64_t now = NowUnix();
        if (expiresAtUnix > 0 && now >= expiresAtUnix) {
            return PollTokenOnce();
        }
        if (!SleepInterruptible(sleepSeconds)) {
            PollResult cancelled;
            cancelled.outcome = PollOutcome::Cancelled;
            return cancelled;
        }
        PollResult step = PollTokenOnce();
        if (step.outcome != PollOutcome::Pending && step.outcome != PollOutcome::SlowDown) {
            return step;
        }
    }
}

bool AccountClient::Refresh() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_refreshInFlight) {
            m_refreshCv.wait(lock, [this]() { return !m_refreshInFlight; });
            return m_lastRefreshSucceeded;
        }
        m_refreshInFlight = true;
    }

    auto finish = [&](bool ok) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastRefreshSucceeded = ok;
        m_refreshInFlight = false;
        m_refreshCv.notify_all();
        return ok;
    };

    const ConfigData conf = Config::Read();
    if (conf.account_refresh_token.empty()) {
        return finish(false);
    }

    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) {
        return finish(false);
    }

    const int64_t now = NowUnix();
    const DeviceProof proof = CreateProof(
        keyPair.secretKeyBase64Url, "refresh", keyPair.publicId, conf.account_refresh_token, now);
    const nlohmann::json reqJson = {
        {"refresh_token", conf.account_refresh_token},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    const HttpResponse resp = PerformHttpPost(ApiBaseUrl() + "/v1/auth/device/refresh", reqJson.dump());
    if (resp.curlCode != 0 || (resp.statusCode >= 500 && resp.statusCode <= 599) || resp.statusCode == 429) {
        return finish(false);
    }

    if (resp.statusCode == 401 || resp.statusCode == 403) {
        MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Integrations.");
        return finish(false);
    }

    if (resp.statusCode != 200) {
        return finish(false);
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        const std::string accessToken = j.value("access_token", "");
        const std::string newRefreshToken = j.value("refresh_token", "");
        const int expiresIn = std::max(1, j.value("expires_in", 900));
        const std::string displayName = ExtractDisplayName(j, conf.account_signed_in_name);
        if (accessToken.empty() || newRefreshToken.empty()) {
            return finish(false);
        }

        Config::Update([&](ConfigData& c) {
            c.account_refresh_token = newRefreshToken;
            c.account_signed_in_name = displayName;
            c.account_device_public_id = keyPair.publicId;
        });
        Config::Save();

        const int64_t issuedAt = NowUnix();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_accessToken = accessToken;
            m_accessTokenExpiresAt = issuedAt + expiresIn;
            m_displayName = displayName;
            m_errorMessage.clear();
            m_state = AccountAuthState::SignedIn;
            ++m_statusVersion;
        }
        return finish(true);
    } catch (...) {
        return finish(false);
    }
}

std::string AccountClient::AccessToken() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_refreshInFlight) {
            m_refreshCv.wait(lock, [this]() { return !m_refreshInFlight; });
        }
        const int64_t now = m_clock ? m_clock() : static_cast<int64_t>(std::time(nullptr));
        if (!m_accessToken.empty() && (m_accessTokenExpiresAt - now) > 60) {
            return m_accessToken;
        }
    }

    if (Config::Read().account_refresh_token.empty()) {
        return {};
    }

    if (!Refresh()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    return m_accessToken;
}

std::string AccountClient::DevicePublicId() {
    const ConfigData conf = Config::Read();
    if (!conf.account_device_public_id.empty()) {
        return conf.account_device_public_id;
    }
    DeviceKeyPair pair;
    if (EnsureDeviceKey(&pair)) {
        return pair.publicId;
    }
    return {};
}

bool AccountClient::Logout() {
    m_cancelPolling.store(true, std::memory_order_relaxed);
    m_workerCv.notify_all();

    const ConfigData conf = Config::Read();
    const std::string refreshToken = conf.account_refresh_token;

    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
    });
    Config::Save();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken.clear();
        m_accessTokenExpiresAt = 0;
        m_displayName.clear();
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage.clear();
        m_pendingSignedOutToast.clear();
        m_state = AccountAuthState::SignedOut;
        ++m_statusVersion;
    }

    if (refreshToken.empty()) {
        return true;
    }

    return SendLogoutRequest(refreshToken);
}

bool AccountClient::SendLogoutRequest(const std::string& refreshToken) {
    if (refreshToken.empty()) return true;
    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) {
        return false;
    }

    const int64_t now = NowUnix();
    const DeviceProof proof = CreateProof(keyPair.secretKeyBase64Url, "logout", keyPair.publicId, refreshToken, now);
    const nlohmann::json reqJson = {
        {"refresh_token", refreshToken},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    const HttpResponse resp = PerformHttpPost(ApiBaseUrl() + "/v1/auth/device/logout", reqJson.dump());
    return resp.curlCode == 0 && resp.statusCode == 200;
}

void AccountClient::BeginSignInAsync(bool openBrowser) {
    m_cancelPolling.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        EnsureWorkerRunningLocked();
        m_workerQueue.clear();
        m_workerQueue.push_back(WorkerJob{WorkerCommand::StartSignIn, openBrowser, {}});
    }
    m_workerCv.notify_all();
}

void AccountClient::CancelAuthorization() {
    m_cancelPolling.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage.clear();
        m_state = Config::Read().account_refresh_token.empty() ? AccountAuthState::SignedOut : AccountAuthState::SignedIn;
        ++m_statusVersion;
    }
    m_workerCv.notify_all();
}

void AccountClient::LogoutAsync() {
    m_cancelPolling.store(true, std::memory_order_relaxed);
    const std::string refreshToken = Config::Read().account_refresh_token;
    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
    });
    Config::Save();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken.clear();
        m_accessTokenExpiresAt = 0;
        m_displayName.clear();
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage.clear();
        m_pendingSignedOutToast.clear();
        m_state = AccountAuthState::SignedOut;
        ++m_statusVersion;
    }
    if (!refreshToken.empty()) {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        EnsureWorkerRunningLocked();
        m_workerQueue.push_back(WorkerJob{WorkerCommand::Logout, false, refreshToken});
    }
    m_workerCv.notify_all();
}

void AccountClient::OpenVerificationBrowser() {
    std::string uri;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        uri = m_verificationUri;
    }
    if (!uri.empty()) {
        OpenUrl(uri);
    }
}

void AccountClient::OpenManageDevicesBrowser() {
    OpenUrl(ManageDevicesUrl());
}

void AccountClient::MarkSignedOut(std::string reason) {
    m_cancelPolling.store(true, std::memory_order_relaxed);
    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
    });
    Config::Save();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accessToken.clear();
        m_accessTokenExpiresAt = 0;
        m_displayName.clear();
        m_deviceCode.clear();
        m_userCode.clear();
        m_verificationUri.clear();
        m_errorMessage = reason;
        if (!reason.empty()) {
            m_pendingSignedOutToast = reason;
            m_state = AccountAuthState::Error;
        } else {
            m_state = AccountAuthState::SignedOut;
        }
        ++m_statusVersion;
    }
}

bool AccountClient::ConsumeSignedOutNotification(std::string& outReason) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_pendingSignedOutToast.empty()) return false;
    outReason = std::move(m_pendingSignedOutToast);
    m_pendingSignedOutToast.clear();
    return true;
}

bool AccountClient::IsSignedIn() const {
    const ConfigData conf = Config::Read();
    if (conf.account_refresh_token.empty()) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state != AccountAuthState::AwaitingApproval;
}

AccountStatusSnapshot AccountClient::GetStatus() const {
    const ConfigData conf = Config::Read();
    std::lock_guard<std::mutex> lock(m_mutex);
    AccountStatusSnapshot snap;
    snap.version = m_statusVersion;
    snap.devicePublicId = conf.account_device_public_id.empty() ? m_devicePublicId : conf.account_device_public_id;
    snap.userCode = m_userCode;
    snap.verificationUri = m_verificationUri;
    snap.expiresAtUnix = m_expiresAtUnix;
    snap.pollIntervalSeconds = m_pollIntervalSeconds;
    snap.errorMessage = m_errorMessage;

    if (m_state == AccountAuthState::AwaitingApproval) {
        snap.state = AccountAuthState::AwaitingApproval;
    } else if (!conf.account_refresh_token.empty()) {
        snap.state = AccountAuthState::SignedIn;
        snap.displayName = conf.account_signed_in_name.empty()
                               ? (m_displayName.empty() ? "OmniStats User" : m_displayName)
                               : conf.account_signed_in_name;
    } else if (m_state == AccountAuthState::Error && !m_errorMessage.empty()) {
        snap.state = AccountAuthState::Error;
    } else {
        snap.state = AccountAuthState::SignedOut;
    }
    return snap;
}

void AccountClient::SyncFromConfig() {
    const ConfigData conf = Config::Read();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_devicePublicId = conf.account_device_public_id;
    m_displayName = conf.account_signed_in_name;
    if (!conf.account_refresh_token.empty()) {
        if (m_state != AccountAuthState::AwaitingApproval) {
            m_state = AccountAuthState::SignedIn;
            m_errorMessage.clear();
        }
    } else if (m_state == AccountAuthState::SignedIn) {
        m_state = AccountAuthState::SignedOut;
        m_accessToken.clear();
        m_accessTokenExpiresAt = 0;
    }
    ++m_statusVersion;
}

void AccountClient::Shutdown() {
    m_stopWorker.store(true, std::memory_order_relaxed);
    m_cancelPolling.store(true, std::memory_order_relaxed);
    m_workerCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    std::lock_guard<std::mutex> lock(m_workerMutex);
    m_workerQueue.clear();
    m_workerStarted = false;
}

void AccountClient::EnsureWorkerRunningLocked() {
    if (m_workerStarted) return;
    m_stopWorker.store(false, std::memory_order_relaxed);
    m_workerStarted = true;
    m_workerThread = std::jthread(&AccountClient::WorkerLoop, this);
}

void AccountClient::WorkerLoop() {
    while (!m_stopWorker.load(std::memory_order_relaxed)) {
        WorkerJob job;
        {
            std::unique_lock<std::mutex> lock(m_workerMutex);
            m_workerCv.wait(lock, [this]() {
                return m_stopWorker.load(std::memory_order_relaxed) || !m_workerQueue.empty();
            });
            if (m_stopWorker.load(std::memory_order_relaxed)) break;
            job = m_workerQueue.front();
            m_workerQueue.pop_front();
        }

        if (job.command == WorkerCommand::Logout) {
            (void)SendLogoutRequest(job.refreshToken);
        } else if (job.command == WorkerCommand::StartSignIn) {
            m_cancelPolling.store(false, std::memory_order_relaxed);
            const StartAuthResult startRes = StartAuthorization();
            if (startRes.ok && !m_cancelPolling.load(std::memory_order_relaxed) &&
                !m_stopWorker.load(std::memory_order_relaxed)) {
                if (job.openBrowser && !startRes.verificationUri.empty()) {
                    OpenUrl(startRes.verificationUri);
                }
                (void)PollToken();
            }
        }
    }
}

AccountClient::HttpResponse AccountClient::PerformHttpPost(const std::string& url, const std::string& jsonBody) {
    HttpTransportFn customTransport;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        customTransport = m_transport;
    }
    if (customTransport) {
        return customTransport(url, jsonBody);
    }

    HttpResponse response;
    const auto parsedUrl = HttpSecurity::ParseUrl(url);
    std::string urlError;
    if (!HttpSecurity::IsAllowedSensitiveUrl(parsedUrl, &urlError)) {
        response.curlCode = CURLE_FAILED_INIT;
        response.statusCode = 0;
        response.body = "{\"error\":{\"code\":\"insecure_url\",\"message\":\"" + urlError + "\"}}";
        return response;
    }

    std::string currentUrl = url;
    constexpr int kMaxRedirects = 5;
    const std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;

    for (int redirectCount = 0; redirectCount <= kMaxRedirects; ++redirectCount) {
        response.body.clear();
        response.curlCode = 0;
        response.statusCode = 0;
        response.retryAfterSeconds = 0;

        CURL* curl = curl_easy_init();
        if (!curl) {
            response.curlCode = CURLE_FAILED_INIT;
            return response;
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");

        CurlHeaderState headerState;
        curl_easy_setopt(curl, CURLOPT_URL, currentUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.size()));
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerState);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS | CURLPROTO_HTTP);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_stopWorker);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

        const CURLcode res = curl_easy_perform(curl);
        response.curlCode = static_cast<int>(res);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
        }
        response.retryAfterSeconds = headerState.retryAfterSeconds;

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return response;
        }

        if (response.statusCode == 301 || response.statusCode == 302 ||
            response.statusCode == 303 || response.statusCode == 307 ||
            response.statusCode == 308) {
            if (redirectCount == kMaxRedirects) {
                response.curlCode = CURLE_TOO_MANY_REDIRECTS;
                response.body = "{\"error\":{\"code\":\"too_many_redirects\",\"message\":\"Maximum redirect limit exceeded\"}}";
                return response;
            }
            const auto val = HttpSecurity::ValidateRedirect(currentUrl, headerState.location, /*isSensitiveRequest=*/true);
            if (!val.allowed) {
                response.curlCode = CURLE_PEER_FAILED_VERIFICATION;
                response.body = "{\"error\":{\"code\":\"unsafe_redirect\",\"message\":\"" + val.reason + "\"}}";
                return response;
            }
            if (response.statusCode == 303) {
                response.curlCode = CURLE_HTTP_RETURNED_ERROR;
                response.body = "{\"error\":{\"code\":\"unsupported_redirect\",\"message\":\"HTTP 303 is not supported for credential POST endpoints\"}}";
                return response;
            }
            currentUrl = val.resolvedUrl;
            continue;
        }
        break;
    }
    return response;
}

int64_t AccountClient::NowUnix() const {
    ClockFn customClock;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        customClock = m_clock;
    }
    if (customClock) {
        return customClock();
    }
    return static_cast<int64_t>(std::time(nullptr));
}

bool AccountClient::SleepInterruptible(int seconds) {
    SleepFn customSleep;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        customSleep = m_sleep;
    }
    if (customSleep) {
        return customSleep(seconds);
    }

    std::unique_lock<std::mutex> lock(m_workerMutex);
    const bool interrupted = m_workerCv.wait_for(
        lock, std::chrono::seconds(std::max(1, seconds)), [this]() {
            return m_cancelPolling.load(std::memory_order_relaxed) ||
                   m_stopWorker.load(std::memory_order_relaxed);
        });
    return !interrupted;
}

void AccountClient::OpenUrl(const std::string& url) {
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
        return;
    }
    OpenUrlFn customOpen;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        customOpen = m_openUrl;
    }
    if (customOpen) {
        customOpen(url);
        return;
    }
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AccountClient::SetTransportForTests(HttpTransportFn transport) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transport = std::move(transport);
}

void AccountClient::SetClockForTests(ClockFn clockFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = std::move(clockFn);
}

void AccountClient::SetSleepForTests(SleepFn sleepFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sleep = std::move(sleepFn);
}

void AccountClient::SetOpenUrlForTests(OpenUrlFn openUrlFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_openUrl = std::move(openUrlFn);
}

void AccountClient::SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_accessToken = std::move(token);
    m_accessTokenExpiresAt = expiresAtUnix;
}

void AccountClient::ResetForTests() {
    Shutdown();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transport = nullptr;
    m_clock = nullptr;
    m_sleep = nullptr;
    m_openUrl = nullptr;
    m_accessToken.clear();
    m_accessTokenExpiresAt = 0;
    m_displayName.clear();
    m_devicePublicId.clear();
    m_deviceCode.clear();
    m_userCode.clear();
    m_verificationUri.clear();
    m_expiresAtUnix = 0;
    m_pollIntervalSeconds = 5;
    m_errorMessage.clear();
    m_pendingSignedOutToast.clear();
    m_state = AccountAuthState::SignedOut;
    m_cancelPolling.store(false, std::memory_order_relaxed);
    m_stopWorker.store(false, std::memory_order_relaxed);
    ++m_statusVersion;
}
