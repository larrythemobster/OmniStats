#include "network/api/AccountClient.hpp"
#include "core/config/Config.hpp"
#include "core/config/AppVersion.hpp"
#include "network/omninet/OmniNet.hpp"
#include "network/telemetry/TelemetryManager.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <iostream>

namespace {
    std::string ExtractDisplayName(const nlohmann::json& j, const std::string& fallback) {
        if (j.is_object() && j.contains("account") && j["account"].is_object()) {
            const auto& acct = j["account"];
            if (acct.contains("display_name") && acct["display_name"].is_string()) {
                std::string name = acct["display_name"].get<std::string>();
                if (!name.empty()) return name;
            }
            if (acct.contains("email") && acct["email"].is_string()) {
                std::string email = acct["email"].get<std::string>();
                if (!email.empty()) return email;
            }
        }
        return fallback.empty() ? "OmniStats User" : fallback;
    }
} // namespace

AccountClient::AccountClient() {
    EnsureSodiumInitialized();
    SyncFromConfig();
}

AccountClient::~AccountClient() {
    Shutdown();
}

bool AccountClient::EnsureDeviceKey(DeviceKeyPair* outKey) {
    return m_identity.EnsureDeviceKey(outKey);
}

AccountClient::StartAuthResult AccountClient::StartAuthorization() {
    return m_authFlow.StartAuthorization(m_identity, [this]() {
        return m_stopWorker.load(std::memory_order_relaxed);
    });
}

AccountClient::PollResult AccountClient::PollTokenOnce() {
    auto result = m_authFlow.PollTokenOnce(m_identity, m_tokenStore, [this]() {
        return m_stopWorker.load(std::memory_order_relaxed);
    });
    if (result.outcome == PollOutcome::Approved && m_telemetry) {
        m_telemetry->RecordMilestone(TelemetryManager::Milestone::AccountLinked);
    }
    return result;
}

AccountClient::PollResult AccountClient::PollToken() {
    auto result = m_authFlow.PollToken(m_identity, m_tokenStore, [this]() {
        return m_stopWorker.load(std::memory_order_relaxed);
    });
    if (result.outcome == PollOutcome::Approved && m_telemetry) {
        m_telemetry->RecordMilestone(TelemetryManager::Milestone::AccountLinked);
    }
    return result;
}

bool AccountClient::Refresh() {
    return m_tokenStore.Refresh(
        m_identity,
        [this]() {
            return m_stopWorker.load(std::memory_order_relaxed);
        },
        [this](const std::string& previousToken, std::string rotatedToken) {
            QueueLogoutForRotatedToken(previousToken, std::move(rotatedToken));
        },
        [this]() {
            MarkSignedOut("Your OmniStats session expired or this device was revoked. Sign in again in Settings > Accounts & services.");
        },
        [this]() {
            m_authFlow.SetState(AccountAuthState::SignedIn);
        });
}

std::string AccountClient::AccessToken() {
    const int64_t now = m_authFlow.NowUnix();
    std::string token = m_tokenStore.AccessToken(now);
    if (!token.empty()) {
        return token;
    }
    if (m_tokenStore.IsSignedIn()) {
        if (Refresh()) {
            return m_tokenStore.AccessToken(now);
        }
    }
    return {};
}

std::string AccountClient::DevicePublicId() {
    std::string id = Config::Read().account_device_public_id;
    if (id.empty()) {
        DeviceKeyPair pair;
        if (EnsureDeviceKey(&pair)) {
            id = pair.publicId;
        }
    }
    return id;
}

bool AccountClient::Logout() {
    const ConfigData conf = Config::Read();
    const std::string refreshToken = conf.account_refresh_token;
    m_authFlow.CancelAuthorization();
    m_tokenStore.Clear();
    m_authFlow.SetState(AccountAuthState::SignedOut);
    if (!refreshToken.empty()) {
        return SendLogoutRequest(refreshToken);
    }
    return true;
}

void AccountClient::BeginSignInAsync(bool openBrowser) {
    m_authFlow.CancelAuthorization();
    m_tokenStore.BumpAuthGeneration();
    std::lock_guard<std::mutex> lock(m_workerMutex);
    EnsureWorkerRunningLocked();
    m_workerQueue.clear();
    WorkerJob job;
    job.command = WorkerCommand::StartSignIn;
    job.openBrowser = openBrowser;
    job.authGeneration = m_tokenStore.AuthGeneration();
    m_workerQueue.push_back(std::move(job));
    m_workerCv.notify_all();
}

void AccountClient::CancelAuthorization() {
    m_authFlow.CancelAuthorization();
}

void AccountClient::LogoutAsync() {
    const ConfigData conf = Config::Read();
    const std::string refreshToken = conf.account_refresh_token;
    m_authFlow.CancelAuthorization();
    m_tokenStore.Clear();
    m_authFlow.SetState(AccountAuthState::SignedOut);

    if (refreshToken.empty()) return;
    std::lock_guard<std::mutex> lock(m_workerMutex);
    EnsureWorkerRunningLocked();
    WorkerJob job;
    job.command = WorkerCommand::Logout;
    job.refreshToken = refreshToken;
    job.authGeneration = m_tokenStore.AuthGeneration();
    m_workerQueue.push_back(std::move(job));
    m_workerCv.notify_all();
}

void AccountClient::OpenVerificationBrowser() {
    m_authFlow.OpenVerificationBrowser();
}

void AccountClient::OpenManageDevicesBrowser() {
    m_authFlow.OpenManageDevicesBrowser();
}

void AccountClient::MarkSignedOut(std::string reason) {
    m_tokenStore.Clear();
    m_authFlow.MarkSignedOut(std::move(reason));
}

bool AccountClient::ConsumeSignedOutNotification(std::string& outReason) {
    return m_authFlow.ConsumeSignedOutNotification(outReason);
}

bool AccountClient::IsSignedIn() const {
    return IsSignedIn(Config::Read());
}

bool AccountClient::IsSignedIn(const ConfigData& conf) const {
    return m_tokenStore.IsSignedIn(conf) &&
           m_authFlow.GetStatus(m_tokenStore).state != AccountAuthState::AwaitingApproval;
}

AccountStatusSnapshot AccountClient::GetStatus() const {
    return m_authFlow.GetStatus(m_tokenStore);
}

void AccountClient::SyncFromConfig() {
    const ConfigData conf = Config::Read();
    if (!conf.account_refresh_token.empty() && !conf.custom_api_key.empty()) {
        Config::Update([](ConfigData& c) { c.custom_api_key.clear(); });
        Config::Save();
    }
    m_tokenStore.SyncFromConfig();
    bool migrateAPIKey = false;
    uint64_t generation = 0;
    {
        if (conf.account_refresh_token.empty() && !conf.custom_api_key.empty() &&
            !m_keyExchangeAttempted && m_authFlow.GetStatus(m_tokenStore).state != AccountAuthState::AwaitingApproval) {
            m_keyExchangeAttempted = true;
            migrateAPIKey = true;
            generation = m_tokenStore.AuthGeneration();
            m_authFlow.SetState(AccountAuthState::Migrating);
        } else if (!conf.account_refresh_token.empty()) {
            if (m_authFlow.GetStatus(m_tokenStore).state != AccountAuthState::AwaitingApproval) {
                m_authFlow.SetState(AccountAuthState::SignedIn);
            }
        } else if (conf.credentials_decryption_failed && !conf.account_device_public_id.empty()) {
            m_authFlow.SetState(AccountAuthState::Error, "Saved credentials could not be decrypted on this device. Sign in again in Settings > Accounts & services.");
            m_tokenStore.Clear();
        } else if (m_authFlow.GetStatus(m_tokenStore).state == AccountAuthState::SignedIn) {
            m_authFlow.SetState(AccountAuthState::SignedOut);
            m_tokenStore.Clear();
        }
    }
    if (migrateAPIKey) {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        EnsureWorkerRunningLocked();
        WorkerJob job;
        job.command = WorkerCommand::ExchangeAPIKey;
        job.authGeneration = generation;
        m_workerQueue.push_back(std::move(job));
        m_workerCv.notify_all();
    }
}

void AccountClient::RetryKeyMigrationAsync() {
    if (m_authFlow.GetStatus(m_tokenStore).state != AccountAuthState::Error) return;
    m_keyExchangeAttempted = false;
    SyncFromConfig();
}

void AccountClient::Shutdown() {
    m_stopWorker.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_workerMutex);
        m_workerQueue.clear();
        m_workerCv.notify_all();
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_workerStarted = false;
}

void AccountClient::SetClockForTests(ClockFn clockFn) {
    m_authFlow.SetClockForTests(clockFn);
    m_tokenStore.SetClockForTests(std::move(clockFn));
}

void AccountClient::SetSleepForTests(SleepFn sleepFn) {
    m_authFlow.SetSleepForTests(std::move(sleepFn));
}

void AccountClient::SetOpenUrlForTests(OpenUrlFn openUrlFn) {
    m_authFlow.SetOpenUrlForTests(std::move(openUrlFn));
}

void AccountClient::SetCachedAccessTokenForTests(std::string token, int64_t expiresAtUnix) {
    m_tokenStore.SetCachedAccessTokenForTests(std::move(token), expiresAtUnix);
}

void AccountClient::ResetForTests() {
    Shutdown();
    m_stopWorker.store(false, std::memory_order_relaxed);
    m_keyExchangeAttempted = false;
    m_tokenStore.ResetForTests();
    m_authFlow.ResetForTests();
}

bool AccountClient::SendLogoutRequest(const std::string& refreshToken) {
    if (refreshToken.empty()) return true;
    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) return false;

    const int64_t now = m_authFlow.NowUnix();
    const DeviceProof proof = CreateProof(keyPair.secretKeyBase64Url, "logout", keyPair.publicId, refreshToken, now);
    const nlohmann::json reqJson = {
        {"refresh_token", refreshToken},
        {"device_public_id", keyPair.publicId},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    OmniNet::Request omniReq;
    omniReq.op = OmniNet::Op::AuthLogout;
    omniReq.payload = reqJson.dump();
    omniReq.shouldAbort = [this]() { return m_stopWorker.load(std::memory_order_relaxed); };
    const OmniNet::Response resp = OmniNet::Send(omniReq);
    return resp.transportError == 0 && resp.status == 200;
}

bool AccountClient::ExchangeAPIKey(uint64_t generation) {
    if (generation != m_tokenStore.AuthGeneration()) return false;

    const ConfigData conf = Config::Read();
    if (conf.custom_api_key.empty() || !conf.account_refresh_token.empty()) return false;

    const auto fail = [this, generation](std::string message) {
        if (generation == m_tokenStore.AuthGeneration()) {
            m_authFlow.SetState(AccountAuthState::Error, std::move(message));
        }
        return false;
    };

    DeviceKeyPair keyPair;
    if (!EnsureDeviceKey(&keyPair)) {
        return fail("Unable to read this device's key. Sign in again in Settings > Accounts & services.");
    }

    const int64_t now = m_authFlow.NowUnix();
    const DeviceProof proof = CreateProof(keyPair.secretKeyBase64Url, "exchange", keyPair.publicId, conf.custom_api_key, now);
    const nlohmann::json reqJson = {
        {"api_key", conf.custom_api_key},
        {"device_public_id", keyPair.publicId},
        {"device_name", LocalDeviceName()},
        {"platform", "windows"},
        {"app_version", AppVersion::Current},
        {"public_key", keyPair.publicKeyBase64Url},
        {"proof", {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}}}};

    OmniNet::Request omniReq;
    omniReq.op = OmniNet::Op::AuthExchangeKey;
    omniReq.payload = reqJson.dump();
    omniReq.shouldAbort = [this]() { return m_stopWorker.load(std::memory_order_relaxed); };

    const OmniNet::Response resp = OmniNet::Send(omniReq);
    if (resp.updateRequired) {
        return fail("This version of OmniStats is out of date. Update from omnistats.org to sign in.");
    }
    if (resp.transportError != 0 || resp.status >= 500 || resp.status == 429) {
        return fail("Unable to link your saved API key right now. It is still saved. Restart OmniStats to retry, or sign in in Settings > Accounts & services.");
    }
    if (resp.status != 200) {
        try {
            const auto body = nlohmann::json::parse(resp.body);
            const std::string code = body.value("error", nlohmann::json{}).value("code", "");
            if (code == "device_limit_reached") {
                return fail("Your account's device limit was reached. Remove an unused device with Manage devices, then restart OmniStats or sign in again.");
            }
            if (code == "email_verification_required") {
                return fail("Verify your OmniStats email, then sign in in Settings > Accounts & services to link this device.");
            }
            if (code == "device_revoked") {
                return fail("This device was revoked. Sign in again and approve it in your browser to reconnect.");
            }
        } catch (...) {
        }
        return fail("Your saved API key could not link this device. Sign in in Settings > Accounts & services.");
    }

    try {
        const auto j = nlohmann::json::parse(resp.body);
        const std::string accessToken = j.value("access_token", "");
        const std::string refreshToken = j.value("refresh_token", "");
        const int expiresIn = j.value("expires_in", 0);
        const std::string displayName = ExtractDisplayName(j, "");
        if (accessToken.empty() || refreshToken.empty() || expiresIn <= 0) {
            return fail("OmniStats returned an incomplete device session. Restart to retry, or sign in in Settings > Accounts & services.");
        }
        if (generation != m_tokenStore.AuthGeneration() || m_stopWorker.load(std::memory_order_relaxed)) {
            (void)SendLogoutRequest(refreshToken);
            return false;
        }
        m_tokenStore.SetTokens(accessToken, now + expiresIn, refreshToken, displayName, keyPair.publicId);
        m_authFlow.SetState(AccountAuthState::SignedIn);
        if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::AccountLinked);
        return true;
    } catch (...) {
        return fail("OmniStats returned an invalid device session. Restart to retry, or sign in in Settings > Accounts & services.");
    }
}

void AccountClient::QueueLogoutForRotatedToken(const std::string& previousToken, std::string rotatedToken) {
    std::lock_guard<std::mutex> lock(m_workerMutex);
    for (auto& job : m_workerQueue) {
        if (job.command == WorkerCommand::Logout && job.refreshToken == previousToken) {
            job.refreshToken = std::move(rotatedToken);
            m_workerCv.notify_all();
            return;
        }
    }
    EnsureWorkerRunningLocked();
    m_workerQueue.push_back(WorkerJob{
        .command = WorkerCommand::Logout,
        .openBrowser = false,
        .refreshToken = std::move(rotatedToken),
        .authGeneration = m_tokenStore.AuthGeneration()});
    m_workerCv.notify_all();
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
                return !m_workerQueue.empty() || m_stopWorker.load(std::memory_order_relaxed);
            });
            if (m_stopWorker.load(std::memory_order_relaxed)) break;
            if (m_workerQueue.empty()) continue;
            job = std::move(m_workerQueue.front());
            m_workerQueue.pop_front();
        }

        switch (job.command) {
        case WorkerCommand::StartSignIn: {
            auto startRes = StartAuthorization();
            if (startRes.ok && job.openBrowser) {
                OpenVerificationBrowser();
            }
            if (startRes.ok) {
                PollToken();
            }
            break;
        }
        case WorkerCommand::ExchangeAPIKey: {
            ExchangeAPIKey(job.authGeneration);
            break;
        }
        case WorkerCommand::Logout: {
            if (!job.refreshToken.empty()) {
                SendLogoutRequest(job.refreshToken);
            }
            break;
        }
        }
    }
}
