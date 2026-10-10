#include "ReplayUploader.hpp"
#include "core/Config.hpp"
#include "core/KeyPressSimulator.hpp"
#include "core/PrivacyLog.hpp"
#include "core/SessionState.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <random>
#include <curl/curl.h>
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <vector>
#include <shared_mutex>

namespace fs = std::filesystem;

static constexpr int64_t kMatchResultWaitLimitSeconds = 30 * 60;

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    if (userp) {
        static_cast<std::string*>(userp)->append(static_cast<char*>(contents), size * nmemb);
    }
    return size * nmemb;
}

static size_t CurlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    if (userdata && buffer) {
        std::string header(buffer, total);
        auto* retryAfter = static_cast<int*>(userdata);
        std::string lower = header;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        if (lower.rfind("retry-after:", 0) == 0) {
            std::string val = header.substr(12);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
                val.erase(0, 1);
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' '))
                val.pop_back();
            try {
                *retryAfter = std::stoi(val);
            } catch (...) {
            }
        }
    }
    return total;
}

static int ProgressCallback(void* clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* running = static_cast<std::atomic<bool>*>(clientp);
    if (running && !running->load()) {
        return 1;
    }
    return 0;
}

static std::string NormalizeVisibility(const std::string& visibility) {
    if (visibility == "private" || visibility == "unlisted" || visibility == "public") {
        return visibility;
    }
    return "unlisted";
}

ReplayUploadResponseDecision EvaluateUploadResponse(
    long httpStatus,
    const std::string& responseBody,
    int currentAttempts,
    int maxAttempts,
    int retryAfterHeaderSeconds,
    int jitterSeconds) {
    ReplayUploadResponseDecision decision;

    if (httpStatus == 201) {
        decision.newState = "done";
        try {
            auto j = nlohmann::json::parse(responseBody, nullptr, false);
            if (!j.is_discarded()) {
                if (j.contains("id") && j["id"].is_string()) {
                    decision.ballchasingId = j["id"].get<std::string>();
                }
                if (j.contains("location") && j["location"].is_string()) {
                    decision.ballchasingUrl = j["location"].get<std::string>();
                }
            }
        } catch (...) {
        }
        if (decision.ballchasingUrl.empty() && !decision.ballchasingId.empty()) {
            decision.ballchasingUrl = "https://ballchasing.com/replay/" + decision.ballchasingId;
        }
        return decision;
    }

    if (httpStatus == 409) {
        decision.newState = "duplicate";
        decision.errorMessage = "Duplicate replay";
        try {
            auto j = nlohmann::json::parse(responseBody, nullptr, false);
            if (!j.is_discarded()) {
                if (j.contains("id") && j["id"].is_string()) {
                    decision.ballchasingId = j["id"].get<std::string>();
                }
                if (j.contains("location") && j["location"].is_string()) {
                    decision.ballchasingUrl = j["location"].get<std::string>();
                }
            }
        } catch (...) {
        }
        if (decision.ballchasingUrl.empty() && !decision.ballchasingId.empty()) {
            decision.ballchasingUrl = "https://ballchasing.com/replay/" + decision.ballchasingId;
        }
        return decision;
    }

    if (httpStatus == 401 || httpStatus == 403) {
        decision.newState = "pending";
        decision.tokenInvalid = true;
        decision.errorMessage = "Invalid Ballchasing API token (HTTP " + std::to_string(httpStatus) + ")";
        return decision;
    }

    if (httpStatus == 400) {
        decision.newState = "failed";
        decision.errorMessage = "Bad request (400)";
        return decision;
    }

    if (httpStatus == 429 || (httpStatus >= 500 && httpStatus <= 599) || httpStatus == 0) {
        if (currentAttempts + 1 >= maxAttempts) {
            decision.newState = "failed";
            decision.errorMessage = "Exceeded retry limit (HTTP " + std::to_string(httpStatus) + ")";
            return decision;
        }

        decision.newState = "pending";
        int exponent = std::min(currentAttempts, 5);
        int delay = 5 * (1 << exponent) + jitterSeconds;
        if (retryAfterHeaderSeconds > 0) {
            delay = std::max(delay, retryAfterHeaderSeconds);
        }
        decision.nextAttemptDelaySeconds = delay;
        decision.errorMessage = "Retry scheduled (HTTP " + std::to_string(httpStatus) + ")";
        return decision;
    }

    decision.newState = "failed";
    decision.errorMessage = "HTTP error " + std::to_string(httpStatus);
    return decision;
}

ReplayFilterResult EvaluateReplayFilter(const ReplayFilterContext& ctx) {
    if (!ctx.filterRankedOnly && !ctx.filterWinsOnly) {
        return ReplayFilterResult::Upload;
    }

    if (!ctx.isAttributed) {
        return ReplayFilterResult::Skip;
    }

    if (!ctx.hasMatchRecord || ctx.isResultPending) {
        return ReplayFilterResult::WaitForMatchResult;
    }

    if (ctx.filterRankedOnly && !ctx.isRanked) {
        return ReplayFilterResult::Skip;
    }

    if (ctx.filterWinsOnly && !ctx.isWin) {
        return ReplayFilterResult::Skip;
    }

    return ReplayFilterResult::Upload;
}

ReplayUploader::ReplayUploader(std::shared_ptr<SessionState> state, std::shared_ptr<DatabaseManager> db)
    : m_state(std::move(state)), m_db(std::move(db)) {
    if (m_db) {
        m_db->RecoverStuckUploadingReplays();
    }
}

ReplayUploader::~ReplayUploader() {
    Stop();
}

void ReplayUploader::Start() {
    if (m_running.load()) return;
    m_running.store(true);
    m_worker = std::jthread(&ReplayUploader::WorkerLoop, this);
    std::cout << "[ReplayUploader] Started\n";
}

void ReplayUploader::Stop() {
    if (!m_running.load()) return;
    m_running.store(false);
    m_cv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    std::cout << "[ReplayUploader] Stopped\n";
}

bool ReplayUploader::IsFileReady(const std::string& path) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    return true;
}

void ReplayUploader::CheckAndSaveReplay() {
    m_saveImminent = false;
    if (!m_state) return;
    const ConfigData conf = Config::Read();

    bool inMatch = false;
    int myTeam = -1;
    std::string guid;
    {
        std::shared_lock<std::shared_mutex> lock(m_state->game.mutex);
        inMatch = m_state->game.inMatch;
        myTeam = m_state->game.myTeam;
        guid = m_state->game.matchGuid;
    }
    const auto now = std::chrono::steady_clock::now();

    if (!m_expectingReplayGuid.empty() && !m_expectingReplayUntil && (!inMatch || guid != m_expectingReplayGuid)) {
        // RL writes the replay file when the match ends, long after the in-match save request.
        m_expectingReplayUntil = now + std::chrono::seconds(60);
    }

    if (!conf.auto_save_replays || !inMatch || guid.empty() || myTeam == -1 || guid == m_lastSavedGuid) {
        m_pendingSaveGuid.clear();
        return;
    }
    if (guid != m_pendingSaveGuid) {
        m_pendingSaveGuid = guid;
        m_pendingSaveSince = now;
    }
    if (now - m_pendingSaveSince < kSaveReplayDelay) {
        m_saveImminent = true;
        return;
    }

    m_lastSavedGuid = guid;
    m_pendingSaveGuid.clear();
    // RL's in-match save replay requires holding the bind.
    ::SimulateSaveReplayKeyPress(conf.key_save_replay, 1500);
    m_expectingReplayGuid = guid;
    m_expectingReplayUntil.reset();
    std::cout << "[ReplayUploader] Executed Save Replay keystroke (VK: " << conf.key_save_replay
              << ") for match " << PrivacyLog::Sensitive(guid, "match GUID") << "\n";
}

void ReplayUploader::ScanReplayDirectories(const std::vector<std::string>& targetDirs, bool initialScan) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& dir : targetDirs) {
        if (!m_running.load()) break;
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;

        for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); it.increment(ec)) {
            if (ec || !m_running.load()) break;
            if (!it->is_regular_file(ec)) continue;

            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext != ".replay") continue;

            const std::string path = it->path().string();
            const std::string filename = it->path().filename().string();

            if (initialScan) {
                if (m_db) {
                    m_db->RecordDiscoveredReplay(path, filename, "", "skipped", "Existing file at startup");
                }
            } else {
                if (m_db && m_db->GetReplayUploadByPath(path).has_value()) {
                    continue;
                }

                if (!IsFileReady(path)) {
                    continue;
                }

                std::string attributedGuid;
                if (!m_expectingReplayGuid.empty() && (!m_expectingReplayUntil || now <= *m_expectingReplayUntil)) {
                    attributedGuid = m_expectingReplayGuid;
                    m_expectingReplayGuid.clear();
                    m_expectingReplayUntil.reset();
                    std::cout << "[ReplayUploader] Confirmed save: attributed "
                              << PrivacyLog::Sensitive(path, "replay path") << " to match "
                              << PrivacyLog::Sensitive(attributedGuid, "match GUID") << "\n";
                }

                ConfigData conf = Config::Read();
                std::string initialState = conf.auto_upload_replays ? "pending" : "skipped";
                std::string initialError = conf.auto_upload_replays ? "" : "Auto-upload disabled";

                if (m_db) {
                    m_db->RecordDiscoveredReplay(path, filename, attributedGuid, initialState, initialError);
                }
                std::cout << "[ReplayUploader] Discovered replay: " << PrivacyLog::Sensitive(path, "replay path")
                          << " (state: " << initialState << ")\n";
            }
        }
    }
}

void ReplayUploader::ProcessPendingUploads() {
    if (!m_db) return;
    ConfigData conf = Config::Read();
    if (!conf.auto_upload_replays) return;
    if (conf.ballchasing_token.empty()) return;
    if (m_state && m_state->ui.ballchasingTokenRejected.load()) return;

    auto pending = m_db->GetPendingReplayUploads(5);
    for (const auto& rec : pending) {
        if (!m_running.load()) break;

        ReplayFilterContext ctx;
        ctx.filterRankedOnly = conf.ballchasing_filter_ranked_only;
        ctx.filterWinsOnly = conf.ballchasing_filter_wins_only;
        ctx.isAttributed = !rec.matchGuid.empty();

        if (ctx.isAttributed) {
            bool found = false, ranked = false, win = false, resultPending = false;
            int playlistId = -1;
            if (m_db->QueryMatchReplayInfo(rec.matchGuid, found, ranked, win, resultPending, playlistId)) {
                ctx.hasMatchRecord = found;
                ctx.isRanked = ranked;
                ctx.isWin = win;
                ctx.isResultPending = resultPending;
            } else {
                ctx.hasMatchRecord = false;
            }
        }

        ReplayFilterResult filterRes = EvaluateReplayFilter(ctx);
        if (filterRes == ReplayFilterResult::Skip) {
            m_db->UpdateReplayUploadState(rec.id, "skipped", rec.attempts, 0, 0, "Filtered out by upload settings");
            std::cout << "[ReplayUploader] Filter skipped upload for " << PrivacyLog::Sensitive(rec.filePath, "replay path") << "\n";
            continue;
        } else if (filterRes == ReplayFilterResult::WaitForMatchResult) {
            const int64_t nowUnix = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            if (nowUnix - rec.createdAt > kMatchResultWaitLimitSeconds) {
                m_db->UpdateReplayUploadState(rec.id, "skipped", rec.attempts, 0, 0, "Match result unavailable for upload filters");
            } else {
                m_db->UpdateReplayUploadState(rec.id, "pending", rec.attempts, nowUnix + 5, 0, "Waiting for match result");
            }
            continue;
        }

        if (!IsFileReady(rec.filePath)) {
            continue;
        }

        m_db->SetReplayUploading(rec.id);
        UploadReplayRecord(rec.id, rec.filePath, rec.matchGuid, rec.attempts);
    }
}

void ReplayUploader::UploadReplayRecord(int64_t id, const std::string& path, const std::string& matchGuid, int currentAttempts) {
    ConfigData conf = Config::Read();
    if (conf.ballchasing_token.empty()) return;

    CURL* curl = curl_easy_init();
    if (!curl) return;

    std::string url = "https://ballchasing.com/api/v2/upload?visibility=" + NormalizeVisibility(conf.ballchasing_visibility);

    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: " + conf.ballchasing_token;
    headers = curl_slist_append(headers, auth.c_str());

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, path.c_str());

    std::string response;
    int retryAfterSeconds = -1;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retryAfterSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_running);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        http_code = 0;
        std::cout << "[ReplayUploader] curl error: " << curl_easy_strerror(res) << "\n";
    }

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 3);
    int jitter = dist(rng);

    auto decision = EvaluateUploadResponse(http_code, response, currentAttempts, 5, retryAfterSeconds, jitter);

    if (decision.tokenInvalid && m_state) {
        m_state->ui.ballchasingTokenRejected.store(true);
        m_state->ui.replayUploadVersion.fetch_add(1, std::memory_order_relaxed);
    }

    int64_t nextAttemptAt = 0;
    if (decision.newState == "pending") {
        nextAttemptAt = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + decision.nextAttemptDelaySeconds;
    }

    if (m_db) {
        const int attempts = decision.tokenInvalid ? currentAttempts : currentAttempts + 1;
        m_db->UpdateReplayUploadState(id, decision.newState, attempts, nextAttemptAt, http_code, decision.errorMessage, decision.ballchasingId, decision.ballchasingUrl);
    }

    if (m_state && !matchGuid.empty() && !decision.ballchasingUrl.empty() &&
        (decision.newState == "done" || decision.newState == "duplicate")) {
        std::lock_guard lock(m_state->ui.replayUploadMutex);
        m_state->ui.lastBallchasingMatchGuid = matchGuid;
        m_state->ui.lastBallchasingUrl = decision.ballchasingUrl;
        m_state->ui.replayUploadVersion.fetch_add(1, std::memory_order_relaxed);
    }

    std::cout << "[ReplayUploader] Upload result for " << PrivacyLog::Sensitive(path, "replay path")
              << " -> state: " << decision.newState << " (HTTP " << http_code << ")\n";
}

void ReplayUploader::PublishUploadState() {
    if (!m_db || !m_state) return;
    const ReplayUploadStatus status = m_db->GetReplayUploadStatus();
    std::lock_guard lock(m_state->ui.replayUploadMutex);
    if (status == m_state->ui.replayUploadStatus) return;
    m_state->ui.replayUploadStatus = status;
    m_state->ui.replayUploadVersion.fetch_add(1, std::memory_order_relaxed);
}

void ReplayUploader::WorkerLoop() {
    std::string base_tagame;
    PWSTR path = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, NULL, &path))) {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
        std::string str(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, &str[0], size_needed, NULL, NULL);
        if (size_needed > 0 && str.back() == '\0') str.pop_back();
        base_tagame = str + "\\My Games\\Rocket League\\TAGame\\";
        CoTaskMemFree(path);
    } else {
        char* upath = getenv("USERPROFILE");
        if (upath) base_tagame = std::string(upath) + "\\Documents\\My Games\\Rocket League\\TAGame\\";
    }

    std::vector<std::string> target_dirs;
    if (!base_tagame.empty()) {
        target_dirs.push_back(base_tagame + "Demos\\");
        target_dirs.push_back(base_tagame + "DemosEpic\\");
    }

    try {
        ScanReplayDirectories(target_dirs, true);
    } catch (const std::exception& e) {
        std::cout << "[ReplayUploader] Initial scan failed: " << e.what() << "\n";
    }

    while (m_running.load()) {
        CheckAndSaveReplay();

        const auto now = std::chrono::steady_clock::now();
        if (!m_expectingReplayGuid.empty() && m_expectingReplayUntil && now > *m_expectingReplayUntil) {
            m_expectingReplayGuid.clear();
            m_expectingReplayUntil.reset();
        }

        try {
            ScanReplayDirectories(target_dirs, false);
            // A long upload here would delay the save keystroke.
            if (!m_saveImminent) ProcessPendingUploads();
            PublishUploadState();
        } catch (const std::exception& e) {
            std::cout << "[ReplayUploader] Worker loop error: " << e.what() << "\n";
        }

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(1000), [this]() { return !m_running.load(); });
        }
    }
}
