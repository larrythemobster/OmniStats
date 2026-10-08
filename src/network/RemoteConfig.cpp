#include "network/RemoteConfig.hpp"
#include "network/AccountClient.hpp"
#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

    constexpr size_t kMaxDismissedIds = 256;

    size_t CurlWriteBoundCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        const size_t total = size * nmemb;
        if (!userp || !contents) return 0;
        auto* body = static_cast<std::string*>(userp);
        if (body->size() + total > RemoteConfig::kMaxConfigPayloadBytes) {
            return 0;
        }
        body->append(static_cast<const char*>(contents), total);
        return total;
    }

    int CurlProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        const auto* stop = static_cast<const std::atomic<bool>*>(clientp);
        return (stop && stop->load(std::memory_order_relaxed)) ? 1 : 0;
    }

    bool ParseFixedDigits(std::string_view sv, size_t pos, size_t count, int& outVal) noexcept {
        if (pos + count > sv.size()) return false;
        int val = 0;
        for (size_t i = 0; i < count; ++i) {
            const char c = sv[pos + i];
            if (c < '0' || c > '9') return false;
            val = val * 10 + (c - '0');
        }
        outVal = val;
        return true;
    }

    bool IsLeapYear(int y) noexcept {
        return (y % 4 == 0) && (y % 100 != 0 || y % 400 == 0);
    }

    int DaysInMonth(int y, int m) noexcept {
        static constexpr int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (m < 1 || m > 12) return 0;
        if (m == 2 && IsLeapYear(y)) return 29;
        return kDays[m - 1];
    }

    int64_t DaysFromCivil(int y, int m, int d) noexcept {
        y -= m <= 2;
        const int era = (y >= 0 ? y : y - 399) / 400;
        const unsigned yoe = static_cast<unsigned>(y - era * 400);
        const unsigned doy = (153u * static_cast<unsigned>(m > 2 ? m - 3 : m + 9) + 2u) / 5u + static_cast<unsigned>(d) - 1u;
        const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
        return static_cast<int64_t>(era) * 146097LL + static_cast<int64_t>(doe) - 719468LL;
    }

    bool IsStrictInt(const nlohmann::json& j) noexcept {
        return j.is_number_integer() && !j.is_boolean();
    }

} // namespace

RemoteConfig& RemoteConfig::Instance() {
    static RemoteConfig instance;
    return instance;
}

RemoteConfig::RemoteConfig() = default;

RemoteConfig::~RemoteConfig() {
    Shutdown();
}

int RemoteConfig::ClampTtlSeconds(int ttl) noexcept {
    return std::clamp(ttl, kMinTtlSeconds, kMaxTtlSeconds);
}

bool RemoteConfig::IsAllowedAnnouncementLinkUrl(std::string_view url) noexcept {
    if (url.empty() || url.size() > 512) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '<' || c == '>') {
            return false;
        }
    }
    constexpr std::string_view kOmniPrefix = "https://omnistats.org/";
    constexpr std::string_view kOmniRoot = "https://omnistats.org";
    constexpr std::string_view kDiscordPrefix = "https://discord.gg/";
    if (url == kOmniRoot || url.rfind(kOmniPrefix, 0) == 0) {
        return true;
    }
    if (url.rfind(kDiscordPrefix, 0) == 0 && url.size() > kDiscordPrefix.size()) {
        return true;
    }
    return false;
}

bool RemoteConfig::ParseRfc3339Unix(std::string_view text, int64_t& outUnix) noexcept {
    outUnix = 0;
    if (text.size() < 20 || text.size() > 64) return false;

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ParseFixedDigits(text, 0, 4, year) || text[4] != '-' ||
        !ParseFixedDigits(text, 5, 2, month) || text[7] != '-' ||
        !ParseFixedDigits(text, 8, 2, day) ||
        (text[10] != 'T' && text[10] != 't') ||
        !ParseFixedDigits(text, 11, 2, hour) || text[13] != ':' ||
        !ParseFixedDigits(text, 14, 2, minute) || text[16] != ':' ||
        !ParseFixedDigits(text, 17, 2, second)) {
        return false;
    }

    if (year < 1970 || year > 2200) return false;
    const int maxDay = DaysInMonth(year, month);
    if (maxDay == 0 || day < 1 || day > maxDay) return false;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return false;

    size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        size_t fracDigits = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            ++pos;
            ++fracDigits;
        }
        if (fracDigits == 0 || fracDigits > 9) return false;
    }

    if (pos >= text.size()) return false;
    int offsetSeconds = 0;
    const char tzChar = text[pos];
    if (tzChar == 'Z' || tzChar == 'z') {
        if (pos + 1 != text.size()) return false;
    } else if (tzChar == '+' || tzChar == '-') {
        int tzHour = 0, tzMin = 0;
        if (pos + 6 != text.size() ||
            !ParseFixedDigits(text, pos + 1, 2, tzHour) ||
            text[pos + 3] != ':' ||
            !ParseFixedDigits(text, pos + 4, 2, tzMin)) {
            return false;
        }
        if (tzHour > 23 || tzMin > 59) return false;
        offsetSeconds = (tzHour * 3600 + tzMin * 60) * (tzChar == '-' ? -1 : 1);
    } else {
        return false;
    }

    const int64_t days = DaysFromCivil(year, month, day);
    const int64_t utcSeconds = days * 86400LL + hour * 3600LL + minute * 60LL + second - offsetSeconds;
    outUnix = utcSeconds;
    return true;
}

bool RemoteConfig::IsValidVersionString(std::string_view version) noexcept {
    if (version.empty() || version.size() > 32) return false;
    int dots = 0;
    size_t partLen = 0;
    for (char c : version) {
        if (c == '.') {
            if (partLen == 0) return false;
            ++dots;
            partLen = 0;
        } else if (c >= '0' && c <= '9') {
            ++partLen;
            if (partLen > 6) return false;
        } else {
            return false;
        }
    }
    return partLen > 0 && dots >= 1 && dots <= 3;
}

int RemoteConfig::CompareVersions(std::string_view a, std::string_view b) noexcept {
    auto parseParts = [](std::string_view v, int parts[4]) {
        parts[0] = parts[1] = parts[2] = parts[3] = 0;
        if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) {
            v.remove_prefix(1);
        }
        int idx = 0;
        int current = 0;
        bool hasDigit = false;
        for (char c : v) {
            if (c == '.') {
                if (idx < 4) parts[idx++] = current;
                current = 0;
                hasDigit = false;
            } else if (c >= '0' && c <= '9') {
                if (current <= 999999) {
                    current = current * 10 + (c - '0');
                }
                hasDigit = true;
            } else {
                break;
            }
        }
        if (hasDigit && idx < 4) {
            parts[idx] = current;
        }
    };

    int pa[4]{}, pb[4]{};
    parseParts(a, pa);
    parseParts(b, pb);
    for (int i = 0; i < 4; ++i) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return 1;
    }
    return 0;
}

bool RemoteConfig::IsVersionBelowMinimum(std::string_view currentVersion, std::string_view minSupportedVersion) noexcept {
    if (minSupportedVersion.empty() || !IsValidVersionString(minSupportedVersion)) {
        return false;
    }
    if (currentVersion.empty()) {
        return false;
    }
    return CompareVersions(currentVersion, minSupportedVersion) < 0;
}

bool RemoteConfig::ParseConfigJson(std::string_view jsonText, RemoteConfigData& outConfig, std::string* outError) {
    auto fail = [&](const char* msg) {
        if (outError) *outError = msg;
        return false;
    };

    if (jsonText.empty()) return fail("empty payload");
    if (jsonText.size() > kMaxConfigPayloadBytes) return fail("payload exceeds size limit");

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonText.begin(), jsonText.end());
    } catch (...) {
        return fail("invalid json");
    }

    if (!j.is_object()) return fail("root is not an object");
    if (!j.contains("schema") || !IsStrictInt(j["schema"]) || j["schema"].get<int>() != 1) {
        return fail("unsupported or missing schema");
    }

    RemoteConfigData parsed;
    parsed.schema = 1;

    if (j.contains("ttl_seconds")) {
        if (!IsStrictInt(j["ttl_seconds"])) return fail("ttl_seconds must be an integer");
        parsed.ttl_seconds = ClampTtlSeconds(j["ttl_seconds"].get<int>());
    } else {
        parsed.ttl_seconds = kDefaultTtlSeconds;
    }

    if (j.contains("announcements")) {
        const auto& annArr = j["announcements"];
        if (!annArr.is_array()) return fail("announcements must be an array");
        if (annArr.size() > kMaxAnnouncementsInPayload) return fail("too many announcements");

        for (const auto& item : annArr) {
            if (!item.is_object()) return fail("announcement must be an object");
            if (!item.contains("id") || !item["id"].is_string()) return fail("announcement id must be string");
            if (!item.contains("severity") || !item["severity"].is_string()) return fail("announcement severity must be string");
            if (!item.contains("title") || !item["title"].is_string()) return fail("announcement title must be string");
            if (!item.contains("body") || !item["body"].is_string()) return fail("announcement body must be string");

            RemoteAnnouncement ann;
            ann.id = item["id"].get<std::string>();
            ann.severity = item["severity"].get<std::string>();
            ann.title = item["title"].get<std::string>();
            ann.body = item["body"].get<std::string>();

            if (ann.id.empty() || ann.id.size() > 64) return fail("announcement id out of bounds");
            if (ann.severity != "info" && ann.severity != "warning" && ann.severity != "critical") {
                return fail("invalid announcement severity");
            }
            if (ann.title.empty() || ann.title.size() > 80) return fail("announcement title out of bounds");
            if (ann.body.size() > 500) return fail("announcement body out of bounds");

            if (item.contains("link_url")) {
                if (!item["link_url"].is_string()) return fail("announcement link_url must be string");
                ann.link_url = item["link_url"].get<std::string>();
                if (ann.link_url.size() > 512) return fail("announcement link_url too long");
                if (!ann.link_url.empty() && !IsAllowedAnnouncementLinkUrl(ann.link_url)) {
                    return fail("announcement link_url not in allowlist");
                }
            }

            if (item.contains("dismissible")) {
                if (!item["dismissible"].is_boolean()) return fail("announcement dismissible must be boolean");
                ann.dismissible = item["dismissible"].get<bool>();
            }

            if (item.contains("ends_at")) {
                if (!item["ends_at"].is_string()) return fail("announcement ends_at must be string");
                ann.ends_at = item["ends_at"].get<std::string>();
                if (ann.ends_at.size() > 64) return fail("announcement ends_at too long");
                if (!ann.ends_at.empty()) {
                    int64_t dummyUnix = 0;
                    if (!ParseRfc3339Unix(ann.ends_at, dummyUnix)) {
                        return fail("announcement ends_at invalid RFC3339");
                    }
                }
            }

            if (parsed.announcements.size() < kMaxAnnouncements) {
                parsed.announcements.push_back(std::move(ann));
            }
        }
    }

    if (j.contains("min_supported_version")) {
        if (!j["min_supported_version"].is_string()) return fail("min_supported_version must be string");
        parsed.min_supported_version = j["min_supported_version"].get<std::string>();
        if (parsed.min_supported_version.size() > 32) return fail("min_supported_version too long");
        if (!parsed.min_supported_version.empty() && !IsValidVersionString(parsed.min_supported_version)) {
            return fail("min_supported_version invalid format");
        }
    }

    if (j.contains("min_version_message")) {
        if (!j["min_version_message"].is_string()) return fail("min_version_message must be string");
        parsed.min_version_message = j["min_version_message"].get<std::string>();
        if (parsed.min_version_message.size() > 300) return fail("min_version_message too long");
    }

    outConfig = std::move(parsed);
    return true;
}

nlohmann::json RemoteConfig::SerializeConfigJson(const RemoteConfigData& config) {
    nlohmann::json annArr = nlohmann::json::array();
    for (const auto& ann : config.announcements) {
        annArr.push_back({{"id", ann.id},
                          {"severity", ann.severity},
                          {"title", ann.title},
                          {"body", ann.body},
                          {"link_url", ann.link_url},
                          {"dismissible", ann.dismissible},
                          {"ends_at", ann.ends_at}});
    }
    return {
        {"schema", config.schema},
        {"ttl_seconds", ClampTtlSeconds(config.ttl_seconds)},
        {"announcements", std::move(annArr)},
        {"min_supported_version", config.min_supported_version},
        {"min_version_message", config.min_version_message}};
}

std::vector<RemoteAnnouncement> RemoteConfig::FilterActiveAnnouncements(
    const std::vector<RemoteAnnouncement>& announcements,
    const std::unordered_set<std::string>& dismissedIds,
    int64_t nowUnix) {
    std::vector<RemoteAnnouncement> result;
    for (const auto& ann : announcements) {
        if (ann.id.empty() || ann.title.empty()) continue;
        if (ann.dismissible && dismissedIds.count(ann.id) != 0) continue;
        if (!ann.ends_at.empty() && nowUnix > 0) {
            int64_t endsUnix = 0;
            if (ParseRfc3339Unix(ann.ends_at, endsUnix) && nowUnix >= endsUnix) {
                continue;
            }
        }
        RemoteAnnouncement copy = ann;
        if (!copy.link_url.empty() && !IsAllowedAnnouncementLinkUrl(copy.link_url)) {
            copy.link_url.clear();
        }
        result.push_back(std::move(copy));
        if (result.size() >= kMaxAnnouncements) break;
    }
    return result;
}

void RemoteConfig::Start() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        LoadDismissedLocked();
        RemoteConfigData cached;
        int64_t fetchedAt = 0;
        const int64_t now = NowUnix();
        if (LoadCachedConfigLocked(cached, fetchedAt) &&
            fetchedAt > 0 && now >= fetchedAt && (now - fetchedAt) < kMaxCacheAgeSeconds) {
            m_snapshot = std::move(cached);
            m_lastFetchedAt = fetchedAt;
            m_hasCachedSnapshot = true;
            m_version.fetch_add(1, std::memory_order_release);
        }
    }

#ifdef OMNISTATS_TEST_ENVIRONMENT
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_transport) {
            return;
        }
    }
#endif

    std::lock_guard<std::mutex> workerLock(m_workerMutex);
    if (m_running.load(std::memory_order_relaxed)) return;
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_running.store(true, std::memory_order_relaxed);
    m_workerThread = std::jthread(&RemoteConfig::WorkerLoop, this);
}

void RemoteConfig::Shutdown() {
    m_stopRequested.store(true, std::memory_order_relaxed);
    m_workerCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
    m_running.store(false, std::memory_order_relaxed);
}

void RemoteConfig::WorkerLoop() {
    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        (void)FetchOnce();
        if (m_stopRequested.load(std::memory_order_relaxed)) break;

        int ttlSeconds = kDefaultTtlSeconds;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ttlSeconds = ClampTtlSeconds(m_snapshot.ttl_seconds);
        }

        std::unique_lock<std::mutex> lock(m_workerMutex);
        m_workerCv.wait_for(lock, std::chrono::seconds(ttlSeconds), [this]() {
            return m_stopRequested.load(std::memory_order_relaxed);
        });
    }
}

bool RemoteConfig::FetchOnce() {
    std::string baseUrl = AccountClient::ApiBaseUrl();
    const std::string url = baseUrl + "/api/v1/client/config";

    const std::string userAgent = std::string("OmniStats-Client/") + AppVersion::Current;
    const HttpResponse resp = PerformHttpGet(url, userAgent);
    const int64_t now = NowUnix();

    RemoteConfigData parsed;
    if (resp.curlCode == 0 && resp.statusCode == 200 && ParseConfigJson(resp.body, parsed)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool changed = !(m_snapshot == parsed);
        m_snapshot = parsed;
        m_lastFetchedAt = now;
        m_hasCachedSnapshot = true;
        SaveCachedConfigLocked(m_snapshot, now);
        if (changed) {
            m_version.fetch_add(1, std::memory_order_release);
        }
        return true;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_hasCachedSnapshot && m_lastFetchedAt > 0 && now >= m_lastFetchedAt &&
        (now - m_lastFetchedAt) < kMaxCacheAgeSeconds) {
        return false;
    }

    RemoteConfigData cached;
    int64_t fetchedAt = 0;
    if (LoadCachedConfigLocked(cached, fetchedAt) &&
        fetchedAt > 0 && now >= fetchedAt && (now - fetchedAt) < kMaxCacheAgeSeconds) {
        const bool changed = !(m_snapshot == cached);
        m_snapshot = std::move(cached);
        m_lastFetchedAt = fetchedAt;
        m_hasCachedSnapshot = true;
        if (changed) {
            m_version.fetch_add(1, std::memory_order_release);
        }
        return false;
    }

    RemoteConfigData defaults;
    const bool changed = !(m_snapshot == defaults);
    m_snapshot = std::move(defaults);
    m_hasCachedSnapshot = false;
    m_lastFetchedAt = 0;
    if (changed) {
        m_version.fetch_add(1, std::memory_order_release);
    }
    return false;
}

RemoteConfig::HttpResponse RemoteConfig::PerformHttpGet(const std::string& url, const std::string& userAgent) {
    HttpTransportFn customTransport;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        customTransport = m_transport;
    }
    if (customTransport) {
        return customTransport(url, userAgent);
    }

    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
        response.curlCode = CURLE_FAILED_INIT;
        return response;
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteBoundCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_stopRequested);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    const CURLcode res = curl_easy_perform(curl);
    response.curlCode = static_cast<int>(res);
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

RemoteConfigData RemoteConfig::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot;
}

bool RemoteConfig::IsBelowMinSupportedVersion(std::string_view currentVersion) const {
    const std::string_view effectiveVersion = currentVersion.empty() ? std::string_view(AppVersion::Current) : currentVersion;
    std::lock_guard<std::mutex> lock(m_mutex);
    return IsVersionBelowMinimum(effectiveVersion, m_snapshot.min_supported_version);
}

std::vector<RemoteAnnouncement> RemoteConfig::ActiveAnnouncements(int64_t nowUnix) const {
    const int64_t effectiveNow = nowUnix > 0 ? nowUnix : NowUnix();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dismissedLoaded) {
        const_cast<RemoteConfig*>(this)->LoadDismissedLocked();
    }
    return FilterActiveAnnouncements(m_snapshot.announcements, m_dismissedIds, effectiveNow);
}

void RemoteConfig::DismissAnnouncement(const std::string& id) {
    if (id.empty() || id.size() > 64) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dismissedLoaded) {
        LoadDismissedLocked();
    }
    if (m_dismissedIds.size() >= kMaxDismissedIds && m_dismissedIds.count(id) == 0) {
        m_dismissedIds.erase(m_dismissedIds.begin());
    }
    if (m_dismissedIds.insert(id).second) {
        SaveDismissedLocked();
        m_version.fetch_add(1, std::memory_order_release);
    }
}

bool RemoteConfig::IsAnnouncementDismissed(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dismissedLoaded) {
        const_cast<RemoteConfig*>(this)->LoadDismissedLocked();
    }
    return m_dismissedIds.count(id) != 0;
}

void RemoteConfig::ClearDismissedAnnouncements() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dismissedIds.clear();
    m_dismissedLoaded = true;
    SaveDismissedLocked();
    m_version.fetch_add(1, std::memory_order_release);
}

int64_t RemoteConfig::NowUnix() const {
    if (m_clock) {
        return m_clock();
    }
    return static_cast<int64_t>(std::time(nullptr));
}

std::string RemoteConfig::CacheFilePath() const {
    if (!m_cacheFileOverride.empty()) {
        return m_cacheFileOverride;
    }
    return Storage::GetDataDirectory() + "remote_config_cache.json";
}

std::string RemoteConfig::DismissedFilePath() const {
    if (!m_dismissedFileOverride.empty()) {
        return m_dismissedFileOverride;
    }
    return Storage::GetDataDirectory() + "remote_config_dismissed.json";
}

bool RemoteConfig::LoadCachedConfigLocked(RemoteConfigData& outConfig, int64_t& outFetchedAt) const {
    outFetchedAt = 0;
    const std::string path = CacheFilePath();
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return false;
    const auto fileSize = fs::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > kMaxConfigPayloadBytes * 2) return false;

    try {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        nlohmann::json wrapper;
        in >> wrapper;
        if (!wrapper.is_object() || !wrapper.contains("fetched_at") || !IsStrictInt(wrapper["fetched_at"]) ||
            !wrapper.contains("config") || !wrapper["config"].is_object()) {
            return false;
        }
        const int64_t fetchedAt = wrapper["fetched_at"].get<int64_t>();
        if (fetchedAt <= 0) return false;
        const std::string dumped = wrapper["config"].dump();
        RemoteConfigData parsed;
        if (!ParseConfigJson(dumped, parsed)) return false;
        outConfig = std::move(parsed);
        outFetchedAt = fetchedAt;
        return true;
    } catch (...) {
        return false;
    }
}

void RemoteConfig::SaveCachedConfigLocked(const RemoteConfigData& config, int64_t fetchedAt) const {
    try {
        Storage::InitializeEnvironment();
        const std::string path = CacheFilePath();
        const fs::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(fsPath.parent_path(), ec);
        }
        const nlohmann::json wrapper = {
            {"fetched_at", fetchedAt},
            {"config", SerializeConfigJson(config)}};
        std::ofstream out(path, std::ios::trunc);
        if (out.is_open()) {
            out << wrapper.dump(2);
        }
    } catch (...) {
    }
}

void RemoteConfig::LoadDismissedLocked() {
    m_dismissedLoaded = true;
    m_dismissedIds.clear();
    const std::string path = DismissedFilePath();
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return;
    const auto fileSize = fs::file_size(path, ec);
    if (ec || fileSize == 0 || fileSize > 32768) return;

    try {
        std::ifstream in(path);
        if (!in.is_open()) return;
        nlohmann::json j;
        in >> j;
        if (!j.is_object() || !j.contains("dismissed") || !j["dismissed"].is_array()) return;
        for (const auto& item : j["dismissed"]) {
            if (item.is_string()) {
                const std::string id = item.get<std::string>();
                if (!id.empty() && id.size() <= 64 && m_dismissedIds.size() < kMaxDismissedIds) {
                    m_dismissedIds.insert(id);
                }
            }
        }
    } catch (...) {
    }
}

void RemoteConfig::SaveDismissedLocked() const {
    try {
        Storage::InitializeEnvironment();
        const std::string path = DismissedFilePath();
        const fs::path fsPath(path);
        if (fsPath.has_parent_path()) {
            std::error_code ec;
            fs::create_directories(fsPath.parent_path(), ec);
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : m_dismissedIds) {
            arr.push_back(id);
        }
        const nlohmann::json wrapper = {{"dismissed", std::move(arr)}};
        std::ofstream out(path, std::ios::trunc);
        if (out.is_open()) {
            out << wrapper.dump();
        }
    } catch (...) {
    }
}

void RemoteConfig::SetTransportForTests(HttpTransportFn transport) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_transport = std::move(transport);
}

void RemoteConfig::SetClockForTests(ClockFn clockFn) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = std::move(clockFn);
}

void RemoteConfig::SetCacheFilePathForTests(std::string path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cacheFileOverride = std::move(path);
}

void RemoteConfig::SetDismissedFilePathForTests(std::string path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_dismissedFileOverride = std::move(path);
    m_dismissedLoaded = false;
}

void RemoteConfig::SetSnapshotForTests(const RemoteConfigData& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = data;
    m_hasCachedSnapshot = true;
    m_lastFetchedAt = NowUnix();
    m_version.fetch_add(1, std::memory_order_release);
}

void RemoteConfig::ResetForTests() {
    Shutdown();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = RemoteConfigData{};
    m_lastFetchedAt = 0;
    m_hasCachedSnapshot = false;
    m_dismissedIds.clear();
    m_dismissedLoaded = false;
    m_transport = nullptr;
    m_clock = nullptr;
    m_cacheFileOverride.clear();
    m_dismissedFileOverride.clear();
    m_version.fetch_add(1, std::memory_order_release);
}
