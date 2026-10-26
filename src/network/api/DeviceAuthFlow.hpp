#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

enum class AccountAuthState {
    SignedOut,
    Migrating,
    AwaitingApproval,
    SignedIn,
    Error
};

struct AccountStatusSnapshot {
    AccountAuthState state = AccountAuthState::SignedOut;
    std::string displayName;
    std::string devicePublicId;
    std::string userCode;
    std::string verificationUri;
    int64_t expiresAtUnix = 0;
    int pollIntervalSeconds = 5;
    std::string errorMessage;
    uint64_t version = 1;
};

class DeviceIdentity;
class TokenStore;

class DeviceAuthFlow {
  public:
    struct StartAuthResult {
        bool ok = false;
        std::string deviceCode;
        std::string userCode;
        std::string verificationUri;
        int expiresIn = 0;
        int pollInterval = 5;
        std::string errorCode;
        std::string errorMessage;
    };

    enum class PollOutcome {
        Pending,
        SlowDown,
        Approved,
        Denied,
        Expired,
        Error,
        Cancelled
    };

    struct PollResult {
        PollOutcome outcome = PollOutcome::Error;
        int pollIntervalSeconds = 5;
        std::string displayName;
        std::string errorCode;
        std::string errorMessage;
    };

    using ClockFn = std::function<int64_t()>;
    using SleepFn = std::function<bool(int seconds)>;
    using OpenUrlFn = std::function<void(const std::string& url)>;

    DeviceAuthFlow();
    ~DeviceAuthFlow() = default;

    DeviceAuthFlow(const DeviceAuthFlow&) = delete;
    DeviceAuthFlow& operator=(const DeviceAuthFlow&) = delete;

    static std::string SiteBaseUrl();
    static std::string ManageDevicesUrl();
    static std::string ResolveVerificationUri(std::string_view rawUri);

    StartAuthResult StartAuthorization(DeviceIdentity& identity,
                                       std::function<bool()> shouldAbort = nullptr);

    PollResult PollTokenOnce(DeviceIdentity& identity,
                             TokenStore& tokenStore,
                             std::function<bool()> shouldAbort = nullptr);

    PollResult PollToken(DeviceIdentity& identity,
                         TokenStore& tokenStore,
                         std::function<bool()> shouldAbort = nullptr);

    void CancelAuthorization();
    void SetState(AccountAuthState state, std::string errorMessage = {});
    void MarkSignedOut(std::string reason);
    bool ConsumeSignedOutNotification(std::string& outReason);

    void OpenVerificationBrowser();
    void OpenManageDevicesBrowser();

    AccountStatusSnapshot GetStatus(const TokenStore& tokenStore) const;

    void SetClockForTests(ClockFn clockFn);
    void SetSleepForTests(SleepFn sleepFn);
    void SetOpenUrlForTests(OpenUrlFn openUrlFn);
    void ResetForTests();
    int64_t NowUnix() const;

  private:
    bool SleepInterruptible(int seconds);
    void OpenUrl(const std::string& url);

    mutable std::mutex m_mutex;
    AccountAuthState m_state = AccountAuthState::SignedOut;
    std::string m_deviceCode;
    std::string m_userCode;
    std::string m_verificationUri;
    int64_t m_expiresAtUnix = 0;
    int m_pollIntervalSeconds = 5;
    std::string m_errorMessage;
    std::string m_pendingSignedOutToast;
    uint64_t m_statusVersion = 1;
    bool m_cancelRequested = false;

    ClockFn m_clock;
    SleepFn m_sleep;
    OpenUrlFn m_openUrl;
};
