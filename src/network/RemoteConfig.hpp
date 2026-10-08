#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <nlohmann/json.hpp>

struct RemoteAnnouncement {
    std::string id;
    std::string severity = "info";
    std::string title;
    std::string body;
    std::string link_url;
    bool dismissible = true;
    std::string ends_at;

    bool operator==(const RemoteAnnouncement& other) const noexcept = default;
};

struct RemoteConfigData {
    int schema = 1;
    int ttl_seconds = 900;
    std::vector<RemoteAnnouncement> announcements;
    std::string min_supported_version;
    std::string min_version_message;

    bool operator==(const RemoteConfigData& other) const noexcept = default;
};

class RemoteConfig {
  public:
    static constexpr int kMinTtlSeconds = 300;
    static constexpr int kMaxTtlSeconds = 3600;
    static constexpr int kDefaultTtlSeconds = 900;
    static constexpr int64_t kMaxCacheAgeSeconds = 24 * 3600;
    static constexpr size_t kMaxConfigPayloadBytes = 65536;
    static constexpr size_t kMaxAnnouncements = 5;
    static constexpr size_t kMaxAnnouncementsInPayload = 25;

    struct HttpResponse {
        int curlCode = 0;
        long statusCode = 0;
        std::string body;
    };

    using HttpTransportFn = std::function<HttpResponse(const std::string& url, const std::string& userAgent)>;
    using ClockFn = std::function<int64_t()>;

    static RemoteConfig& Instance();

    RemoteConfig();
    ~RemoteConfig();

    RemoteConfig(const RemoteConfig&) = delete;
    RemoteConfig& operator=(const RemoteConfig&) = delete;

    static int ClampTtlSeconds(int ttl) noexcept;
    static bool IsAllowedAnnouncementLinkUrl(std::string_view url) noexcept;
    static bool ParseRfc3339Unix(std::string_view text, int64_t& outUnix) noexcept;
    static bool IsValidVersionString(std::string_view version) noexcept;
    static int CompareVersions(std::string_view a, std::string_view b) noexcept;
    static bool IsVersionBelowMinimum(std::string_view currentVersion, std::string_view minSupportedVersion) noexcept;
    static bool ParseConfigJson(std::string_view jsonText, RemoteConfigData& outConfig, std::string* outError = nullptr);
    static nlohmann::json SerializeConfigJson(const RemoteConfigData& config);
    static std::vector<RemoteAnnouncement> FilterActiveAnnouncements(
        const std::vector<RemoteAnnouncement>& announcements,
        const std::unordered_set<std::string>& dismissedIds,
        int64_t nowUnix);

    void Start();
    void Shutdown();
    bool FetchOnce();

    RemoteConfigData GetSnapshot() const;
    uint64_t Version() const noexcept {
        return m_version.load(std::memory_order_acquire);
    }

    bool IsBelowMinSupportedVersion(std::string_view currentVersion = {}) const;

    std::vector<RemoteAnnouncement> ActiveAnnouncements(int64_t nowUnix = 0) const;
    void DismissAnnouncement(const std::string& id);
    bool IsAnnouncementDismissed(const std::string& id) const;
    void ClearDismissedAnnouncements();

    void SetTransportForTests(HttpTransportFn transport);
    void SetClockForTests(ClockFn clockFn);
    void SetCacheFilePathForTests(std::string path);
    void SetDismissedFilePathForTests(std::string path);
    void SetSnapshotForTests(const RemoteConfigData& data);
    void ResetForTests();

  private:
    void WorkerLoop();
    HttpResponse PerformHttpGet(const std::string& url, const std::string& userAgent);
    int64_t NowUnix() const;
    std::string CacheFilePath() const;
    std::string DismissedFilePath() const;
    bool LoadCachedConfigLocked(RemoteConfigData& outConfig, int64_t& outFetchedAt) const;
    void SaveCachedConfigLocked(const RemoteConfigData& config, int64_t fetchedAt) const;
    void LoadDismissedLocked();
    void SaveDismissedLocked() const;

    mutable std::mutex m_mutex;
    RemoteConfigData m_snapshot;
    int64_t m_lastFetchedAt = 0;
    bool m_hasCachedSnapshot = false;
    std::unordered_set<std::string> m_dismissedIds;
    bool m_dismissedLoaded = false;
    std::atomic<uint64_t> m_version{1};

    HttpTransportFn m_transport;
    ClockFn m_clock;
    std::string m_cacheFileOverride;
    std::string m_dismissedFileOverride;

    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    std::jthread m_workerThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
};
