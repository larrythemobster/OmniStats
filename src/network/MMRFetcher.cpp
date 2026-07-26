#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>

namespace {

    struct TournamentRankThreshold {
        int minMmr;
        const char* tier;
        const char* division;
    };

    static constexpr std::array<TournamentRankThreshold, 85> TournamentRankThresholds = {{{-4, "Bronze I", "Div I"},
                                                                                          {119, "Bronze I", "Div II"},
                                                                                          {138, "Bronze I", "Div III"},
                                                                                          {157, "Bronze I", "Div IV"},
                                                                                          {162, "Bronze II", "Div I"},
                                                                                          {179, "Bronze II", "Div II"},
                                                                                          {198, "Bronze II", "Div III"},
                                                                                          {217, "Bronze II", "Div IV"},
                                                                                          {221, "Bronze III", "Div I"},
                                                                                          {239, "Bronze III", "Div II"},
                                                                                          {258, "Bronze III", "Div III"},
                                                                                          {277, "Bronze III", "Div IV"},
                                                                                          {283, "Silver I", "Div I"},
                                                                                          {299, "Silver I", "Div II"},
                                                                                          {318, "Silver I", "Div III"},
                                                                                          {337, "Silver I", "Div IV"},
                                                                                          {342, "Silver II", "Div I"},
                                                                                          {359, "Silver II", "Div II"},
                                                                                          {378, "Silver II", "Div III"},
                                                                                          {397, "Silver II", "Div IV"},
                                                                                          {403, "Silver III", "Div I"},
                                                                                          {419, "Silver III", "Div II"},
                                                                                          {438, "Silver III", "Div III"},
                                                                                          {457, "Silver III", "Div IV"},
                                                                                          {461, "Gold I", "Div I"},
                                                                                          {478, "Gold I", "Div II"},
                                                                                          {498, "Gold I", "Div III"},
                                                                                          {517, "Gold I", "Div IV"},
                                                                                          {521, "Gold II", "Div I"},
                                                                                          {539, "Gold II", "Div II"},
                                                                                          {558, "Gold II", "Div III"},
                                                                                          {577, "Gold II", "Div IV"},
                                                                                          {581, "Gold III", "Div I"},
                                                                                          {598, "Gold III", "Div II"},
                                                                                          {618, "Gold III", "Div III"},
                                                                                          {637, "Gold III", "Div IV"},
                                                                                          {641, "Platinum I", "Div I"},
                                                                                          {659, "Platinum I", "Div II"},
                                                                                          {678, "Platinum I", "Div III"},
                                                                                          {697, "Platinum I", "Div IV"},
                                                                                          {701, "Platinum II", "Div I"},
                                                                                          {719, "Platinum II", "Div II"},
                                                                                          {738, "Platinum II", "Div III"},
                                                                                          {757, "Platinum II", "Div IV"},
                                                                                          {761, "Platinum III", "Div I"},
                                                                                          {779, "Platinum III", "Div II"},
                                                                                          {798, "Platinum III", "Div III"},
                                                                                          {817, "Platinum III", "Div IV"},
                                                                                          {824, "Diamond I", "Div I"},
                                                                                          {844, "Diamond I", "Div II"},
                                                                                          {868, "Diamond I", "Div III"},
                                                                                          {892, "Diamond I", "Div IV"},
                                                                                          {901, "Diamond II", "Div I"},
                                                                                          {924, "Diamond II", "Div II"},
                                                                                          {948, "Diamond II", "Div III"},
                                                                                          {972, "Diamond II", "Div IV"},
                                                                                          {981, "Diamond III", "Div I"},
                                                                                          {1004, "Diamond III", "Div II"},
                                                                                          {1028, "Diamond III", "Div III"},
                                                                                          {1052, "Diamond III", "Div IV"},
                                                                                          {1061, "Champion I", "Div I"},
                                                                                          {1096, "Champion I", "Div II"},
                                                                                          {1128, "Champion I", "Div III"},
                                                                                          {1162, "Champion I", "Div IV"},
                                                                                          {1181, "Champion II", "Div I"},
                                                                                          {1215, "Champion II", "Div II"},
                                                                                          {1254, "Champion II", "Div III"},
                                                                                          {1282, "Champion II", "Div IV"},
                                                                                          {1301, "Champion III", "Div I"},
                                                                                          {1337, "Champion III", "Div II"},
                                                                                          {1368, "Champion III", "Div III"},
                                                                                          {1402, "Champion III", "Div IV"},
                                                                                          {1421, "Grand Champion I", "Div I"},
                                                                                          {1460, "Grand Champion I", "Div II"},
                                                                                          {1498, "Grand Champion I", "Div III"},
                                                                                          {1537, "Grand Champion I", "Div IV"},
                                                                                          {1561, "Grand Champion II", "Div I"},
                                                                                          {1600, "Grand Champion II", "Div II"},
                                                                                          {1638, "Grand Champion II", "Div III"},
                                                                                          {1677, "Grand Champion II", "Div IV"},
                                                                                          {1701, "Grand Champion III", "Div I"},
                                                                                          {1745, "Grand Champion III", "Div II"},
                                                                                          {1788, "Grand Champion III", "Div III"},
                                                                                          {1832, "Grand Champion III", "Div IV"},
                                                                                          {1861, "Supersonic Legend", ""}}};

    static bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out) {
        for (const char* key : keys) {
            if (!stats.contains(key) || !stats[key].is_object()) continue;
            const auto& stat = stats[key];
            if (stat.contains("value") && stat["value"].is_number()) {
                out = stat["value"].get<int>();
                return true;
            }
        }
        return false;
    }

    static bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr) {
        auto it = playlistMMRs.find(playlistName);
        return it == playlistMMRs.end() || mmr > it->second;
    }

#ifdef OMNISTATS_TEST_ENVIRONMENT
    static constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(20);
    static constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(20);
    static constexpr auto kTransientRetryDelay = std::chrono::milliseconds(20);
#else
    static constexpr auto kPostMatchInitialDelay = std::chrono::milliseconds(2500);
    static constexpr auto kStalePostMatchRetryDelay = std::chrono::milliseconds(3000);
    static constexpr auto kTransientRetryDelay = std::chrono::milliseconds(3000);
#endif

} // namespace

// Helper function for libcurl to write the HTTP response into a std::string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static int CIProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    std::atomic<bool>* running = (std::atomic<bool>*)clientp;
    if (running && !running->load()) {
        return 1; // Abort
    }
    return 0;
}

MMRFetcher::MMRFetcher(std::shared_ptr<SessionState> state,
                       std::shared_ptr<DatabaseManager> dbManager)
    : m_state(std::move(state)), m_dbManager(std::move(dbManager)) {}

MMRFetcher::~MMRFetcher() {
    Stop();
}

std::string MMRFetcher::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";

    const TournamentRankThreshold* threshold = nullptr;
    for (const auto& entry : TournamentRankThresholds) {
        if (mmr < entry.minMmr) break;
        threshold = &entry;
    }

    if (!threshold) return "Unranked";
    if (threshold->division[0] == '\0') return threshold->tier;
    return std::string(threshold->tier) + " " + threshold->division;
}

std::string MMRFetcher::PlaylistNameForTrackerId(int playlistId) {
    switch (playlistId) {
    case 10:
        return "1v1";
    case 11:
        return "2v2";
    case 13:
        return "3v3";
    case 27:
        return "hoops";
    case 28:
        return "rumble";
    case 29:
        return "dropshot";
    case 30:
        return "snowday";
    case 34:
        return "t";
    case 43:
        return "heatseeker";
    case 0:
        return "casual";
    default:
        return "";
    }
}

MMRProfileTotals MMRFetcher::ExtractProfileTotals(const nlohmann::json& jsonResp) {
    MMRProfileTotals totals;
    if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
        !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
        return totals;
    }

    for (const auto& seg : jsonResp["data"]["segments"]) {
        if (!seg.is_object()) continue;
        if (seg.contains("type") && seg["type"].is_string() && seg["type"] == "playlist") continue;
        if (!seg.contains("stats") || !seg["stats"].is_object()) continue;

        const auto& stats = seg["stats"];
        if (totals.totalWins < 0) {
            (void)TryReadStatValue(stats, {"wins", "Wins"}, totals.totalWins);
        }
    }

    return totals;
}

bool MMRFetcher::IsPostMatchMmrStale(int previousMmr, int fetchedMmr, int previousMatches, int fetchedMatches) {
    if (fetchedMmr <= 0) return true;
    if (previousMmr <= 0 || fetchedMmr != previousMmr) return false;
    if (previousMatches > 0 && fetchedMatches > previousMatches) return false;
    return true;
}

#ifdef OMNISTATS_TEST_ENVIRONMENT
size_t MMRFetcher::PendingRequestCountForTests() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return m_queue.size();
}
#endif

void MMRFetcher::Start() {
    if (m_isRunning) return;
    m_isRunning = true;
    m_workerThread = std::jthread(&MMRFetcher::WorkerLoop, this);
    std::cout << "[MMRFetcher] Background thread started.\n";
}

void MMRFetcher::Stop() {
    m_isRunning = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MMRFetcher::Enqueue(const std::string& primaryId, const std::string& name) {
    if (!Config::Read().enable_mmr_tracking || primaryId.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_rosterQueuedOrInFlight.count(primaryId)) return;
        m_rosterQueuedOrInFlight.insert(primaryId);

        MMRRequest request;
        request.primaryId = primaryId;
        request.name = name;
        request.reason = MMRRequestReason::Roster;
        request.retriesRemaining = 2;
        request.notBefore = std::chrono::steady_clock::now();
        m_queue.push_back(std::move(request));
    }
    m_cv.notify_one();
}

void MMRFetcher::EnqueuePostMatch(const std::string& primaryId,
                                  const std::string& name,
                                  const std::string& matchGuid,
                                  const std::string& playlist,
                                  int previousMmr,
                                  int previousMatches) {
    if (!Config::Read().enable_mmr_tracking || primaryId.empty() || matchGuid.empty() || playlist.empty()) return;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_pendingPostMatchGuids.count(matchGuid) || m_completedPostMatchGuids.count(matchGuid)) return;

        m_pendingPostMatchGuids.insert(matchGuid);

        MMRRequest request;
        request.primaryId = primaryId;
        request.name = name;
        request.reason = MMRRequestReason::PostMatch;
        request.matchGuid = matchGuid;
        request.playlist = playlist;
        request.previousMmr = previousMmr;
        request.previousMatches = previousMatches;
        request.retriesRemaining = 2;
        request.notBefore = std::chrono::steady_clock::now() + kPostMatchInitialDelay;
        m_queue.push_back(std::move(request));
    }
    m_cv.notify_one();
}

std::string MMRFetcher::GetTRNPlatform(const std::string& primaryId) {
    size_t delim = primaryId.find('|');
    if (delim == std::string::npos) return "";

    std::string plat = primaryId.substr(0, delim);
    // Trim potential spaces
    plat.erase(0, plat.find_first_not_of(" \t\r\n"));
    plat.erase(plat.find_last_not_of(" \t\r\n") + 1);

    // Convert to lowercase for case-insensitive matching
    std::transform(plat.begin(), plat.end(), plat.begin(), ::tolower);

    if (plat == "epic" || plat == "epicgames") return "epic";
    if (plat == "steam") return "steam";
    if (plat == "ps4" || plat == "psn" || plat == "playstation") return "psn";
    if (plat == "xboxone" || plat == "xbox" || plat == "xbl") return "xbl";
    if (plat == "switch" || plat == "nintendo") return "switch";
    return "";
}

void MMRFetcher::WorkerLoop() {
    auto& ci = CurlImpersonate::Instance();
    if (!ci.EnsureAvailable()) {
        std::cout << "[MMRFetcher] WARNING: curl-impersonate not available. "
                  << "MMR fetching will be disabled.\n";
    }

    while (m_isRunning) {
        MMRRequest req;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            while (m_isRunning && m_queue.empty()) {
                m_cv.wait(lock);
            }
            if (!m_isRunning && m_queue.empty()) break;

            auto nextIt = std::min_element(
                m_queue.begin(), m_queue.end(),
                [](const MMRRequest& lhs, const MMRRequest& rhs) {
                    return lhs.notBefore < rhs.notBefore;
                });

            const auto now = std::chrono::steady_clock::now();
            if (nextIt->notBefore > now) {
                const auto wakeAt = nextIt->notBefore;
                m_cv.wait_until(lock, wakeAt);
                continue;
            }

            req = std::move(*nextIt);
            m_queue.erase(nextIt);
        }

        const bool requeued = FetchProfile(req);
        if (!requeued) FinishRequest(req);

        std::unique_lock<std::mutex> lock(m_queueMutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return !m_isRunning; });
    }
}

bool MMRFetcher::ScheduleRetry(MMRRequest req, std::chrono::milliseconds delay, const char* reason) {
    if (!m_isRunning || req.retriesRemaining <= 0) return false;

    --req.retriesRemaining;
    req.notBefore = std::chrono::steady_clock::now() + delay;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(std::move(req));
    }
    std::cout << "[MMRFetcher] Retrying MMR request after " << delay.count()
              << " ms (" << reason << ").\n";
    m_cv.notify_one();
    return true;
}

void MMRFetcher::FinishRequest(const MMRRequest& req) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (req.reason == MMRRequestReason::PostMatch) {
        m_pendingPostMatchGuids.erase(req.matchGuid);
    } else {
        m_rosterQueuedOrInFlight.erase(req.primaryId);
    }
}

bool MMRFetcher::FetchProfile(MMRRequest req) {
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        std::cout << "[MMRFetcher] Skipping " << PrivacyLog::Sensitive(req.name, "player name")
                  << ": curl-impersonate not loaded\n";
        return false;
    }

    const std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) return false;

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return false;

    const size_t delim = req.primaryId.find('|');
    std::string ident;
    if (plat == "steam") {
        if (delim != std::string::npos) {
            std::string sub = req.primaryId.substr(delim + 1);
            const size_t secondDelim = sub.find('|');
            ident = secondDelim != std::string::npos ? sub.substr(0, secondDelim) : sub;
        } else {
            ident = req.primaryId;
        }
    } else {
        ident = req.name;
    }

    char* escapedIdent = ci.easy_escape(ci_curl, ident.c_str(), static_cast<int>(ident.length()));
    if (!escapedIdent) {
        ci.easy_cleanup(ci_curl);
        return false;
    }
    const std::string finalIdent = escapedIdent;
    ci.free_ptr(escapedIdent);

    const std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + finalIdent;
    std::cout << "[MMRFetcher] Fetching " << PrivacyLog::Sensitive(req.name, "player name") << " via " << plat << "...\n";

    ci.easy_impersonate(ci_curl, "chrome136", 0);

    std::string readBuffer;
    void* headers = nullptr;
    headers = ci.slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = ci.slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = ci.slist_append(headers, "Origin: https://rocketleague.tracker.network");
    headers = ci.slist_append(headers, "Referer: https://rocketleague.tracker.network/");
    headers = ci.slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36");

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 15L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFODATA, &m_isRunning);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 0L);

    const int res = ci.easy_perform(ci_curl);
    long httpCode = 0;
    ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0 || httpCode != 200) {
        std::cout << "[MMRFetcher] Failed to fetch " << PrivacyLog::Sensitive(req.name, "player name")
                  << " (HTTP " << httpCode << ") - Curl error: " << res << "\n";

        if (httpCode == 429) {
            if (ScheduleRetry(req, std::chrono::seconds(15), "rate limited")) return true;
        } else if (res != 0 || httpCode == 408 || (httpCode >= 500 && httpCode <= 599)) {
            if (ScheduleRetry(req, kTransientRetryDelay, "transient Tracker failure")) return true;
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }

    try {
        const nlohmann::json jsonResp = nlohmann::json::parse(readBuffer);
        const MMRProfileTotals profileTotals = ExtractProfileTotals(jsonResp);
        if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
            !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
            if (ScheduleRetry(req, kTransientRetryDelay, "incomplete Tracker response")) return true;
            return false;
        }

        int bestMMR = 0;
        std::string bestTier = "Unranked";
        std::string bestPlaylistName = "best";
        std::map<std::string, int> playlistMMRs;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;

        for (const auto& seg : jsonResp["data"]["segments"]) {
            if (!seg.is_object() || !seg.contains("type") || !seg["type"].is_string() || seg["type"] != "playlist" ||
                !seg.contains("attributes") || !seg["attributes"].is_object()) {
                continue;
            }

            const auto& attrs = seg["attributes"];
            if (!attrs.contains("playlistId") || !attrs["playlistId"].is_number_integer()) continue;

            const std::string playlistName = PlaylistNameForTrackerId(attrs["playlistId"].get<int>());
            if (playlistName.empty() || !seg.contains("stats") || !seg["stats"].is_object()) continue;

            const auto& stats = seg["stats"];
            if (!stats.contains("rating") || !stats["rating"].is_object()) continue;
            const auto& rating = stats["rating"];
            if (!rating.contains("value") || !rating["value"].is_number()) continue;

            const int mmr = rating["value"].get<int>();
            std::string tier = "Unranked";
            if (stats.contains("tier") && stats["tier"].is_object()) {
                const auto& tierObj = stats["tier"];
                if (tierObj.contains("metadata") && tierObj["metadata"].is_object() &&
                    tierObj["metadata"].contains("name") && tierObj["metadata"]["name"].is_string()) {
                    tier = tierObj["metadata"]["name"].get<std::string>();
                }
            }

            std::string division;
            if (stats.contains("division") && stats["division"].is_object()) {
                const auto& divObj = stats["division"];
                if (divObj.contains("metadata") && divObj["metadata"].is_object() &&
                    divObj["metadata"].contains("name") && divObj["metadata"]["name"].is_string()) {
                    division = divObj["metadata"]["name"].get<std::string>();
                }
                if (division.empty() && divObj.contains("displayValue") && divObj["displayValue"].is_string()) {
                    division = divObj["displayValue"].get<std::string>();
                }
                if (division.empty() && divObj.contains("value") && divObj["value"].is_number()) {
                    switch (divObj["value"].get<int>()) {
                    case 1:
                        division = "Div I";
                        break;
                    case 2:
                        division = "Div II";
                        break;
                    case 3:
                        division = "Div III";
                        break;
                    case 4:
                        division = "Div IV";
                        break;
                    default:
                        break;
                    }
                }
            }

            if (!division.empty()) {
                const size_t pos = division.find("Division ");
                if (pos != std::string::npos) division.replace(pos, 9, "Div ");
                if (tier != "Unranked") tier += " " + division;
            }
            if (playlistName == "t") tier = GetTournamentTierForMmr(mmr);

            int matches = 0;
            if (stats.contains("matchesPlayed") && stats["matchesPlayed"].is_object() &&
                stats["matchesPlayed"].contains("value") && stats["matchesPlayed"]["value"].is_number()) {
                matches = stats["matchesPlayed"]["value"].get<int>();
            }

            if (ShouldReplacePlaylistBucket(playlistMMRs, playlistName, mmr)) {
                playlistMMRs[playlistName] = mmr;
                playlistTiers[playlistName] = tier;
            }
            playlistMatches[playlistName] += matches;

            if (playlistName != "casual" && playlistName != "t" && mmr > bestMMR) {
                bestMMR = mmr;
                bestTier = tier;
                bestPlaylistName = playlistName;
            }
        }

        playlistMMRs["best"] = bestMMR;
        playlistTiers["best"] = bestTier;
        playlistMatches["best"] = playlistMatches.count(bestPlaylistName) ? playlistMatches[bestPlaylistName] : 0;

        int postMatchMmr = 0;
        int postMatchMatches = 0;
        if (req.reason == MMRRequestReason::PostMatch) {
            const auto playlistIt = playlistMMRs.find(req.playlist);
            if (playlistIt != playlistMMRs.end()) postMatchMmr = playlistIt->second;
            const auto matchesIt = playlistMatches.find(req.playlist);
            if (matchesIt != playlistMatches.end()) postMatchMatches = matchesIt->second;
        }

        const bool stalePostMatch = req.reason == MMRRequestReason::PostMatch &&
                                    IsPostMatchMmrStale(req.previousMmr,
                                                        postMatchMmr,
                                                        req.previousMatches,
                                                        postMatchMatches);
        const bool retryStalePostMatch = stalePostMatch && req.retriesRemaining > 0;
        const int graphMmr = postMatchMmr > 0 ? postMatchMmr : req.previousMmr;

        bool appendPostMatchPoint = false;
        if (req.reason == MMRRequestReason::PostMatch && !retryStalePostMatch) {
            std::lock_guard<std::mutex> queueLock(m_queueMutex);
            appendPostMatchPoint = m_completedPostMatchGuids.insert(req.matchGuid).second;
        }

        {
            std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
            std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);

            if (m_state->game.roster.count(req.primaryId)) {
                auto& player = m_state->game.roster[req.primaryId];
                player.playlists = playlistMMRs;
                player.playlistTiers = playlistTiers;
                player.playlistMatches = playlistMatches;
                player.totalWins = profileTotals.totalWins;
                player.mmr = bestMMR;
                player.rankTier = bestTier;
                player.fetched = true;
                player.fetchFailed = false;
            }

            if (req.primaryId == m_state->game.myPrimaryId) {
                if (m_state->history.initialMmr == -1 && bestMMR > 0) {
                    m_state->history.initialMmr = bestMMR;
                }
                if (req.reason == MMRRequestReason::Roster && bestMMR > 0) {
                    m_state->history.mmrHistoryY.push_back(static_cast<float>(bestMMR));
                    m_state->history.mmrHistoryX.push_back(static_cast<float>(m_state->history.mmrHistoryY.size()));
                } else if (appendPostMatchPoint && bestMMR > 0) {
                    m_state->history.mmrHistoryY.push_back(static_cast<float>(bestMMR));
                    m_state->history.mmrHistoryX.push_back(static_cast<float>(m_state->history.mmrHistoryY.size()));
                }

                for (const auto& [playlistName, mmr] : playlistMMRs) {
                    if (mmr <= 0) continue;
                    if (req.reason == MMRRequestReason::PostMatch && playlistName == req.playlist) continue;
                    if (m_state->history.playlistInitialMmr.count(playlistName) == 0) {
                        m_state->history.playlistInitialMmr[playlistName] = mmr;
                    }
                    m_state->game.sessionTotals.mmrChangeByPlaylist[playlistName] =
                        mmr - m_state->history.playlistInitialMmr[playlistName];

                    auto& history = m_state->history.playlistHistoryY[playlistName];
                    if (history.empty() || history.back() != mmr) {
                        history.push_back(static_cast<float>(mmr));
                    }
                }

                if (appendPostMatchPoint && graphMmr > 0) {
                    if (m_state->history.playlistInitialMmr.count(req.playlist) == 0) {
                        m_state->history.playlistInitialMmr[req.playlist] = req.previousMmr > 0 ? req.previousMmr : graphMmr;
                    }
                    auto& history = m_state->history.playlistHistoryY[req.playlist];
                    history.push_back(static_cast<float>(graphMmr));
                    m_state->game.sessionTotals.mmrChangeByPlaylist[req.playlist] =
                        graphMmr - m_state->history.playlistInitialMmr[req.playlist];
                }

                std::string activePlaylist = req.reason == MMRRequestReason::PostMatch ? req.playlist : "";
                if (activePlaylist.empty()) {
                    const std::string arenaKey = !m_state->game.arenaAsset.empty() ? m_state->game.arenaAsset : m_state->game.arenaName;
                    activePlaylist = GamemodeUtils::InferFromSnapshot(
                        m_state->game.maxPlayersSeen,
                        static_cast<int>(m_state->game.roster.size()),
                        m_state->ui.rosterMmrCategory.load(),
                        m_state->ui.graphMmrCategory.load(),
                        arenaKey);
                }

                if (activePlaylist != "Unknown" && m_state->game.sessionTotals.mmrChangeByPlaylist.count(activePlaylist)) {
                    m_state->game.sessionTotals.totalMmrChange =
                        static_cast<float>(m_state->game.sessionTotals.mmrChangeByPlaylist[activePlaylist]);
                } else {
                    const std::string graphCat = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                    if (m_state->game.sessionTotals.mmrChangeByPlaylist.count(graphCat)) {
                        m_state->game.sessionTotals.totalMmrChange =
                            static_cast<float>(m_state->game.sessionTotals.mmrChangeByPlaylist[graphCat]);
                    } else if (bestMMR > 0 && m_state->history.initialMmr != -1) {
                        m_state->game.sessionTotals.totalMmrChange =
                            static_cast<float>(bestMMR - m_state->history.initialMmr);
                    }
                }
            }

            std::cout << "[MMRFetcher] Updated: " << PrivacyLog::Sensitive(req.name, "player name")
                      << " -> Best: " << bestMMR << "\n";
            m_state->game.version++;
            m_state->history.version++;
        }

        if (retryStalePostMatch) {
            return ScheduleRetry(req, kStalePostMatchRetryDelay, "post-match MMR not updated yet");
        }

        if (appendPostMatchPoint && graphMmr > 0) {
            if (auto db = m_dbManager.lock()) {
                db->AsyncUpdateMatchPlayerMmr(req.matchGuid, req.primaryId, graphMmr);
            }
        }
        return false;
    } catch (const std::exception& e) {
        std::cout << "[MMRFetcher] JSON Parse Error: " << e.what() << "\n";
        if (ScheduleRetry(req, kTransientRetryDelay, "invalid Tracker response")) return true;

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }
}
