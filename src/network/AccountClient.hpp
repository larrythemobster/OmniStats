#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

enum class AccountAuthState {
    SignedOut,
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

class AccountClient {
  public:
    struct DeviceKeyPair {
        std::string publicKeyBase64Url;
        std::string secretKeyBase64Url;
        std::string publicId;
    };

    struct DeviceProof {
        int64_t timestamp = 0;
        std::string nonce;
        std::string signature;
        std::string canonical;
    };

    struct HttpResponse {
        int curlCode = 0;
        long statusCode = 0;
        std::string body;
        long retryAfterSeconds = 0;
    };

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

    using HttpTransportFn = std::function<HttpResponse(const std::string& url, const std::string& jsonBody)>;
    using ClockFn = std::function<int64_t()>;
    using SleepFn = std::function<bool(int seconds)>;
    using OpenUrlFn = std::function<void(const std::string& url)>;

    static AccountClient& Instance();
    static bool EnsureSodiumInitialized();

    AccountClient();
    ~AccountClient();

    AccountClient(const AccountClient&) = delete;
    AccountClient& operator=(const AccountClient&) = delete;

    static std::string Base64UrlEncode(const uint8_t* data, size_t len);
    static bool Base64UrlDecode(std::string_view encoded, std::vector<uint8_t>& out);
    static std::string Sha256Hex(std::string_view input);
    static std::string DerivePublicIdForPublicKey(std::string_view publicKeyBase64Url);
    static DeviceKeyPair DeriveKeyPairFromSeed(const std::array<uint8_t, 32>& seed);
    static bool DeriveKeyPairFromSecret(std::string_view secretKeyBase64Url, DeviceKeyPair& out);
    static std::string BuildProofCanonical(std::string_view operation,
                                           std::string_view publicId,
                                           int64_t timestamp,
                                           std::string_view nonceBase64Url,
                                           std::string_view credential);
    static DeviceProof SignProofDeterministic(std::string_view secretKeyBase64Url,
                                              std::string_view operation,
                                              std::string_view publicId,
                                              int64_t timestamp,
                                              std::string_view nonceBase64Url,
                                              std::string_view credential);
    static DeviceProof CreateProof(std::string_view secretKeyBase64Url,
                                   std::string_view operation,
                                   std::string_view publicId,
                                   std::string_view credential,
                                   int64_t timestamp);

    static std::string ApiBaseUrl();
    static std::string SiteBaseUrl(std::string_view apiBaseUrl = {});
    static std::string ManageDevicesUrl();
    static std::string ResolveVerificationUri(std::string_view rawUri, std::string_view apiBaseUrl = {});
    static std::string LocalDeviceName();

    bool EnsureDeviceKey(DeviceKeyPair* outKey = nullptr);
    StartAuthResult StartAuthorization();
    PollResult PollTokenOnce();
    PollResult PollToken();
    bool Refresh();
    std::string AccessToken();
    std::string DevicePublicId();
    bool Logout();

    void BeginSignInAsync(bool openBrowser = true);
    void CancelAuthorization();
    void LogoutAsync();
    void OpenVerificationBrowser();
    void OpenManageDevicesBrowser();
    void MarkSignedOut(std::string reason = {});
    bool ConsumeSignedOutNotification(std::string& outReason);

    bool IsSignedIn() const;
    AccountStatusSnapshot GetStatus() const;
    void SyncFromConfig();
    void Shutdown();

    void SetTransportForTests(HttpTransportFn transport);
    void SetClockForTests(ClockFn clockFn);
    void SetSleepForTests(SleepFn sleepFn);
    void SetOpenUrlForTests(OpenUrlFn openUrlFn);
    void SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix);
    void ResetForTests();

  private:
    enum class WorkerCommand {
        StartSignIn,
        Logout
    };

    struct WorkerJob {
        WorkerCommand command = WorkerCommand::StartSignIn;
        bool openBrowser = true;
        std::string refreshToken;
    };

    bool SendLogoutRequest(const std::string& refreshToken);
    void EnsureWorkerRunningLocked();
    void WorkerLoop();
    HttpResponse PerformHttpPost(const std::string& url, const std::string& jsonBody);
    int64_t NowUnix() const;
    bool SleepInterruptible(int seconds);
    void OpenUrl(const std::string& url);

    mutable std::mutex m_mutex;
    std::condition_variable m_refreshCv;
    bool m_refreshInFlight = false;
    bool m_lastRefreshSucceeded = false;

    AccountAuthState m_state = AccountAuthState::SignedOut;
    std::string m_displayName;
    std::string m_devicePublicId;
    std::string m_deviceCode;
    std::string m_userCode;
    std::string m_verificationUri;
    int64_t m_expiresAtUnix = 0;
    int m_pollIntervalSeconds = 5;
    std::string m_errorMessage;
    std::string m_pendingSignedOutToast;
    uint64_t m_statusVersion = 1;

    std::string m_accessToken;
    int64_t m_accessTokenExpiresAt = 0;

    HttpTransportFn m_transport;
    ClockFn m_clock;
    SleepFn m_sleep;
    OpenUrlFn m_openUrl;

    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    std::deque<WorkerJob> m_workerQueue;
    std::jthread m_workerThread;
    bool m_workerStarted = false;
    std::atomic<bool> m_cancelPolling{false};
    std::atomic<bool> m_stopWorker{false};
};
