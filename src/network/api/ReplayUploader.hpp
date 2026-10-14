#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>
#include <vector>
#include <optional>

class SessionState;
class DatabaseManager;

enum class ReplayFilterResult {
    Upload,
    Skip,
    WaitForMatchResult
};

struct ReplayFilterContext {
    bool filterRankedOnly = false;
    bool filterWinsOnly = false;
    bool isAttributed = false;
    bool hasMatchRecord = false;
    bool isRanked = false;
    bool isWin = false;
    bool isResultPending = false;
};

struct ReplayUploadResponseDecision {
    std::string newState; // done, duplicate, failed, pending
    int nextAttemptDelaySeconds = 0;
    bool tokenInvalid = false;
    std::string ballchasingId;
    std::string ballchasingUrl;
    std::string errorMessage;
};

ReplayUploadResponseDecision EvaluateUploadResponse(
    long httpStatus,
    const std::string& responseBody,
    int currentAttempts,
    int maxAttempts = 5,
    int retryAfterHeaderSeconds = -1,
    int jitterSeconds = 0);

ReplayFilterResult EvaluateReplayFilter(const ReplayFilterContext& ctx);

inline constexpr auto kSaveReplayDelay = std::chrono::seconds(3);

class ReplayUploader {
  public:
    ReplayUploader(std::shared_ptr<SessionState> state = nullptr,
                   std::shared_ptr<DatabaseManager> db = nullptr);
    ~ReplayUploader();

    void Start();
    void Stop();

  private:
    void WorkerLoop();
    void PublishUploadState();
    bool IsFileReady(const std::string& path);
    void CheckAndSaveReplay();
    void ScanReplayDirectories(const std::vector<std::string>& targetDirs, bool initialScan);
    void ProcessPendingUploads();
    void UploadReplayRecord(int64_t id, const std::string& path, const std::string& matchGuid, int currentAttempts);

    std::jthread m_worker;
    std::atomic<bool> m_running{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;

    std::shared_ptr<SessionState> m_state;
    std::shared_ptr<DatabaseManager> m_db;

    std::string m_lastSavedGuid;
    std::string m_pendingSaveGuid;
    std::chrono::steady_clock::time_point m_pendingSaveSince{};
    bool m_saveImminent = false;

    std::string m_expectingReplayGuid;
    std::optional<std::chrono::steady_clock::time_point> m_expectingReplayUntil;
};
