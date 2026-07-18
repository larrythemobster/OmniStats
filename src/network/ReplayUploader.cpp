#include "ReplayUploader.hpp"
#include "core/Config.hpp"
#include "core/KeyPressSimulator.hpp"
#include "core/PrivacyLog.hpp"
#include "core/SessionState.hpp"
#include <iostream>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")
#include <filesystem>
#include <vector>
#include <shared_mutex>

namespace fs = std::filesystem;

static size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    std::atomic<bool>* running = (std::atomic<bool>*)clientp;
    if (running && !running->load()) {
        return 1; // Abort transfer
    }
    return 0;
}

static std::string NormalizeVisibility(const std::string& visibility) {
    if (visibility == "private" || visibility == "unlisted" || visibility == "public") {
        return visibility;
    }
    return "unlisted";
}

ReplayUploader::ReplayUploader(std::shared_ptr<SessionState> state)
    : m_state(state), m_lastSaveAttempt(std::chrono::steady_clock::now()) {}

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
    // Attempt to open the file with exclusive access. If another process still has it open for writing,
    // this call will fail. Use CreateFileA with dwShareMode = 0 to request exclusive access.
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return false; // likely still being written or otherwise locked
    }
    CloseHandle(h);
    return true;
}

void ReplayUploader::UploadReplay(const std::string& path) {
    ConfigData conf = Config::Read();
    if (conf.ballchasing_token.empty()) {
        std::cout << "[ReplayUploader] No ballchasing token configured\n";
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cout << "[ReplayUploader] curl_easy_init failed\n";
        return;
    }

    std::string url = "https://ballchasing.com/api/v2/upload?visibility=" + NormalizeVisibility(conf.ballchasing_visibility);

    struct curl_slist* headers = nullptr;
    std::string auth = "Authorization: " + conf.ballchasing_token;
    headers = curl_slist_append(headers, auth.c_str());

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_filedata(part, path.c_str());

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);

    // Abort if app is shutting down
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &m_running);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        std::cout << "[ReplayUploader] curl error: " << curl_easy_strerror(res) << "\n";
    } else {
        std::cout << "[ReplayUploader] Uploaded replay " << PrivacyLog::Sensitive(path, "replay path") << " (HTTP " << http_code << ")\n";
        if (PrivacyLog::DebugEnabled()) {
            std::cout << "[ReplayUploader] Response: " << response << "\n";
        }
    }

    curl_slist_free_all(headers);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
}

void ReplayUploader::CheckAndSaveReplay() {
    ConfigData conf = Config::Read();
    if (!m_state || !conf.auto_save_replays) return;

    std::shared_lock<std::shared_mutex> lock(m_state->game.mutex);

    // Detect match end: was in match, now not in match
    if (m_wasInMatch && !m_state->game.inMatch) {
        // Check debounce: don't save too frequently (within 5 seconds)
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - m_lastSaveAttempt);
        if (elapsed.count() < 5) return;

        m_lastSaveAttempt = now;

        // Simulate the Save Replay keystroke (default: VK_BACK)
        // We use SimulateSaveReplayKeyPress to avoid intrusive keyboard simulation when alt-tabbed
        std::cout << "[ReplayUploader] Match ended. Simulating Save Replay keystroke...\n";
        SimulateSaveReplayKeyPress(conf.key_save_replay);
    }

    // Update state tracker
    m_wasInMatch = m_state->game.inMatch;
}

void ReplayUploader::WorkerLoop() {
    // Determine Rocket League TAGame path
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

    // Initial scan: mark existing files as processed to avoid backlog uploads
    try {
        for (const auto& dir : target_dirs) {
            if (fs::exists(dir)) {
                for (auto& p : fs::directory_iterator(dir)) {
                    if (!p.is_regular_file()) continue;
                    auto ext = p.path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                    if (!m_running.load()) break;
                    if (ext == ".replay") {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        MarkProcessed(p.path().string());
                    }
                }
            }
            if (!m_running.load()) break;
        }
    } catch (const std::exception& e) {
        std::cout << "[ReplayUploader] Initial scan failed: " << e.what() << "\n";
    }

    while (m_running.load()) {
        // Check for match end and auto-save
        CheckAndSaveReplay();

        try {
            for (const auto& dir : target_dirs) {
                if (fs::exists(dir)) {
                    for (auto& p : fs::directory_iterator(dir)) {
                        if (!p.is_regular_file()) continue;
                        if (!m_running.load()) break;
                        auto path = p.path().string();
                        auto ext = p.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                        if (ext != ".replay") continue;

                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            if (m_processed.count(path)) continue;
                        }

                        if (!Config::Read().auto_upload_replays) {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            MarkProcessed(path);
                            continue;
                        }

                        // Wait a brief moment to make sure file write completes
                        {
                            std::unique_lock<std::mutex> lock(m_mutex);
                            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this]() { return !m_running.load(); });
                        }
                        if (!m_running.load()) break;

                        if (!IsFileReady(path)) {
                            // Try again later
                            continue;
                        }

                        // Mark processed before upload to avoid duplicates
                        {
                            std::lock_guard<std::mutex> lock(m_mutex);
                            MarkProcessed(path);
                        }

                        std::cout << "[ReplayUploader] New replay detected: " << PrivacyLog::Sensitive(path, "replay path") << "\n";
                        UploadReplay(path);
                    }
                }
                if (!m_running.load()) break;
            }
        } catch (const std::exception& e) {
            std::cout << "[ReplayUploader] Error while scanning: " << e.what() << "\n";
        }

        // Sleep interval
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::seconds(5), [this]() { return !m_running.load(); });
        }
    }
}

void ReplayUploader::MarkProcessed(const std::string& path) {
    m_processed.insert(path);
    m_processedQueue.push(path);
    if (m_processed.size() > 10000) {
        std::string oldest = m_processedQueue.front();
        m_processedQueue.pop();
        m_processed.erase(oldest);
    }
}

void ReplayUploader::SimulateSaveReplayKeyPress(int key) {
    ::SimulateSaveReplayKeyPress(key, 50);
}
