#include "network/api/TokenStore.hpp"
#include "network/api/DeviceIdentity.hpp"
#include "core/config/Config.hpp"
#include "network/omninet/OmniNet.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>

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

    int64_t CurrentUnix() {
        return static_cast<int64_t>(std::time(nullptr));
    }

    std::string ServerErrorCode(const std::string& body) {
        try {
            const auto j = nlohmann::json::parse(body);
            if (j.contains("error") && j["error"].is_object()) {
                const std::string code = j["error"].value("code", "");
                if (!code.empty()) return " (" + code + ")";
            }
        } catch (...) {
        }
        return {};
    }

} // namespace

TokenStore::TokenStore() {
    SyncFromConfig();
}

bool TokenStore::IsSignedIn() const {
    const ConfigData conf = Config::Read();
    return IsSignedIn(conf);
}

bool TokenStore::IsSignedIn(const ConfigData& conf) const {
    return !conf.account_refresh_token.empty();
}

std::string TokenStore::AccessToken(int64_t nowUnix) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_accessToken.empty()) {
        return {};
    }
    const int64_t now = nowUnix > 0 ? nowUnix : (m_clock ? m_clock() : CurrentUnix());
    if ((m_accessTokenExpiresAt - now) > 60) {
        return m_accessToken;
    }
    return {};
}

std::string TokenStore::DevicePublicId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_devicePublicId;
}

std::string TokenStore::DisplayName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_displayName;
}

uint64_t TokenStore::AuthGeneration() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_authGeneration;
}

void TokenStore::BumpAuthGeneration() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_authGeneration;
}

void TokenStore::SyncFromConfig() {
    const ConfigData conf = Config::Read();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_displayName = conf.account_signed_in_name.empty() ? "OmniStats User" : conf.account_signed_in_name;
    m_devicePublicId = conf.account_device_public_id;
    if (conf.account_refresh_token.empty()) {
        m_accessToken.clear();
        m_accessTokenExpiresAt = 0;
    }
}

void TokenStore::SetTokens(const std::string& accessToken,
                           int64_t expiresAtUnix,
                           const std::string& refreshToken,
                           const std::string& displayName,
                           const std::string& devicePublicId) {
    Config::Update([&](ConfigData& c) {
        c.account_refresh_token = refreshToken;
        c.account_signed_in_name = displayName;
        c.account_device_public_id = devicePublicId;
        c.custom_api_key.clear();
        c.custom_api_enabled = true;
    });
    Config::Save();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_accessToken = accessToken;
    m_accessTokenExpiresAt = expiresAtUnix;
    m_displayName = displayName;
    m_devicePublicId = devicePublicId;
    ++m_authGeneration;
}

void TokenStore::Clear() {
    Config::Update([](ConfigData& c) {
        c.account_refresh_token.clear();
        c.account_signed_in_name.clear();
        c.custom_api_key.clear();
    });
    Config::Save();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_accessToken.clear();
    m_accessTokenExpiresAt = 0;
    m_displayName.clear();
    ++m_authGeneration;
}

bool TokenStore::Refresh(DeviceIdentity& identity,
                         std::function<bool()> shouldAbort,
                         std::function<void(const std::string&, std::string)> revokeRotatedToken,
                         std::function<void()> onRejected,
                         std::function<void()> onSuccess) {
    uint64_t generation = 0;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_refreshInFlight) {
            m_refreshCv.wait(lock, [this]() { return !m_refreshInFlight; });
            return m_lastRefreshSucceeded;
        }
        m_refreshInFlight = true;
        generation = m_authGeneration;
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

    DeviceIdentity::DeviceKeyPair keyPair;
    if (!identity.EnsureDeviceKey(&keyPair)) {
        return finish(false);
    }

    const int64_t now = NowUnix();
    const DeviceIdentity::DeviceProof proof = identity.CreateProof(
        keyPair.secretKeyBase64Url, "refresh", keyPair.publicId, conf.account_refresh_token, now);
    const nlohmann::json reqJson = {
        {"refresh_token", conf.account_refresh_token},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    OmniNet::Request omniReq;
    omniReq.op = OmniNet::Op::AuthRefresh;
    omniReq.payload = reqJson.dump();
    omniReq.shouldAbort = std::move(shouldAbort);

    const OmniNet::Response resp = OmniNet::Send(omniReq);
    if (resp.updateRequired) {
        return finish(false);
    }
    if (resp.transportError != 0 || (resp.status >= 500 && resp.status <= 599) || resp.status == 429) {
        return finish(false);
    }
    if (resp.status == 401 || resp.status == 403) {
        std::cout << "[TokenStore] Refresh rejected with HTTP " << resp.status << ServerErrorCode(resp.body)
                  << "; the saved credentials are discarded.\n";
        if (onRejected)
            onRejected();
        else
            Clear();
        return finish(false);
    }
    if (resp.status != 200) {
        std::cout << "[TokenStore] Refresh failed with HTTP " << resp.status << ServerErrorCode(resp.body)
                  << "; the saved credentials are kept.\n";
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

        const int64_t issuedAt = NowUnix();
        bool loggedOutMeanwhile = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            loggedOutMeanwhile = generation != m_authGeneration;
            if (!loggedOutMeanwhile) {
                Config::Update([&](ConfigData& c) {
                    c.account_refresh_token = newRefreshToken;
                    c.account_signed_in_name = displayName;
                    c.account_device_public_id = keyPair.publicId;
                    c.custom_api_key.clear();
                });
                m_accessToken = accessToken;
                m_accessTokenExpiresAt = issuedAt + expiresIn;
                m_displayName = displayName;
            }
        }
        if (loggedOutMeanwhile) {
            if (revokeRotatedToken) {
                revokeRotatedToken(conf.account_refresh_token, newRefreshToken);
            }
            return finish(false);
        }
        Config::Save();
        if (onSuccess) onSuccess();
        return finish(true);
    } catch (...) {
        return finish(false);
    }
}

void TokenStore::SetCachedAccessToken(std::string token, int64_t expiresAtUnix) {
    SetCachedAccessTokenForTests(std::move(token), expiresAtUnix);
}

void TokenStore::SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_accessToken = std::move(token);
    m_accessTokenExpiresAt = expiresAtUnix;
}

void TokenStore::SetClockForTests(ClockFn clockFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = std::move(clockFn);
}

int64_t TokenStore::NowUnix() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_clock) return m_clock();
    return CurrentUnix();
}

void TokenStore::ResetForTests() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = nullptr;
    m_accessToken.clear();
    m_accessTokenExpiresAt = 0;
    m_displayName.clear();
    m_devicePublicId.clear();
    m_refreshInFlight = false;
    m_lastRefreshSucceeded = false;
    ++m_authGeneration;
}
