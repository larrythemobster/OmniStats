#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

struct ConfigData;
class DeviceIdentity;

class TokenStore {
  public:
    TokenStore();
    ~TokenStore() = default;

    TokenStore(const TokenStore&) = delete;
    TokenStore& operator=(const TokenStore&) = delete;

    using ClockFn = std::function<int64_t()>;

    bool IsSignedIn() const;
    bool IsSignedIn(const ConfigData& conf) const;

    std::string AccessToken(int64_t nowUnix = 0) const;
    std::string DevicePublicId() const;
    std::string DisplayName() const;

    void SyncFromConfig();
    void SetTokens(const std::string& accessToken,
                   int64_t expiresAtUnix,
                   const std::string& refreshToken,
                   const std::string& displayName,
                   const std::string& devicePublicId);
    void Clear();

    bool Refresh(DeviceIdentity& identity,
                 std::function<bool()> shouldAbort = nullptr,
                 std::function<void(const std::string&, std::string)> revokeRotatedToken = nullptr,
                 std::function<void()> onRejected = nullptr,
                 std::function<void()> onSuccess = nullptr);

    void SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix);
    void ResetForTests();
    void SetClockForTests(ClockFn clockFn);
    int64_t NowUnix() const;

    uint64_t AuthGeneration() const;
    void BumpAuthGeneration();

  private:
    mutable std::mutex m_mutex;
    std::condition_variable m_refreshCv;
    bool m_refreshInFlight = false;
    bool m_lastRefreshSucceeded = false;
    uint64_t m_authGeneration = 0;

    std::string m_accessToken;
    int64_t m_accessTokenExpiresAt = 0;
    std::string m_displayName;
    std::string m_devicePublicId;
    ClockFn m_clock;
};
