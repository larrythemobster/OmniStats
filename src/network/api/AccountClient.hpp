#pragma once

#include "network/api/DeviceIdentity.hpp"
#include "network/api/DeviceAuthFlow.hpp"
#include "network/api/TokenStore.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

class TelemetryManager;

struct ConfigData;

class AccountClient {
  public:
    using DeviceKeyPair = DeviceIdentity::DeviceKeyPair;
    using DeviceProof = DeviceIdentity::DeviceProof;
    using StartAuthResult = DeviceAuthFlow::StartAuthResult;
    using PollOutcome = DeviceAuthFlow::PollOutcome;
    using PollResult = DeviceAuthFlow::PollResult;

    using ClockFn = DeviceAuthFlow::ClockFn;
    using SleepFn = DeviceAuthFlow::SleepFn;
    using OpenUrlFn = DeviceAuthFlow::OpenUrlFn;
    void SetTelemetryManager(std::shared_ptr<TelemetryManager> tm) {
        m_telemetry = std::move(tm);
    }

    AccountClient();
    ~AccountClient();

    AccountClient(const AccountClient&) = delete;
    AccountClient& operator=(const AccountClient&) = delete;

    static bool EnsureSodiumInitialized() {
        return DeviceIdentity::EnsureSodiumInitialized();
    }
    static std::string Base64UrlEncode(const uint8_t* data, size_t len) {
        return DeviceIdentity::Base64UrlEncode(data, len);
    }
    static bool Base64UrlDecode(std::string_view encoded, std::vector<uint8_t>& out) {
        return DeviceIdentity::Base64UrlDecode(encoded, out);
    }
    static std::string Sha256Hex(std::string_view input) {
        return DeviceIdentity::Sha256Hex(input);
    }
    static std::string DerivePublicIdForPublicKey(std::string_view publicKeyBase64Url) {
        return DeviceIdentity::DerivePublicIdForPublicKey(publicKeyBase64Url);
    }
    static DeviceKeyPair DeriveKeyPairFromSeed(const std::array<uint8_t, 32>& seed) {
        return DeviceIdentity::DeriveKeyPairFromSeed(seed);
    }
    static bool DeriveKeyPairFromSecret(std::string_view secretKeyBase64Url, DeviceKeyPair& out) {
        return DeviceIdentity::DeriveKeyPairFromSecret(secretKeyBase64Url, out);
    }
    static std::string BuildProofCanonical(std::string_view operation,
                                           std::string_view publicId,
                                           int64_t timestamp,
                                           std::string_view nonceBase64Url,
                                           std::string_view credential) {
        return DeviceIdentity::BuildProofCanonical(operation, publicId, timestamp, nonceBase64Url, credential);
    }
    static DeviceProof SignProofDeterministic(std::string_view secretKeyBase64Url,
                                              std::string_view operation,
                                              std::string_view publicId,
                                              int64_t timestamp,
                                              std::string_view nonceBase64Url,
                                              std::string_view credential) {
        return DeviceIdentity::SignProofDeterministic(secretKeyBase64Url, operation, publicId, timestamp, nonceBase64Url, credential);
    }
    static DeviceProof CreateProof(std::string_view secretKeyBase64Url,
                                   std::string_view operation,
                                   std::string_view publicId,
                                   std::string_view credential,
                                   int64_t timestamp) {
        return DeviceIdentity::CreateProof(secretKeyBase64Url, operation, publicId, credential, timestamp);
    }

    static std::string SiteBaseUrl() {
        return DeviceAuthFlow::SiteBaseUrl();
    }
    static std::string ManageDevicesUrl() {
        return DeviceAuthFlow::ManageDevicesUrl();
    }
    static std::string ResolveVerificationUri(std::string_view rawUri) {
        return DeviceAuthFlow::ResolveVerificationUri(rawUri);
    }
    static std::string LocalDeviceName() {
        return DeviceIdentity::LocalDeviceName();
    }

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
    bool IsSignedIn(const ConfigData& conf) const;
    AccountStatusSnapshot GetStatus() const;
    void SyncFromConfig();
    void RetryKeyMigrationAsync();
    void Shutdown();

    void SetClockForTests(ClockFn clockFn);
    void SetSleepForTests(SleepFn sleepFn);
    void SetOpenUrlForTests(OpenUrlFn openUrlFn);
    void SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix);
    void ResetForTests();

    DeviceIdentity& Identity() {
        return m_identity;
    }
    DeviceAuthFlow& AuthFlow() {
        return m_authFlow;
    }
    TokenStore& Tokens() {
        return m_tokenStore;
    }

  private:
    enum class WorkerCommand {
        StartSignIn,
        ExchangeAPIKey,
        Logout
    };

    struct WorkerJob {
        WorkerCommand command = WorkerCommand::StartSignIn;
        bool openBrowser = true;
        std::string refreshToken;
        uint64_t authGeneration = 0;
    };

    bool SendLogoutRequest(const std::string& refreshToken);
    bool ExchangeAPIKey(uint64_t generation);
    void QueueLogoutForRotatedToken(const std::string& previousToken, std::string rotatedToken);
    void EnsureWorkerRunningLocked();
    void WorkerLoop();

    DeviceIdentity m_identity;
    DeviceAuthFlow m_authFlow;
    TokenStore m_tokenStore;

    mutable std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    std::deque<WorkerJob> m_workerQueue;
    std::jthread m_workerThread;
    bool m_workerStarted = false;
    bool m_keyExchangeAttempted = false;
    std::atomic<bool> m_stopWorker{false};
    std::shared_ptr<TelemetryManager> m_telemetry;
};
